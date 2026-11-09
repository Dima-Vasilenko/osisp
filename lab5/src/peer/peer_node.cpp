#include "peer/peer_node.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "common/framing.h"
#include "common/socket_utils.h"

namespace {

std::atomic<bool> g_stop_signal{false};

using Clock = std::chrono::steady_clock;

void sleep_checked(const std::atomic<bool>& stop, int milliseconds) {
    const auto until = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (!stop.load() && !g_stop_signal.load() && Clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

}  // namespace

void request_stop_signal() {
    g_stop_signal.store(true);
}

bool stop_signal_requested() {
    return g_stop_signal.load();
}

PeerNode::PeerNode(PeerConfig config, std::string log_file)
    : config_(std::move(config)),
      logger_("PEER", config_.id, std::move(log_file), config_.quiet) {}

PeerNode::~PeerNode() {
    stop();
}

bool PeerNode::stopping() const {
    return stop_.load() || g_stop_signal.load();
}

int PeerNode::open_listener(std::string& error) {
    return create_listener(config_.host, config_.port, error);
}

int PeerNode::run() {
    std::string error;
    listener_fd_ = open_listener(error);
    if (listener_fd_ < 0) {
        logger_.log("LISTEN_ERROR",
                    "host=" + config_.host +
                        " port=" + std::to_string(config_.port) +
                        " error=" + error);
        return 1;
    }

    logger_.log("START", "host=" + config_.host +
                             " port=" + std::to_string(config_.port) +
                             " id=" + config_.id);

    listener_thread_ = std::thread(&PeerNode::listener_loop, this);
    for (const auto& peer : config_.peers) {
        add_worker(std::thread(&PeerNode::connector_loop, this, peer.first,
                               peer.second));
    }

    sleep_checked(stop_, config_.connect_wait_ms);
    if (config_.messages > 0) {
        run_experiment();
    } else {
        run_interactive();
    }

    stop();
    return 0;
}

void PeerNode::add_worker(std::thread worker) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers_.emplace_back(std::move(worker));
}

std::vector<std::shared_ptr<PeerNode::Connection>>
PeerNode::connection_snapshot() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_;
}

void PeerNode::listener_loop() {
    while (!stopping()) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listener_fd_, &read_set);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        const int selected =
            ::select(listener_fd_ + 1, &read_set, nullptr, nullptr, &timeout);
        if (selected == 0) {
            continue;
        }
        if (selected < 0) {
            if (errno == EINTR) {
                continue;
            }
            logger_.log("SELECT_ERROR",
                        std::string("select: ") + std::strerror(errno));
            break;
        }

        sockaddr_storage address{};
        socklen_t address_length = sizeof(address);
        const int client_fd =
            ::accept(listener_fd_, reinterpret_cast<sockaddr*>(&address),
                     &address_length);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            logger_.log("ACCEPT_ERROR",
                        std::string("accept: ") + std::strerror(errno));
            continue;
        }

        set_socket_timeouts(client_fd, 500, config_.timeout_ms);
        auto connection = std::make_shared<Connection>();
        connection->fd = client_fd;
        connection->remote = endpoint_to_string(address, address_length);
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.push_back(connection);
        }
        stats_.accepted.fetch_add(1);
        logger_.log("CONNECT_ACCEPTED", "remote=" + connection->remote);
        add_worker(std::thread(&PeerNode::handle_connection, this, connection));
    }
}

void PeerNode::connector_loop(std::string host, int port) {
    const std::string target = host + ":" + std::to_string(port);
    while (!stopping()) {
        std::string error;
        const int fd =
            connect_with_timeout(host, port, std::min(config_.timeout_ms, 1000),
                                 error);
        if (fd < 0) {
            stats_.connect_failures.fetch_add(1);
            logger_.log("CONNECT_FAIL", "peer=" + target + " error=" + error);
            sleep_checked(stop_, 300);
            continue;
        }

        set_socket_timeouts(fd, 500, config_.timeout_ms);
        auto connection = std::make_shared<Connection>();
        connection->fd = fd;
        connection->remote = target;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.push_back(connection);
        }
        stats_.established.fetch_add(1);
        logger_.log("CONNECT_ESTABLISHED", "peer=" + target);

        handle_connection(connection);
        break;
    }
}

void PeerNode::handle_connection(std::shared_ptr<Connection> connection) {
    send_hello(connection);

    while (!stopping() && !connection->closed.load()) {
        std::string payload;
        std::string error;
        const IoResult result = recv_frame(connection->fd, payload, error);

        if (result == IoResult::Timeout) {
            continue;
        }
        if (result == IoResult::Closed) {
            logger_.log("PEER_CLOSED", "peer=" + connection->remote);
            break;
        }
        if (result == IoResult::Error) {
            logger_.log("RECV_ERROR",
                        "peer=" + connection->remote + " error=" + error);
            break;
        }

        stats_.bytes_received.fetch_add(payload.size() + 4);
        proto::Message message;
        std::string parse_error;
        if (!proto::decode(payload, message, parse_error)) {
            stats_.protocol_errors.fetch_add(1);
            logger_.log("PROTOCOL_ERROR", "peer=" + connection->remote +
                                              " error=" + parse_error);
            send_error(connection, "BAD_MESSAGE", parse_error);
            continue;
        }
        handle_message(connection, message);
    }

    cleanup_connection(connection);
}

void PeerNode::cleanup_connection(
    const std::shared_ptr<Connection>& connection) {
    if (!connection->closed.exchange(true)) {
        ::shutdown(connection->fd, SHUT_RDWR);
        ::close(connection->fd);
    }
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(
            std::remove(connections_.begin(), connections_.end(), connection),
            connections_.end());
    }
    logger_.log("DISCONNECTED",
                "peer=" + (connection->peer_id.empty() ? connection->remote
                                                       : connection->peer_id));
}

bool PeerNode::send_payload(const std::shared_ptr<Connection>& connection,
                            const std::string& payload) {
    std::lock_guard<std::mutex> lock(connection->write_mutex);
    if (connection->closed.load()) {
        return false;
    }
    std::string error;
    if (!send_frame(connection->fd, payload, error)) {
        logger_.log("SEND_ERROR",
                    "peer=" + connection->remote + " error=" + error);
        ::shutdown(connection->fd, SHUT_RDWR);
        return false;
    }
    stats_.bytes_sent.fetch_add(payload.size() + 4);
    return true;
}

void PeerNode::send_hello(const std::shared_ptr<Connection>& connection) {
    proto::Message message;
    message.type = "HELLO";
    message.sender = config_.id;
    if (send_payload(connection, proto::encode(message))) {
        logger_.log("HELLO_SEND", "peer=" + connection->remote);
    }
}

void PeerNode::send_error(const std::shared_ptr<Connection>& connection,
                          const std::string& code, const std::string& text) {
    proto::Message message;
    message.type = "ERROR";
    message.sender = config_.id;
    message.code = code;
    message.text = text;
    send_payload(connection, proto::encode(message));
}

void PeerNode::handle_message(const std::shared_ptr<Connection>& connection,
                              const proto::Message& message) {
    if (message.type == "HELLO") {
        if (connection->peer_id.empty()) {
            connection->peer_id = message.sender;
            logger_.log("HELLO_RECV",
                        "peer=" + connection->peer_id + " remote=" +
                            connection->remote);
        } else if (connection->peer_id != message.sender) {
            logger_.log("HELLO_MISMATCH",
                        "expected=" + connection->peer_id + " got=" +
                            message.sender);
        }
        return;
    }

    if (message.type == "ERROR") {
        logger_.log("ERROR_RECV", "from=" + message.sender +
                                      " code=" + message.code +
                                      " text=" + message.text);
        return;
    }

    if (message.type == "CHAT") {
        handle_chat(connection, message);
    }
}

void PeerNode::handle_chat(const std::shared_ptr<Connection>& connection,
                           const proto::Message& message) {
    logger_.log("RECV_CHAT", "msgid=" + message.msgid +
                                 " from=" + message.sender +
                                 " dest=" + message.dest +
                                 " ttl=" + std::to_string(message.ttl));

    {
        std::lock_guard<std::mutex> lock(seen_mutex_);
        if (!seen_messages_.insert(message.msgid).second) {
            stats_.dropped_dup.fetch_add(1);
            logger_.log("DUP", "msgid=" + message.msgid);
            return;
        }
    }

    if (message.dest == config_.id) {
        stats_.delivered.fetch_add(1);
        std::string latency_detail;
        const std::int64_t now_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (message.sent_us > 0 && now_us >= message.sent_us) {
            const std::uint64_t latency =
                static_cast<std::uint64_t>(now_us - message.sent_us);
            stats_.latency_samples.fetch_add(1);
            stats_.latency_us_sum.fetch_add(latency);

            std::uint64_t current_min = stats_.latency_us_min.load();
            while ((current_min == 0 || latency < current_min) &&
                   !stats_.latency_us_min.compare_exchange_weak(
                       current_min, latency)) {
            }
            std::uint64_t current_max = stats_.latency_us_max.load();
            while (latency > current_max &&
                   !stats_.latency_us_max.compare_exchange_weak(
                       current_max, latency)) {
            }
            latency_detail = " latency_us=" + std::to_string(latency);
        }

        logger_.log("DELIVER", "msgid=" + message.msgid +
                                   " from=" + message.sender +
                                   " text=" + message.text +
                                   latency_detail);
        std::cout << "[CHAT] from=" << message.sender
                  << " msgid=" << message.msgid << " text=" << message.text
                  << '\n';
        std::cout.flush();
        return;
    }

    if (message.ttl > 0) {
        forward_chat(connection, message, message.ttl - 1);
    } else {
        stats_.dropped_ttl.fetch_add(1);
        logger_.log("DROP_TTL", "msgid=" + message.msgid +
                                    " dest=" + message.dest);
    }
}

void PeerNode::forward_chat(const std::shared_ptr<Connection>& connection,
                            const proto::Message& message, int ttl) {
    proto::Message forwarded = message;
    forwarded.ttl = ttl;
    const std::string payload = proto::encode(forwarded);

    const auto connections = connection_snapshot();
    int targets = 0;
    for (const auto& candidate : connections) {
        if (candidate == connection || candidate->closed.load() ||
            candidate->peer_id.empty()) {
            continue;
        }
        if (send_payload(candidate, payload)) {
            ++targets;
        }
    }

    if (targets == 0) {
        stats_.dropped_no_route.fetch_add(1);
        logger_.log("NO_ROUTE", "msgid=" + message.msgid +
                                    " dest=" + message.dest);
        return;
    }

    stats_.forwarded.fetch_add(1);
    logger_.log("FORWARD", "msgid=" + message.msgid +
                               " from=" + message.sender +
                               " dest=" + message.dest +
                               " ttl=" + std::to_string(ttl) +
                               " targets=" + std::to_string(targets));
}

std::string PeerNode::make_message_id() {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto sequence = message_counter_.fetch_add(1);
    return config_.id + "-" + std::to_string(milliseconds) + "-" +
           std::to_string(sequence);
}

void PeerNode::send_chat(const std::string& destination,
                         const std::string& text) {
    proto::Message message;
    message.type = "CHAT";
    message.sender = config_.id;
    message.msgid = make_message_id();
    message.dest = destination;
    message.ttl = config_.ttl;
    message.text = text;
    message.sent_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    {
        std::lock_guard<std::mutex> lock(seen_mutex_);
        seen_messages_.insert(message.msgid);
    }

    const std::string payload = proto::encode(message);
    const auto connections = connection_snapshot();
    int targets = 0;
    for (const auto& connection : connections) {
        if (connection->closed.load() || connection->peer_id.empty()) {
            continue;
        }
        if (send_payload(connection, payload)) {
            ++targets;
        }
    }

    stats_.sent_chat.fetch_add(1);
    logger_.log("SEND_CHAT", "msgid=" + message.msgid +
                                 " dest=" + destination +
                                 " ttl=" + std::to_string(message.ttl) +
                                 " targets=" + std::to_string(targets) +
                                 " size=" + std::to_string(payload.size()));
    if (targets == 0) {
        logger_.log("NO_CONNECTION", "msgid=" + message.msgid);
    }
}

void PeerNode::run_experiment() {
    if (config_.dest.empty()) {
        logger_.log("EXPERIMENT_ERROR", "destination is not set");
        return;
    }

    logger_.log("EXPERIMENT_START",
                "messages=" + std::to_string(config_.messages) +
                    " dest=" + config_.dest +
                    " interval_ms=" + std::to_string(config_.send_interval_ms) +
                    " ttl=" + std::to_string(config_.ttl));

    for (int index = 0; index < config_.messages && !stopping(); ++index) {
        send_chat(config_.dest,
                  config_.text + " #" + std::to_string(index + 1));
        sleep_checked(stop_, config_.send_interval_ms);
    }
    logger_.log("SENT_ALL",
                "messages=" + std::to_string(config_.messages));

    int wait_ms = config_.run_ms;
    if (wait_ms == 0 && config_.expect_messages == 0) {
        wait_ms = 1000;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
    while (!stopping() && Clock::now() < deadline) {
        if (config_.expect_messages > 0 &&
            stats_.delivered.load() >=
                static_cast<std::uint64_t>(config_.expect_messages)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void PeerNode::run_interactive() {
    logger_.log("INTERACTIVE",
                "commands: msg <dest> <text> | stats | help | quit");
    const auto deadline =
        config_.run_ms > 0
            ? Clock::now() + std::chrono::milliseconds(config_.run_ms)
            : Clock::time_point::max();

    std::string line;
    while (!stopping()) {
        if (config_.run_ms > 0 && Clock::now() >= deadline) {
            break;
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        const int selected =
            ::select(STDIN_FILENO + 1, &read_set, nullptr, nullptr, &timeout);
        if (selected == 0) {
            continue;
        }
        if (selected < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (!std::getline(std::cin, line)) {
            if (config_.run_ms > 0) {
                while (!stopping() && Clock::now() < deadline) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));
                }
            }
            break;
        }

        if (line == "quit" || line == "exit") {
            break;
        }
        if (line == "stats") {
            print_stats();
            continue;
        }
        if (line == "help") {
            std::cout << "msg <dest> <text>  send a chat message\n"
                         "stats              print counters\n"
                         "quit               stop this peer\n";
            continue;
        }
        if (line.rfind("msg ", 0) == 0) {
            const std::string rest = line.substr(4);
            const std::size_t separator = rest.find(' ');
            if (separator == std::string::npos || separator == 0) {
                std::cout << "usage: msg <dest> <text>\n";
                continue;
            }
            send_chat(rest.substr(0, separator),
                      rest.substr(separator + 1));
            continue;
        }
        std::cout << "unknown command; type 'help'\n";
    }
}

void PeerNode::print_stats() {
    std::ostringstream details;
    details << "sent_chat=" << stats_.sent_chat.load()
            << " delivered=" << stats_.delivered.load()
            << " forwarded=" << stats_.forwarded.load()
            << " dropped_ttl=" << stats_.dropped_ttl.load()
            << " dropped_dup=" << stats_.dropped_dup.load()
            << " dropped_no_route=" << stats_.dropped_no_route.load()
            << " protocol_errors=" << stats_.protocol_errors.load()
            << " bytes_sent=" << stats_.bytes_sent.load()
            << " bytes_received=" << stats_.bytes_received.load()
            << " accepted=" << stats_.accepted.load()
            << " established=" << stats_.established.load()
            << " connect_failures=" << stats_.connect_failures.load()
            << " latency_samples=" << stats_.latency_samples.load()
            << " latency_us_sum=" << stats_.latency_us_sum.load()
            << " latency_us_min=" << stats_.latency_us_min.load()
            << " latency_us_max=" << stats_.latency_us_max.load();
    logger_.log("SUMMARY", details.str());
}

void PeerNode::write_stats_file() {
    if (config_.stats_file.empty()) {
        return;
    }
    std::ofstream out(config_.stats_file, std::ios::out | std::ios::trunc);
    if (!out) {
        logger_.log("STATS_ERROR",
                    "cannot open " + config_.stats_file);
        return;
    }
    out << "id=" << config_.id << ' '
        << "sent_chat=" << stats_.sent_chat.load() << ' '
        << "delivered=" << stats_.delivered.load() << ' '
        << "forwarded=" << stats_.forwarded.load() << ' '
        << "dropped_ttl=" << stats_.dropped_ttl.load() << ' '
        << "dropped_dup=" << stats_.dropped_dup.load() << ' '
        << "dropped_no_route=" << stats_.dropped_no_route.load() << ' '
        << "protocol_errors=" << stats_.protocol_errors.load() << ' '
        << "bytes_sent=" << stats_.bytes_sent.load() << ' '
        << "bytes_received=" << stats_.bytes_received.load() << ' '
        << "accepted=" << stats_.accepted.load() << ' '
        << "established=" << stats_.established.load() << ' '
        << "connect_failures=" << stats_.connect_failures.load() << ' '
        << "latency_samples=" << stats_.latency_samples.load() << ' '
        << "latency_us_sum=" << stats_.latency_us_sum.load() << ' '
        << "latency_us_min=" << stats_.latency_us_min.load() << ' '
        << "latency_us_max=" << stats_.latency_us_max.load() << '\n';
}

void PeerNode::shutdown_connections() {
    const auto connections = connection_snapshot();
    for (const auto& connection : connections) {
        if (!connection->closed.load()) {
            ::shutdown(connection->fd, SHUT_RDWR);
        }
    }
}

void PeerNode::stop() {
    if (stop_.exchange(true)) {
        return;
    }

    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }
    if (listener_fd_ >= 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
    }

    shutdown_connections();

    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& connection : connections_) {
            if (!connection->closed.exchange(true)) {
                ::close(connection->fd);
            }
        }
        connections_.clear();
    }

    logger_.log("STOP", std::string("reason=") +
                            (stop_signal_requested() ? "signal" : "normal"));
    print_stats();
    write_stats_file();
}

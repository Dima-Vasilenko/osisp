#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "common/protocol.h"

struct PeerConfig {
    std::string id;
    std::string host = "127.0.0.1";
    int port = 20028;
    std::vector<std::pair<std::string, int>> peers;
    int ttl = 3;
    int timeout_ms = 3000;
    int messages = 0;
    std::string dest;
    std::string text = "hello";
    int send_interval_ms = 100;
    int run_ms = 0;
    int expect_messages = 0;
    int connect_wait_ms = 700;
    std::string log_file;
    std::string stats_file;
    bool quiet = false;
};

struct PeerStats {
    std::atomic<std::uint64_t> sent_chat{0};
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> forwarded{0};
    std::atomic<std::uint64_t> dropped_ttl{0};
    std::atomic<std::uint64_t> dropped_dup{0};
    std::atomic<std::uint64_t> dropped_no_route{0};
    std::atomic<std::uint64_t> protocol_errors{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> established{0};
    std::atomic<std::uint64_t> connect_failures{0};
    std::atomic<std::uint64_t> latency_samples{0};
    std::atomic<std::uint64_t> latency_us_sum{0};
    std::atomic<std::uint64_t> latency_us_min{0};
    std::atomic<std::uint64_t> latency_us_max{0};
};

void request_stop_signal();
bool stop_signal_requested();

class PeerNode {
public:
    PeerNode(PeerConfig config, std::string log_file);
    ~PeerNode();

    int run();
    void stop();

private:
    struct Connection {
        int fd = -1;
        std::string peer_id;
        std::string remote;
        std::mutex write_mutex;
        std::atomic<bool> closed{false};
    };

    int open_listener(std::string& error);
    void listener_loop();
    void connector_loop(std::string host, int port);
    void handle_connection(std::shared_ptr<Connection> connection);
    void cleanup_connection(const std::shared_ptr<Connection>& connection);

    bool send_payload(const std::shared_ptr<Connection>& connection,
                      const std::string& payload);
    void send_hello(const std::shared_ptr<Connection>& connection);
    void send_error(const std::shared_ptr<Connection>& connection,
                    const std::string& code, const std::string& text);
    void handle_message(const std::shared_ptr<Connection>& connection,
                        const proto::Message& message);
    void handle_chat(const std::shared_ptr<Connection>& connection,
                     const proto::Message& message);
    void forward_chat(const std::shared_ptr<Connection>& connection,
                      const proto::Message& message, int ttl);
    void send_chat(const std::string& destination, const std::string& text);

    void run_experiment();
    void run_interactive();
    void print_stats();
    void write_stats_file();
    std::string make_message_id();

    std::vector<std::shared_ptr<Connection>> connection_snapshot();
    void add_worker(std::thread worker);
    void shutdown_connections();
    bool stopping() const;

    PeerConfig config_;
    Logger logger_;
    PeerStats stats_;
    std::atomic<bool> stop_{false};

    std::mutex connections_mutex_;
    std::vector<std::shared_ptr<Connection>> connections_;

    std::mutex seen_mutex_;
    std::unordered_set<std::string> seen_messages_;

    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    std::thread listener_thread_;

    int listener_fd_ = -1;
    std::atomic<std::uint64_t> message_counter_{0};
};

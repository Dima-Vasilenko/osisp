#include "common/framing.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>

#include "common/protocol.h"

namespace {

bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

}  // namespace

IoResult recv_exact(int fd, char* buffer, std::size_t length,
                    std::string& error) {
    std::size_t total = 0;
    while (total < length) {
        const ssize_t received =
            ::recv(fd, buffer + total, length - total, 0);
        if (received > 0) {
            total += static_cast<std::size_t>(received);
            continue;
        }
        if (received == 0) {
            error = "connection closed by peer";
            return IoResult::Closed;
        }
        if (errno == EINTR) {
            continue;
        }
        if (would_block() && total == 0) {
            error = "receive timeout";
            return IoResult::Timeout;
        }
        if (would_block()) {
            error = "receive timeout in the middle of a message";
            return IoResult::Error;
        }
        error = std::string("recv: ") + std::strerror(errno);
        return IoResult::Error;
    }
    return IoResult::Ok;
}

IoResult recv_frame(int fd, std::string& payload, std::string& error) {
    std::uint32_t network_length = 0;
    const IoResult header_result =
        recv_exact(fd, reinterpret_cast<char*>(&network_length),
                   sizeof(network_length), error);
    if (header_result != IoResult::Ok) {
        return header_result;
    }

    const std::uint32_t length = ntohl(network_length);
    if (length == 0 || length > proto::kMaxPayload) {
        error = "invalid frame length";
        return IoResult::Error;
    }

    payload.resize(length);
    return recv_exact(fd, payload.data(), payload.size(), error);
}

bool send_all(int fd, const char* data, std::size_t length,
              std::string& error) {
    std::size_t total = 0;
    while (total < length) {
        const ssize_t sent =
            ::send(fd, data + total, length - total, MSG_NOSIGNAL);
        if (sent > 0) {
            total += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0) {
            error = std::string("send: ") + std::strerror(errno);
        } else {
            error = "send returned zero";
        }
        return false;
    }
    return true;
}

bool send_frame(int fd, const std::string& payload, std::string& error) {
    if (payload.empty()) {
        error = "attempt to send an empty frame";
        return false;
    }
    if (payload.size() > proto::kMaxPayload) {
        error = "frame is too large";
        return false;
    }

    const std::uint32_t network_length =
        htonl(static_cast<std::uint32_t>(payload.size()));
    std::string buffer;
    buffer.resize(sizeof(network_length) + payload.size());
    std::memcpy(buffer.data(), &network_length, sizeof(network_length));
    std::memcpy(buffer.data() + sizeof(network_length), payload.data(),
                payload.size());
    return send_all(fd, buffer.data(), buffer.size(), error);
}

#include "common/socket_utils.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace {

std::string gai_error(int code) {
    return std::string("getaddrinfo: ") + gai_strerror(code);
}

addrinfo* resolve(const std::string& host, int port, bool passive,
                  std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (passive) {
        hints.ai_flags = AI_PASSIVE;
    }

    const std::string service = std::to_string(port);
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(),
                                 service.c_str(), &hints, &result);
    if (rc != 0) {
        error = gai_error(rc);
        return nullptr;
    }
    return result;
}

}  // namespace

int create_listener(const std::string& host, int port, std::string& error) {
    addrinfo* addresses = resolve(host, port, true, error);
    if (addresses == nullptr) {
        return -1;
    }

    int listener = -1;
    error.clear();
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        const int fd =
            ::socket(address->ai_family, address->ai_socktype,
                     address->ai_protocol);
        if (fd < 0) {
            error = std::string("socket: ") + std::strerror(errno);
            continue;
        }

        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (::bind(fd, address->ai_addr, address->ai_addrlen) == 0 &&
            ::listen(fd, 16) == 0) {
            listener = fd;
            break;
        }
        error = std::string("bind/listen: ") + std::strerror(errno);
        ::close(fd);
    }
    ::freeaddrinfo(addresses);

    if (listener < 0 && error.empty()) {
        error = "no suitable address for listening";
    }
    return listener;
}

int connect_with_timeout(const std::string& host, int port, int timeout_ms,
                         std::string& error) {
    addrinfo* addresses = resolve(host, port, false, error);
    if (addresses == nullptr) {
        return -1;
    }

    int connected_fd = -1;
    error.clear();
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        const int fd =
            ::socket(address->ai_family, address->ai_socktype,
                     address->ai_protocol);
        if (fd < 0) {
            error = std::string("socket: ") + std::strerror(errno);
            continue;
        }

        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int result = ::connect(fd, address->ai_addr, address->ai_addrlen);
        if (result < 0 && errno != EINPROGRESS) {
            error = std::string("connect: ") + std::strerror(errno);
            ::close(fd);
            continue;
        }

        if (result < 0) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            timeval timeout{};
            timeout.tv_sec = timeout_ms / 1000;
            timeout.tv_usec = (timeout_ms % 1000) * 1000;

            const int selected =
                ::select(fd + 1, nullptr, &writable, nullptr, &timeout);
            if (selected == 0) {
                error = "connect timeout";
                ::close(fd);
                continue;
            }
            if (selected < 0) {
                if (errno == EINTR) {
                    error = "connect interrupted";
                } else {
                    error = std::string("select: ") + std::strerror(errno);
                }
                ::close(fd);
                continue;
            }

            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                             &length) < 0) {
                error = std::string("getsockopt: ") + std::strerror(errno);
                ::close(fd);
                continue;
            }
            if (socket_error != 0) {
                error = std::string("connect: ") +
                        std::strerror(socket_error);
                ::close(fd);
                continue;
            }
        }

        ::fcntl(fd, F_SETFL, flags);
        int no_delay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                     sizeof(no_delay));
        set_socket_timeouts(fd, 500, timeout_ms);
        connected_fd = fd;
        break;
    }
    ::freeaddrinfo(addresses);

    if (connected_fd < 0 && error.empty()) {
        error = "no suitable address for connection";
    }
    return connected_fd;
}

void set_socket_timeouts(int fd, int receive_ms, int send_ms) {
    timeval receive_timeout{};
    receive_timeout.tv_sec = receive_ms / 1000;
    receive_timeout.tv_usec = (receive_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                 sizeof(receive_timeout));

    timeval send_timeout{};
    send_timeout.tv_sec = send_ms / 1000;
    send_timeout.tv_usec = (send_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                 sizeof(send_timeout));
}

std::string endpoint_to_string(const sockaddr_storage& address,
                               socklen_t address_length) {
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    const int rc = ::getnameinfo(
        reinterpret_cast<const sockaddr*>(&address), address_length, host,
        sizeof(host), service, sizeof(service),
        NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        return "unknown";
    }
    return std::string(host) + ":" + service;
}

bool parse_host_port(const std::string& text, std::pair<std::string, int>& out,
                     std::string& error) {
    const std::size_t separator = text.rfind(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= text.size()) {
        error = "peer must have the form host:port";
        return false;
    }

    const std::string host = text.substr(0, separator);
    int port = 0;
    try {
        std::size_t used = 0;
        port = std::stoi(text.substr(separator + 1), &used);
        if (used != text.size() - separator - 1) {
            error = "invalid peer port";
            return false;
        }
    } catch (...) {
        error = "invalid peer port";
        return false;
    }
    if (port < 1 || port > 65535) {
        error = "peer port is out of range";
        return false;
    }
    out = {host, port};
    return true;
}

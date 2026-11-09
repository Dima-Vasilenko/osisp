#pragma once

#include <netdb.h>
#include <sys/socket.h>

#include <string>
#include <utility>

int create_listener(const std::string& host, int port, std::string& error);
int connect_with_timeout(const std::string& host, int port, int timeout_ms,
                         std::string& error);
void set_socket_timeouts(int fd, int receive_ms, int send_ms);
std::string endpoint_to_string(const sockaddr_storage& address,
                               socklen_t address_length);
bool parse_host_port(const std::string& text, std::pair<std::string, int>& out,
                     std::string& error);

#pragma once

#include <cstddef>
#include <string>

enum class IoResult { Ok, Timeout, Closed, Error };

IoResult recv_exact(int fd, char* buffer, std::size_t length, std::string& error);
IoResult recv_frame(int fd, std::string& payload, std::string& error);
bool send_all(int fd, const char* data, std::size_t length, std::string& error);
bool send_frame(int fd, const std::string& payload, std::string& error);

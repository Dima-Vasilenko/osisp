#pragma once

#include <cstdint>
#include <string>

namespace proto {

constexpr std::uint32_t kMaxPayload = 65536;
constexpr int kMaxTextBytes = 4000;
constexpr int kMaxTtl = 64;
constexpr int kVersion = 1;

struct Message {
    std::string type;
    int version = kVersion;
    std::string sender;
    std::string msgid;
    std::string dest;
    int ttl = 0;
    std::string text;
    std::string code;
    std::int64_t sent_us = 0;
};

std::string encode(const Message& message);
bool decode(const std::string& payload, Message& message, std::string& error);

}  // namespace proto

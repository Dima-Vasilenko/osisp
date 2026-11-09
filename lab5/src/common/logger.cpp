#include "common/logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger::Logger(std::string role, std::string participant_id, std::string path,
               bool quiet)
    : role_(std::move(role)),
      participant_id_(std::move(participant_id)),
      quiet_(quiet) {
    if (!path.empty()) {
        file_.open(path, std::ios::out | std::ios::app);
        if (!file_) {
            std::cerr << "warning: cannot open log file '" << path << "'\n";
        }
    }
}

Logger::~Logger() = default;

std::string Logger::timestamp() const {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) %
        1000;
    const std::time_t raw_time = std::chrono::system_clock::to_time_t(now);

    std::tm local_time{};
    localtime_r(&raw_time, &local_time);

    std::ostringstream out;
    out << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << milliseconds.count();
    return out.str();
}

void Logger::log(const std::string& event, const std::string& details) {
    std::ostringstream line;
    line << '[' << timestamp() << "] ROLE=" << role_
         << " id=" << participant_id_ << " event=" << event;
    if (!details.empty()) {
        line << ' ' << details;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!quiet_) {
        std::cout << line.str() << '\n';
        std::cout.flush();
    }
    if (file_) {
        file_ << line.str() << '\n';
        file_.flush();
    }
}

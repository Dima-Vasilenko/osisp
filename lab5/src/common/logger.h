#pragma once

#include <fstream>
#include <mutex>
#include <string>

class Logger {
public:
    Logger(std::string role, std::string participant_id, std::string path,
           bool quiet = false);
    ~Logger();

    void log(const std::string& event, const std::string& details = "");

private:
    std::string timestamp() const;

    std::string role_;
    std::string participant_id_;
    bool quiet_ = false;
    std::ofstream file_;
    mutable std::mutex mutex_;
};

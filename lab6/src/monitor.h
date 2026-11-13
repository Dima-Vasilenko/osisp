#pragma once

#include <string>

struct MonitorConfig {
    std::string root;
    std::string student_id;
    std::string key;
    std::string log_file;
    std::string ready_file;
    bool quiet = false;
    int timeout_ms = 5000;
    int max_events = 0;
    int debounce_ms = 60;
};

void request_monitor_stop();
int run_monitor(const MonitorConfig& config);

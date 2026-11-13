#pragma once

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct Value {
    std::string type;
    std::string data;

    bool operator==(const Value& other) const {
        return type == other.type && data == other.data;
    }
    bool operator!=(const Value& other) const { return !(*this == other); }
};

struct Entry {
    std::string key;
    std::string name;
    Value value;
};

struct Snapshot {
    std::map<std::string, Entry> values;
    std::set<std::string> keys;
};

struct Diff {
    std::vector<Entry> added;
    std::vector<Entry> removed;
    std::vector<std::pair<Entry, Entry>> changed;
    std::vector<std::string> keys_added;
    std::vector<std::string> keys_removed;

    int total() const {
        return static_cast<int>(added.size() + removed.size() +
                                changed.size() + keys_added.size() +
                                keys_removed.size());
    }
};

class Logger {
public:
    Logger(std::string role, std::string path, bool quiet)
        : role_(std::move(role)), quiet_(quiet) {
        if (!path.empty()) {
            file_.open(path, std::ios::out | std::ios::app);
            if (!file_) {
                std::cerr << "warning: cannot open log file '" << path << "'\n";
            }
        }
    }

    void log(const std::string& event, const std::string& details = "") {
        std::ostringstream line;
        line << '[' << timestamp() << "] " << role_ << " event=" << event;
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

private:
    std::string timestamp() const {
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

    std::string role_;
    bool quiet_ = false;
    std::ofstream file_;
    std::mutex mutex_;
};

inline std::string system_error(const std::string& function,
                                const std::string& operation) {
    const int code = errno;
    return function + "() failed while " + operation + ": errno=" +
           std::to_string(code) + " (" + std::strerror(code) + ")";
}

inline bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

inline std::string escape_text(const std::string& text) {
    std::string out;
    for (char ch : text) {
        if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

inline std::string unescape_text(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        const char next = text[++i];
        if (next == 'n') {
            out.push_back('\n');
        } else if (next == 'r') {
            out.push_back('\r');
        } else if (next == '\\') {
            out.push_back('\\');
        } else {
            out.push_back('\\');
            out.push_back(next);
        }
    }
    return out;
}

inline bool normalize_type(const std::string& text, std::string& type) {
    std::string upper;
    for (char ch : text) {
        upper.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(ch))));
    }
    if (upper == "REG_SZ" || upper == "REG_EXPAND_SZ" ||
        upper == "REG_DWORD" || upper == "REG_QWORD" ||
        upper == "REG_MULTI_SZ" || upper == "REG_BINARY") {
        type = upper;
        return true;
    }
    return false;
}

inline bool parse_value(const std::string& type, const std::string& data,
                        Value& value, std::string& error) {
    std::string normalized;
    if (!normalize_type(type, normalized)) {
        error = "unknown value type '" + type + "'";
        return false;
    }

    if (normalized == "REG_DWORD" || normalized == "REG_QWORD") {
        if (data.empty()) {
            error = normalized + " expects a decimal number";
            return false;
        }
        for (char ch : data) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                error = normalized + " expects a non-negative decimal number";
                return false;
            }
        }
        errno = 0;
        char* end = nullptr;
        const unsigned long long number = std::strtoull(data.c_str(), &end, 10);
        if (errno != 0 || end == data.c_str() || *end != '\0' ||
            (normalized == "REG_DWORD" && number > 0xffffffffULL)) {
            error = normalized + " value is out of range";
            return false;
        }
    } else if (normalized == "REG_BINARY") {
        for (char ch : data) {
            if (!std::isxdigit(static_cast<unsigned char>(ch))) {
                error = "REG_BINARY expects hexadecimal digits";
                return false;
            }
        }
    }

    value.type = normalized;
    value.data = data;
    return true;
}

inline std::string short_value(const Value& value) {
    std::string text = escape_text(value.data);
    if (text.size() > 60) {
        text = text.substr(0, 57) + "...";
    }
    return text;
}

inline Diff diff_snapshots(const Snapshot& before, const Snapshot& after) {
    Diff diff;
    for (const auto& item : after.values) {
        const auto old = before.values.find(item.first);
        if (old == before.values.end()) {
            diff.added.push_back(item.second);
        } else if (old->second.value != item.second.value) {
            diff.changed.emplace_back(old->second, item.second);
        }
    }
    for (const auto& item : before.values) {
        if (after.values.find(item.first) == after.values.end()) {
            diff.removed.push_back(item.second);
        }
    }
    for (const auto& key : after.keys) {
        if (before.keys.find(key) == before.keys.end()) {
            diff.keys_added.push_back(key);
        }
    }
    for (const auto& key : before.keys) {
        if (after.keys.find(key) == after.keys.end()) {
            diff.keys_removed.push_back(key);
        }
    }
    return diff;
}

inline long long realtime_us() {
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0;
    }
    return static_cast<long long>(now.tv_sec) * 1000000LL + now.tv_nsec / 1000;
}

inline long long monotonic_us() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<long long>(now.tv_sec) * 1000000LL + now.tv_nsec / 1000;
}

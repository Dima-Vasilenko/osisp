#include "monitor.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "common.h"
#include "registry.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

constexpr uint32_t WATCH_MASK =
    IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE |
    IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;

std::string format_mask(uint32_t mask) {
    static const struct {
        uint32_t bit;
        const char* name;
    } items[] = {
        {IN_CREATE, "IN_CREATE"},           {IN_DELETE, "IN_DELETE"},
        {IN_MODIFY, "IN_MODIFY"},           {IN_ATTRIB, "IN_ATTRIB"},
        {IN_CLOSE_WRITE, "IN_CLOSE_WRITE"}, {IN_MOVED_FROM, "IN_MOVED_FROM"},
        {IN_MOVED_TO, "IN_MOVED_TO"},       {IN_DELETE_SELF, "IN_DELETE_SELF"},
        {IN_MOVE_SELF, "IN_MOVE_SELF"},     {IN_IGNORED, "IN_IGNORED"},
        {IN_Q_OVERFLOW, "IN_Q_OVERFLOW"},
    };
    std::string result;
    for (const auto& item : items) {
        if ((mask & item.bit) != 0) {
            result += result.empty() ? "" : "|";
            result += item.name;
        }
    }
    return result.empty() ? "UNKNOWN" : result;
}

bool add_watch(int fd, const Registry& registry, const std::string& key,
               std::string& error) {
    const std::string dir = registry.fs_path(key);
    if (dir.empty()) {
        error = "invalid key path '" + key + "'";
        return false;
    }
    if (inotify_add_watch(fd, dir.c_str(), WATCH_MASK) < 0) {
        error = system_error("inotify_add_watch", "watching key '" + key + "'");
        return false;
    }
    return true;
}

bool read_events(int fd, int& event_count, uint32_t& all_mask, bool& relevant,
                 std::string& error) {
    alignas(struct inotify_event) char buffer[65536];
    const ssize_t length = read(fd, buffer, sizeof(buffer));
    if (length < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return true;
        }
        error = system_error("read", "reading inotify events");
        return false;
    }

    size_t offset = 0;
    while (offset + sizeof(struct inotify_event) <=
           static_cast<size_t>(length)) {
        const auto* event =
            reinterpret_cast<const struct inotify_event*>(buffer + offset);
        offset += sizeof(struct inotify_event) + event->len;
        ++event_count;
        all_mask |= event->mask;

        if ((event->mask & IN_Q_OVERFLOW) != 0) {
            relevant = true;
            continue;
        }
        if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
            relevant = true;
            continue;
        }
        const std::string name = event->len > 0 ? event->name : "";
        if (!name.empty() && name.front() == '.') {
            continue;  // temporary file used by set_value()
        }
        if (ends_with(name, ".val")) {
            relevant = true;
        }
    }
    return true;
}

bool wait_for_events(int fd, int wait_ms, int& event_count,
                     uint32_t& all_mask, bool& relevant, std::string& error) {
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = poll(&descriptor, 1, wait_ms);
    if (ready < 0) {
        if (errno == EINTR) {
            return true;
        }
        error = system_error("poll", "waiting for registry changes");
        return false;
    }
    return ready == 0 ||
           read_events(fd, event_count, all_mask, relevant, error);
}

void log_diff(const Diff& diff, Logger& log) {
    for (const Entry& entry : diff.added) {
        log.log("REG_ADD", "key=" + entry.key + " name=" + entry.name +
                               " type=" + entry.value.type + " value=" +
                               short_value(entry.value));
    }
    for (const Entry& entry : diff.removed) {
        log.log("REG_DEL", "key=" + entry.key + " name=" + entry.name +
                               " type=" + entry.value.type + " old=" +
                               short_value(entry.value));
    }
    for (const auto& change : diff.changed) {
        log.log("REG_MODIFY", "key=" + change.second.key + " name=" +
                                  change.second.name + " type=" +
                                  change.second.value.type + " old=" +
                                  short_value(change.first.value) + " new=" +
                                  short_value(change.second.value));
    }
    for (const std::string& key : diff.keys_added) {
        log.log("REG_KEY_ADD", "key=" + key);
    }
    for (const std::string& key : diff.keys_removed) {
        log.log("REG_KEY_DEL", "key=" + key);
    }
}

}  // namespace

void request_monitor_stop() {
    g_stop = 1;
}

int run_monitor(const MonitorConfig& config) {
    g_stop = 0;
    Logger log("MONITOR", config.log_file, config.quiet);
    Registry registry(config.root, config.student_id);

    std::string error;
    if (!registry.key_exists(config.key, error)) {
        std::cerr << "monitor: " << error << '\n';
        log.log("ERROR", "operation=open_key key=" + config.key + " " + error);
        return 1;
    }

    const int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) {
        error = system_error("inotify_init1", "creating inotify instance");
        std::cerr << "monitor: " << error << '\n';
        log.log("ERROR", "operation=inotify_init " + error);
        return 1;
    }
    if (!add_watch(fd, registry, config.key, error)) {
        std::cerr << "monitor: " << error << '\n';
        log.log("ERROR", "operation=add_watch key=" + config.key + " " + error);
        close(fd);
        return 1;
    }

    Snapshot previous;
    if (!registry.read_snapshot(config.key, previous, error)) {
        std::cerr << "monitor: " << error << '\n';
        log.log("ERROR", "operation=read_snapshot key=" + config.key + " " +
                             error);
        close(fd);
        return 1;
    }

    if (!config.ready_file.empty()) {
        std::ofstream ready(config.ready_file, std::ios::out | std::ios::trunc);
        if (ready) {
            ready << "ready\n";
        } else {
            log.log("WARN",
                    "cannot create ready file '" + config.ready_file + "'");
        }
    }

    log.log("MONITOR_START", "key=" + config.key + " timeout_ms=" +
                                 std::to_string(config.timeout_ms) +
                                 " max_events=" +
                                 std::to_string(config.max_events));

    const bool has_deadline = config.timeout_ms > 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(config.timeout_ms);
    int notifications = 0;
    std::string stop_status = "OK";

    while (config.max_events <= 0 || notifications < config.max_events) {
        if (g_stop != 0) {
            stop_status = "INTERRUPTED";
            break;
        }

        int wait_ms = -1;
        if (has_deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                log.log("MONITOR_TIMEOUT",
                        "notifications=" + std::to_string(notifications));
                stop_status = "TIMEOUT";
                break;
            }
            wait_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now)
                    .count());
            if (wait_ms < 1) {
                wait_ms = 1;
            }
        }

        int event_count = 0;
        uint32_t all_mask = 0;
        bool relevant = false;
        if (!wait_for_events(fd, wait_ms, event_count, all_mask, relevant,
                             error)) {
            std::cerr << "monitor: " << error << '\n';
            log.log("ERROR", "operation=wait_events " + error);
            stop_status = "ERROR";
            break;
        }
        if (g_stop != 0) {
            stop_status = "INTERRUPTED";
            break;
        }
        if (event_count == 0) {
            continue;
        }

        const long long detected_us = realtime_us();
        if (config.debounce_ms > 0) {
            const auto quiet_deadline =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(config.debounce_ms);
            while (std::chrono::steady_clock::now() < quiet_deadline) {
                const auto now = std::chrono::steady_clock::now();
                const int rest = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        quiet_deadline - now)
                        .count());
                if (!wait_for_events(fd, rest < 1 ? 1 : rest, event_count,
                                     all_mask, relevant, error)) {
                    std::cerr << "monitor: " << error << '\n';
                    log.log("ERROR", "operation=wait_events " + error);
                    stop_status = "ERROR";
                    break;
                }
            }
            if (stop_status == "ERROR") {
                break;
            }
        }
        if (!relevant) {
            continue;
        }

        const long long read_start_us = monotonic_us();
        Snapshot current;
        if (!registry.read_snapshot(config.key, current, error)) {
            std::cerr << "monitor: " << error << '\n';
            log.log("REG_READ_ERROR",
                    "key=" + config.key + " events=" + format_mask(all_mask) +
                        " " + error);
            stop_status = error.find("does not exist") != std::string::npos
                              ? "TARGET_REMOVED"
                              : "ERROR";
            if (stop_status == "TARGET_REMOVED") {
                log.log("MONITOR_TARGET_REMOVED", "key=" + config.key);
            }
            break;
        }

        const Diff diff = diff_snapshots(previous, current);
        const long long read_us = monotonic_us() - read_start_us;
        log.log("REG_NOTIFY", "key=" + config.key +
                                  " events=" + format_mask(all_mask) +
                                  " ts_us=" + std::to_string(detected_us));
        if (diff.total() > 0) {
            ++notifications;
            log.log("REG_DIFF", "added=" + std::to_string(diff.added.size()) +
                                    " removed=" +
                                    std::to_string(diff.removed.size()) +
                                    " changed=" +
                                    std::to_string(diff.changed.size()) +
                                    " keys_added=" +
                                    std::to_string(diff.keys_added.size()) +
                                    " keys_removed=" +
                                    std::to_string(diff.keys_removed.size()) +
                                    " read_us=" + std::to_string(read_us));
            log_diff(diff, log);
        } else {
            log.log("REG_DIFF", "added=0 removed=0 changed=0 keys_added=0 "
                                "keys_removed=0 read_us=" +
                                    std::to_string(read_us));
        }
        previous = current;
    }

    if (close(fd) != 0) {
        const std::string close_error =
            system_error("close", "closing inotify descriptor");
        std::cerr << "monitor: " << close_error << '\n';
        log.log("ERROR", "operation=close " + close_error);
    }
    log.log("MONITOR_STOP", "notifications=" + std::to_string(notifications) +
                                " status=" + stop_status);
    return stop_status == "ERROR" ? 1 : 0;
}

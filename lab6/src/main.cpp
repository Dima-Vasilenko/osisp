#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <iostream>

#include "common.h"
#include "monitor.h"
#include "registry.h"

namespace {

struct CommandLine {
    std::string command;
    std::map<std::string, std::string> values;
    std::set<std::string> flags;

    bool flag(const std::string& name) const {
        return flags.find(name) != flags.end();
    }
    std::string get(const std::string& name,
                    const std::string& fallback = "") const {
        const auto it = values.find(name);
        return it == values.end() ? fallback : it->second;
    }
    int get_int(const std::string& name, int fallback,
                std::string& error) const {
        const auto it = values.find(name);
        if (it == values.end()) {
            return fallback;
        }
        errno = 0;
        char* end = nullptr;
        const long value = std::strtol(it->second.c_str(), &end, 10);
        if (errno != 0 || end == it->second.c_str() || *end != '\0') {
            error = "option --" + name + " expects an integer";
            return fallback;
        }
        return static_cast<int>(value);
    }
};

bool parse_command_line(int argc, char** argv, CommandLine& out,
                        std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--", 0) != 0) {
            if (!out.command.empty()) {
                error = "unexpected argument '" + argument + "'";
                return false;
            }
            out.command = argument;
            continue;
        }

        std::string name = argument.substr(2);
        const size_t equals = name.find('=');
        if (equals != std::string::npos) {
            out.values[name.substr(0, equals)] = name.substr(equals + 1);
        } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
            out.values[name] = argv[++i];
        } else {
            out.flags.insert(name);
        }
    }
    return true;
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " <command> [options]\n\n"
        << "Commands: init, set, delete, list, monitor\n\n"
        << "Common: --root DIR --student-id ID --run-id ID --key KEY\n"
        << "        --log-file FILE --quiet\n"
        << "init:   --count N --seed N\n"
        << "set:    --name NAME --type TYPE --value TEXT\n"
        << "delete: --name NAME | --tree\n"
        << "monitor: --timeout-ms N --max-events N --debounce-ms N\n"
        << "         --ready-file FILE\n";
}

void handle_stop_signal(int) {
    request_monitor_stop();
}

std::string default_root() {
    const char* home = std::getenv("HOME");
    return home == nullptr || *home == '\0'
               ? ".osip_lab06_registry"
               : std::string(home) + "/.osip_lab06_registry";
}

bool numeric_id(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(),
                                        [](char ch) { return std::isdigit(ch); });
}

void fail(Logger& log, const std::string& operation,
          const std::string& error) {
    std::cerr << "error: " << operation << ": " << error << '\n';
    log.log("ERROR", "operation=" + operation + " message=" + error);
}

}  // namespace

int main(int argc, char** argv) {
    CommandLine cmd;
    std::string error;
    if (!parse_command_line(argc, argv, cmd, error)) {
        std::cerr << "error: " << error << "\n\n";
        print_usage(argv[0]);
        return 1;
    }
    if (cmd.command.empty() || cmd.flag("help")) {
        print_usage(argv[0]);
        return cmd.command.empty() ? 1 : 0;
    }

    const std::string root = cmd.get("root", default_root());
    const std::string student_id = cmd.get("student-id", "028");
    if (!numeric_id(student_id)) {
        std::cerr << "error: --student-id must contain digits only\n";
        return 1;
    }
    const std::string run_id = cmd.get("run-id", "1");
    const std::string log_file = cmd.get("log-file");
    const bool quiet = cmd.flag("quiet");

    Registry registry(root, student_id);
    const std::string default_key = registry.learning_key(run_id);
    const std::string key = cmd.get("key", default_key);

    if (cmd.command == "monitor") {
        MonitorConfig config;
        config.root = root;
        config.student_id = student_id;
        config.key = key;
        config.log_file = log_file;
        config.ready_file = cmd.get("ready-file");
        config.quiet = quiet;
        config.timeout_ms = cmd.get_int("timeout-ms", 5000, error);
        config.max_events = cmd.get_int("max-events", 0, error);
        config.debounce_ms = cmd.get_int("debounce-ms", 60, error);
        if (!error.empty()) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        if (config.timeout_ms < 0 || config.max_events < 0 ||
            config.debounce_ms < 0) {
            std::cerr << "error: monitor limits must be >= 0\n";
            return 1;
        }
        std::signal(SIGINT, handle_stop_signal);
        std::signal(SIGTERM, handle_stop_signal);
        return run_monitor(config);
    }

    Logger log("REGISTRY", log_file, quiet);

    if (cmd.command == "init") {
        const int count = cmd.get_int("count", 0, error);
        const int seed = cmd.get_int("seed", 3716, error);
        if (!error.empty() || count < 0 || seed < 0) {
            fail(log, "init", error.empty() ? "count and seed must be >= 0"
                                            : error);
            return 1;
        }
        if (!registry.init_run(run_id, count, static_cast<unsigned>(seed),
                               error)) {
            fail(log, "init", error);
            return 1;
        }
        log.log("REG_CREATE_KEY", "key=" + default_key + " status=OK");
        if (count > 0) {
            log.log("REG_SEED", "key=" + default_key + " count=" +
                                    std::to_string(count) + " seed=" +
                                    std::to_string(seed));
        }
        log.log("REG_CLOSE", "key=" + default_key + " status=OK");
        return 0;
    }

    if (cmd.command == "set") {
        if (!cmd.values.count("name") != 0) {
            fail(log, "set", "missing required option --name");
            return 1;
        }
        Value value;
        if (!parse_value(cmd.get("type", "REG_SZ"), cmd.get("value"), value,
                         error)) {
            fail(log, "set", error);
            return 1;
        }
        const std::string name = cmd.get("name");
        if (!registry.set_value(key, name, value, error)) {
            fail(log, "set", error);
            return 1;
        }
        log.log("REG_SET", "key=" + key + " name=" + name + " type=" +
                               value.type + " status=OK");
        log.log("REG_CLOSE", "key=" + key + " status=OK");
        return 0;
    }

    if (cmd.command == "delete") {
        if (cmd.values.count("name") != 0) {
            const std::string name = cmd.get("name");
            if (!registry.delete_value(key, name, error)) {
                fail(log, "delete", error);
                return 1;
            }
            log.log("REG_DELETE_VALUE",
                    "key=" + key + " name=" + name + " status=OK");
        } else {
            const bool tree = cmd.flag("tree");
            if (!registry.delete_key(key, tree, error)) {
                fail(log, "delete", error);
                return 1;
            }
            log.log(tree ? "REG_DELETE_TREE" : "REG_DELETE_KEY",
                    "key=" + key + " status=OK");
        }
        log.log("REG_CLOSE", "key=" + key + " status=OK");
        return 0;
    }

    if (cmd.command == "list") {
        Snapshot snapshot;
        if (!registry.read_snapshot(key, snapshot, error)) {
            fail(log, "list", error);
            return 1;
        }
        for (const std::string& item : snapshot.keys) {
            std::cout << "KEY\t" << item << '\n';
        }
        for (const auto& item : snapshot.values) {
            const Entry& entry = item.second;
            std::cout << "VALUE\t" << entry.key << '\t' << entry.name << '\t'
                      << entry.value.type << '\t' << entry.value.data << '\n';
        }
        log.log("REG_ENUM", "key=" + key + " values=" +
                                std::to_string(snapshot.values.size()) +
                                " subkeys=" + std::to_string(snapshot.keys.size()) +
                                " status=OK");
        log.log("REG_CLOSE", "key=" + key + " status=OK");
        return 0;
    }

    std::cerr << "error: unknown command '" << cmd.command << "'\n\n";
    print_usage(argv[0]);
    return 1;
}

#include "peer/options.h"

#include <iostream>

#include "common/socket_utils.h"

namespace {

bool parse_number(const std::string& text, int& value) {
    try {
        std::size_t used = 0;
        const int parsed = std::stoi(text, &used);
        if (used != text.size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

void print_usage(const char* program) {
    std::cout
        << "Decentralized TCP chat peer (lab 05, variant 6)\n\n"
        << "Usage: " << program << " --id ID --port PORT [options]\n\n"
        << "Network options:\n"
        << "  --id ID                 peer identifier\n"
        << "  --host HOST             listening address (default 127.0.0.1)\n"
        << "  --port PORT             listening port, 20000..29999\n"
        << "  --peer HOST:PORT        peer to connect to (repeatable)\n"
        << "  --ttl N                 initial TTL for outgoing messages\n"
        << "  --timeout-ms N          connect/send timeout in milliseconds\n"
        << "  --connect-wait-ms N     delay before automatic sending\n\n"
        << "Automatic experiment:\n"
        << "  --messages N            send N messages and keep running\n"
        << "  --dest ID               destination peer identifier\n"
        << "  --text TEXT             message text (default: hello)\n"
        << "  --send-interval-ms N    interval between messages\n"
        << "  --run-ms N              additional waiting time after sending\n"
        << "  --expect-messages N     stop waiting after N delivered messages\n\n"
        << "Logging:\n"
        << "  --log-file PATH         append event log to file\n"
        << "  --stats-file PATH       write final counters to file\n"
        << "  --quiet                 do not duplicate the log to stdout\n\n"
        << "Interactive commands: msg <dest> <text>, stats, help, quit\n";
}

bool parse_options(int argc, char** argv, PeerConfig& config,
                   std::string& error, bool& show_help) {
    show_help = false;
    config = PeerConfig{};

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto next_value = [&](std::string& value) {
            if (index + 1 >= argc) {
                error = "missing value for " + argument;
                return false;
            }
            value = argv[++index];
            return true;
        };
        auto next_number = [&](int& value) {
            std::string text;
            if (!next_value(text)) {
                return false;
            }
            if (!parse_number(text, value)) {
                error = "invalid number for " + argument;
                return false;
            }
            return true;
        };

        if (argument == "--help" || argument == "-h") {
            show_help = true;
            return true;
        }
        if (argument == "--id") {
            if (!next_value(config.id)) {
                return false;
            }
        } else if (argument == "--host") {
            if (!next_value(config.host)) {
                return false;
            }
        } else if (argument == "--port") {
            if (!next_number(config.port)) {
                return false;
            }
        } else if (argument == "--peer") {
            std::string value;
            if (!next_value(value)) {
                return false;
            }
            std::pair<std::string, int> peer;
            if (!parse_host_port(value, peer, error)) {
                return false;
            }
            config.peers.push_back(peer);
        } else if (argument == "--ttl") {
            if (!next_number(config.ttl)) {
                return false;
            }
        } else if (argument == "--timeout-ms") {
            if (!next_number(config.timeout_ms)) {
                return false;
            }
        } else if (argument == "--messages") {
            if (!next_number(config.messages)) {
                return false;
            }
        } else if (argument == "--dest") {
            if (!next_value(config.dest)) {
                return false;
            }
        } else if (argument == "--text") {
            if (!next_value(config.text)) {
                return false;
            }
        } else if (argument == "--send-interval-ms") {
            if (!next_number(config.send_interval_ms)) {
                return false;
            }
        } else if (argument == "--run-ms") {
            if (!next_number(config.run_ms)) {
                return false;
            }
        } else if (argument == "--expect-messages") {
            if (!next_number(config.expect_messages)) {
                return false;
            }
        } else if (argument == "--connect-wait-ms") {
            if (!next_number(config.connect_wait_ms)) {
                return false;
            }
        } else if (argument == "--log-file") {
            if (!next_value(config.log_file)) {
                return false;
            }
        } else if (argument == "--stats-file") {
            if (!next_value(config.stats_file)) {
                return false;
            }
        } else if (argument == "--quiet") {
            config.quiet = true;
        } else {
            error = "unknown option " + argument;
            return false;
        }
    }

    if (config.id.empty()) {
        error = "--id is required";
        return false;
    }
    if (config.port < 20000 || config.port > 29999) {
        error = "--port must be in the range 20000..29999";
        return false;
    }
    if (config.ttl < 0 || config.ttl > proto::kMaxTtl) {
        error = "--ttl must be in the range 0..64";
        return false;
    }
    if (config.timeout_ms <= 0) {
        error = "--timeout-ms must be positive";
        return false;
    }
    if (config.messages < 0 || config.send_interval_ms < 0 ||
        config.run_ms < 0 || config.expect_messages < 0 ||
        config.connect_wait_ms < 0) {
        error = "message and timing parameters must not be negative";
        return false;
    }
    if (config.messages > 0 && config.dest.empty()) {
        error = "--dest is required when --messages is greater than zero";
        return false;
    }
    return true;
}

#include <csignal>
#include <iostream>

#include "peer/options.h"
#include "peer/peer_node.h"

namespace {

void handle_signal(int) {
    request_stop_signal();
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    PeerConfig config;
    std::string error;
    bool show_help = false;
    if (!parse_options(argc, argv, config, error, show_help)) {
        std::cerr << "error: " << error << "\n\n";
        print_usage(argv[0]);
        return 1;
    }
    if (show_help) {
        print_usage(argv[0]);
        return 0;
    }

    PeerNode node(config, config.log_file);
    return node.run();
}

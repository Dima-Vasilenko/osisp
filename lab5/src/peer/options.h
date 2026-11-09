#pragma once

#include <string>

#include "peer/peer_node.h"

bool parse_options(int argc, char** argv, PeerConfig& config,
                   std::string& error, bool& show_help);
void print_usage(const char* program);

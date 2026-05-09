#pragma once

// cli/agent_history.h
//
// `quorum agent history --name <id>` — print the .history audit trail
// for an agent's CONTEXT.md to stdout. The audit trail is written by
// vault/context_history.h every time CONTEXT.md is overwritten (CLI
// regenerate, web PUT). Capped at the most recent 20 entries.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "utils/discover.h"

namespace fs = std::filesystem;
namespace sui::quorum::cli {

inline int show_history(const std::string& name) {
    if (name.empty()) {
        std::cerr << "ERROR: agent history requires --name <agent_id>\n";
        return 1;
    }

    auto project_root = sui::quorum::discover_project_root();
    if (!project_root) {
        std::cerr << "ERROR: no .quorum/ found. Run 'quorum init' first.\n";
        return 1;
    }

    auto history_path = *project_root + "/.quorum/vaults/" + name + "/CONTEXT.md.history";
    if (!fs::exists(history_path)) {
        std::cout << "No history yet for agent '" << name << "'.\n";
        return 0;
    }

    std::ifstream in(history_path, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "ERROR: cannot read " << history_path << "\n";
        return 1;
    }
    std::cout << in.rdbuf();
    return 0;
}

} // namespace sui::quorum::cli

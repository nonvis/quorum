#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <iostream>

namespace sui::quorum {

struct DaemonConfig {
    std::string pid_file = "/tmp/quorum.pid";
    std::string data_dir = "./data";
    std::string log_level = "info";
};

struct ChainConfig {
    std::string network = "testnet";
    std::string rpc_url = "https://fullnode.testnet.sui.io:443";
    std::string package_id = "0x0";
    uint64_t gas_budget = 100000000;
};

struct WalrusConfig {
    std::string aggregator_url;
    std::string publisher_url;
    bool enabled = false;
};

struct SealConfig {
    bool enabled = false;
};

struct LocalInferenceConfig {
    bool enabled = false;
    std::string url = "http://localhost:11434";
    std::string model = "llama3.1:8b";
    uint64_t timeout_seconds = 30;
};

struct FrontierInferenceConfig {
    std::string provider = "anthropic";
    std::string model = "claude-sonnet-4-5-20250929";
    std::string api_key_env = "ANTHROPIC_API_KEY";
    uint64_t timeout_seconds = 60;
    uint64_t max_tokens = 4096;
};

struct InferenceConfig {
    LocalInferenceConfig local;
    FrontierInferenceConfig frontier;
};

struct ConsensusConfig {
    uint64_t max_rounds = 3;
    uint64_t round_timeout_seconds = 7200;
    bool human_escalation = true;
};

struct AgentRef {
    std::string config_path;
};

struct QuorumConfig {
    DaemonConfig daemon;
    ChainConfig chain;
    WalrusConfig walrus;
    SealConfig seal;
    InferenceConfig inference;
    ConsensusConfig consensus;
    std::vector<AgentRef> agents;
};

namespace detail {

inline std::string trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
    return std::string(sv);
}

inline std::string unquote(std::string_view sv) {
    auto s = trim(sv);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

inline std::string strip_comment(const std::string& line) {
    bool in_quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') in_quote = !in_quote;
        if (line[i] == '#' && !in_quote) return line.substr(0, i);
    }
    return line;
}

} // namespace detail

inline std::optional<QuorumConfig> load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "ERROR: Cannot open config: " << path << "\n";
        return std::nullopt;
    }

    QuorumConfig cfg;
    std::string line;
    std::string section;
    std::string subsection;

    while (std::getline(file, line)) {
        line = detail::strip_comment(line);
        if (line.empty()) continue;

        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;

        auto trimmed = detail::trim(line);
        if (trimmed.empty()) continue;

        // List item
        if (trimmed.starts_with("- ")) {
            auto rest = detail::trim(trimmed.substr(2));
            if (rest.starts_with("config:")) {
                auto val = detail::unquote(rest.substr(7));
                cfg.agents.push_back({val});
            }
            continue;
        }

        auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        auto key = detail::trim(trimmed.substr(0, colon));
        auto val = detail::unquote(trimmed.substr(colon + 1));

        // Section header
        if (val.empty() && indent == 0) {
            section = key;
            subsection.clear();
            continue;
        }

        // Subsection header
        if (val.empty() && indent >= 2 && indent <= 4) {
            subsection = key;
            continue;
        }

        // Value assignment
        if (section == "daemon") {
            if (key == "pid_file") cfg.daemon.pid_file = val;
            else if (key == "data_dir") cfg.daemon.data_dir = val;
            else if (key == "log_level") cfg.daemon.log_level = val;
        } else if (section == "chain") {
            if (key == "network") cfg.chain.network = val;
            else if (key == "rpc_url") cfg.chain.rpc_url = val;
            else if (key == "package_id") cfg.chain.package_id = val;
            else if (key == "gas_budget") {
                try { cfg.chain.gas_budget = std::stoull(val); } catch (...) {}
            }
        } else if (section == "walrus") {
            if (key == "aggregator_url") cfg.walrus.aggregator_url = val;
            else if (key == "publisher_url") cfg.walrus.publisher_url = val;
            else if (key == "enabled") cfg.walrus.enabled = (val == "true");
        } else if (section == "seal") {
            if (key == "enabled") cfg.seal.enabled = (val == "true");
        } else if (section == "inference") {
            if (subsection == "local") {
                if (key == "enabled") cfg.inference.local.enabled = (val == "true");
                else if (key == "url") cfg.inference.local.url = val;
                else if (key == "model") cfg.inference.local.model = val;
                else if (key == "timeout_seconds") {
                    try { cfg.inference.local.timeout_seconds = std::stoull(val); } catch (...) {}
                }
            } else if (subsection == "frontier") {
                if (key == "provider") cfg.inference.frontier.provider = val;
                else if (key == "model") cfg.inference.frontier.model = val;
                else if (key == "api_key_env") cfg.inference.frontier.api_key_env = val;
                else if (key == "timeout_seconds") {
                    try { cfg.inference.frontier.timeout_seconds = std::stoull(val); } catch (...) {}
                }
                else if (key == "max_tokens") {
                    try { cfg.inference.frontier.max_tokens = std::stoull(val); } catch (...) {}
                }
            }
        } else if (section == "consensus") {
            if (key == "max_rounds") {
                try { cfg.consensus.max_rounds = std::stoull(val); } catch (...) {}
            } else if (key == "round_timeout_seconds") {
                try { cfg.consensus.round_timeout_seconds = std::stoull(val); } catch (...) {}
            } else if (key == "human_escalation") {
                cfg.consensus.human_escalation = (val == "true");
            }
        }
    }

    return cfg;
}

} // namespace sui::quorum

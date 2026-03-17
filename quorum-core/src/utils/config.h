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
    std::string knowledge_dir = "./data/knowledge";
    std::string target_dir;                // project-level working directory for agents
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

struct BudgetConfig {
    double daily_limit_usd = 10.0;     // pause dispatch when exceeded
    double hourly_limit_usd = 3.0;     // pause dispatch when exceeded
    uint64_t task_timeout_seconds = 300; // kill claude -p after this
};

struct ConversationConfig {
    bool enabled = true;
    int default_max_rounds = 20;       // max turns per conversation
    double default_budget_usd = 5.0;
    std::string leader;                // leader agent id (required for team mode)
    std::vector<std::string> default_path;  // optional default routing sequence
};

struct AgentMetadata {
    std::string id;
    std::string name;
    std::string agent_class = "analyst";  // "analyst" or "executor"
    std::string config_path;
    std::string vault_path;
    std::string context_file;
    std::string target_dir;               // working directory for claude -p (inherits from daemon.target_dir)
};

struct QuorumConfig {
    DaemonConfig daemon;
    ChainConfig chain;
    WalrusConfig walrus;
    SealConfig seal;
    InferenceConfig inference;
    ConsensusConfig consensus;
    BudgetConfig budget;
    ConversationConfig conversations;
    std::vector<AgentMetadata> agents;
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

inline std::optional<AgentMetadata> load_agent_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "ERROR: Cannot open agent config: " << path << "\n";
        return std::nullopt;
    }

    AgentMetadata agent;
    agent.config_path = path;
    std::string line;
    std::string section;

    while (std::getline(file, line)) {
        line = detail::strip_comment(line);
        if (line.empty()) continue;

        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;

        auto trimmed = detail::trim(line);
        if (trimmed.empty()) continue;
        if (trimmed.starts_with("- ")) continue;  // skip list items

        auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        auto key = detail::trim(trimmed.substr(0, colon));
        auto val = detail::unquote(trimmed.substr(colon + 1));

        // Section header (no value after colon)
        if (val.empty()) {
            section = key;
            continue;
        }

        // Top-level fields (indent < 2)
        if (indent < 2) {
            if (key == "id") agent.id = val;
            else if (key == "agent_class") agent.agent_class = val;
            else if (key == "name") agent.name = val;
            else if (key == "vault_path") agent.vault_path = val;
            else if (key == "context_file") agent.context_file = val;
        }

        // Fields inside "executor" section
        if (section == "executor") {
            if (key == "target_dir") agent.target_dir = val;
        }
    }

    if (agent.id.empty()) {
        std::cerr << "ERROR: Agent config missing 'id' field: " << path << "\n";
        return std::nullopt;
    }

    return agent;
}

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
                auto agent = load_agent_config(val);
                if (agent) {
                    cfg.agents.push_back(*agent);
                } else {
                    std::cerr << "WARNING: Failed to load agent config: " << val << "\n";
                }
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
            else if (key == "knowledge_dir") cfg.daemon.knowledge_dir = val;
            else if (key == "target_dir") cfg.daemon.target_dir = val;
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
        } else if (section == "budget") {
            if (key == "daily_limit_usd") {
                try { cfg.budget.daily_limit_usd = std::stod(val); } catch (...) {}
            } else if (key == "hourly_limit_usd") {
                try { cfg.budget.hourly_limit_usd = std::stod(val); } catch (...) {}
            } else if (key == "task_timeout_seconds") {
                try { cfg.budget.task_timeout_seconds = std::stoull(val); } catch (...) {}
            }
        } else if (section == "conversations") {
            if (key == "enabled") cfg.conversations.enabled = (val == "true");
            else if (key == "default_max_rounds") {
                try { cfg.conversations.default_max_rounds = std::stoi(val); } catch (...) {}
            }
            else if (key == "default_budget_usd") {
                try { cfg.conversations.default_budget_usd = std::stod(val); } catch (...) {}
            }
            else if (key == "leader") cfg.conversations.leader = val;
            else if (key == "default_path") {
                auto stripped = val;
                if (!stripped.empty() && stripped.front() == '[') stripped.erase(stripped.begin());
                if (!stripped.empty() && stripped.back() == ']') stripped.pop_back();
                std::string item;
                for (char c : stripped) {
                    if (c == ',') {
                        auto trimmed = detail::trim(item);
                        if (!trimmed.empty()) cfg.conversations.default_path.push_back(trimmed);
                        item.clear();
                    } else {
                        item += c;
                    }
                }
                auto trimmed = detail::trim(item);
                if (!trimmed.empty()) cfg.conversations.default_path.push_back(trimmed);
            }
        }
    }

    // Apply project-level target_dir as default for agents that don't override
    if (!cfg.daemon.target_dir.empty()) {
        for (auto& agent : cfg.agents) {
            if (agent.target_dir.empty()) {
                agent.target_dir = cfg.daemon.target_dir;
            }
        }
    }

    return cfg;
}

} // namespace sui::quorum

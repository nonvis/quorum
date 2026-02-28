#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace sui::quorum {

// Static rule-based router: maps task types to agent names
class Router {
public:
    void add_route(const std::string& task_type, const std::string& agent_name) {
        routes_[task_type].push_back(agent_name);
    }

    [[nodiscard]] std::vector<std::string> resolve(const std::string& task_type) const {
        auto it = routes_.find(task_type);
        if (it != routes_.end()) return it->second;
        return {};
    }

private:
    std::unordered_map<std::string, std::vector<std::string>> routes_;
};

} // namespace sui::quorum

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sui::quorum {

using EventHandler = std::function<void(const std::string& payload)>;

class EventDispatcher {
public:
    void on(const std::string& event, EventHandler handler) {
        handlers_[event].push_back(std::move(handler));
    }

    void emit(const std::string& event, const std::string& payload = {}) {
        auto it = handlers_.find(event);
        if (it != handlers_.end()) {
            for (auto& handler : it->second) {
                handler(payload);
            }
        }
    }

private:
    std::unordered_map<std::string, std::vector<EventHandler>> handlers_;
};

} // namespace sui::quorum

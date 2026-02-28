#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace sui::quorum {

struct Message {
    std::string topic;
    std::string sender;
    std::string payload;
    uint64_t timestamp{0};
};

using MessageHandler = std::function<void(const Message&)>;

class MessageBus {
public:
    void subscribe(const std::string& topic, MessageHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_[topic].push_back(std::move(handler));
    }

    void publish(Message msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(msg));
    }

    void drain() {
        std::queue<Message> local;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(local, queue_);
        }
        while (!local.empty()) {
            auto& msg = local.front();
            auto it = subscribers_.find(msg.topic);
            if (it != subscribers_.end()) {
                for (auto& handler : it->second) {
                    handler(msg);
                }
            }
            local.pop();
        }
    }

    [[nodiscard]] size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::queue<Message> queue_;
    std::unordered_map<std::string, std::vector<MessageHandler>> subscribers_;
};

} // namespace sui::quorum

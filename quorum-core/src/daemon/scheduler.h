#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sui::quorum {

struct ScheduledTask {
    std::string name;
    uint64_t interval_seconds;
    uint64_t last_run{0};
    std::function<void()> action;
};

class Scheduler {
public:
    void add(std::string name, uint64_t interval_seconds, std::function<void()> action) {
        tasks_.push_back({
            .name = std::move(name),
            .interval_seconds = interval_seconds,
            .last_run = 0,
            .action = std::move(action),
        });
    }

    void tick(uint64_t now) {
        for (auto& task : tasks_) {
            if (now - task.last_run >= task.interval_seconds) {
                task.action();
                task.last_run = now;
            }
        }
    }

    [[nodiscard]] size_t task_count() const { return tasks_.size(); }

private:
    std::vector<ScheduledTask> tasks_;
};

} // namespace sui::quorum

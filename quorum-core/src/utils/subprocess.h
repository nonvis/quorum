#pragma once

#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <sys/wait.h>

namespace sui::quorum {

struct CommandResult {
    std::string output;
    int exit_code{-1};
};

// Run a shell command and capture stdout + exit code
[[nodiscard]] inline std::optional<CommandResult> run_command(const std::string& cmd) {
    std::array<char, 4096> buffer;
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }
    int status = pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return CommandResult{.output = std::move(output), .exit_code = exit_code};
}

} // namespace sui::quorum

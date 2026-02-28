#pragma once

#include <optional>
#include <string>

namespace sui::quorum {

struct ParsedOutput {
    std::string summary;
    std::string action;
    std::string raw;
};

// Parses structured output from LLM responses
class OutputParser {
public:
    [[nodiscard]] ParsedOutput parse(const std::string& raw_output) const {
        return {
            .summary = raw_output.substr(0, raw_output.find('\n')),
            .action = {},
            .raw = raw_output,
        };
    }
};

} // namespace sui::quorum

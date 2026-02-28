#pragma once

#include <string>
#include <vector>

namespace sui::quorum {

// Assembles context for agent invocation from vault contents
// Implementation pending — will read CONTEXT.md + relevant knowledge files
class ContextAssembler {
public:
    [[nodiscard]] std::string assemble(const std::string& agent_name,
                                        const std::string& vault_dir) const {
        // Placeholder: returns empty context until vault system is implemented
        (void)agent_name;
        (void)vault_dir;
        return {};
    }
};

} // namespace sui::quorum

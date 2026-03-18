#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "utils/subprocess.h"
#include "utils/json.h"

namespace fs = std::filesystem;
namespace sui::quorum::cli {

struct AgentCreateParams {
    std::string role;
    std::string name;
    std::string project;       // subfolder under configs/agents/
    std::string description;   // optional
    std::string skill_file;    // optional
    std::string target_dir;    // optional, for doer agents
    std::string data_dir;      // from cfg.daemon.data_dir
    bool no_ai = false;        // skip claude -p, copy template as-is
};

inline int create_agent(const AgentCreateParams& p) {
    // 1. Validate role
    static const std::vector<std::string> valid_roles = {
        "leader", "thinker", "doer", "reviewer", "scribe", "librarian"
    };
    bool role_valid = false;
    for (const auto& r : valid_roles) if (r == p.role) { role_valid = true; break; }
    if (!role_valid) {
        std::cerr << "ERROR: invalid role '" << p.role << "'. "
                  << "Valid: leader, thinker, doer, reviewer, scribe, librarian\n";
        return 1;
    }

    // 2. Derive paths
    auto agent_class = (p.role == "doer") ? "executor" : "analyst";
    auto config_dir = "configs/agents/" + p.project;
    auto config_path = config_dir + "/" + p.name + ".yaml";
    auto vault_dir = p.data_dir + "/vaults/" + p.name;
    auto context_path = vault_dir + "/CONTEXT.md";
    auto knowledge_dir = vault_dir + "/knowledge";

    // 3. Check for existing agent
    if (fs::exists(config_path)) {
        std::cerr << "ERROR: agent config already exists: " << config_path << "\n";
        return 1;
    }

    // 4. Create directories
    fs::create_directories(config_dir);
    fs::create_directories(knowledge_dir);

    // 5. Generate YAML config
    std::string yaml;
    yaml += "id: " + p.name + "\n";
    yaml += "name: \"" + p.name + "\"\n";
    yaml += "role: " + p.role + "\n";
    if (!p.description.empty()) {
        yaml += "description: \"" + p.description + "\"\n";
    }
    yaml += "\n";
    yaml += "vault_path: " + vault_dir + "/\n";
    yaml += "context_file: " + context_path + "\n";
    if (!p.skill_file.empty()) {
        yaml += "skill_file: " + p.skill_file + "\n";
    }
    if (p.role == "doer") {
        yaml += "\nexecutor:\n";
        yaml += "  target_dir: " + (p.target_dir.empty() ? "." : p.target_dir) + "\n";
        yaml += "  allowed_tools: all\n";
    }

    {
        std::ofstream out(config_path, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "ERROR: cannot write " << config_path << "\n";
            return 1;
        }
        out << yaml;
    }
    std::cout << "  Created: " << config_path << "\n";

    // 6. Generate CONTEXT.md
    auto template_path = std::string("docs/templates/CONTEXT_TEMPLATE.md");
    bool template_exists = fs::exists(template_path);

    if (p.no_ai || !template_exists) {
        // Offline mode: copy template as-is, or write minimal fallback
        if (template_exists) {
            fs::copy_file(template_path, context_path);
            std::cout << "  Created: " << context_path << " (template copy — fill placeholders)\n";
        } else {
            std::ofstream ctx(context_path, std::ios::trunc);
            ctx << "# " << p.name << " — Agent Context\n\n"
                << "## Role\n\nYou are the **" << p.name << "** ("
                << p.role << "). " << p.description << "\n";
            ctx.close();
            std::cout << "  Created: " << context_path << " (minimal — template not found)\n";
        }
    } else {
        // AI mode: use claude -p to fill the template
        std::ifstream tmpl(template_path);
        std::string template_content{
            std::istreambuf_iterator<char>(tmpl),
            std::istreambuf_iterator<char>()
        };

        std::string gen_prompt;
        gen_prompt += "Generate a CONTEXT.md for a Quorum agent with these details:\n\n";
        gen_prompt += "- ID: " + p.name + "\n";
        gen_prompt += "- Role: " + p.role + "\n";
        gen_prompt += "- Agent class: " + std::string(agent_class) + "\n";
        if (!p.description.empty())
            gen_prompt += "- Description: " + p.description + "\n";
        if (!p.skill_file.empty())
            gen_prompt += "- Skill file: " + p.skill_file + "\n";
        if (!p.target_dir.empty())
            gen_prompt += "- Target directory: " + p.target_dir + "\n";
        gen_prompt += "\nFill in the template below:\n";
        gen_prompt += "- Replace all {placeholders} with appropriate content\n";
        gen_prompt += "- Choose ";
        gen_prompt += (p.role == "doer") ? "Variant B (doer)" : "Variant A (analyst)";
        gen_prompt += " for the Output Rules section and DELETE the other variant\n";
        gen_prompt += "- Remove ALL HTML comments (<!-- ... -->)\n";
        gen_prompt += "- Keep the [INJECTED] Team Roster section header but leave it empty\n";
        gen_prompt += "- Output ONLY the filled markdown, nothing else\n\n";
        gen_prompt += "--- TEMPLATE START ---\n";
        gen_prompt += template_content;
        gen_prompt += "\n--- TEMPLATE END ---\n";

        auto temp_path = "/tmp/quorum_agent_gen_" + p.name + ".txt";
        {
            std::ofstream f(temp_path, std::ios::trunc);
            f << gen_prompt;
        }

        std::cout << "  Generating CONTEXT.md via claude -p...\n";
        auto cmd = "env -u CLAUDECODE cat " + temp_path
            + " | claude -p --dangerously-skip-permissions"
            + " --disallowedTools \"Write,Edit,NotebookEdit\""
            + " --output-format json 2>&1";

        auto result = run_command(cmd);
        std::remove(temp_path.c_str());

        bool ai_ok = false;
        if (result && result->exit_code == 0) {
            auto text = json::extract_string(result->output, "result");
            if (text && !text->empty()) {
                // Strip outer markdown fences if claude wrapped output
                auto content = *text;
                if (content.starts_with("```")) {
                    auto first_nl = content.find('\n');
                    if (first_nl != std::string::npos)
                        content = content.substr(first_nl + 1);
                    if (content.ends_with("```\n"))
                        content = content.substr(0, content.size() - 4);
                    else if (content.ends_with("```"))
                        content = content.substr(0, content.size() - 3);
                }

                std::ofstream ctx(context_path, std::ios::trunc);
                ctx << content;
                ai_ok = true;
                std::cout << "  Created: " << context_path << " (AI-generated)\n";
            }
        }

        if (!ai_ok) {
            std::cerr << "  WARNING: claude -p failed, copying template as fallback\n";
            fs::copy_file(template_path, context_path);
            std::cout << "  Created: " << context_path << " (template copy — fill placeholders)\n";
        }
    }

    std::cout << "  Created: " << knowledge_dir << "/\n";
    std::cout << "\nAgent '" << p.name << "' scaffolded. Add to your project YAML:\n";
    std::cout << "  - config: " << config_path << "\n";

    return 0;
}

} // namespace sui::quorum::cli

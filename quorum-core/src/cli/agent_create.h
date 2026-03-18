#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "utils/subprocess.h"
#include "utils/json.h"
#include "utils/config.h"
#include "utils/discover.h"
#include "cli/skills.h"

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
    bool regenerate = false;   // regenerate CONTEXT.md without changing fields
};

// Generate (or regenerate) CONTEXT.md for an agent.
// Returns description string: "AI-generated", "template copy", or "minimal".
inline std::string generate_context_md(
    const std::string& context_path,
    const std::string& agent_name,
    const std::string& role,
    const std::string& description,
    const std::string& skill_file,
    const std::string& target_dir,
    bool no_ai)
{
    auto agent_class_str = (role == "doer") ? "executor" : "analyst";
    auto template_path = std::string("docs/templates/CONTEXT_TEMPLATE.md");
    bool template_exists = fs::exists(template_path);

    if (no_ai || !template_exists) {
        if (template_exists) {
            fs::copy_file(template_path, context_path, fs::copy_options::overwrite_existing);
            return "template copy -- fill placeholders";
        } else {
            std::ofstream ctx(context_path, std::ios::trunc);
            ctx << "# " << agent_name << " -- Agent Context\n\n"
                << "## Role\n\nYou are the **" << agent_name << "** ("
                << role << "). " << description << "\n";
            ctx.close();
            return "minimal -- template not found";
        }
    }

    // AI mode: use claude -p to fill the template
    std::ifstream tmpl(template_path);
    std::string template_content{
        std::istreambuf_iterator<char>(tmpl),
        std::istreambuf_iterator<char>()
    };

    std::string gen_prompt;
    gen_prompt += "Generate a CONTEXT.md for a Quorum agent with these details:\n\n";
    gen_prompt += "- ID: " + agent_name + "\n";
    gen_prompt += "- Role: " + role + "\n";
    gen_prompt += "- Agent class: " + std::string(agent_class_str) + "\n";
    if (!description.empty())
        gen_prompt += "- Description: " + description + "\n";
    if (!skill_file.empty())
        gen_prompt += "- Skill file: " + skill_file + "\n";
    if (!target_dir.empty())
        gen_prompt += "- Target directory: " + target_dir + "\n";
    gen_prompt += "\nFill in the template below:\n";
    gen_prompt += "- Replace all {placeholders} with appropriate content\n";
    gen_prompt += "- Choose ";
    gen_prompt += (role == "doer") ? "Variant B (doer)" : "Variant A (analyst)";
    gen_prompt += " for the Output Rules section and DELETE the other variant\n";
    gen_prompt += "- Remove ALL HTML comments (<!-- ... -->)\n";
    gen_prompt += "- Keep the [INJECTED] Team Roster section header but leave it empty\n";
    gen_prompt += "- Output ONLY the filled markdown, nothing else\n\n";
    gen_prompt += "--- TEMPLATE START ---\n";
    gen_prompt += template_content;
    gen_prompt += "\n--- TEMPLATE END ---\n";

    auto temp_path = "/tmp/quorum_agent_gen_" + agent_name + ".txt";
    {
        std::ofstream f(temp_path, std::ios::trunc);
        f << gen_prompt;
    }

    std::cout << "  Generating CONTEXT.md via claude -p...\n";
    auto cmd = "env -u CLAUDECODE cat " + temp_path
        + " | claude -p --dangerously-skip-permissions"
        + " --disallowedTools \"Write,Edit,NotebookEdit\""
        + " --output-format json 2>&1";

    auto result = sui::quorum::run_command(cmd);
    std::remove(temp_path.c_str());

    if (result && result->exit_code == 0) {
        auto text = sui::quorum::json::extract_string(result->output, "result");
        if (text && !text->empty()) {
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
            return "AI-generated";
        }
    }

    std::cerr << "  WARNING: claude -p failed, copying template as fallback\n";
    fs::copy_file(template_path, context_path, fs::copy_options::overwrite_existing);
    return "template copy -- fill placeholders";
}

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

    // 1b. Detect project-local .quorum/ layout
    auto project_root = sui::quorum::discover_project_root();
    bool is_local = project_root.has_value();

    // 2. Derive paths
    std::string root_prefix = is_local ? (*project_root + "/") : "";
    auto config_dir = is_local ? (root_prefix + ".quorum/agents") : ("configs/agents/" + p.project);
    auto config_path = config_dir + "/" + p.name + ".yaml";
    auto vault_dir = is_local ? (root_prefix + ".quorum/vaults/" + p.name) : (p.data_dir + "/vaults/" + p.name);
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
    auto yaml_vault = is_local ? (".quorum/vaults/" + p.name + "/") : (vault_dir + "/");
    auto yaml_context = is_local ? (".quorum/vaults/" + p.name + "/CONTEXT.md") : context_path;
    yaml += "vault_path: " + yaml_vault + "\n";
    yaml += "context_file: " + yaml_context + "\n";
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
    auto gen_type = generate_context_md(context_path, p.name, p.role,
                                         p.description, p.skill_file,
                                         p.target_dir, p.no_ai);
    std::cout << "  Created: " << context_path << " (" << gen_type << ")\n";

    std::cout << "  Created: " << knowledge_dir << "/\n";

    if (is_local) {
        std::cout << "\nAgent '" << p.name << "' created. It will be auto-loaded from .quorum/agents/\n";
    } else {
        std::cout << "\nAgent '" << p.name << "' scaffolded. Add to your project YAML:\n";
        std::cout << "  - config: " << config_path << "\n";
    }

    // Suggest skills for doer agents
    if (p.role == "doer" && p.skill_file.empty()) {
        auto root = sui::quorum::discover_project_root();
        if (root) {
            auto skills = sui::quorum::cli::discover_skills(*root);
            if (!skills.empty()) {
                std::cout << "\nTip: Available skills for this doer agent:\n";
                for (const auto& s : skills) {
                    std::cout << "  quorum agent modify --name " << p.name
                              << " --skill " << s.id << "\n";
                }
            }
        }
    }

    return 0;
}

inline int modify_agent(const AgentCreateParams& overrides) {
    // 1. Find .quorum/
    auto project_root = sui::quorum::discover_project_root();
    if (!project_root) {
        std::cerr << "ERROR: no .quorum/ found. Run 'quorum init' first.\n";
        return 1;
    }

    auto root = *project_root;
    auto config_path = root + "/.quorum/agents/" + overrides.name + ".yaml";
    if (!fs::exists(config_path)) {
        std::cerr << "ERROR: agent not found: " << overrides.name << "\n";
        auto agents_dir = root + "/.quorum/agents";
        if (fs::exists(agents_dir)) {
            std::cerr << "Available agents:\n";
            for (const auto& entry : fs::directory_iterator(agents_dir)) {
                if (entry.path().extension() == ".yaml")
                    std::cerr << "  - " << entry.path().stem().string() << "\n";
            }
        }
        return 1;
    }

    // 2. Load existing config
    auto existing = sui::quorum::load_agent_config(config_path);
    if (!existing) {
        std::cerr << "ERROR: failed to parse " << config_path << "\n";
        return 1;
    }

    // 3. Apply overrides (only non-empty fields)
    bool changed = false;
    if (!overrides.role.empty() && overrides.role != existing->role) {
        // Validate role
        static const std::vector<std::string> valid_roles = {
            "leader", "thinker", "doer", "reviewer", "scribe", "librarian"
        };
        bool role_valid = false;
        for (const auto& r : valid_roles) if (r == overrides.role) { role_valid = true; break; }
        if (!role_valid) {
            std::cerr << "ERROR: invalid role '" << overrides.role << "'. "
                      << "Valid: leader, thinker, doer, reviewer, scribe, librarian\n";
            return 1;
        }
        existing->role = overrides.role;
        existing->agent_class = (overrides.role == "doer") ? "executor" : "analyst";
        changed = true;
    }
    if (!overrides.description.empty() && overrides.description != existing->description) {
        existing->description = overrides.description;
        changed = true;
    }
    if (!overrides.skill_file.empty() && overrides.skill_file != existing->skill_file) {
        existing->skill_file = overrides.skill_file;
        changed = true;
    }
    if (!overrides.target_dir.empty() && overrides.target_dir != existing->target_dir) {
        existing->target_dir = overrides.target_dir;
        changed = true;
    }

    if (!changed && !overrides.regenerate) {
        std::cout << "No changes specified for '" << overrides.name << "'. Use flags to update:\n";
        std::cout << "  --role <role>          Change agent role\n";
        std::cout << "  --description \"...\"    Change description\n";
        std::cout << "  --skill-file <path>    Set/change skill file\n";
        std::cout << "  --target-dir <path>    Set/change target directory\n";
        std::cout << "  --regenerate           Regenerate CONTEXT.md from current config\n";
        return 0;
    }

    // 4. Rewrite YAML config
    auto yaml_vault = ".quorum/vaults/" + existing->id + "/";
    auto yaml_context = ".quorum/vaults/" + existing->id + "/CONTEXT.md";

    std::string yaml;
    yaml += "id: " + existing->id + "\n";
    yaml += "name: \"" + existing->name + "\"\n";
    yaml += "role: " + existing->role + "\n";
    if (!existing->description.empty()) {
        yaml += "description: \"" + existing->description + "\"\n";
    }
    yaml += "\n";
    yaml += "vault_path: " + yaml_vault + "\n";
    yaml += "context_file: " + yaml_context + "\n";
    if (!existing->skill_file.empty()) {
        yaml += "skill_file: " + existing->skill_file + "\n";
    }
    if (existing->role == "doer" || existing->agent_class == "executor") {
        yaml += "\nexecutor:\n";
        yaml += "  target_dir: " + (existing->target_dir.empty() ? "." : existing->target_dir) + "\n";
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
    std::cout << "  Updated: " << config_path << "\n";

    // 5. Regenerate CONTEXT.md
    auto context_path_abs = root + "/" + yaml_context;
    auto gen_type = generate_context_md(context_path_abs, existing->id,
                                         existing->role, existing->description,
                                         existing->skill_file, existing->target_dir,
                                         overrides.no_ai);
    std::cout << "  Regenerated: " << context_path_abs << " (" << gen_type << ")\n";

    std::cout << "\nAgent '" << existing->id << "' modified.\n";
    return 0;
}

inline int list_agents() {
    auto project_root = sui::quorum::discover_project_root();
    if (!project_root) {
        std::cerr << "No .quorum/ found. Run 'quorum init' first.\n";
        return 1;
    }

    auto agents_dir = *project_root + "/.quorum/agents";
    if (!fs::exists(agents_dir)) {
        std::cout << "No agents configured.\n";
        return 0;
    }

    // Collect and sort
    std::vector<sui::quorum::AgentMetadata> agents;
    for (const auto& entry : fs::directory_iterator(agents_dir)) {
        if (entry.path().extension() != ".yaml") continue;
        auto agent = sui::quorum::load_agent_config(entry.path().string());
        if (agent) agents.push_back(*agent);
    }
    std::sort(agents.begin(), agents.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });

    std::cout << "Agents:\n";
    for (const auto& a : agents) {
        std::cout << "  " << a.id;
        if (!a.role.empty()) std::cout << " (" << a.role << ")";
        if (!a.description.empty()) std::cout << " -- " << a.description;
        std::cout << "\n";
    }
    if (agents.empty()) {
        std::cout << "  (none)\n";
    }
    return 0;
}

} // namespace sui::quorum::cli

#pragma once

// Phase 12 — `quorum ask "<question>" [--project <path|name>]`.
//
// A READ-ONLY CLI that lets a colleague "ask the project's manager" a question.
// It invokes Quorum's leader agent single-shot and read-only (analyst-clamped),
// feeding it the project's accumulated knowledge (curated Pitch / Decision Log /
// Roadmap + scribe learnings + a vault digest) AND letting it read the live
// project (Read/Grep/Glob, cwd = project root), then prints the synthesized
// answer.
//
// It is `quorum librarian curate` MINUS the block-parse/diff/apply machinery —
// it answers a question instead of emitting structured blocks. The single live
// `claude -p` invocation lives only in run_ask, modelled on cli/agent_create.h's
// synchronous analyst-clamped pattern. The two helpers below (resolve_project_path
// + assemble_manager_prompt) are PURE and spend NO claude tokens, so they are
// unit-tested directly (tests/unit/test_ask.cpp).
//
// Header-only, matches the cli/librarian_curate.h / cli/agent_create.h convention.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "cli/librarian_curate.h"   // detail::slurp, detail::scribe_vault_digest
#include "vault/scribe_writer.h"    // detail::read_file_text
#include "utils/subprocess.h"       // run_command
#include "utils/json.h"             // json::extract_string

namespace sui::quorum::cli {

struct AskOptions {
    std::string project;    // path OR project name; defaults to cwd ("." )
    std::string question;   // the question to ask the manager
    std::string agent;      // optional --agent <name>; empty = manager default
};

// Resolve an --project argument to an absolute project root containing .quorum/.
//
//   1. If `arg` is an existing directory, use it as-is.
//   2. Otherwise treat `arg` as a project NAME and resolve <HOME>/nonvis/<name>/
//      (HOME via getenv; a literal "~" prefix is expanded too).
//
// In both cases <resolved>/.quorum must exist. On failure `err` is set to a
// clear message and "" is returned. On success the absolute path is returned.
// PURE: filesystem reads only, no claude.
[[nodiscard]] inline std::string resolve_project_path(const std::string& arg,
                                                      std::string& err) {
    namespace fs = std::filesystem;
    err.clear();

    auto home_env = []() -> std::string {
        const char* h = std::getenv("HOME");
        return h ? std::string(h) : std::string{};
    };

    // Expand a leading "~" to $HOME for the path-as-given check.
    std::string given = arg;
    if (!given.empty() && given[0] == '~') {
        auto home = home_env();
        if (!home.empty()) {
            given = home + given.substr(1);
        }
    }

    fs::path candidate;
    std::error_code ec;
    if (!given.empty() && fs::is_directory(given, ec)) {
        // Case 1: an existing directory used as-is.
        candidate = fs::absolute(given, ec);
        if (ec) candidate = given;
    } else {
        // Case 2: treat arg as a project NAME under <HOME>/nonvis/<name>/.
        auto home = home_env();
        if (home.empty()) {
            err = "cannot resolve project name '" + arg +
                  "': HOME is unset";
            return "";
        }
        candidate = fs::path(home) / "nonvis" / arg;
    }

    auto quorum_dir = candidate / ".quorum";
    std::error_code qec;
    if (!fs::exists(quorum_dir, qec)) {
        err = "no .quorum/ found at " + candidate.string() +
              " — is this a Quorum-managed project?";
        return "";
    }
    return candidate.string();
}

// Compose the manager-persona prompt fed to the leader agent. Gathers, when
// present and degrading gracefully on absence:
//   - Pitch/00 - Introduction.md, Pitch/01 - Anti-goals.md  (curated Pitch)
//   - 00 - Decision Log.md, 01 - Roadmap.md                 (curated layer)
//   - .quorum/learnings.md                                  (scribe learnings)
//   - a digest of .quorum/vaults/scribe/knowledge/          (filenames + recent)
// then embeds the question and a manager-persona instruction block. Never errors
// on a missing file. PURE: filesystem reads only, no claude.
[[nodiscard]] inline std::string assemble_manager_prompt(
    const std::string& project_root, const std::string& question) {
    namespace fs = std::filesystem;
    fs::path root(project_root);

    // Read a curated-layer file via scribe_writer's helper; empty if absent.
    // The curated layer lives under <root>/.quorum/librarian/ (curated_base).
    auto read_rel = [&](const std::string& rel) -> std::string {
        auto p = sui::quorum::detail::curated_base(project_root) / rel;
        std::error_code ec;
        if (!fs::exists(p, ec)) return {};
        return sui::quorum::detail::read_file_text(p);
    };

    // Append "### <label> (<file>)\n\n<fenced contents | absent note>\n\n".
    auto append_section = [](std::string& out, const std::string& label,
                             const std::string& rel, const std::string& body) {
        out += "### " + label + " (" + rel + ")\n\n";
        if (body.empty()) {
            out += "(not present)\n\n";
        } else {
            out += "```\n" + body;
            if (body.back() != '\n') out += "\n";
            out += "```\n\n";
        }
    };

    std::string p;
    p += "You are the manager of this project. A colleague is asking you a "
         "question from outside the project.\n\n";
    p += "Answer it from the project's recorded knowledge below; you MAY also "
         "read the project's files (Read/Grep/Glob) to fill gaps the notes "
         "don't cover. Be concise, direct, and cite which source(s) you drew on "
         "(curated Pitch / Decision Log / Roadmap / learnings / live code). If "
         "the knowledge base genuinely doesn't cover it and the code doesn't "
         "either, say so plainly.\n\n";

    p += "## Question\n\n";
    p += question;
    if (p.empty() || p.back() != '\n') p += "\n";
    p += "\n";

    p += "## Project knowledge\n\n";
    p += "### Curated aspirational layer\n\n";
    append_section(p, "Pitch / Introduction", "Pitch/00 - Introduction.md",
                   read_rel("Pitch/00 - Introduction.md"));
    append_section(p, "Pitch / Anti-goals", "Pitch/01 - Anti-goals.md",
                   read_rel("Pitch/01 - Anti-goals.md"));
    append_section(p, "Decision Log", "00 - Decision Log.md",
                   read_rel("00 - Decision Log.md"));
    append_section(p, "Roadmap", "01 - Roadmap.md",
                   read_rel("01 - Roadmap.md"));

    p += "### Scribe learnings (.quorum/learnings.md)\n\n";
    {
        auto learnings =
            sui::quorum::cli::detail::slurp(root / ".quorum" / "learnings.md");
        if (learnings.empty()) {
            p += "(no learnings.md yet)\n\n";
        } else {
            p += "```\n" + learnings;
            if (learnings.back() != '\n') p += "\n";
            p += "```\n\n";
        }
    }

    p += "### Scribe knowledge vault digest\n\n";
    p += sui::quorum::cli::detail::scribe_vault_digest(
             root / ".quorum" / "vaults" / "scribe" / "knowledge");
    p += "\n";

    p += "## Answer\n\n";
    p += "Answer the colleague's question now. Cite your source(s).\n";
    return p;
}

// ---- Agent path (optional --agent <name>) ---------------------------------
//
// An agent is configured at <project_root>/.quorum/agents/<name>.yaml as a FLAT
// `key: value` file (some values quoted). `--agent <name>` targets that agent
// directly instead of the generic manager. The helpers below are PURE.

namespace detail {

// List the available agent name stems (filename without ".yaml") under
// <project_root>/.quorum/agents/, sorted ascending. PURE: filesystem only.
[[nodiscard]] inline std::vector<std::string> list_agent_names(
    const std::string& project_root) {
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    auto dir = fs::path(project_root) / ".quorum" / "agents";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return names;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".yaml") continue;
        names.push_back(e.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Strip a single pair of surrounding double-quotes (and trim outer whitespace)
// from a flat-YAML scalar value. PURE.
[[nodiscard]] inline std::string unquote_yaml_value(std::string v) {
    auto l = v.find_first_not_of(" \t");
    auto r = v.find_last_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    v = v.substr(l, r - l + 1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v = v.substr(1, v.size() - 2);
    }
    return v;
}

// Parse a single top-level `key: value` field from a flat-YAML agent config.
// Returns the unquoted value, or "" if the key is absent. PURE.
[[nodiscard]] inline std::string parse_agent_field(const std::string& yaml_text,
                                                   const std::string& key) {
    std::istringstream in(yaml_text);
    std::string line;
    while (std::getline(in, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        // Key is everything before the first colon, trimmed; require an exact
        // match so e.g. "skill_file" doesn't shadow a hypothetical "skill".
        std::string k = line.substr(0, colon);
        auto kl = k.find_first_not_of(" \t");
        auto kr = k.find_last_not_of(" \t");
        if (kl == std::string::npos) continue;
        k = k.substr(kl, kr - kl + 1);
        if (k != key) continue;
        return unquote_yaml_value(line.substr(colon + 1));
    }
    return {};
}

}  // namespace detail

// Resolve a skill_file reference relative to the project root, mirroring
// context_assembler.h:417-435: expand a leading "~/", then treat a relative
// path as project-rooted; absolute paths are used as-is. PURE.
[[nodiscard]] inline std::string resolve_skill_path(
    const std::string& project_root, const std::string& skill_file) {
    namespace fs = std::filesystem;
    if (skill_file.empty()) return {};
    std::string spath = skill_file;
    if (spath.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home) spath = std::string(home) + spath.substr(1);
    }
    fs::path skill_path(spath);
    if (skill_path.is_relative() && !project_root.empty()) {
        auto rooted = fs::path(project_root) / skill_path;
        return rooted.string();
    }
    return skill_path.string();
}

// Compose the agent-persona prompt fed to a configured agent. Mirrors
// assemble_manager_prompt's structure + graceful-degrade-on-missing-file style:
//   - persona naming the agent + its role, scoped to read-only colleague Q&A
//   - the question
//   - the agent's skill file (resolved relative→project-rooted), embedded
//   - the agent's recorded knowledge (scribe_vault_digest of its vault's
//     knowledge/ dir), or "(no recorded knowledge)" if vault_path is empty
//   - an answer instruction
// Never errors on a missing file. PURE: filesystem reads only, no claude.
[[nodiscard]] inline std::string assemble_agent_prompt(
    const std::string& project_root, const std::string& agent_name,
    const std::string& role, const std::string& skill_file,
    const std::string& vault_path, const std::string& question) {
    namespace fs = std::filesystem;
    fs::path root(project_root);

    std::string p;
    p += "You are the **" + agent_name + "** (" + role +
         ") of this project. A colleague is asking you a question from outside "
         "the project. Answer from your skill and your recorded knowledge "
         "below; you MAY also read the live project (Read/Grep/Glob, cwd = "
         "project root) to fill gaps. Be concise and direct, and cite which "
         "source(s) you drew on. If your knowledge and the code genuinely "
         "don't cover it, say so.\n\n";

    p += "## Question\n\n";
    p += question;
    if (p.empty() || p.back() != '\n') p += "\n";
    p += "\n";

    // Skill file — resolve relative→project-rooted (context_assembler.h:417-435).
    auto resolved_skill = resolve_skill_path(project_root, skill_file);
    p += "## Your skill (" + skill_file + ")\n\n";
    {
        std::error_code ec;
        if (!resolved_skill.empty() && fs::exists(resolved_skill, ec)) {
            auto body = sui::quorum::detail::read_file_text(resolved_skill);
            p += "```\n" + body;
            if (body.empty() || body.back() != '\n') p += "\n";
            p += "```\n\n";
        } else {
            p += "(skill file not found: " + skill_file + ")\n\n";
        }
    }

    // Recorded knowledge — the agent's own vault knowledge dir.
    p += "## Your recorded knowledge\n\n";
    if (vault_path.empty()) {
        p += "(no recorded knowledge)\n\n";
    } else {
        p += sui::quorum::cli::detail::scribe_vault_digest(
                 root / vault_path / "knowledge");
        p += "\n";
    }

    p += "## Answer\n\n";
    p += "Answer the colleague's question now. Cite your source(s).\n";
    return p;
}

// Top-level entrypoint for `quorum ask`. Resolves the project root, assembles
// the manager prompt, then runs ONE live read-only `claude -p` invocation with
// cwd = project root so the leader can read the live code. Prints a one-line
// header then the synthesized answer. Returns 0 on success; 1 on resolution
// failure (printed BEFORE any claude call) or claude invocation failure.
[[nodiscard]] inline int run_ask(const AskOptions& opts) {
    namespace fs = std::filesystem;

    if (opts.question.empty()) {
        std::cerr << "ERROR: ask requires a question. "
                     "Usage: quorum ask \"<question>\" [--project <path|name>]\n";
        return 1;
    }

    // 1. Resolve project root (default cwd). Errors print and exit BEFORE claude.
    std::string arg = opts.project.empty() ? std::string(".") : opts.project;
    std::string err;
    std::string project_root = resolve_project_path(arg, err);
    if (project_root.empty()) {
        std::cerr << "ERROR: " << err << "\n";
        return 1;
    }

    // 2. Assemble the prompt. Default = generic manager. With --agent <name>,
    //    target that configured agent instead (resolve + parse its yaml first;
    //    a missing agent prints the available-agents list and exits 1 BEFORE
    //    any claude call).
    std::string prompt;
    std::string header_role = "manager";
    if (opts.agent.empty()) {
        prompt = assemble_manager_prompt(project_root, opts.question);
    } else {
        auto yaml_path = fs::path(project_root) / ".quorum" / "agents" /
                         (opts.agent + ".yaml");
        std::error_code ec;
        if (!fs::exists(yaml_path, ec)) {
            auto names = detail::list_agent_names(project_root);
            std::string available;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i) available += ", ";
                available += names[i];
            }
            std::cerr << "ERROR: no agent '" << opts.agent << "' in "
                      << project_root << " — available: " << available << "\n";
            return 1;
        }
        auto yaml_text = sui::quorum::detail::read_file_text(yaml_path);
        auto role = detail::parse_agent_field(yaml_text, "role");
        auto skill_file = detail::parse_agent_field(yaml_text, "skill_file");
        auto vault_path = detail::parse_agent_field(yaml_text, "vault_path");
        prompt = assemble_agent_prompt(project_root, opts.agent, role,
                                       skill_file, vault_path, opts.question);
        header_role = opts.agent;
    }

    // 3. Write the prompt to a temp file (ABSOLUTE path so the cd below doesn't
    //    break the `cat`).
    auto temp_path =
        "/tmp/quorum_ask_" + std::to_string(::getpid()) + ".txt";
    {
        std::ofstream f(temp_path, std::ios::trunc);
        f << prompt;
    }

    // 4. Live claude -p invocation — read-only (Write/Edit/NotebookEdit
    //    disallowed), cwd = project root so the leader can Read/Grep/Glob the
    //    live code. Modelled on cli/agent_create.h's synchronous pattern.
    std::cerr << "Asking the " << header_role
              << " (claude -p, read-only)...\n";
    auto cmd = "cd \"" + project_root + "\" && env -u CLAUDECODE cat " +
               temp_path +
               " | claude -p --dangerously-skip-permissions"
               " --disallowedTools \"Write,Edit,NotebookEdit\""
               " --output-format json 2>&1";
    auto result = sui::quorum::run_command(cmd);
    std::remove(temp_path.c_str());

    if (!result || result->exit_code != 0) {
        std::cerr << "ERROR: claude -p invocation failed";
        if (result) std::cerr << " (exit " << result->exit_code << ")";
        std::cerr << "\n";
        return 1;
    }
    auto text = sui::quorum::json::extract_string(result->output, "result");
    if (!text || text->empty()) {
        std::cerr << "ERROR: manager produced no answer\n";
        return 1;
    }

    // 5. Strip any surrounding ``` code fences (mirror agent_create.h).
    std::string answer = *text;
    if (answer.starts_with("```")) {
        auto first_nl = answer.find('\n');
        if (first_nl != std::string::npos)
            answer = answer.substr(first_nl + 1);
        if (answer.ends_with("```\n"))
            answer = answer.substr(0, answer.size() - 4);
        else if (answer.ends_with("```"))
            answer = answer.substr(0, answer.size() - 3);
    }

    // 6. Print a one-line header + the answer.
    auto project_name = fs::path(project_root).filename().string();
    if (project_name.empty()) {
        // Trailing slash: take the parent's filename.
        project_name = fs::path(project_root).parent_path().filename().string();
    }
    std::cout << "=== " << project_name << " " << header_role << " ===\n\n";
    std::cout << answer;
    if (answer.empty() || answer.back() != '\n') std::cout << "\n";
    return 0;
}

}  // namespace sui::quorum::cli

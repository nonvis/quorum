// tests/unit/test_team_presets.cpp
// Unit tests for team presets (load_team_presets).
//
// Run:  cd build && cmake .. && make test_team_presets && ./test_team_presets

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "utils/config.h"

namespace fs = std::filesystem;

// --- helpers ----------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;
static int g_test_num = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static std::string make_temp_dir() {
    auto dir = fs::temp_directory_path() /
        ("quorum_test_teams_" + std::to_string(getpid()) + "_" + std::to_string(g_test_num++));
    fs::create_directories(dir);
    return dir.string();
}

static void cleanup_temp(const std::string& path) {
    fs::remove_all(path);
}

// --- Test A: load_team_presets with 2 YAML files ----------------------------

static void test_load_team_presets() {
    std::cout << "\n=== A. load_team_presets ===\n\n";

    auto tmp = make_temp_dir();

    // Create full.yaml
    {
        std::ofstream out(tmp + "/full.yaml");
        out << "name: Full Pipeline\n"
            << "default_path: [leader, thinker, doer]\n";
    }

    // Create quick.yaml
    {
        std::ofstream out(tmp + "/quick.yaml");
        out << "name: Quick Build\n"
            << "default_path: [leader, doer]\n";
    }

    auto teams = sui::quorum::load_team_presets(tmp);

    check(teams.size() == 2, "A: returns 2 presets");
    check(teams[0].id == "full", "A: first preset id is 'full'");
    check(teams[0].name == "Full Pipeline", "A: first preset name is 'Full Pipeline'");
    check(teams[0].default_path.size() == 3, "A: first preset has 3 path entries");
    check(teams[1].id == "quick", "A: second preset id is 'quick'");
    check(teams[1].name == "Quick Build", "A: second preset name is 'Quick Build'");
    check(teams[1].default_path.size() == 2, "A: second preset has 2 path entries");

    cleanup_temp(tmp);
}

// --- Test B: empty directory ------------------------------------------------

static void test_empty_directory() {
    std::cout << "\n=== B. empty directory ===\n\n";

    auto tmp = make_temp_dir();

    auto teams = sui::quorum::load_team_presets(tmp);
    check(teams.empty(), "B: returns empty vector for empty directory");

    cleanup_temp(tmp);
}

// --- Test C: nonexistent directory ------------------------------------------

static void test_nonexistent_directory() {
    std::cout << "\n=== C. nonexistent directory ===\n\n";

    auto teams = sui::quorum::load_team_presets("/tmp/nonexistent_quorum_test");
    check(teams.empty(), "C: returns empty vector for nonexistent directory");
}

// --- Test D: YAML parsing ---------------------------------------------------

static void test_yaml_parsing() {
    std::cout << "\n=== D. YAML parsing ===\n\n";

    auto tmp = make_temp_dir();

    {
        std::ofstream out(tmp + "/pipeline.yaml");
        out << "name: Full Pipeline\n"
            << "default_path: [leader, thinker, doer]\n";
    }

    auto teams = sui::quorum::load_team_presets(tmp);
    check(teams.size() == 1, "D: returns 1 preset");
    check(teams[0].name == "Full Pipeline", "D: name is 'Full Pipeline'");
    check(teams[0].default_path.size() == 3, "D: has 3 path entries");
    check(teams[0].default_path[0] == "leader", "D: path[0] is 'leader'");
    check(teams[0].default_path[1] == "thinker", "D: path[1] is 'thinker'");
    check(teams[0].default_path[2] == "doer", "D: path[2] is 'doer'");

    cleanup_temp(tmp);
}

// --- Test E: id from filename -----------------------------------------------

static void test_id_from_filename() {
    std::cout << "\n=== E. id from filename ===\n\n";

    auto tmp = make_temp_dir();

    {
        std::ofstream out(tmp + "/my-team.yaml");
        out << "name: My Team\n"
            << "default_path: [leader]\n";
    }

    {
        std::ofstream out(tmp + "/analysis.yml");
        out << "name: Analysis\n"
            << "default_path: [leader, thinker]\n";
    }

    auto teams = sui::quorum::load_team_presets(tmp);
    check(teams.size() == 2, "E: returns 2 presets (.yaml and .yml)");

    // Sorted alphabetically: analysis, my-team
    check(teams[0].id == "analysis", "E: .yml file loaded with id 'analysis'");
    check(teams[1].id == "my-team", "E: .yaml file loaded with id 'my-team'");

    cleanup_temp(tmp);
}

// --- Test F: alphabetical sort ----------------------------------------------

static void test_alphabetical_sort() {
    std::cout << "\n=== F. alphabetical sort ===\n\n";

    auto tmp = make_temp_dir();

    {
        std::ofstream out(tmp + "/c-team.yaml");
        out << "name: C Team\ndefault_path: [leader]\n";
    }
    {
        std::ofstream out(tmp + "/a-team.yaml");
        out << "name: A Team\ndefault_path: [leader]\n";
    }
    {
        std::ofstream out(tmp + "/b-team.yaml");
        out << "name: B Team\ndefault_path: [leader]\n";
    }

    auto teams = sui::quorum::load_team_presets(tmp);
    check(teams.size() == 3, "F: returns 3 presets");
    check(teams[0].id == "a-team", "F: first is 'a-team'");
    check(teams[1].id == "b-team", "F: second is 'b-team'");
    check(teams[2].id == "c-team", "F: third is 'c-team'");

    cleanup_temp(tmp);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Team Presets Tests ===\n";

    test_load_team_presets();
    test_empty_directory();
    test_nonexistent_directory();
    test_yaml_parsing();
    test_id_from_filename();
    test_alphabetical_sort();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}

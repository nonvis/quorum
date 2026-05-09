// tests/unit/test_rubric_parser.cpp
// Phase 8 Track 2 / Track 6 #24 — rubric format + loader + resolver.
//
// Verifies that agent/rubric.h:
//   - parses a well-formed rubric (frontmatter, categories, items, IDs, weights)
//   - rejects rubrics without YAML frontmatter
//   - skips malformed items (no weight) without crashing
//   - skips items with zero / negative weight, with stderr warning
//   - returns an empty Rubric when only frontmatter is present
//   - resolves project override over shipped template
//   - falls back to template when no override exists
//   - returns nullopt when neither exists
//
// Run:  cd build && cmake .. && make test_rubric_parser && ./test_rubric_parser

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "agent/rubric.h"

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

static fs::path make_tmp_dir(const std::string& tag) {
    auto dir = fs::temp_directory_path() /
        ("quorum_test_rubric_" + tag + "_" +
         std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << content;
}

static void cleanup(const fs::path& path) {
    fs::remove_all(path);
}

// --- Test A: well-formed rubric ---------------------------------------------

static void test_well_formed() {
    std::cout << "\n=== A. well-formed rubric: 2 categories, 4 items ===\n\n";

    auto dir = make_tmp_dir("well_formed");
    auto path = dir / "rubric.md";

    write_file(path,
        "---\n"
        "name: move-dev\n"
        "version: v1\n"
        "---\n"
        "\n"
        "# Rubric: move-dev (v1)\n"
        "\n"
        "## Compilation & Tests (weight 30)\n"
        "- [ ] (5) Code compiles cleanly\n"
        "- [ ] (3) No new warnings\n"
        "\n"
        "## Move 2024 idioms (weight 25)\n"
        "- [ ] (4) Uses public(package) modifiers correctly\n"
        "- [ ] (2) Receivers named self\n"
    );

    auto r = sui::quorum::load_rubric(path);
    check(r.has_value(), "A: parser returns Rubric");
    check(r->name == "move-dev", "A: name == 'move-dev'");
    check(r->version == "v1", "A: version == 'v1'");
    check(r->items.size() == 4, "A: 4 items parsed");
    check(r->total_weight == (5 + 3 + 4 + 2),
          "A: total_weight == 14 (5+3+4+2)");

    // Categories preserved verbatim (without the `(weight N)` suffix).
    check(r->items[0].category == "Compilation & Tests",
          "A: item[0].category == 'Compilation & Tests'");
    check(r->items[3].category == "Move 2024 idioms",
          "A: item[3].category == 'Move 2024 idioms'");

    // Weights match.
    check(r->items[0].weight == 5, "A: item[0].weight == 5");
    check(r->items[1].weight == 3, "A: item[1].weight == 3");
    check(r->items[2].weight == 4, "A: item[2].weight == 4");
    check(r->items[3].weight == 2, "A: item[3].weight == 2");

    // IDs are slugified <category-slug>.<description-slug>.
    check(r->items[0].id == "compilation-tests.code-compiles-cleanly",
          "A: item[0].id slug correct");
    check(r->items[1].id == "compilation-tests.no-new-warnings",
          "A: item[1].id slug correct");
    check(r->items[2].id == "move-2024-idioms.uses-public-package-modifiers-correctly",
          "A: item[2].id slug correct");
    check(r->items[3].id == "move-2024-idioms.receivers-named-self",
          "A: item[3].id slug correct");

    cleanup(dir);
}

// --- Test B: missing frontmatter --------------------------------------------

static void test_missing_frontmatter() {
    std::cout << "\n=== B. missing frontmatter → nullopt ===\n\n";

    auto dir = make_tmp_dir("no_fm");
    auto path = dir / "rubric.md";

    write_file(path,
        "# Rubric: bare\n"
        "\n"
        "## Cat A\n"
        "- [ ] (5) Item one\n"
    );

    auto r = sui::quorum::load_rubric(path);
    check(!r.has_value(), "B: parser returns nullopt for no-frontmatter rubric");

    cleanup(dir);
}

// --- Test C: malformed item line (no weight) --------------------------------

static void test_malformed_item() {
    std::cout << "\n=== C. malformed item without (N) → silently skipped ===\n\n";

    auto dir = make_tmp_dir("malformed");
    auto path = dir / "rubric.md";

    // Two items: one well-formed, one missing the (N) annotation.
    // Documented behavior: missing-weight item is skipped silently so that
    // authors can keep prose bullets / TODO notes in the rubric file.
    write_file(path,
        "---\n"
        "name: test\n"
        "version: v1\n"
        "---\n"
        "## Cat\n"
        "- [ ] (5) Good item\n"
        "- [ ] no weight here\n"
    );

    auto r = sui::quorum::load_rubric(path);
    check(r.has_value(), "C: parser still returns Rubric");
    check(r->items.size() == 1, "C: only the well-formed item is kept");
    check(r->items[0].description == "Good item",
          "C: kept item is the well-formed one");
    check(r->total_weight == 5, "C: total_weight == 5 (malformed item ignored)");

    cleanup(dir);
}

// --- Test D: zero / negative weight -----------------------------------------

static void test_zero_negative_weight() {
    std::cout << "\n=== D. zero / negative weight items skipped, parser still returns ===\n\n";

    auto dir = make_tmp_dir("zero_weight");
    auto path = dir / "rubric.md";

    write_file(path,
        "---\n"
        "name: test\n"
        "version: v1\n"
        "---\n"
        "## Cat\n"
        "- [ ] (3) Real item\n"
        "- [ ] (0) Zero-weight item\n"
        "- [ ] (-2) Negative-weight item\n"
    );

    auto r = sui::quorum::load_rubric(path);
    check(r.has_value(), "D: parser still returns Rubric");
    check(r->items.size() == 1, "D: only the positive-weight item is kept");
    check(r->items[0].weight == 3, "D: kept item has weight 3");
    check(r->total_weight == 3, "D: total_weight == 3 (others dropped)");

    cleanup(dir);
}

// --- Test E: empty rubric (frontmatter only) --------------------------------

static void test_empty_rubric() {
    std::cout << "\n=== E. empty rubric (frontmatter only) → empty items, total_weight 0 ===\n\n";

    auto dir = make_tmp_dir("empty");
    auto path = dir / "rubric.md";

    write_file(path,
        "---\n"
        "name: skeleton\n"
        "version: v0\n"
        "---\n"
    );

    auto r = sui::quorum::load_rubric(path);
    check(r.has_value(), "E: parser returns Rubric");
    check(r->name == "skeleton", "E: name preserved");
    check(r->version == "v0", "E: version preserved");
    check(r->items.empty(), "E: items list is empty");
    check(r->total_weight == 0, "E: total_weight == 0");

    cleanup(dir);
}

// --- Test F/G/H helpers: stage a synthetic project + a synthetic CWD --------
//
// resolve_rubric() looks at:
//   1. <project_root>/.quorum/rubrics/<rs>/rubric.md  (override — wins)
//   2. <cwd>/templates/rubrics/<rs>/rubric.md         (template fallback)
//
// To exercise (2) deterministically we chdir into a tmp dir that has its own
// `templates/rubrics/<rs>/rubric.md` tree. We restore CWD afterwards.

struct CwdGuard {
    fs::path saved;
    explicit CwdGuard() : saved(fs::current_path()) {}
    ~CwdGuard() { std::error_code ec; fs::current_path(saved, ec); }
};

static const char* OVERRIDE_BODY =
    "---\n"
    "name: test\n"
    "version: project-override\n"
    "---\n"
    "## Cat\n"
    "- [ ] (1) Override item\n";

static const char* TEMPLATE_BODY =
    "---\n"
    "name: test\n"
    "version: template-fallback\n"
    "---\n"
    "## Cat\n"
    "- [ ] (2) Template item\n";

// --- Test F: project override wins over template ----------------------------

static void test_resolve_override_wins() {
    std::cout << "\n=== F. resolve_rubric: project override wins over template ===\n\n";

    auto sandbox = make_tmp_dir("resolve_override");
    auto project = sandbox / "project";
    fs::create_directories(project);

    write_file(project / ".quorum" / "rubrics" / "test" / "rubric.md",
               OVERRIDE_BODY);
    write_file(sandbox / "templates" / "rubrics" / "test" / "rubric.md",
               TEMPLATE_BODY);

    CwdGuard g;
    fs::current_path(sandbox);

    auto r = sui::quorum::resolve_rubric(project.string(), "test");
    check(r.has_value(), "F: resolve returns Rubric");
    check(r->version == "project-override",
          "F: project override version is selected");
    check(r->items.size() == 1 && r->items[0].weight == 1,
          "F: override's items are the ones returned");

    cleanup(sandbox);
}

// --- Test G: template-only (no override) ------------------------------------

static void test_resolve_template_only() {
    std::cout << "\n=== G. resolve_rubric: template fallback when no override ===\n\n";

    auto sandbox = make_tmp_dir("resolve_template");
    auto project = sandbox / "project";
    fs::create_directories(project);  // project exists but has no .quorum/rubrics/

    write_file(sandbox / "templates" / "rubrics" / "test" / "rubric.md",
               TEMPLATE_BODY);

    CwdGuard g;
    fs::current_path(sandbox);

    auto r = sui::quorum::resolve_rubric(project.string(), "test");
    check(r.has_value(), "G: resolve returns Rubric (template path)");
    check(r->version == "template-fallback",
          "G: template version is selected");
    check(r->items.size() == 1 && r->items[0].weight == 2,
          "G: template's items are returned");

    cleanup(sandbox);
}

// --- Test H: neither exists -------------------------------------------------

static void test_resolve_none() {
    std::cout << "\n=== H. resolve_rubric: neither override nor template → nullopt ===\n\n";

    auto sandbox = make_tmp_dir("resolve_none");
    auto project = sandbox / "project";
    fs::create_directories(project);

    CwdGuard g;
    fs::current_path(sandbox);

    auto r = sui::quorum::resolve_rubric(project.string(), "nonexistent");
    check(!r.has_value(),
          "H: nullopt when neither override nor template is present");

    cleanup(sandbox);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 8 Track 2 / Track 6 #24 — rubric parser tests\n";
    std::cout << "=====================================================\n";

    test_well_formed();
    test_missing_frontmatter();
    test_malformed_item();
    test_zero_negative_weight();
    test_empty_rubric();
    test_resolve_override_wins();
    test_resolve_template_only();
    test_resolve_none();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}

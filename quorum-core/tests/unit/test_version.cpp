// tests/unit/test_version.cpp
// Unit tests for the `quorum version` / `quorum --version` line formatter
// (utils/version.h). Pure: no git, no generated header, no filesystem.
//
// Run:  ctest --test-dir <build> -R test_version --output-on-failure

#include <cstdlib>
#include <iostream>
#include <string>

#include "utils/version.h"

static int g_passed = 0;
static int g_failed = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static void expect_line(const std::string& got, const std::string& want,
                        const char* msg) {
    if (got != want) {
        std::cerr << "  want: " << want << "\n";
        std::cerr << "  got : " << got << "\n";
    }
    check(got == want, msg);
}

int main() {
    std::cout << "=== Quorum Version Line Tests ===\n\n";

    using sui::quorum::format_version_line;
    const std::string kUtc = "2026-09-04T21:40:12Z";

    // 1. The committed-tree shape.
    expect_line(format_version_line("0.1.0", "dca6e90", false, kUtc),
                "quorum 0.1.0 (dca6e90) built 2026-09-04T21:40:12Z",
                "1: clean tree -> 'quorum 0.1.0 (dca6e90) built <utc>'");

    // 2. Uncommitted work in the tree the binary was built from.
    expect_line(format_version_line("0.1.0", "dca6e90", true, kUtc),
                "quorum 0.1.0 (dca6e90-dirty) built 2026-09-04T21:40:12Z",
                "2: dirty tree -> the sha carries a '-dirty' suffix");

    // 3. Built outside a git checkout: say 'unknown', never an empty field.
    expect_line(format_version_line("0.1.0", "", false, kUtc),
                "quorum 0.1.0 (unknown) built 2026-09-04T21:40:12Z",
                "3: empty sha -> '(unknown)'");

    // 4. Same for a missing stamp time.
    expect_line(format_version_line("0.1.0", "dca6e90", false, ""),
                "quorum 0.1.0 (dca6e90) built unknown",
                "4: empty build stamp -> 'built unknown'");

    // 5. The version is the CMake project VERSION, not a literal.
    expect_line(format_version_line("0.2.0", "dca6e90", false, kUtc),
                "quorum 0.2.0 (dca6e90) built 2026-09-04T21:40:12Z",
                "5: version passes through verbatim");

    // 6. The dirty suffix is independent of the sha fallback.
    expect_line(format_version_line("0.1.0", "", true, kUtc),
                "quorum 0.1.0 (unknown-dirty) built 2026-09-04T21:40:12Z",
                "6: empty sha + dirty -> '(unknown-dirty)'");

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";
    return g_failed > 0 ? 1 : 0;
}

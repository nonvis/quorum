#pragma once

// Portable "what is the absolute path of THIS running executable?" helper.
//
// Why this exists: several CLI subcommands (init, knower refresh, benchmark)
// need to find the quorum *repo root* so they can locate shipped templates /
// scripts (`<repo>/templates/...`, `<repo>/scripts/...`). They derive it from
// the binary's own location: the binary is `<repo>/build/quorum_daemon`, so
// `parent_path().parent_path()` of the canonicalized exe path == `<repo>`.
//
// The old approach used `fs::canonical(argv[0])`, which only works when the
// shell passes a path-bearing argv[0] (e.g. `./build/quorum_daemon` or an
// absolute path). When invoked via the installed PATH symlink (`quorum`),
// argv[0] is the BARE string "quorum" and `fs::canonical("quorum")` resolves
// it relative to the CWD (the *target* project), throws, and the repo-root
// resolution silently fails — so init handed the knowers the generic thinker
// SKILL instead of their lens SKILLs. This helper asks the OS for the real
// path of the running image, which is independent of how argv[0] was spelled,
// and canonicalizes it so the `~/.local/bin/quorum` install symlink is
// followed back to the real `<repo>/build/quorum_daemon`.
//
// PURE, header-only, never throws: returns "" on any failure so callers can
// keep their best-effort/empty-on-failure contract (init must never abort on
// this).
//
// macOS  : _NSGetExecutablePath (<mach-o/dyld.h>)
// Linux  : fs::canonical("/proc/self/exe")
// other  : "" (callers fall back to the argv[0] path)
//
// Matches the utils/subprocess.h / utils/file_io.h header-only convention.

#include <filesystem>
#include <string>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#endif

namespace sui::quorum {

// Absolute, symlink-resolved path of the currently-running executable.
// Returns "" if the platform is unsupported or any step fails. Never throws.
[[nodiscard]] inline std::string self_executable_path() {
    namespace fs = std::filesystem;
    try {
#if defined(__APPLE__)
        // First call with a null buffer to learn the required size, then fill.
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);  // size now holds the needed length
        std::vector<char> buf(size);
        if (_NSGetExecutablePath(buf.data(), &size) != 0) {
            return "";  // buffer too small (shouldn't happen after sizing call)
        }
        // _NSGetExecutablePath may return a path with symlinks / `..`; canonical
        // follows the ~/.local/bin/quorum install symlink to the real binary.
        return fs::canonical(fs::path(buf.data())).string();
#elif defined(__linux__)
        return fs::canonical("/proc/self/exe").string();
#else
        return "";
#endif
    } catch (...) {
        return "";
    }
}

// Derive the quorum repo root from the running executable.
//
// Resolution order:
//   1. self_executable_path() — robust to how argv[0] was spelled (PATH name,
//      relative, absolute); this is the correct path for the installed CLI.
//   2. fall back to fs::canonical(argv0) — preserves prior behavior on odd
//      platforms where self_executable_path() returns "".
//
// In both cases the repo root is `<exe>/../..` because the binary lives at
// `<repo>/build/quorum_daemon`. Returns "" if neither path resolves (never
// throws); callers treat "" as "couldn't locate repo root" and fall back to
// their own discovery ladder.
[[nodiscard]] inline std::string quorum_repo_root_from_exe(const char* argv0) {
    namespace fs = std::filesystem;

    auto self = self_executable_path();
    if (!self.empty()) {
        try {
            return fs::path(self).parent_path().parent_path().string();
        } catch (...) {
            // fall through to argv0 fallback
        }
    }

    if (argv0 != nullptr) {
        try {
            return fs::canonical(fs::path(argv0))
                .parent_path()
                .parent_path()
                .string();
        } catch (...) {
            // fall through to empty
        }
    }

    return "";
}

}  // namespace sui::quorum

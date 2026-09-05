#pragma once

// The one line `quorum version` / `quorum --version` prints.
//
// Pure formatter: the caller supplies the facts. The facts themselves are baked
// into the generated header quorum_version.h at BUILD time (see
// src/version_stamp.cmake) -- the CLI never shells out to git at runtime.

#include <string>
#include <string_view>

namespace sui::quorum {

// "quorum <version> (<sha>[-dirty]) built <utc>"
//
// Empty sha / utc degrade to "unknown" rather than printing an empty field: a
// binary built outside a git checkout still has to be able to say so.
inline std::string format_version_line(std::string_view version,
                                       std::string_view git_sha,
                                       bool dirty,
                                       std::string_view built_utc) {
    const std::string_view sha = git_sha.empty() ? std::string_view("unknown") : git_sha;
    const std::string_view utc = built_utc.empty() ? std::string_view("unknown") : built_utc;

    std::string s = "quorum ";
    s += version;
    s += " (";
    s += sha;
    if (dirty) s += "-dirty";
    s += ") built ";
    s += utc;
    return s;
}

}  // namespace sui::quorum

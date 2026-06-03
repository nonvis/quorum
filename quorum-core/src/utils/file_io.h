#pragma once

// Generic filesystem helpers — read a file fully + atomic write-via-rename.
//
// These were previously detail:: helpers inside vault/scribe_writer.h. Phase 14
// retired the scribe role and deleted scribe_writer.h; the two helpers are NOT
// scribe-specific (supervisor_init.h, ask.h, and the librarian curator all used
// them as plain file I/O), so they live here in utils/ for any caller.
//
// Header-only, matches the utils/subprocess.h / utils/json.h convention.

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace sui::quorum::detail {

// Read a file fully into a string. Returns an empty string if the file is
// absent or unreadable (never throws).
inline std::string read_file_text(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Atomic write: write to .tmp.<pid>, fsync, rename into place. Returns true on
// success; populates `err` with a human-readable message on failure.
inline bool atomic_write_text(const std::filesystem::path& target,
                              const std::string& content,
                              std::string& err) {
    auto tmp = target;
    tmp += ".tmp." + std::to_string(::getpid());

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "atomic_write_text: failed to open temp file: " + tmp.string();
            return false;
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f) {
            err = "atomic_write_text: failed to write temp file: " + tmp.string();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
        f.flush();
    }

    // fsync the temp file so the renamed payload is durable.
    int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd >= 0) {
        (void)::fsync(fd);
        ::close(fd);
    }

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        err = "atomic_write_text: failed to rename temp into place: " +
              ec.message();
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
    }
    return true;
}

}  // namespace sui::quorum::detail

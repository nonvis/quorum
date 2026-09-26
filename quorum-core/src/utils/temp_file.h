#pragma once

// Private temp files for the text Quorum hands to `claude -p` (the task
// prompt, the system prompt, `ask` and `agent create` prompts).
//
// Why this exists: the invoker used to write /tmp/quorum_prompt_<task_id>.txt.
// Task ids restart at 1 in every project's database, so two projects running
// at once wrote the same file and one agent could be sent the other project's
// prompt; the files were world-readable; and a crash left them behind.
//
// TempFile::create() makes <tmpdir>/quorum-<pid>-<tag>-XXXXXX with mkstemp:
// a unique name, mode 0600. The file is removed when the TempFile goes out of
// scope. sweep_stale_temp_files() removes files whose owning pid is gone, so a
// daemon start cleans up after a crashed one without touching live runs.
//
// Header-only, no exceptions: failures come back as an empty path().

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace sui::quorum {

class TempFile {
public:
    TempFile() = default;
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& other) noexcept : path_(std::exchange(other.path_, {})) {}
    TempFile& operator=(TempFile&& other) noexcept {
        if (this != &other) {
            remove();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }
    ~TempFile() { remove(); }

    // A new private file holding `content`; path() is "" on any failure.
    [[nodiscard]] static TempFile create(std::string_view tag, std::string_view content) {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto dir = fs::temp_directory_path(ec);
        if (ec) dir = "/tmp";
        auto tmpl = (dir / ("quorum-" + std::to_string(::getpid()) + "-" +
                            std::string(tag) + "-XXXXXX")).string();
        int fd = ::mkstemp(tmpl.data());
        if (fd < 0) return {};
        TempFile file;
        file.path_ = tmpl;
        const char* p = content.data();
        size_t left = content.size();
        while (left > 0) {
            ssize_t n = ::write(fd, p, left);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ::close(fd);
                return {};  // `file` removes the partial file
            }
            p += n;
            left -= static_cast<size_t>(n);
        }
        if (::close(fd) != 0) return {};
        return file;
    }

    [[nodiscard]] const std::string& path() const { return path_; }

    void remove() {
        if (!path_.empty()) {
            std::remove(path_.c_str());
            path_.clear();
        }
    }

private:
    std::string path_;
};

// Remove temp files left by processes that no longer run:
//   quorum-<pid>-*                          when <pid> is gone
//   quorum_prompt_* / quorum_sysprompt_*    (the old names) older than a day
// Returns how many were removed. Never throws.
inline int sweep_stale_temp_files(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    int removed = 0;
    const auto day_ago = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        bool stale = false;
        if (name.starts_with("quorum-")) {
            auto dash = name.find('-', 7);
            if (dash == std::string::npos) continue;
            const auto pid_text = name.substr(7, dash - 7);
            if (pid_text.empty() ||
                pid_text.find_first_not_of("0123456789") != std::string::npos) continue;
            const auto pid = static_cast<pid_t>(std::strtol(pid_text.c_str(), nullptr, 10));
            if (pid <= 0) continue;
            stale = ::kill(pid, 0) != 0 && errno == ESRCH;
        } else if (name.starts_with("quorum_prompt_") || name.starts_with("quorum_sysprompt_")) {
            std::error_code tec;
            auto mtime = fs::last_write_time(it->path(), tec);
            stale = !tec && mtime < day_ago;
        }
        std::error_code fec;
        if (stale && fs::is_regular_file(it->path(), fec) && fs::remove(it->path(), fec)) {
            ++removed;
        }
    }
    return removed;
}

inline int sweep_stale_temp_files() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    return sweep_stale_temp_files(ec ? std::filesystem::path("/tmp") : dir);
}

}  // namespace sui::quorum

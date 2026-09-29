#include "tech_news/platform.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include "tech_news/text.hpp"

extern char** environ;
namespace tech_news {
std::filesystem::path data_path() {
    auto* home = std::getenv("HOME");
    std::filesystem::path base;
#ifdef __APPLE__
    if (home) base = std::filesystem::path(home) / "Library" / "Application Support";
#else
    auto* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && std::filesystem::path(xdg).is_absolute())
        base = xdg;
    else if (home)
        base = std::filesystem::path(home) / ".local" / "share";
#endif
    if (base.empty() || !base.is_absolute()) throw std::runtime_error("Cannot locate user storage; set HOME to an absolute path");
    const auto directory = base / "tech-news";
    try {
        std::filesystem::create_directories(directory);
        if (std::filesystem::is_symlink(directory)) throw std::runtime_error("Cache directory must not be a symbolic link");
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
    } catch (const std::filesystem::filesystem_error&) {
        throw std::runtime_error("Cannot create private user cache directory (check storage space and permissions)");
    }
    return directory / "news.sqlite3";
}

void open_browser(const std::string& input) {
    auto url = normalize_url(input);
    if (url.empty()) throw std::runtime_error("Cannot open an unsafe URL");
#ifdef __APPLE__
    const char* program = "/usr/bin/open";
#else
    const char* program = "/usr/bin/xdg-open";
#endif
    // posix_spawn never evaluates URL characters as shell commands. Reap asynchronously.
    char* args[] = {const_cast<char*>(program), url.data(), nullptr};
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) throw std::runtime_error("Could not configure browser launcher");
    struct Cleanup {
        posix_spawn_file_actions_t* value;
        ~Cleanup() { posix_spawn_file_actions_destroy(value); }
    } cleanup{&actions};
    if (posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0) != 0)
        throw std::runtime_error("Could not redirect browser launcher output");
    pid_t pid = 0;
    if (posix_spawn(&pid, program, &actions, nullptr, args, environ) != 0)
        throw std::runtime_error("Browser launcher unavailable; install xdg-utils on Linux");
    std::thread([pid] { while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }).detach();
}
}  // namespace tech_news

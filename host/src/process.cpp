#include "process.hpp"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#define MALGRIND_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define MALGRIND_ENVIRON environ
#endif

namespace malgrind::host {

namespace {
// Spawns argv with the given file actions; returns the pid or -1.
pid_t spawn(const std::vector<std::string>& argv, posix_spawn_file_actions_t* actions, char* const* envp = MALGRIND_ENVIRON) {
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    return posix_spawnp(&pid, args[0], actions, nullptr, args.data(), envp) == 0 ? pid : -1;
}

int wait_for(pid_t pid) {
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
} // namespace

std::string capture(const std::vector<std::string>& argv) {
    std::array<int, 2> fds{};
    if (argv.empty() || pipe(fds.data()) != 0) return {};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    const pid_t pid = spawn(argv, &actions);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    std::string out;
    if (pid > 0) {
        std::array<char, 4096> buf;
        for (ssize_t n; (n = read(fds[0], buf.data(), buf.size())) > 0;) out.append(buf.data(), static_cast<std::size_t>(n));
        wait_for(pid);
    }
    close(fds[0]);
    return out;
}

int run(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    const pid_t pid = spawn(argv, nullptr);
    return pid > 0 ? wait_for(pid) : -1;
}

int run_lines(const std::vector<std::string>& argv, const std::vector<std::string>& env_add,
              const std::function<void(std::string_view line)>& on_line) {
    std::array<int, 2> fds{};
    if (argv.empty() || pipe(fds.data()) != 0) return -1;
    // this process's environment, minus the names that env_add sets, plus env_add
    std::vector<std::string> env;
    for (char** e = MALGRIND_ENVIRON; e && *e; ++e) {
        const std::string_view entry = *e;
        const bool replaced = std::ranges::any_of(env_add, [&](const std::string& add) {
            return entry.substr(0, entry.find('=') + 1) == std::string_view(add).substr(0, add.find('=') + 1);
        });
        if (!replaced) env.emplace_back(entry);
    }
    env.insert(env.end(), env_add.begin(), env_add.end());
    std::vector<char*> envp;
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0); // tools must not wait for input
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    const pid_t pid = spawn(argv, &actions, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (pid <= 0) {
        close(fds[0]);
        return -1;
    }
    std::string pending;
    auto flush_lines = [&](bool all) {
        for (std::size_t at; (at = pending.find_first_of("\r\n")) != std::string::npos;) {
            if (at > 0) on_line(std::string_view(pending).substr(0, at));
            pending.erase(0, at + 1);
        }
        if (all && !pending.empty()) on_line(pending), pending.clear();
    };
    std::array<char, 4096> buf;
    for (ssize_t n; (n = read(fds[0], buf.data(), buf.size())) > 0;) {
        pending.append(buf.data(), static_cast<std::size_t>(n));
        flush_lines(false);
    }
    flush_lines(true);
    close(fds[0]);
    return wait_for(pid);
}

} // namespace malgrind::host

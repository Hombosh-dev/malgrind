#pragma once

// Child process spawning and PATH lookup without a shell.
// Windows refuses .bat/.cmd launchers to prevent command-line argument injection.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host::os {

// Manageable child process with merged stdout/stderr line streaming.
class Child {
public:
    Child() noexcept;
    Child(Child&&) noexcept;
    Child& operator=(Child&&) noexcept;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child();

    // Spawns argv with `env_add` ("NAME=value") added to this process's environment.
    // stdout and stderr are merged and delivered line-by-line to `on_line` on an internal reader thread.
    // Returns std::nullopt if the child cannot be started.
    static std::optional<Child> spawn(const std::vector<std::string>& argv, const std::vector<std::string>& env_add,
                                      std::function<void(std::string_view line)> on_line);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool running() const;
    // Terminates the child if still running (POSIX: SIGTERM, wait ~200 ms, then SIGKILL; Windows: TerminateProcess)
    // and joins the line-reader thread.
    void kill();
    // Waits up to `timeout_ms` for the child to exit; returns its exit code, or std::nullopt if still running (or invalid).
    std::optional<int> wait_for(int timeout_ms);
    // Waits for the child to exit and joins the line-reader thread; returns exit code or -1 on error/invalid.
    int wait();

private:
    struct Impl;
    explicit Child(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// stdout of the child process (stderr discarded). Empty if it cannot be started.
std::string capture(const std::vector<std::string>& argv);

// Run with inherited stdio. Returns the exit code, or -1 if the process cannot be started.
int run(const std::vector<std::string>& argv);

// Runs argv with `env_add` ("NAME=value") added to this process's environment. stdout and stderr arrive, in order,
// as lines (a lone \r also ends a line, so progress bars become lines). Returns the exit code, or -1 if it cannot
// be started.
int run_lines(const std::vector<std::string>& argv, const std::vector<std::string>& env_add,
              const std::function<void(std::string_view line)>& on_line);

// First match in PATH. Windows: if `name` has no extension, tries each PATHEXT entry, ignoring .bat/.cmd.
[[nodiscard]] std::optional<std::filesystem::path> find_in_path(const std::string& name);

// Pure, testable on every OS: MSVCRT quoting rules (backslashes before quotes doubled, args with space/tab/quote quoted).
[[nodiscard]] std::wstring windows_command_line(const std::vector<std::string>& argv_utf8);

} // namespace malgrind::host::os

#pragma once

// Steps that both front-ends need: the app's JSON protocol (serve.cpp) and the terminal commands (cli.cpp).

#include "devices.hpp"
#include "github.hpp"
#include "malgrind/target_info.hpp"
#include "os/serial.hpp"
#include "pio.hpp"
#include "source.hpp"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

inline constexpr unsigned kBaud = 921600; // what the agent uses

inline constexpr const char* kNoPio =
    "PlatformIO (pio) was not found: install it with `pip install platformio` (attaching to a running board works without it)";

// The error for a port that could not be opened; Busy becomes "busy: ...".
[[nodiscard]] std::runtime_error open_error(const std::string& port, const os::OpenFailure& failure);

// Opens the port exclusively. Throws open_error.
[[nodiscard]] Source open_source(const std::string& port);

// After an upload the board resets and its USB port can vanish or renumber for a moment: keeps trying for about 5 s.
[[nodiscard]] Source open_after_upload(const std::string& device_ref,
                                       const std::function<std::vector<Device>()>& devices_fn = list_devices);

struct Prepared {
    std::filesystem::path pio;
    ProjectInfo project;
    std::string env, port;
};
// pio, the project and its environment, checked. `env` empty: the project's default. Throws with a reason.
[[nodiscard]] Prepared prepare_project(const std::filesystem::path& home, const std::filesystem::path& dir, std::string env,
                                       std::string port);

// Runs a pio invocation, handing every output line to on_line. Throws if it fails.
void run_pio(const Invocation& invocation, const std::function<void(std::string_view line)>& on_line);

struct ProbeResult {
    bool busy = false;  // somebody else has the port
    bool denied = false; // this user may not open the port (not in its group, Linux)
    bool agent = false; // a malgrind frame arrived
    Arch arch{};
    unsigned ncores = 0;
    double cpu_mhz = 0;
};
// Listens to the port for up to 2 s for a malgrind frame. Never records and never resets the board. Throws open_error.
[[nodiscard]] ProbeResult probe_port(const std::string& port);

using LineFn = std::function<void(std::string_view line)>;

// Shallow-clones the repository into `dest` (which must not exist; git's progress goes to on_line) and returns the PlatformIO
// project inside it: the clone itself or its only firmware/* folder. Never asks for a password (a private repository
// fails at once). On any failure `dest` is removed again and this throws.
[[nodiscard]] std::filesystem::path clone_project(const GithubRepo& repo, const std::filesystem::path& dest, const LineFn& on_line);

// Builds the project with the agent (`build`), backs up what is on the board (see backup.hpp), then flashes (`upload`): nothing
// is flashed unless the backup succeeded. A board that already runs the agent is not backed up again (its flash holds our
// own build). The two commands come from make_plan (plan.hpp) or from agent_command; the agent's sdkconfig must have been
// written before. Progress and the path of the backup arrive through on_line. Throws.
void flash_agent(const Prepared& prepared, const std::filesystem::path& home, const std::filesystem::path& backup_dir,
                 const Invocation& build, const Invocation& upload, const LineFn& on_line);

// Writes the newest backup of the board on `port` back, byte for byte (esptool verifies it). Throws, naming the board,
// if there is no backup of it.
void restore_flash(const std::filesystem::path& pio, const std::filesystem::path& home, const std::string& port,
                   const std::filesystem::path& backup_dir, const LineFn& on_line);

// One way to profile the board that is plugged in, and, when it is not possible, what is missing.
struct Method {
    std::string id; // "attach", "new_run", "no_flash"
    bool available = false;
    std::string why; // empty when available
};
// `project_problem`: why the chosen project cannot be used with the agent (empty: fine, or none chosen yet).
[[nodiscard]] std::vector<Method> methods(const ProbeResult& probe, bool have_pio, const std::string& project_problem);

} // namespace malgrind::host

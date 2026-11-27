#pragma once

// OpenOCD discovery, process lifecycle, and TCL RPC framing/client.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

using LineFn = std::function<void(std::string_view line)>;

struct OpenOcdInstall {
    std::filesystem::path exe;
    std::filesystem::path scripts;
};

// Locates OpenOCD:
//   - When `package` is non-default (not empty and not "tool-openocd"):
//       1. PlatformIO packages under os::platformio_core_dirs(home): packages/<package> (or <package>@*)
//       2. PATH (via os::find_in_path("openocd"))
//       3. POSIX only: home / ".local/bin" / exe_name("openocd")
//   - When `package` is "tool-openocd" (default):
//       1. PATH (via os::find_in_path("openocd"))
//       2. PlatformIO packages under os::platformio_core_dirs(home): packages/tool-openocd, then packages/tool-openocd*
//       3. POSIX only: home / ".local/bin" / exe_name("openocd")
// `scripts` is the first directory under the package (depth <= 3) containing interface/stlink.cfg (or standard
// relative script directories for PATH / local installs).
[[nodiscard]] std::optional<OpenOcdInstall> find_openocd(const std::filesystem::path& home,
                                                         std::string_view package = "tool-openocd");

// Pure framing codec for OpenOCD's TCL RPC server (`\x1a`-terminated requests and replies).
class TclRpcCodec {
public:
    static constexpr char kDelimiter = '\x1a';

    [[nodiscard]] static std::string request(std::string_view cmd);
    [[nodiscard]] std::optional<std::string> feed(std::string_view bytes);
    void clear() noexcept { buf_.clear(); }

private:
    std::string buf_;
};

class OpenOcd {
public:
    OpenOcd(OpenOcd&&) noexcept;
    OpenOcd& operator=(OpenOcd&&) noexcept;
    OpenOcd(const OpenOcd&) = delete;
    OpenOcd& operator=(const OpenOcd&) = delete;
    ~OpenOcd();

    // Starts OpenOCD with `-s <scripts>` (when present), `config_args`, disabled gdb/telnet ports, and an ephemeral
    // loopback `tcl_port`, retrying port selection once on startup race. Throws std::runtime_error with OpenOCD's
    // recent output lines on failure or timeout.
    static OpenOcd start(const OpenOcdInstall& install, const std::vector<std::string>& config_args,
                         const LineFn& log = {}, int startup_timeout_ms = 5000);

    // Connects directly to an already-listening TCL RPC loopback port.
    static OpenOcd connect(std::uint16_t port, int timeout_ms = 1000);

    [[nodiscard]] std::string call(std::string_view tcl, int timeout_ms = 1000);
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::vector<std::string> recent_lines() const;

private:
    struct Impl;
    explicit OpenOcd(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace malgrind::host

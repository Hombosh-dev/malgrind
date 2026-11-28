#include "openocd.hpp"

#include "os/paths.hpp"
#include "os/process.hpp"
#include "os/socket.hpp"
#include "os/tools.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <format>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace malgrind::host {

using namespace std::chrono_literals;

namespace {

constexpr std::size_t kMaxRecentLines = 32;
constexpr int kShutdownWaitMs = 2000;

bool has_stlink_cfg(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "interface/stlink.cfg", ec);
}

bool has_script_subdirs(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir / "interface", ec) && fs::is_directory(dir / "target", ec);
}

fs::path find_scripts_under_package(const fs::path& pkg_dir) {
    std::error_code ec;
    if (!fs::is_directory(pkg_dir, ec)) return {};

    // Breadth-first up to depth 3 so `scripts/` wins over deeper paths, deterministically sorted per level.
    std::vector<fs::path> all_dirs{pkg_dir};
    std::vector<fs::path> level{pkg_dir};
    for (int depth = 0; depth < 3 && !level.empty(); ++depth) {
        std::vector<fs::path> next;
        for (const auto& dir : level) {
            std::vector<fs::path> children;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.is_directory(ec)) children.push_back(entry.path());
            }
            std::ranges::sort(children);
            for (auto& child : children) {
                all_dirs.push_back(child);
                next.push_back(std::move(child));
            }
        }
        level = std::move(next);
    }

    for (const auto& dir : all_dirs) {
        if (has_stlink_cfg(dir)) return dir;
    }
    for (const auto& dir : all_dirs) {
        if (has_script_subdirs(dir)) return dir;
    }
    return {};
}

fs::path find_scripts_near_exe(const fs::path& exe) {
    const fs::path bin_dir = exe.parent_path();
    const fs::path prefix = bin_dir.parent_path();
    std::vector<fs::path> candidates;
    if (!bin_dir.empty()) candidates.push_back(bin_dir / "scripts");
    if (!prefix.empty()) {
        candidates.push_back(prefix / "scripts");
        candidates.push_back(prefix / "openocd/scripts");
        candidates.push_back(prefix / "share/openocd/scripts");
    }
    for (const auto& cand : candidates) {
        if (has_stlink_cfg(cand)) return cand;
    }
    for (const auto& cand : candidates) {
        if (has_script_subdirs(cand)) return cand;
    }
    return {};
}

std::optional<OpenOcdInstall> inspect_package_dir(const fs::path& pkg_dir, const std::string& bin_name) {
    std::error_code ec;
    if (!fs::is_directory(pkg_dir, ec)) return std::nullopt;
    for (const fs::path& exe : {pkg_dir / "bin" / bin_name, pkg_dir / "openocd/bin" / bin_name, pkg_dir / bin_name}) {
        if (fs::is_regular_file(exe, ec)) {
            return OpenOcdInstall{exe, find_scripts_under_package(pkg_dir)};
        }
    }
    return std::nullopt;
}

std::uint16_t allocate_loopback_port() {
    auto listener = os::TcpListener::bind_loopback(0);
    if (!listener) throw std::runtime_error("cannot allocate a local TCP port for OpenOCD");
    const std::uint16_t port = listener->port();
    if (port == 0) throw std::runtime_error("cannot determine ephemeral local TCP port for OpenOCD");
    return port;
}

std::string join_recent_lines(const std::vector<std::string>& lines) {
    if (lines.empty()) return "(no output)";
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out.push_back('\n');
        out.append(lines[i]);
    }
    return out;
}

std::string format_with_logs(std::string_view headline, const std::vector<std::string>& lines) {
    if (lines.empty()) return std::string(headline);
    return std::format("{}:\n{}", headline, join_recent_lines(lines));
}

std::optional<OpenOcdInstall> find_in_pio_packages(const fs::path& home, std::string_view effective_pkg,
                                                   const std::string& bin_name) {
    for (const auto& core : os::platformio_core_dirs(home)) {
        const fs::path packages_dir = core / "packages";
        const fs::path preferred = packages_dir / os::from_utf8(effective_pkg);
        if (auto found = inspect_package_dir(preferred, bin_name)) return found;

        std::error_code ec;
        std::vector<fs::path> matches;
        const std::string versioned_prefix = std::string(effective_pkg) + "@";
        for (const auto& entry : fs::directory_iterator(packages_dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::string name = os::to_utf8(entry.path().filename());
            const bool match = effective_pkg == "tool-openocd"
                                   ? (name.starts_with("tool-openocd") && name != effective_pkg)
                                   : name.starts_with(versioned_prefix);
            if (match) matches.push_back(entry.path());
        }
        std::ranges::sort(matches);
        for (const auto& pkg : matches) {
            if (auto found = inspect_package_dir(pkg, bin_name)) return found;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<OpenOcdInstall> find_openocd(const fs::path& home, std::string_view package) {
    const std::string bin_name = os::exe_name("openocd");
    const std::string_view effective_pkg = package.empty() ? std::string_view("tool-openocd") : package;

    // Non-default packages (vendor forks) must not be shadowed by a generic system openocd on PATH.
    if (effective_pkg != "tool-openocd") {
        if (auto found = find_in_pio_packages(home, effective_pkg, bin_name)) return found;
    }

    if (const auto from_path = os::find_in_path("openocd")) {
        return OpenOcdInstall{*from_path, find_scripts_near_exe(*from_path)};
    }

    if (effective_pkg == "tool-openocd") {
        if (auto found = find_in_pio_packages(home, effective_pkg, bin_name)) return found;
    }

    if constexpr (os::kind() != os::Kind::Windows) {
        if (!home.empty()) {
            const fs::path local = home / ".local/bin" / bin_name;
            std::error_code ec;
            if (fs::is_regular_file(local, ec)) {
                return OpenOcdInstall{local, find_scripts_near_exe(local)};
            }
        }
    }

    return std::nullopt;
}

std::string TclRpcCodec::request(std::string_view cmd) {
    std::string out;
    out.reserve(cmd.size() + 1);
    out.append(cmd);
    out.push_back(kDelimiter);
    return out;
}

std::optional<std::string> TclRpcCodec::feed(std::string_view bytes) {
    buf_.append(bytes);
    const auto pos = buf_.find(kDelimiter);
    if (pos == std::string::npos) return std::nullopt;
    std::string reply = buf_.substr(0, pos);
    buf_.erase(0, pos + 1);
    return reply;
}

struct OpenOcd::Impl {
    os::Child child;
    std::uint16_t port = 0;
    LineFn log;

    mutable std::mutex log_mu;
    std::deque<std::string> lines;

    std::mutex io_mu;
    os::TcpStream stream;
    TclRpcCodec codec;

    ~Impl() { close_gracefully(); }

    void on_line(std::string_view line) {
        {
            std::scoped_lock lock(log_mu);
            if (lines.size() >= kMaxRecentLines) lines.pop_front();
            lines.emplace_back(line);
        }
        if (log) log(line);
    }

    std::vector<std::string> recent_lines() const {
        std::scoped_lock lock(log_mu);
        return {lines.begin(), lines.end()};
    }

    void close_gracefully() noexcept {
        try {
            if (stream.valid()) {
                (void)stream.write_all(TclRpcCodec::request("shutdown"));
            }
        } catch (...) {
        }
        if (child.valid()) {
            if (!child.wait_for(kShutdownWaitMs)) {
                child.kill();
            }
        }
        stream = os::TcpStream{};
    }
};

OpenOcd::OpenOcd(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
OpenOcd::OpenOcd(OpenOcd&&) noexcept = default;
OpenOcd& OpenOcd::operator=(OpenOcd&&) noexcept = default;
OpenOcd::~OpenOcd() = default;

OpenOcd OpenOcd::start(const OpenOcdInstall& install, const std::vector<std::string>& config_args, const LineFn& log,
                       int startup_timeout_ms) {
    if (install.exe.empty()) {
        throw std::runtime_error("cannot start OpenOCD: executable path is empty");
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::uint16_t port = allocate_loopback_port();

        std::vector<std::string> argv;
        argv.push_back(os::to_utf8(install.exe));
        if (!install.scripts.empty()) {
            argv.push_back("-s");
            argv.push_back(os::to_utf8(install.scripts));
        }
        argv.insert(argv.end(), config_args.begin(), config_args.end());
        argv.push_back("-c");
        argv.push_back("gdb_port disabled");
        argv.push_back("-c");
        argv.push_back("telnet_port disabled");
        argv.push_back("-c");
        argv.push_back(std::format("tcl_port {}", port));

        auto impl = std::make_unique<Impl>();
        impl->port = port;
        impl->log = log;

        auto child = os::Child::spawn(argv, {}, [raw = impl.get()](std::string_view line) { raw->on_line(line); });
        if (!child) {
            throw std::runtime_error(std::format("cannot start OpenOCD ({})", os::to_utf8(install.exe)));
        }
        impl->child = std::move(*child);

        using clock = std::chrono::steady_clock;
        const int effective_timeout = startup_timeout_ms > 0 ? startup_timeout_ms : 1;
        const auto deadline = clock::now() + std::chrono::milliseconds(effective_timeout);

        bool early_exit = false;
        for (;;) {
            if (!impl->child.running()) {
                const int exit_code = impl->child.wait();
                if (attempt == 0) {
                    early_exit = true;
                    break;
                }
                throw std::runtime_error(
                    format_with_logs(std::format("OpenOCD exited during startup (code {})", exit_code), impl->recent_lines()));
            }

            const auto now = clock::now();
            if (now >= deadline) break;
            const auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const int slice_ms = static_cast<int>(std::clamp<long long>(left_ms, 1, 100));

            if (auto stream = os::TcpStream::connect_loopback(port, slice_ms)) {
                impl->stream = std::move(*stream);
                return OpenOcd(std::move(impl));
            }

            const auto after_connect = clock::now();
            if (after_connect >= deadline) break;
            std::this_thread::sleep_for(std::min<clock::duration>(20ms, deadline - after_connect));
        }

        if (early_exit) continue;

        impl->child.kill();
        throw std::runtime_error(format_with_logs(
            std::format("timed out waiting {} ms for OpenOCD TCL RPC on 127.0.0.1:{}", effective_timeout, port),
            impl->recent_lines()));
    }

    throw std::runtime_error("cannot start OpenOCD");
}

OpenOcd OpenOcd::connect(std::uint16_t port, int timeout_ms) {
    if (port == 0) {
        throw std::runtime_error("cannot connect to OpenOCD TCL RPC on port 0");
    }
    auto stream = os::TcpStream::connect_loopback(port, timeout_ms);
    if (!stream) {
        throw std::runtime_error(std::format("cannot connect to OpenOCD TCL RPC on 127.0.0.1:{}", port));
    }
    auto impl = std::make_unique<Impl>();
    impl->port = port;
    impl->stream = std::move(*stream);
    return OpenOcd(std::move(impl));
}

std::string OpenOcd::call(std::string_view tcl, int timeout_ms) {
    if (!impl_) throw std::runtime_error("OpenOCD client is not connected");
    if (tcl.find(TclRpcCodec::kDelimiter) != std::string_view::npos) {
        throw std::invalid_argument("OpenOCD TCL command must not contain 0x1a");
    }

    std::scoped_lock lock(impl_->io_mu);
    if (!impl_->stream.valid()) {
        throw std::runtime_error("OpenOCD TCL RPC is not connected");
    }
    if (impl_->child.valid() && !impl_->child.running()) {
        const int exit_code = impl_->child.wait();
        impl_->stream = os::TcpStream{};
        throw std::runtime_error(
            format_with_logs(std::format("OpenOCD is not running (exit code {})", exit_code), impl_->recent_lines()));
    }

    if (!impl_->stream.write_all(TclRpcCodec::request(tcl))) {
        impl_->stream = os::TcpStream{};
        if (impl_->child.valid()) {
            if (const auto exit_code = impl_->child.wait_for(200)) {
                throw std::runtime_error(
                    format_with_logs(std::format("OpenOCD exited (code {})", *exit_code), impl_->recent_lines()));
            }
        }
        throw std::runtime_error(format_with_logs("failed to write OpenOCD TCL command", impl_->recent_lines()));
    }

    using clock = std::chrono::steady_clock;
    const auto deadline = timeout_ms > 0 ? clock::now() + std::chrono::milliseconds(timeout_ms) : clock::time_point{};
    std::array<char, 4096> buf{};

    for (;;) {
        int remaining = 0;
        if (timeout_ms > 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            if (left <= 0) {
                throw std::runtime_error(std::format("OpenOCD TCL command timed out after {} ms", timeout_ms));
            }
            remaining = static_cast<int>(left);
        }

        const auto n = impl_->stream.read(buf, remaining);
        if (!n) {
            impl_->stream = os::TcpStream{};
            if (impl_->child.valid()) {
                if (const auto exit_code = impl_->child.wait_for(200)) {
                    throw std::runtime_error(
                        format_with_logs(std::format("OpenOCD closed connection (exit code {})", *exit_code),
                                         impl_->recent_lines()));
                }
            }
            throw std::runtime_error(format_with_logs("OpenOCD TCL connection closed", impl_->recent_lines()));
        }
        if (*n == 0) {
            throw std::runtime_error(std::format("OpenOCD TCL command timed out after {} ms", timeout_ms));
        }
        if (auto reply = impl_->codec.feed(std::string_view(buf.data(), *n))) {
            return *reply;
        }
    }
}

std::uint16_t OpenOcd::port() const noexcept { return impl_ ? impl_->port : 0; }

bool OpenOcd::running() const { return impl_ && impl_->child.valid() ? impl_->child.running() : false; }

std::vector<std::string> OpenOcd::recent_lines() const {
    return impl_ ? impl_->recent_lines() : std::vector<std::string>{};
}

} // namespace malgrind::host

#include "openocd_source.hpp"

#include "discover.hpp"
#include "malgrind/profiler.hpp"
#include "symbols.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <stdexcept>
#include <utility>

namespace malgrind::host {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t kCoreDebugDemcrAddr = 0xE000EDFCu;
constexpr std::uint32_t kCoreDebugCpuidAddr = 0xE000ED00u;
constexpr std::uint32_t kDwtPcsrAddr = 0xE000101Cu;
constexpr std::uint32_t kDemcrTrcenaBit = 0x01000000u;
constexpr std::uint32_t kSleepRawPc = 0xFFFFFFFFu;
constexpr std::uint16_t kDefaultHaltPollHz = 50;

std::string_view trim_ascii(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool has_unsafe_control_char(std::string_view s) {
    for (unsigned char c : s) {
        if ((c < 0x20 && c != '\t' && c != '\r' && c != '\n') || c == 0x7f) return true;
    }
    return false;
}

bool is_safe_target_name(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    if (name.front() == '-') return false;
    return std::ranges::all_of(name, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-';
    });
}

bool is_hex_digits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::optional<std::uint32_t> parse_hex_u32(std::string_view hex) {
    if (hex.starts_with("0x") || hex.starts_with("0X")) hex.remove_prefix(2);
    if (!is_hex_digits(hex)) return std::nullopt;
    std::uint32_t val = 0;
    const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), val, 16);
    if (ec != std::errc{} || ptr != hex.data() + hex.size()) return std::nullopt;
    return val;
}

std::string build_dwt_batch_cmd(std::uint16_t batch_size) {
    if (batch_size <= 1) return std::format("mrw 0x{:08X}", kDwtPcsrAddr);
    return std::format("set r {{}}; for {{set i 0}} {{$i < {}}} {{incr i}} {{ lappend r [mrw 0x{:08X}] }}; set r",
                       batch_size, kDwtPcsrAddr);
}

bool cpuid_lacks_dwt_pcsr(std::uint32_t cpuid) noexcept {
    // ARM implementer = 0x41; PartNo 0xC20 (M0), 0xC21 (M1), 0xC60 (M0+), 0xD20 (M23) lack full DWT PCSR.
    if (((cpuid >> 24) & 0xFFu) != 0x41u) return false;
    const unsigned part = (cpuid >> 4) & 0xFFFu;
    return part == 0xC20u || part == 0xC21u || part == 0xC60u || part == 0xD20u;
}

bool enable_dwt_trcena(OpenOcd& ocd, int timeout_ms, bool verify_readback) {
    const auto demcr = parse_openocd_u32(ocd.call(std::format("mrw 0x{:08X}", kCoreDebugDemcrAddr), timeout_ms));
    if (!demcr) return false;
    if ((*demcr & kDemcrTrcenaBit) == 0) {
        (void)ocd.call(std::format("mww 0x{:08X} 0x{:08x}", kCoreDebugDemcrAddr, *demcr | kDemcrTrcenaBit), timeout_ms);
        if (verify_readback) {
            const auto after = parse_openocd_u32(ocd.call(std::format("mrw 0x{:08X}", kCoreDebugDemcrAddr), timeout_ms));
            if (!after || (*after & kDemcrTrcenaBit) == 0) return false;
        }
    }
    return true;
}

std::string query_target_type(OpenOcd& ocd, std::string_view target_name, int timeout_ms) {
    if (!is_safe_target_name(target_name)) return {};
    const auto raw = ocd.call(std::format("{} cget -type", target_name), timeout_ms);
    return std::string(trim_ascii(raw));
}

bool is_arm_cortex_m_type(std::string_view type) {
    return type == "cortex_m" || type == "hla_target";
}

bool is_mock_unknown_reply(std::string_view reply) {
    return reply.empty() || reply.starts_with("ok:");
}

void refine_arch_from_target_type(std::string_view type, std::optional<Arch>& arch) {
    if (arch) return;
    if (is_arm_cortex_m_type(type)) {
        arch = Arch::ArmV7M;
    } else if (type == "riscv") {
        arch = Arch::RiscV32;
    }
}

std::uint32_t map_sampled_pc(std::uint32_t raw_pc) noexcept {
    return raw_pc == kSleepRawPc ? kSleepPseudoPc : raw_pc;
}

} // namespace

std::optional<std::uint32_t> parse_openocd_u32(std::string_view token) {
    token = trim_ascii(token);
    while (!token.empty() && token.front() == '{') token.remove_prefix(1);
    while (!token.empty() && token.back() == '}') token.remove_suffix(1);
    token = trim_ascii(token);
    if (token.empty()) return std::nullopt;

    int base = 10;
    if (token.starts_with("0x") || token.starts_with("0X")) {
        token.remove_prefix(2);
        base = 16;
    }
    if (token.empty()) return std::nullopt;
    for (unsigned char c : token) {
        if (base == 16 ? std::isxdigit(c) == 0 : std::isdigit(c) == 0) return std::nullopt;
    }
    std::uint32_t val = 0;
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), val, base);
    if (ec != std::errc{} || ptr != token.data() + token.size()) return std::nullopt;
    return val;
}

std::optional<std::uint32_t> parse_openocd_pc(std::string_view reply) {
    reply = trim_ascii(reply);
    if (reply.empty()) return std::nullopt;
    if (reply.starts_with("Error:") || reply.starts_with("error:")) return std::nullopt;

    const auto colon = reply.find(':');
    const std::string_view search_area = (colon != std::string_view::npos) ? trim_ascii(reply.substr(colon + 1)) : reply;

    // Look for an explicit 0x/0X hex token first (avoids matching bit-width "(32)" before ':').
    for (std::size_t i = 0; i + 2 < search_area.size(); ++i) {
        if (search_area[i] == '0' && (search_area[i + 1] == 'x' || search_area[i + 1] == 'X')) {
            if (i > 0 && !std::isspace(static_cast<unsigned char>(search_area[i - 1])) &&
                search_area[i - 1] != '(' && search_area[i - 1] != '{') {
                continue;
            }
            std::size_t end = i + 2;
            while (end < search_area.size() && std::isxdigit(static_cast<unsigned char>(search_area[end]))) ++end;
            if (end < search_area.size() && !std::isspace(static_cast<unsigned char>(search_area[end])) &&
                search_area[end] != ')' && search_area[end] != '}' && search_area[end] != ',') {
                continue;
            }
            if (const auto parsed = parse_hex_u32(search_area.substr(i, end - i))) return parsed;
        }
    }

    // Fallback when ':' is present or the entire reply is a bare hex literal without 0x.
    if (colon != std::string_view::npos || is_hex_digits(search_area)) {
        std::size_t end = 0;
        while (end < search_area.size() && !std::isspace(static_cast<unsigned char>(search_area[end]))) ++end;
        const std::string_view first_tok = search_area.substr(0, end);
        if (first_tok.size() <= 8) return parse_hex_u32(first_tok);
    }

    return std::nullopt;
}

std::optional<std::vector<std::string>> parse_openocd_target_names(std::string_view reply) {
    if (has_unsafe_control_char(reply)) return std::nullopt;
    std::vector<std::string> names;
    std::size_t i = 0;
    while (i < reply.size()) {
        while (i < reply.size() && std::isspace(static_cast<unsigned char>(reply[i]))) ++i;
        const std::size_t start = i;
        while (i < reply.size() && !std::isspace(static_cast<unsigned char>(reply[i]))) ++i;
        if (i > start) {
            const std::string_view tok = reply.substr(start, i - start);
            if (!is_safe_target_name(tok)) return std::nullopt;
            names.emplace_back(tok);
        }
    }
    return names;
}

std::optional<std::vector<std::uint32_t>> parse_openocd_pcsr_batch(std::string_view reply) {
    std::vector<std::uint32_t> out;
    std::size_t i = 0;
    while (i < reply.size()) {
        while (i < reply.size() && std::isspace(static_cast<unsigned char>(reply[i]))) ++i;
        const std::size_t start = i;
        while (i < reply.size() && !std::isspace(static_cast<unsigned char>(reply[i]))) ++i;
        if (i > start) {
            const auto val = parse_openocd_u32(reply.substr(start, i - start));
            if (!val) return std::nullopt;
            out.push_back(*val);
        }
    }
    if (out.empty()) return std::nullopt;
    return out;
}

OpenOcdSource::OpenOcdSource(OpenOcd ocd, OpenOcdSourceOptions options)
    : options_(std::move(options)), ocd_(std::move(ocd)) {
    if (options_.label.empty()) options_.label = "openocd";
    thread_ = std::thread([this] { sample_loop(); });
}

OpenOcdSource::~OpenOcdSource() { close(); }

std::unique_ptr<OpenOcdSource> OpenOcdSource::start(const OpenOcdInstall& install,
                                                    const std::vector<std::string>& config_args,
                                                    OpenOcdSourceOptions options,
                                                    const LineFn& log,
                                                    int startup_timeout_ms) {
    OpenOcd ocd = OpenOcd::start(install, config_args, log, startup_timeout_ms);
    return std::make_unique<OpenOcdSource>(std::move(ocd), std::move(options));
}

std::unique_ptr<OpenOcdSource> OpenOcdSource::start_with_config(const OpenOcdInstall& install,
                                                                const ProbeConfig& config,
                                                                OpenOcdSourceOptions options,
                                                                const LineFn& log,
                                                                int startup_timeout_ms) {
    if (options.sample_mode == ProbeSampleMode::Auto) options.sample_mode = config.sample_mode;
    if (!options.arch && config.target_arch) options.arch = config.target_arch;
    if ((options.label.empty() || options.label == "openocd") && !config.interface_desc.empty()) {
        options.label = config.target_cfg.empty()
                            ? config.interface_desc
                            : std::format("{} ({})", config.interface_desc, config.target_cfg);
    }
    return start(install, config.openocd_args, std::move(options), log, startup_timeout_ms);
}

void OpenOcdSource::close() {
    std::scoped_lock lifecycle_lock(lifecycle_mu_);
    stop_.store(true);
    stop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    {
        std::scoped_lock lock(fifo_mu_);
        eof_ = true;
    }
    fifo_cv_.notify_all();
    ocd_.reset();
}

std::optional<std::size_t> OpenOcdSource::read(std::span<std::uint8_t> out, int timeout_ms) {
    if (out.empty()) return std::size_t{0};
    std::unique_lock lock(fifo_mu_);
    if (fifo_.empty() && !eof_) {
        const auto wait = std::chrono::milliseconds(std::max(0, timeout_ms));
        fifo_cv_.wait_for(lock, wait, [&] { return !fifo_.empty() || eof_; });
    }
    if (fifo_.empty()) {
        return eof_ ? std::nullopt : std::optional<std::size_t>(0);
    }
    const std::size_t n = std::min(out.size(), fifo_.size());
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = fifo_.front();
        fifo_.pop_front();
    }
    return n;
}

std::string OpenOcdSource::describe() const { return options_.label; }

std::string OpenOcdSource::last_error() const {
    std::scoped_lock lock(fifo_mu_);
    return last_error_;
}

ProbeSampleMode OpenOcdSource::effective_sample_mode() const noexcept {
    return effective_mode_.load();
}

std::optional<Arch> OpenOcdSource::detected_arch() const noexcept {
    const int raw = detected_arch_.load();
    if (raw < 0) return std::nullopt;
    return static_cast<Arch>(raw);
}

unsigned OpenOcdSource::detected_cores() const noexcept {
    return detected_cores_.load();
}

void OpenOcdSource::push_bytes(std::span<const std::uint8_t> data) {
    if (data.empty()) return;
    {
        std::scoped_lock lock(fifo_mu_);
        fifo_.insert(fifo_.end(), data.begin(), data.end());
    }
    fifo_cv_.notify_all();
}

void OpenOcdSource::sample_loop() {
    std::optional<malgrind::Profiler> profiler;
    bool pending_samples = false;

    auto flush_now = [&] {
        if (!profiler) return;
        profiler->flush([this](const std::uint8_t* d, std::size_t n) { push_bytes({d, n}); }, [] {}, 0);
        pending_samples = false;
    };

    try {
        if (!ocd_) throw std::runtime_error("OpenOCD client is not connected");
        const int timeout_ms = options_.rpc_timeout_ms > 0 ? options_.rpc_timeout_ms : 1000;

        std::optional<Arch> arch = options_.arch;
        if (!arch && !options_.elf.empty()) arch = elf_arch(options_.elf);

        std::vector<std::string> targets;
        bool targets_queried = false;
        auto load_targets = [&] {
            if (targets_queried) return;
            targets_queried = true;
            const auto reply = ocd_->call("target names", timeout_ms);
            const auto parsed = parse_openocd_target_names(reply);
            if (!parsed) throw std::runtime_error("OpenOCD returned unsafe target names");
            targets = *parsed;
            if (targets.size() > malgrind::kMaxCores) targets.resize(malgrind::kMaxCores);
        };

        ProbeSampleMode mode = options_.sample_mode;
        if (mode == ProbeSampleMode::DwtPcsr) {
            if (!enable_dwt_trcena(*ocd_, timeout_ms, false)) {
                throw std::runtime_error("failed to enable CoreDebug DEMCR.TRCENA for DWT PCSR sampling");
            }
            if (!arch) arch = Arch::ArmV7M;
        } else if (mode == ProbeSampleMode::HaltResume) {
            load_targets();
            if (!arch && !targets.empty()) {
                refine_arch_from_target_type(query_target_type(*ocd_, targets.front(), timeout_ms), arch);
            }
        } else {
            // Auto mode: avoid touching ARM CoreDebug addresses on architectures that cannot have DWT.
            if (arch && *arch != Arch::ArmV7M) {
                mode = ProbeSampleMode::HaltResume;
                load_targets();
            } else {
                load_targets();
                std::string target_type;
                if (!targets.empty()) {
                    target_type = query_target_type(*ocd_, targets.front(), timeout_ms);
                    refine_arch_from_target_type(target_type, arch);
                }

                bool can_try_dwt = is_mock_unknown_reply(target_type) || is_arm_cortex_m_type(target_type);
                if (can_try_dwt) {
                    const auto cpuid =
                        parse_openocd_u32(ocd_->call(std::format("mrw 0x{:08X}", kCoreDebugCpuidAddr), timeout_ms));
                    if (cpuid && cpuid_lacks_dwt_pcsr(*cpuid)) can_try_dwt = false;
                }
                if (can_try_dwt && enable_dwt_trcena(*ocd_, timeout_ms, true)) {
                    const auto probe_pcsr =
                        parse_openocd_u32(ocd_->call(std::format("mrw 0x{:08X}", kDwtPcsrAddr), timeout_ms));
                    can_try_dwt = probe_pcsr.has_value();
                } else {
                    can_try_dwt = false;
                }

                if (can_try_dwt) {
                    mode = ProbeSampleMode::DwtPcsr;
                    if (!arch) arch = Arch::ArmV7M;
                } else {
                    mode = ProbeSampleMode::HaltResume;
                }
            }
        }

        const Arch final_arch = arch.value_or(Arch::ArmV7M);
        const unsigned ncores =
            (mode == ProbeSampleMode::DwtPcsr || targets.empty()) ? 1u : static_cast<unsigned>(targets.size());
        const std::uint16_t effective_rate_hz =
            options_.poll_hz > 0 ? options_.poll_hz
                                 : (mode == ProbeSampleMode::HaltResume ? kDefaultHaltPollHz : std::uint16_t{0});

        effective_mode_.store(mode);
        detected_arch_.store(static_cast<int>(final_arch));
        detected_cores_.store(ncores);

        const malgrind::HelloPayload hello{
            .seq = 0,
            .arch = final_arch,
            .ncores = static_cast<std::uint8_t>(ncores),
            .cpu_hz = 0,
            .agent_ver = 2,
            .caps = malgrind::kCapSamples,
            .state = 1,
            .rate_hz = effective_rate_hz,
            .analyses = malgrind::kCapSamples,
        };
        std::array<std::uint8_t, 64> hello_buf{};
        const std::size_t hello_n = malgrind::encode_hello(hello_buf.data(), hello_buf.size(), hello);
        if (hello_n > 0) push_bytes({hello_buf.data(), hello_n});

        profiler.emplace(malgrind::TargetInfo{"probe", final_arch, 0, static_cast<std::uint8_t>(ncores)},
                         effective_rate_hz);

        const std::uint16_t batch_size = std::max<std::uint16_t>(1, options_.dwt_batch_size);
        const std::string dwt_cmd = build_dwt_batch_cmd(batch_size);
        const auto flush_interval = options_.flush_interval.count() > 0 ? options_.flush_interval : 250ms;
        const auto tick_period =
            effective_rate_hz > 0
                ? std::chrono::microseconds(
                      std::max<long long>(1000, (1'000'000LL * (mode == ProbeSampleMode::DwtPcsr ? batch_size : 1)) /
                                                    effective_rate_hz))
                : 1ms;

        auto last_flush = Clock::now();
        auto next_tick = Clock::now();

        while (!stop_.load()) {
            if (mode == ProbeSampleMode::DwtPcsr) {
                const auto reply = ocd_->call(dwt_cmd, timeout_ms);
                const auto pcsrs = parse_openocd_pcsr_batch(reply);
                if (!pcsrs) {
                    throw std::runtime_error(std::format("unexpected OpenOCD DWT PCSR reply: {}", trim_ascii(reply)));
                }
                for (const std::uint32_t raw : *pcsrs) {
                    if (raw == 0) continue;
                    const std::uint32_t pc = map_sampled_pc(raw);
                    profiler->record(0, &pc, 1, 0);
                    pending_samples = true;
                }
            } else {
                (void)ocd_->call("halt", timeout_ms);
                for (unsigned c = 0; c < ncores; ++c) {
                    if (!targets.empty()) {
                        (void)ocd_->call(std::format("targets {}", targets[c]), timeout_ms);
                    }
                    const auto reply = ocd_->call("reg pc", timeout_ms);
                    if (const auto raw = parse_openocd_pc(reply); raw && *raw != 0) {
                        const std::uint32_t pc = map_sampled_pc(*raw);
                        profiler->record(c, &pc, 1, 0);
                        pending_samples = true;
                    }
                }
                (void)ocd_->call("resume", timeout_ms);
            }

            const auto now = Clock::now();
            if (now - last_flush >= flush_interval) {
                flush_now();
                last_flush = now;
            }

            if (stop_.load()) break;
            next_tick += tick_period;
            if (next_tick < now) next_tick = now;
            const auto wake_at = std::min(next_tick, last_flush + flush_interval);
            std::unique_lock stop_lock(stop_mu_);
            stop_cv_.wait_until(stop_lock, wake_at, [&] { return stop_.load(); });
        }
    } catch (const std::exception& e) {
        std::scoped_lock lock(fifo_mu_);
        last_error_ = e.what();
    } catch (...) {
        std::scoped_lock lock(fifo_mu_);
        last_error_ = "unknown error while sampling via OpenOCD";
    }

    if (pending_samples) flush_now();
    {
        std::scoped_lock lock(fifo_mu_);
        eof_ = true;
    }
    fifo_cv_.notify_all();
}

} // namespace malgrind::host

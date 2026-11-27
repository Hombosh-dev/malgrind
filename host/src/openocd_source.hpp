#pragma once

// ByteSource backed by an OpenOCD TCL RPC connection: samples target PCs via DWT PCSR (non-intrusive)
// or halt / reg pc / resume and emits wire v2 Hello + flat 1-frame Sample frames into a thread-safe FIFO.

#include "families/family.hpp"
#include "malgrind/target_info.hpp"
#include "openocd.hpp"
#include "probe_config.hpp"
#include "source.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace malgrind::host {

struct OpenOcdSourceOptions {
    ProbeSampleMode sample_mode = ProbeSampleMode::Auto;
    std::optional<Arch> arch{};
    std::filesystem::path elf{};
    std::uint16_t poll_hz = 0; // 0: mode default (50 Hz for halt/resume; continuous batches for DWT PCSR)
    std::uint16_t dwt_batch_size = 64;
    std::chrono::milliseconds flush_interval{250};
    int rpc_timeout_ms = 1000;
    std::string label{"openocd"};
};

// Pure OpenOCD TCL reply parsers (exposed for unit tests and internal reuse).
[[nodiscard]] std::optional<std::uint32_t> parse_openocd_u32(std::string_view token);
[[nodiscard]] std::optional<std::uint32_t> parse_openocd_pc(std::string_view reply);
[[nodiscard]] std::optional<std::vector<std::string>> parse_openocd_target_names(std::string_view reply);
[[nodiscard]] std::optional<std::vector<std::uint32_t>> parse_openocd_pcsr_batch(std::string_view reply);

class OpenOcdSource final : public ByteSource {
public:
    explicit OpenOcdSource(OpenOcd ocd, OpenOcdSourceOptions options = {});
    ~OpenOcdSource() override;

    [[nodiscard]] static std::unique_ptr<OpenOcdSource> start(const OpenOcdInstall& install,
                                                              const std::vector<std::string>& config_args,
                                                              OpenOcdSourceOptions options = {},
                                                              const LineFn& log = {},
                                                              int startup_timeout_ms = 5000);

    [[nodiscard]] static std::unique_ptr<OpenOcdSource> start_with_config(const OpenOcdInstall& install,
                                                                          const ProbeConfig& config,
                                                                          OpenOcdSourceOptions options = {},
                                                                          const LineFn& log = {},
                                                                          int startup_timeout_ms = 5000);

    void close();

    [[nodiscard]] std::optional<std::size_t> read(std::span<std::uint8_t> out, int timeout_ms = 200) override;
    [[nodiscard]] std::string describe() const override;

    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] ProbeSampleMode effective_sample_mode() const noexcept;
    [[nodiscard]] std::optional<Arch> detected_arch() const noexcept;
    [[nodiscard]] unsigned detected_cores() const noexcept;

private:
    void sample_loop();
    void push_bytes(std::span<const std::uint8_t> data);

    OpenOcdSourceOptions options_;
    std::optional<OpenOcd> ocd_;

    mutable std::mutex fifo_mu_;
    std::condition_variable fifo_cv_;
    std::deque<std::uint8_t> fifo_;
    bool eof_ = false;
    std::string last_error_;

    std::mutex stop_mu_;
    std::condition_variable stop_cv_;
    std::atomic<bool> stop_{false};

    std::atomic<ProbeSampleMode> effective_mode_{ProbeSampleMode::Auto};
    std::atomic<int> detected_arch_{-1};
    std::atomic<unsigned> detected_cores_{0};

    std::mutex lifecycle_mu_;
    std::thread thread_;
};

} // namespace malgrind::host

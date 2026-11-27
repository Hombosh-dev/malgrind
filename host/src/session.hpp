#pragma once

// One run as the rest of the program sees it: a stream of bytes from the board (or from a stored run) that is
// decoded, symbolized and aggregated. A live session also records every byte into the run store.

#include "aggregate.hpp"
#include "memcheck.hpp"
#include "series.hpp"
#include "source.hpp"
#include "store.hpp"
#include "symbols.hpp"
#include "wire.hpp"

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace malgrind::host {

// Receives {"event": ...} objects: log (text that is not a frame), target, notice, ended. May be called from
// the session's reader thread, so it must be thread-safe.
using Emit = std::function<void(const nlohmann::json& event)>;

class Session {
public:
    // Reads `source` on a thread of its own until it ends or the session is destroyed. `elf` empty: look for the
    // firmware ELF under `project` when the first frame names the architecture; with no project, names stay raw addresses.
    [[nodiscard]] static std::unique_ptr<Session> live(std::unique_ptr<ByteSource> source, RunWriter writer,
                                                       std::filesystem::path elf, std::filesystem::path project,
                                                       Emit emit, std::uint64_t large_run_bytes = kLargeRunBytes,
                                                       std::filesystem::path home = {});
    // A stored run, fully loaded before this returns. nullptr if there is no such run.
    [[nodiscard]] static std::unique_ptr<Session> replay(const RunStore& store, std::string_view id, Emit emit,
                                                         std::filesystem::path home = {});

    ~Session(); // live: stops the thread and closes the run (writer.finish)
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] bool live() const { return live_; } // false once the board stopped sending, and for replays
    [[nodiscard]] std::string run_id() const { return run_id_; }

    // nullopt until the target is known (first sample frame or HELLO beacon).
    [[nodiscard]] std::optional<std::string> snapshot(std::optional<unsigned> core, Aggregator::Ms window,
                                                      std::optional<Aggregator::Ms> at) const;
    [[nodiscard]] std::optional<std::string> functions(std::optional<unsigned> core, Aggregator::Ms window,
                                                       std::optional<Aggregator::Ms> at, std::string_view focus) const;
    [[nodiscard]] std::string memory_json(Aggregator::Ms window, std::optional<Aggregator::Ms> at = std::nullopt) const;
    [[nodiscard]] std::string tasks_json(Aggregator::Ms window, std::optional<Aggregator::Ms> at = std::nullopt) const;
    [[nodiscard]] std::string memcheck_json(std::optional<Aggregator::Ms> at = std::nullopt,
                                            unsigned leak_age_s = 30) const;

    [[nodiscard]] std::uint32_t caps() const { return caps_.load(); }
    [[nodiscard]] std::uint8_t agent_state() const { return agent_state_.load(); }
    bool agent_start(std::uint16_t rate_hz = 0, std::uint32_t analyses = 0);
    bool agent_stop();

    // Folded stacks ("core0;main;f;g 12" per line), the input of flamegraph.pl and speedscope. nullopt before the first frame.
    [[nodiscard]] std::optional<std::string> folded(std::optional<unsigned> core = std::nullopt,
                                                    Aggregator::Ms window = Aggregator::Ms{0},
                                                    std::optional<Aggregator::Ms> at = std::nullopt) const;

    // Speedscope JSON profile for the run in [at - window, at]. nullopt before the first frame.
    [[nodiscard]] std::optional<std::string> speedscope_json(std::string_view name,
                                                             std::optional<unsigned> core = std::nullopt,
                                                             Aggregator::Ms window = Aggregator::Ms{0},
                                                             std::optional<Aggregator::Ms> at = std::nullopt) const;

    // True once the first frame has arrived and at least one matching sample falls in [at - window, at].
    [[nodiscard]] bool has_samples(std::optional<unsigned> core = std::nullopt,
                                   Aggregator::Ms window = Aggregator::Ms{0},
                                   std::optional<Aggregator::Ms> at = std::nullopt) const;

    // All non-empty symbolized stacks of the run so far, outermost frame first. Empty before the first frame.
    [[nodiscard]] std::vector<NamedStack> named_stacks(std::optional<unsigned> core = std::nullopt) const;

private:
    Session(Emit emit, std::filesystem::path elf, std::filesystem::path project, bool search_elf,
            std::filesystem::path home);
    void on_bytes(std::span<const std::uint8_t> bytes, Aggregator::Ms t); // decode, then aggregate
    void ensure_memcheck();
    void setup_target(Arch arch, std::uint8_t ncores, std::uint32_t cpu_hz, std::uint16_t rate_hz,
                      std::uint32_t caps, std::uint8_t state, std::uint16_t agent_ver);
    bool send_command(const Command& cmd);
    void notice(std::string text) const;
    // Runs fn on the writer. A failing disk (full, run deleted) ends recording with a notice; the live view goes on.
    void record(const std::function<void(RunWriter&)>& fn);
    void read_loop(std::unique_ptr<ByteSource> source);

    Emit emit_;
    std::filesystem::path elf_, project_;
    bool search_elf_;
    std::filesystem::path home_;
    std::string run_id_;
    std::optional<RunWriter> writer_; // only touched by the reader thread (and by replay before it returns)
    bool recording_ = true;
    std::uint64_t large_run_bytes_ = kLargeRunBytes;
    std::uint16_t desired_rate_hz_ = 0;
    FrameReader reader_;
    HeapSeries heap_series_;
    TaskTable task_table_;
    std::optional<Symbolizer> symbols_;
    std::optional<LeakTracker> memcheck_;   // after symbols_: destroyed before symbols_
    std::optional<Aggregator> aggregator_;  // after symbols_: destroyed first
    std::atomic<Aggregator*> ready_{nullptr}; // set once setup_target() is complete; queries from other threads use it
    std::atomic<LeakTracker*> memcheck_ready_{nullptr};
    std::atomic<std::uint32_t> caps_{kCapSamples};
    std::atomic<std::uint32_t> observed_caps_{0};
    std::atomic<std::uint8_t> agent_state_{0};
    std::atomic<std::uint16_t> agent_ver_{0};
    std::atomic<bool> saw_hello_{false};
    std::atomic<bool> auto_started_{false};
    std::atomic<bool> live_{false}, stop_{false};
    std::mutex source_mutex_;
    ByteSource* active_source_ = nullptr; // guarded by source_mutex_; source is owned by read_loop
    std::thread thread_; // last: joined before anything it uses goes away
};

} // namespace malgrind::host

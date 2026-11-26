#include "session.hpp"

#include "discover.hpp"
#include "families/family.hpp"
#include "os/paths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <variant>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string large_run_notice(std::uint64_t threshold_bytes, std::uint64_t current_bytes) {
    if (threshold_bytes >= 1024ull * 1024) {
        return std::format("this run has reached {} MB on disk", current_bytes / (1024ull * 1024));
    }
    if (threshold_bytes >= 1024ull) {
        return std::format("this run has reached {} KB on disk", current_bytes / 1024ull);
    }
    return std::format("this run has reached {} bytes on disk", current_bytes);
}
} // namespace

Session::Session(Emit emit, fs::path elf, fs::path project, bool search_elf)
    : emit_(std::move(emit)), elf_(std::move(elf)), project_(std::move(project)), search_elf_(search_elf) {}

std::unique_ptr<Session> Session::live(std::unique_ptr<ByteSource> source, RunWriter writer, fs::path elf,
                                       fs::path project, Emit emit, std::uint64_t large_run_bytes) {
    std::unique_ptr<Session> s(new Session(std::move(emit), std::move(elf), std::move(project), true));
    s->run_id_ = writer.meta().id;
    s->desired_rate_hz_ = writer.meta().settings
        ? static_cast<std::uint16_t>(std::min(writer.meta().settings->rate_hz, 65535u))
        : std::uint16_t{0};
    s->writer_.emplace(std::move(writer));
    s->large_run_bytes_ = large_run_bytes;
    s->active_source_ = source.get();
    s->live_ = true;
    s->thread_ = std::thread([raw = s.get(), src = std::move(source)]() mutable { raw->read_loop(std::move(src)); });
    return s;
}

std::unique_ptr<Session> Session::replay(const RunStore& store, std::string_view id, Emit emit) {
    const auto meta = store.get(id);
    if (!meta) return nullptr;
    std::unique_ptr<Session> s(new Session(std::move(emit), store.elf(id), {}, false));
    s->run_id_ = meta->id;
    for (const auto& chunk : store.chunks(id)) s->on_bytes(chunk.bytes, Aggregator::Ms{chunk.t_ms});
    return s;
}

Session::~Session() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void Session::notice(std::string text) const { emit_({{"event", "notice"}, {"text", std::move(text)}}); }

void Session::record(const std::function<void(RunWriter&)>& fn) {
    if (!writer_ || !recording_) return;
    try {
        fn(*writer_);
    } catch (const std::exception& e) {
        recording_ = false;
        notice(std::format("cannot record this run any more: {}", e.what()));
    }
}

void Session::read_loop(std::unique_ptr<ByteSource> source) {
    std::unique_ptr<ByteSource> owned = std::move(source);
    struct SourceClear {
        Session& session;
        ~SourceClear() {
            std::scoped_lock lock(session.source_mutex_);
            session.active_source_ = nullptr;
            session.live_ = false;
        }
    } clear_guard{*this};
    {
        std::scoped_lock lock(source_mutex_);
        active_source_ = owned.get();
    }

    const auto start = Clock::now();
    auto elapsed = [&] { return std::chrono::duration_cast<Aggregator::Ms>(Clock::now() - start); };
    std::array<std::uint8_t, 4096> buf;
    bool hinted = false;
    bool warned_large = false;
    bool ended = false;
    while (!stop_) {
        const auto n = owned ? owned->read(buf) : std::nullopt;
        if (!n) {
            ended = true;
            break;
        }
        if (*n == 0) {
            if (!ready_ && !hinted && elapsed() > 5s) {
                hinted = true;
                notice("no malgrind frames yet: is the agent running on the board, and is the baud rate right?");
            }
            continue;
        }
        const auto t = elapsed();
        const std::span bytes(buf.data(), *n);
        record([&](RunWriter& w) { w.append(static_cast<std::uint32_t>(t.count()), bytes); }); // on disk before it is decoded
        on_bytes(bytes, t);
        if (!warned_large && recording_ && large_run_bytes_ > 0 && writer_ && writer_->bytes() >= large_run_bytes_) {
            warned_large = true;
            notice(large_run_notice(large_run_bytes_, writer_->bytes()));
        }
    }
    record([&](RunWriter& w) { w.finish(static_cast<std::uint64_t>(elapsed().count())); });
    {
        std::scoped_lock lock(source_mutex_);
        active_source_ = nullptr;
        live_ = false;
    }
    if (ended) emit_({{"event", "ended"}, {"run", run_id_}, {"reason", "eof"}});
}

void Session::on_bytes(std::span<const std::uint8_t> bytes, Aggregator::Ms t) {
    auto res = reader_.feed(bytes);
    if (!res.text.empty()) emit_({{"event", "log"}, {"text", std::move(res.text)}});
    for (auto& msg : res.messages) {
        std::visit(
            [&](auto& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, Hello>) {
                    saw_hello_ = true;
                    const std::uint32_t merged_caps = m.caps | observed_caps_.load();
                    caps_.store(merged_caps);
                    agent_state_.store(m.state);
                    agent_ver_.store(m.agent_ver);
                    if (!ready_) {
                        setup_target(m.arch, m.ncores, m.cpu_hz, m.rate_hz, merged_caps, m.state, m.agent_ver);
                    } else if (auto* agg = ready_.load()) {
                        agg->set_target(m.arch, m.ncores, m.cpu_hz, m.rate_hz);
                        agg->set_agent_status(merged_caps, m.state);
                    }
                    if (live_ && m.state == 0 && !auto_started_.exchange(true)) {
                        (void)send_command(
                            Command{kCmdStart, desired_rate_hz_, m.caps & (kCapSamples | kCapHeap | kCapTasks)});
                    }
                } else if constexpr (std::is_same_v<T, HeapStats>) {
                    observed_caps_.fetch_or(kCapHeap);
                    const std::uint32_t updated_caps = caps_.fetch_or(kCapHeap) | kCapHeap;
                    heap_series_.add(std::move(m));
                    if (auto* agg = ready_.load()) agg->set_agent_status(updated_caps, agent_state_.load());
                } else if constexpr (std::is_same_v<T, TaskStats>) {
                    observed_caps_.fetch_or(kCapTasks);
                    const std::uint32_t updated_caps = caps_.fetch_or(kCapTasks) | kCapTasks;
                    task_table_.add(std::move(m));
                    if (auto* agg = ready_.load()) agg->set_agent_status(updated_caps, agent_state_.load());
                }
            },
            msg);
    }
    for (auto& frame : res.frames) {
        observed_caps_.fetch_or(kCapSamples);
        const std::uint32_t updated_caps = caps_.fetch_or(kCapSamples) | kCapSamples;
        if (!saw_hello_) agent_state_.store(1);
        if (!ready_) {
            setup_target(frame.arch, frame.ncores, frame.cpu_hz, frame.rate_hz, updated_caps, agent_state_.load(),
                         agent_ver_.load());
        } else if (auto* agg = ready_.load()) {
            agg->set_agent_status(updated_caps, agent_state_.load());
        }
        ready_.load()->add(std::move(frame), t);
    }
}

void Session::setup_target(Arch arch, std::uint8_t ncores, std::uint32_t cpu_hz, std::uint16_t rate_hz,
                           std::uint32_t caps, std::uint8_t state, std::uint16_t agent_ver) {
    fs::path elf = elf_;
    // Only inside a project the caller named: the working directory of a GUI app is arbitrary, and an ELF of another
    // firmware would label every address with a wrong function.
    if (elf.empty() && search_elf_ && !project_.empty()) {
        if (const auto found = find_elf(arch, project_, project_)) elf = *found;
    }
    if (elf.empty()) {
        notice(std::format("no {} firmware ELF found: functions show as raw addresses", arch_name(arch)));
    }
    const auto tool = find_addr2line(arch);
    if (!elf.empty() && tool.empty())
        notice(std::format("no addr2line found for {}: functions show as raw addresses", arch_name(arch)));
    std::vector<ExtraElf> extras;
    if (const Family* family = family_for_arch(arch)) {
        extras = family->extra_symbol_files(os::home_dir(), arch, elf);
    }
    symbols_.emplace(elf, tool, std::move(extras));
    aggregator_.emplace(*symbols_);
    aggregator_->set_target(arch, ncores, cpu_hz, rate_hz);
    aggregator_->set_agent_status(caps, state);
    record([&](RunWriter& w) {
        w.set_target(arch, ncores, cpu_hz / 1e6);
        if (!elf.empty() && search_elf_) w.set_elf(elf);
    });
    emit_({{"event", "target"},
           {"arch", std::string(arch_name(arch))},
           {"ncores", ncores},
           {"cpu_mhz", cpu_hz / 1e6},
           {"elf", elf.empty() ? nlohmann::json(nullptr) : nlohmann::json(os::to_utf8(elf))},
           {"caps", caps},
           {"state", state},
           {"agent_ver", agent_ver}});
    ready_ = &*aggregator_; // last: queries from other threads only see a complete setup
}

bool Session::send_command(const Command& cmd) {
    std::scoped_lock lock(source_mutex_);
    if (!live_ || stop_ || !active_source_) return false;
    const auto bytes = encode_command(cmd);
    return active_source_->write(bytes);
}

bool Session::agent_start(std::uint16_t rate_hz, std::uint32_t analyses) {
    if (!send_command(Command{kCmdStart, rate_hz, analyses})) return false;
    auto_started_ = true;
    return true;
}

bool Session::agent_stop() {
    return send_command(Command{kCmdStop, 0, 0});
}

std::string Session::memory_json(Aggregator::Ms window, std::optional<Aggregator::Ms> at) const {
    return heap_series_.json(window, at);
}

std::string Session::tasks_json(Aggregator::Ms window, std::optional<Aggregator::Ms> at) const {
    return task_table_.json(window, at);
}

std::optional<std::string> Session::snapshot(std::optional<unsigned> core, Aggregator::Ms window,
                                             std::optional<Aggregator::Ms> at) const {
    const auto* agg = ready_.load();
    return agg ? std::optional(agg->snapshot_json(core, window, at)) : std::nullopt;
}

std::optional<std::string> Session::functions(std::optional<unsigned> core, Aggregator::Ms window,
                                              std::optional<Aggregator::Ms> at, std::string_view focus) const {
    const auto* agg = ready_.load();
    return agg ? std::optional(agg->functions_json(core, window, at, focus)) : std::nullopt;
}

std::optional<std::string> Session::folded(std::optional<unsigned> core, Aggregator::Ms window,
                                           std::optional<Aggregator::Ms> at) const {
    const auto* agg = ready_.load();
    return agg ? std::optional(agg->folded(core, window, at)) : std::nullopt;
}

std::optional<std::string> Session::speedscope_json(std::string_view name, std::optional<unsigned> core,
                                                    Aggregator::Ms window, std::optional<Aggregator::Ms> at) const {
    const auto* agg = ready_.load();
    return agg ? std::optional(agg->speedscope_json(name, core, window, at)) : std::nullopt;
}

bool Session::has_samples(std::optional<unsigned> core, Aggregator::Ms window,
                          std::optional<Aggregator::Ms> at) const {
    const auto* agg = ready_.load();
    return agg && agg->has_samples(core, window, at);
}

std::vector<NamedStack> Session::named_stacks(std::optional<unsigned> core) const {
    const auto* agg = ready_.load();
    return agg ? agg->named_stacks(core, Aggregator::Ms{0}, std::nullopt) : std::vector<NamedStack>{};
}

} // namespace malgrind::host

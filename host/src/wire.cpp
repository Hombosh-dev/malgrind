#include "wire.hpp"

#include "malgrind/profiler.hpp" // crc32

#include <algorithm>
#include <array>
#include <optional>

namespace malgrind::host {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'M', 'L', 'C', '1'};
constexpr std::size_t kMinBody = 2;
constexpr std::size_t kMaxBody = 16 * 1024;

// Bounds-checked little-endian reader; any overrun clears `ok`.
struct Cursor {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    bool ok = true;

    [[nodiscard]] std::size_t remaining() const noexcept { return data.size() - pos; }
    [[nodiscard]] bool at_end() const noexcept { return pos == data.size(); }

    std::uint32_t take(unsigned bytes) noexcept {
        if (bytes > remaining()) return ok = false, 0;
        std::uint32_t v = 0;
        for (unsigned i = bytes; i-- > 0;) v = (v << 8) | data[pos + i];
        pos += bytes;
        return v;
    }

    std::string take_name(std::size_t width = kTaskNameLen) {
        if (width > remaining()) {
            ok = false;
            return {};
        }
        const auto span = data.subspan(pos, width);
        pos += width;
        const auto nul = std::find(span.begin(), span.end(), std::uint8_t{0});
        return std::string(reinterpret_cast<const char*>(span.data()),
                           static_cast<std::size_t>(nul - span.begin()));
    }
};

bool read_pcs(Cursor& c, std::uint32_t depth, std::vector<std::uint32_t>& out) {
    if (!c.ok || c.remaining() < std::size_t{depth} * 4u) return false;
    out.reserve(depth);
    for (std::uint32_t i = 0; i < depth; ++i) out.push_back(c.take(4));
    return c.ok;
}

std::optional<Frame> decode_samples_payload(Cursor& c) {
    Frame f;
    f.seq = static_cast<std::uint16_t>(c.take(2));
    f.arch = static_cast<Arch>(c.take(1));
    f.ncores = static_cast<std::uint8_t>(c.take(1));
    f.core = static_cast<std::uint8_t>(c.take(1));
    f.cpu_hz = c.take(4);
    f.rate_hz = static_cast<std::uint16_t>(c.take(2));
    f.samples = static_cast<std::uint16_t>(c.take(2));
    f.dropped = static_cast<std::uint16_t>(c.take(2));
    f.isr_avg = c.take(4);
    f.isr_max = c.take(4);
    f.flush_cyc = c.take(4);
    const auto n_entries = c.take(2);
    if (!c.ok || c.remaining() < std::size_t{n_entries} * 3u) return std::nullopt;
    f.stacks.reserve(n_entries);
    for (std::uint32_t i = 0; i < n_entries; ++i) {
        Stack s{static_cast<std::uint16_t>(c.take(2)), {}};
        const auto depth = c.take(1);
        if (!read_pcs(c, depth, s.pcs)) return std::nullopt;
        f.stacks.push_back(std::move(s));
    }
    if (!c.ok || !c.at_end()) return std::nullopt;
    return f;
}

std::optional<Hello> decode_hello(Cursor& c) {
    Hello h;
    h.seq = static_cast<std::uint16_t>(c.take(2));
    h.arch = static_cast<Arch>(c.take(1));
    h.ncores = static_cast<std::uint8_t>(c.take(1));
    h.cpu_hz = c.take(4);
    h.agent_ver = static_cast<std::uint16_t>(c.take(2));
    h.caps = c.take(4);
    h.state = static_cast<std::uint8_t>(c.take(1));
    h.rate_hz = static_cast<std::uint16_t>(c.take(2));
    h.analyses = c.take(4);
    if (!c.ok || !c.at_end()) return std::nullopt;
    return h;
}

std::optional<HeapStats> decode_heap(Cursor& c) {
    HeapStats h;
    h.seq = static_cast<std::uint16_t>(c.take(2));
    h.t_ms = c.take(4);
    const auto n = c.take(1);
    if (!c.ok || c.remaining() < std::size_t{n} * 17u) return std::nullopt;
    h.regions.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        HeapRegionInfo r;
        r.region = static_cast<std::uint8_t>(c.take(1));
        r.total = c.take(4);
        r.free = c.take(4);
        r.min_free = c.take(4);
        r.largest = c.take(4);
        h.regions.push_back(r);
    }
    if (!c.ok || !c.at_end()) return std::nullopt;
    return h;
}

std::optional<TaskStats> decode_tasks(Cursor& c) {
    TaskStats ts;
    ts.seq = static_cast<std::uint16_t>(c.take(2));
    ts.t_ms = c.take(4);
    ts.total_runtime = c.take(4);
    const auto n = c.take(1);
    if (!c.ok || c.remaining() < std::size_t{n} * 31u) return std::nullopt;
    ts.tasks.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        TaskInfo t;
        t.handle = c.take(4);
        t.name = c.take_name();
        t.prio = static_cast<std::uint8_t>(c.take(1));
        t.core = static_cast<std::uint8_t>(c.take(1));
        t.state = static_cast<std::uint8_t>(c.take(1));
        t.runtime = c.take(4);
        t.stack_hwm = c.take(4);
        ts.tasks.push_back(std::move(t));
    }
    if (!c.ok || !c.at_end()) return std::nullopt;
    return ts;
}

std::optional<AllocBatch> decode_allocs(Cursor& c) {
    AllocBatch batch;
    batch.seq = static_cast<std::uint16_t>(c.take(2));
    const auto n = c.take(2);
    if (!c.ok || c.remaining() < std::size_t{n} * 14u) return std::nullopt;
    batch.records.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        AllocRecord rec;
        rec.op = static_cast<std::uint8_t>(c.take(1));
        rec.t_ms = c.take(4);
        rec.addr = c.take(4);
        rec.size = c.take(4);
        const auto depth = c.take(1);
        if (!read_pcs(c, depth, rec.pcs)) return std::nullopt;
        batch.records.push_back(std::move(rec));
    }
    if (!c.ok || !c.at_end()) return std::nullopt;
    return batch;
}

std::optional<AgentError> decode_error(Cursor& c) {
    AgentError err;
    err.seq = static_cast<std::uint16_t>(c.take(2));
    err.t_ms = c.take(4);
    err.code = static_cast<std::uint8_t>(c.take(1));
    err.arg = c.take(4);
    err.name = c.take_name();
    const auto depth = c.take(1);
    if (!read_pcs(c, depth, err.pcs)) return std::nullopt;
    if (!c.ok || !c.at_end()) return std::nullopt;
    return err;
}

enum class DecodeOutcome { Ok, UnknownKind, Invalid };

template <class T>
DecodeOutcome push_message(std::optional<T> msg, FrameReader::Result& out) {
    if (!msg) return DecodeOutcome::Invalid;
    out.messages.push_back(std::move(*msg));
    return DecodeOutcome::Ok;
}

DecodeOutcome dispatch_body(std::span<const std::uint8_t> body, FrameReader::Result& out) {
    Cursor c{body};
    const auto ver = c.take(1);
    if (!c.ok) return DecodeOutcome::Invalid;
    if (ver == 1) {
        auto frame = decode_samples_payload(c);
        if (!frame) return DecodeOutcome::Invalid;
        out.frames.push_back(std::move(*frame));
        return DecodeOutcome::Ok;
    }
    if (ver != kWireVersion) return DecodeOutcome::Invalid;
    const auto kind = c.take(1);
    if (!c.ok) return DecodeOutcome::Invalid;
    switch (kind) {
        case kKindSamples: {
            auto frame = decode_samples_payload(c);
            if (!frame) return DecodeOutcome::Invalid;
            out.frames.push_back(std::move(*frame));
            return DecodeOutcome::Ok;
        }
        case kKindHello:  return push_message(decode_hello(c), out);
        case kKindHeap:   return push_message(decode_heap(c), out);
        case kKindTasks:  return push_message(decode_tasks(c), out);
        case kKindAllocs: return push_message(decode_allocs(c), out);
        case kKindError:  return push_message(decode_error(c), out);
        default:          return DecodeOutcome::UnknownKind;
    }
}

} // namespace

std::string_view arch_name(Arch arch) noexcept {
    switch (arch) {
        case Arch::Host: return "host";
        case Arch::Xtensa: return "xtensa";
        case Arch::ArmV7M: return "armv7m";
        case Arch::RiscV32: return "riscv32";
    }
    return "unknown";
}

FrameReader::Result FrameReader::feed(std::span<const std::uint8_t> data) {
    buf_.insert(buf_.end(), data.begin(), data.end());
    Result result;
    std::size_t pos = 0;
    auto rest = [&] { return std::span<const std::uint8_t>(buf_).subspan(pos); };
    auto to_text = [&](std::size_t n) {
        result.text.append(reinterpret_cast<const char*>(buf_.data() + pos), n);
        pos += n;
    };

    for (;;) {
        auto r = rest();
        const auto magic = std::search(r.begin(), r.end(), kMagic.begin(), kMagic.end());
        if (magic == r.end()) {
            // keep the longest tail that could still become a magic
            std::size_t keep = 0;
            for (std::size_t k = std::min<std::size_t>(3, r.size()); k > 0 && !keep; --k)
                if (std::equal(r.end() - k, r.end(), kMagic.begin())) keep = k;
            to_text(r.size() - keep);
            break;
        }
        to_text(static_cast<std::size_t>(magic - r.begin()));
        r = rest();
        if (r.size() < 6) break;
        const std::size_t len = r[4] | (r[5] << 8);
        if (len < kMinBody || len > kMaxBody) { to_text(1); continue; } // false magic
        const std::size_t total = 6 + len + 4;
        if (r.size() < total) break;
        const std::uint32_t crc = r[6 + len] | (r[7 + len] << 8) | (r[8 + len] << 16) | (std::uint32_t{r[9 + len]} << 24);
        if (malgrind::crc32(r.data() + 4, len + 2) != crc) { to_text(1); continue; } // corrupted: resync
        switch (dispatch_body(r.subspan(6, len), result)) {
            case DecodeOutcome::Ok:
            case DecodeOutcome::UnknownKind:
                pos += total;
                break;
            case DecodeOutcome::Invalid:
                to_text(1);
                break;
        }
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
    return result;
}

} // namespace malgrind::host

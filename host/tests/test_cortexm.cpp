// Host unit tests for the shared Cortex-M port (`platforms/cortexm/`).
#include "malgrind/platform/cortexm.hpp"

#include "malgrind/profiler.hpp"
#include "wire.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <queue>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct MockHarness {
    std::vector<std::uint8_t> tx;
    std::queue<std::uint8_t> rx;
    std::vector<std::uint32_t> cycle_seq;
    std::size_t cycle_idx = 0;
    std::uint32_t fallback_cycles = 0;
    std::uint32_t now_ms = 0;
    std::vector<std::pair<bool, std::uint16_t>> state_changes;

    void clear_io() {
        tx.clear();
        while (!rx.empty()) rx.pop();
        state_changes.clear();
    }

    void push_rx(const std::vector<std::uint8_t>& bytes) {
        for (std::uint8_t b : bytes) rx.push(b);
    }

    std::uint32_t next_cycle() {
        if (cycle_idx < cycle_seq.size()) {
            return cycle_seq[cycle_idx++];
        }
        return fallback_cycles;
    }
};

void mock_write(void* ctx, const std::uint8_t* data, std::size_t len) {
    auto* h = static_cast<MockHarness*>(ctx);
    h->tx.insert(h->tx.end(), data, data + len);
}

std::size_t mock_read(void* ctx, std::uint8_t* dst, std::size_t cap) {
    auto* h = static_cast<MockHarness*>(ctx);
    std::size_t n = 0;
    while (n < cap && !h->rx.empty()) {
        dst[n++] = h->rx.front();
        h->rx.pop();
    }
    return n;
}

std::uint32_t mock_cycles(void* ctx) {
    return static_cast<MockHarness*>(ctx)->next_cycle();
}

std::uint32_t mock_millis(void* ctx) {
    return static_cast<MockHarness*>(ctx)->now_ms;
}

void mock_on_state_change(void* ctx, bool sampling, std::uint16_t rate_hz) {
    static_cast<MockHarness*>(ctx)->state_changes.emplace_back(sampling, rate_hz);
}

ExceptionFrame make_frame(std::uint32_t pc, std::uint32_t lr) {
    return ExceptionFrame{
        .r0 = 0,
        .r1 = 0,
        .r2 = 0,
        .r3 = 0,
        .r12 = 0,
        .lr = lr,
        .pc = pc,
        .xpsr = 0x01000000u,
    };
}

malgrind::host::FrameReader::Result drain_tx(MockHarness& h) {
    malgrind::host::FrameReader reader;
    auto res = reader.feed(h.tx);
    h.tx.clear();
    return res;
}

void test_uninitialized_and_config_validation() {
    MockHarness h;
    const ExceptionFrame f = make_frame(0x08001001u, 0x08002001u);

    // Must be safe no-ops before init.
    malgrind_cortexm_sample_isr(0, &f);
    malgrind_cortexm_poll();
    assert(h.tx.empty());

    // Invalid configs must return false.
    CortexMConfig bad_no_write{
        .target_name = "stm32",
        .cpu_hz = 168'000'000,
        .rate_hz = 997,
        .start_sampling = true,
        .num_cores = 1,
        .has_dwt_cyccnt = true,
        .ctx = &h,
        .write_bytes = nullptr,
        .read_bytes = mock_read,
        .cycle_counter = mock_cycles,
        .millis = mock_millis,
        .on_state_change = mock_on_state_change,
    };
    assert(!malgrind_cortexm_init(bad_no_write));

    CortexMConfig bad_no_millis = bad_no_write;
    bad_no_millis.write_bytes = mock_write;
    bad_no_millis.millis = nullptr;
    assert(!malgrind_cortexm_init(bad_no_millis));

    CortexMConfig bad_zero_cores = bad_no_write;
    bad_zero_cores.write_bytes = mock_write;
    bad_zero_cores.num_cores = 0;
    assert(!malgrind_cortexm_init(bad_zero_cores));

    CortexMConfig bad_too_many_cores = bad_no_write;
    bad_too_many_cores.write_bytes = mock_write;
    bad_too_many_cores.num_cores = static_cast<std::uint8_t>(malgrind::kMaxCores + 1u);
    assert(!malgrind_cortexm_init(bad_too_many_cores));
}

void test_depth2_and_exc_return_filtering() {
    MockHarness h;
    const CortexMConfig cfg{
        .target_name = "stm32f4",
        .cpu_hz = 168'000'000,
        .rate_hz = 997,
        .start_sampling = true,
        .num_cores = 1,
        .has_dwt_cyccnt = true,
        .ctx = &h,
        .write_bytes = mock_write,
        .read_bytes = mock_read,
        .cycle_counter = mock_cycles,
        .millis = mock_millis,
        .on_state_change = nullptr,
    };
    assert(malgrind_cortexm_init(cfg));

    // Null frame and invalid core_id are ignored.
    malgrind_cortexm_sample_isr(0, nullptr);
    const ExceptionFrame normal = make_frame(0x08001235u, 0x08001011u);
    malgrind_cortexm_sample_isr(1, &normal);

    // Valid leaf + LR with Thumb bits set -> masked with ~1u.
    malgrind_cortexm_sample_isr(0, &normal);

    // Zero LR and ARM EXC_RETURN values -> depth 1 only.
    const std::uint32_t filtered_lrs[] = {
        0u,
        0xFFFFFFF1u,
        0xFFFFFFF9u,
        0xFFFFFFFDu,
        0xFFFFFFE1u,
        0xFFFFFFE9u,
        0xFFFFFFEDu,
    };
    for (std::uint32_t lr : filtered_lrs) {
        const ExceptionFrame f = make_frame(0x08002003u, lr);
        malgrind_cortexm_sample_isr(0, &f);
    }

    h.now_ms = 250;
    malgrind_cortexm_poll();

    const auto res = drain_tx(h);
    assert(res.text.empty());
    assert(res.messages.empty());
    assert(res.frames.size() == 1);

    const auto& frame = res.frames[0];
    assert(frame.arch == malgrind::Arch::ArmV7M);
    assert(frame.ncores == 1 && frame.core == 0);
    assert(frame.cpu_hz == 168'000'000);
    assert(frame.rate_hz == 997);
    assert(frame.samples == 8 && frame.dropped == 0);
    assert(frame.stacks.size() == 2);

    bool saw_depth2 = false;
    bool saw_depth1 = false;
    for (const auto& st : frame.stacks) {
        if (st.pcs.size() == 2) {
            saw_depth2 = true;
            assert(st.count == 1);
            assert(st.pcs == (std::vector<std::uint32_t>{0x08001234u, 0x08001010u}));
        } else if (st.pcs.size() == 1) {
            saw_depth1 = true;
            assert(st.count == 7);
            assert(st.pcs == (std::vector<std::uint32_t>{0x08002002u}));
        } else {
            assert(false);
        }
    }
    assert(saw_depth2 && saw_depth1);
}

void test_cycle_accounting_dwt_and_1us_timer() {
    // 1. DWT CYCCNT wraparound + flush overhead accounting excluding write_bytes wait time.
    {
        MockHarness h;
        h.cycle_seq = {
            // First ISR sample: 0xFFFFFFF0 -> 0x00000010 => 32 cycles.
            0xFFFFFFF0u, 0x00000010u,
            // First flush: start=0x100, tw=0x110, tw_end=0x160 (wait=80), end=0x1A0 (total=160 -> cpu=80).
            0x00000100u, 0x00000110u, 0x00000160u, 0x000001A0u,
            // Second ISR sample: 0x200 -> 0x240 => 64 cycles.
            0x00000200u, 0x00000240u,
        };
        h.fallback_cycles = 0x00000300u;

        const CortexMConfig cfg{
            .target_name = "stm32f4",
            .cpu_hz = 168'000'000,
            .rate_hz = 997,
            .start_sampling = true,
            .num_cores = 1,
            .has_dwt_cyccnt = true,
            .ctx = &h,
            .write_bytes = mock_write,
            .read_bytes = nullptr,
            .cycle_counter = mock_cycles,
            .millis = mock_millis,
            .on_state_change = nullptr,
        };
        assert(malgrind_cortexm_init(cfg));

        const ExceptionFrame f = make_frame(0x08003001u, 0x08003101u);
        malgrind_cortexm_sample_isr(0, &f);
        h.now_ms = 250;
        malgrind_cortexm_poll();

        auto res1 = drain_tx(h);
        assert(res1.frames.size() == 1);
        assert(res1.frames[0].isr_avg == 32u);
        assert(res1.frames[0].isr_max == 32u);
        assert(res1.frames[0].flush_cyc == 0u);

        malgrind_cortexm_sample_isr(0, &f);
        h.now_ms = 500;
        malgrind_cortexm_poll();

        auto res2 = drain_tx(h);
        assert(res2.frames.size() == 1);
        assert(res2.frames[0].isr_avg == 64u);
        assert(res2.frames[0].isr_max == 64u);
        assert(res2.frames[0].flush_cyc == 80u);
    }

    // 2. Cortex-M0/M0+ (!has_dwt_cyccnt): 1 us hardware timer scaling + 32-bit wraparound.
    {
        MockHarness h;
        h.cycle_seq = {
            // Sample 1: 1000 us -> 1020 us => 20 us * 125_000_000 / 1_000_000 = 2500 cycles.
            1000u, 1020u,
            // Sample 2 (wraps 32-bit counter): 0xFFFFFFF6 -> 0x0000000A => 20 us = 2500 cycles.
            0xFFFFFFF6u, 0x0000000Au,
        };
        h.fallback_cycles = 5000u;

        const CortexMConfig cfg{
            .target_name = "rp2040",
            .cpu_hz = 125'000'000,
            .rate_hz = 997,
            .start_sampling = true,
            .num_cores = 1,
            .has_dwt_cyccnt = false,
            .ctx = &h,
            .write_bytes = mock_write,
            .read_bytes = nullptr,
            .cycle_counter = mock_cycles,
            .millis = mock_millis,
            .on_state_change = nullptr,
        };
        assert(malgrind_cortexm_init(cfg));

        const ExceptionFrame f = make_frame(0x20000101u, 0x10000501u);
        malgrind_cortexm_sample_isr(0, &f);
        malgrind_cortexm_sample_isr(0, &f);

        h.now_ms = 250;
        malgrind_cortexm_poll();

        const auto res = drain_tx(h);
        assert(res.frames.size() == 1);
        assert(res.frames[0].samples == 2);
        assert(res.frames[0].isr_avg == 2500u);
        assert(res.frames[0].isr_max == 2500u);
    }
}

void test_sleeping_boot_commands_and_stop() {
    MockHarness h;
    const CortexMConfig cfg{
        .target_name = "stm32g4",
        .cpu_hz = 170'000'000,
        .rate_hz = 997,
        .start_sampling = false,
        .num_cores = 1,
        .has_dwt_cyccnt = true,
        .ctx = &h,
        .write_bytes = mock_write,
        .read_bytes = mock_read,
        .cycle_counter = mock_cycles,
        .millis = mock_millis,
        .on_state_change = mock_on_state_change,
    };
    assert(malgrind_cortexm_init(cfg));

    // Sleeping: ISR samples must be ignored.
    const ExceptionFrame f = make_frame(0x08004001u, 0x08004021u);
    malgrind_cortexm_sample_isr(0, &f);

    // First poll at t=0 emits sleeping HELLO beacon.
    malgrind_cortexm_poll();
    {
        const auto res = drain_tx(h);
        assert(res.frames.empty());
        assert(res.messages.size() == 1);
        const auto& hello = std::get<malgrind::host::Hello>(res.messages[0]);
        assert(hello.seq == 0);
        assert(hello.arch == malgrind::Arch::ArmV7M);
        assert(hello.ncores == 1);
        assert(hello.cpu_hz == 170'000'000);
        assert(hello.agent_ver == 2);
        assert(hello.caps == malgrind::kCapSamples);
        assert(hello.state == 0);
        assert(hello.rate_hz == 997);
        assert(hello.analyses == 0);
    }

    // 500 ms later: no periodic HELLO yet.
    h.now_ms = 500;
    malgrind_cortexm_poll();
    assert(h.tx.empty());

    // 1000 ms later: second sleeping HELLO beacon.
    h.now_ms = 1000;
    malgrind_cortexm_poll();
    {
        const auto res = drain_tx(h);
        assert(res.frames.empty());
        assert(res.messages.size() == 1);
        const auto& hello = std::get<malgrind::host::Hello>(res.messages[0]);
        assert(hello.seq == 1 && hello.state == 0);
    }

    // Send wire v2 START with updated rate_hz = 500.
    h.now_ms = 1050;
    h.push_rx(malgrind::host::encode_command(malgrind::Command{malgrind::kCmdStart, 500, malgrind::kCapSamples}));
    malgrind_cortexm_poll();
    assert(h.state_changes.size() == 1);
    assert(h.state_changes[0] == (std::pair<bool, std::uint16_t>{true, 500}));
    {
        const auto res = drain_tx(h);
        assert(res.frames.empty());
        assert(res.messages.size() == 1);
        const auto& hello = std::get<malgrind::host::Hello>(res.messages[0]);
        assert(hello.state == 1 && hello.rate_hz == 500 && hello.analyses == malgrind::kCapSamples);
    }

    // Record depth-2 sample; verify it does not flush before 250 ms, then flushes at 250 ms.
    malgrind_cortexm_sample_isr(0, &f);
    h.now_ms = 1299;
    malgrind_cortexm_poll();
    assert(h.tx.empty());

    h.now_ms = 1300;
    malgrind_cortexm_poll();
    {
        const auto res = drain_tx(h);
        assert(res.messages.empty());
        assert(res.frames.size() == 1);
        assert(res.frames[0].arch == malgrind::Arch::ArmV7M);
        assert(res.frames[0].rate_hz == 500);
        assert(res.frames[0].samples == 1);
        assert(res.frames[0].stacks.size() == 1);
        assert(res.frames[0].stacks[0].pcs == (std::vector<std::uint32_t>{0x08004000u, 0x08004020u}));
    }

    // Test START rate clamping [10..5000] and default on rate_hz == 0.
    h.state_changes.clear();
    h.now_ms = 1310;
    h.push_rx(malgrind::host::encode_command(malgrind::Command{malgrind::kCmdStart, 2, malgrind::kCapSamples}));
    malgrind_cortexm_poll();
    h.push_rx(malgrind::host::encode_command(malgrind::Command{malgrind::kCmdStart, 10000, malgrind::kCapSamples}));
    malgrind_cortexm_poll();
    h.push_rx(malgrind::host::encode_command(malgrind::Command{malgrind::kCmdStart, 0, malgrind::kCapSamples}));
    malgrind_cortexm_poll();
    assert(h.state_changes.size() == 3);
    assert(h.state_changes[0] == (std::pair<bool, std::uint16_t>{true, 10}));
    assert(h.state_changes[1] == (std::pair<bool, std::uint16_t>{true, 5000}));
    assert(h.state_changes[2] == (std::pair<bool, std::uint16_t>{true, 997}));
    h.tx.clear();

    // Send HELLO command while active.
    h.push_rx(malgrind::host::encode_command(malgrind::Command{malgrind::kCmdHello, 0, 0}));
    malgrind_cortexm_poll();
    {
        const auto res = drain_tx(h);
        assert(res.messages.size() == 1);
        assert(std::get<malgrind::host::Hello>(res.messages[0]).state == 1);
    }

    // Send STOP command -> stops recording, notifies callback, and resumes sleeping HELLO beacons.
    h.state_changes.clear();
    h.now_ms = 1400;
    h.push_rx(malgrind::host::encode_command(malgrind::Command{malgrind::kCmdStop, 0, 0}));
    malgrind_cortexm_poll();
    assert(h.state_changes.size() == 1);
    assert(h.state_changes[0] == (std::pair<bool, std::uint16_t>{false, 997}));
    {
        const auto res = drain_tx(h);
        assert(res.frames.empty());
        assert(res.messages.size() == 1);
        const auto& hello = std::get<malgrind::host::Hello>(res.messages[0]);
        assert(hello.state == 0 && hello.analyses == 0);
    }

    // Samples while stopped must not be recorded; after 1000 ms, periodic sleeping HELLO resumes.
    malgrind_cortexm_sample_isr(0, &f);
    h.now_ms = 2400;
    malgrind_cortexm_poll();
    {
        const auto res = drain_tx(h);
        assert(res.frames.empty());
        assert(res.messages.size() == 1);
        assert(std::get<malgrind::host::Hello>(res.messages[0]).state == 0);
    }
}

void test_active_boot_dual_core_and_millis_wraparound() {
    MockHarness h;
    h.now_ms = 0xFFFFFF80u;
    const CortexMConfig cfg{
        .target_name = "rp2040",
        .cpu_hz = 133'000'000,
        .rate_hz = 400,
        .start_sampling = true,
        .num_cores = 2,
        .has_dwt_cyccnt = false,
        .ctx = &h,
        .write_bytes = mock_write,
        .read_bytes = nullptr,
        .cycle_counter = mock_cycles,
        .millis = mock_millis,
        .on_state_change = nullptr,
    };
    assert(malgrind_cortexm_init(cfg));

    const ExceptionFrame f0 = make_frame(0x10001001u, 0x10002001u);
    const ExceptionFrame f1 = make_frame(0x10003001u, 0xFFFFFFFDu);
    malgrind_cortexm_sample_isr(0, &f0);
    malgrind_cortexm_sample_isr(1, &f1);

    // Advance 250 ms across 32-bit millis wraparound.
    h.now_ms = static_cast<std::uint32_t>(0xFFFFFF80u + 250u);
    malgrind_cortexm_poll();

    const auto res = drain_tx(h);
    assert(res.messages.empty());
    assert(res.frames.size() == 2);
    assert(res.frames[0].arch == malgrind::Arch::ArmV7M);
    assert(res.frames[0].ncores == 2 && res.frames[0].core == 0);
    assert(res.frames[0].rate_hz == 400 && res.frames[0].samples == 1);
    assert(res.frames[0].stacks.size() == 1);
    assert(res.frames[0].stacks[0].pcs == (std::vector<std::uint32_t>{0x10001000u, 0x10002000u}));

    assert(res.frames[1].arch == malgrind::Arch::ArmV7M);
    assert(res.frames[1].ncores == 2 && res.frames[1].core == 1);
    assert(res.frames[1].rate_hz == 400 && res.frames[1].samples == 1);
    assert(res.frames[1].stacks.size() == 1);
    assert(res.frames[1].stacks[0].pcs == (std::vector<std::uint32_t>{0x10003000u}));
}

} // namespace

int main() {
    test_uninitialized_and_config_validation();
    test_depth2_and_exc_return_filtering();
    test_cycle_accounting_dwt_and_1us_timer();
    test_sleeping_boot_commands_and_stop();
    test_active_boot_dual_core_and_millis_wraparound();
    std::puts("cortexm ok");
}

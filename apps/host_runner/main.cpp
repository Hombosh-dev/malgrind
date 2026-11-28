// Demo workload that profiles itself and streams malgrind frames to stdout:
//   ./host_runner [seconds] [phase_seconds] | malgrind attach --port - --elf ./host_runner
// (the app starts it itself: "Demo on this computer")
//
// Three stages take turns, one per phase (15 s by default), so the flamegraph visibly moves while it runs:
//   filter    process_signal -> fir_filter      -> hot_loop
//   compress  compress       -> huffman_encode  -> hot_loop
//   radio     radio_task     -> handle_packet   -> hot_loop
// In a phase the active stage gets 70 ms of every 100 ms cycle, the other two 10 ms each, and 10 ms are idle.
#include "malgrind/platform/host.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>

using malgrind::platform::HostPort;

static constexpr std::uint16_t kRateHz = 997;

static double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

[[gnu::noinline]] static void hot_loop(double ms, volatile unsigned* sink) {
    // check the clock only now and then: clock_gettime lives in the vDSO, outside the ELF
    for (const double end = now_ms() + ms; now_ms() < end;)
        for (int i = 0; i < 20000; ++i) *sink += 1;
}

// One call chain per stage, so each stage is its own tower in the flamegraph.
[[gnu::noinline]] static void fir_filter(double ms, volatile unsigned* sink) { hot_loop(ms, sink); }
[[gnu::noinline]] static void process_signal(double ms, volatile unsigned* sink) { fir_filter(ms, sink); }
[[gnu::noinline]] static void huffman_encode(double ms, volatile unsigned* sink) { hot_loop(ms, sink); }
[[gnu::noinline]] static void compress(double ms, volatile unsigned* sink) { huffman_encode(ms, sink); }
[[gnu::noinline]] static void handle_packet(double ms, volatile unsigned* sink) { hot_loop(ms, sink); }
[[gnu::noinline]] static void radio_task(double ms, volatile unsigned* sink) { handle_packet(ms, sink); }

[[gnu::noinline]] static void idle() {
    timespec ts{0, 10'000'000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {} // the sampling signal interrupts sleeps
}

int main(int argc, char** argv) {
    const double run_ms = (argc > 1 ? std::atof(argv[1]) : 1e9) * 1e3;
    const double phase_ms = (argc > 2 && std::atof(argv[2]) > 0 ? std::atof(argv[2]) : 15.0) * 1e3;
    static malgrind::Profiler profiler(HostPort::info(), kRateHz);
    HostPort::start(profiler, kRateHz);

    volatile unsigned sink = 0;
    std::uint32_t flush_cycles = 0;
    const double t_start = now_ms(), t_end = t_start + run_ms;
    double next_flush = t_start + 250;
    while (now_ms() < t_end) {
        const int phase = static_cast<int>((now_ms() - t_start) / phase_ms) % 3;
        const double filter = phase == 0 ? 70 : 10, compression = phase == 1 ? 70 : 10, radio = phase == 2 ? 70 : 10;
        process_signal(filter, &sink);
        compress(compression, &sink);
        radio_task(radio, &sink);
        idle();
        if (now_ms() >= next_flush) {
            next_flush += 250;
            const std::uint32_t t0 = HostPort::cycles();
            profiler.flush(
                [](const std::uint8_t* d, std::size_t n) { std::fwrite(d, 1, n, stdout); std::fflush(stdout); },
                [] {}, // single thread: the sampler cannot run while we are here
                flush_cycles);
            flush_cycles = HostPort::cycles() - t0;
        }
    }
}

// Demo workload that profiles itself and streams malgrind frames to stdout:
//   ./host_runner [seconds] | malgrind attach --port - --elf ./host_runner
// Expected shares: busy_a 60%, busy_b 30%, idle 10%.
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
[[gnu::noinline]] static void busy_a(volatile unsigned* sink) { hot_loop(60, sink); }
[[gnu::noinline]] static void busy_b(volatile unsigned* sink) { hot_loop(30, sink); }
[[gnu::noinline]] static void idle() {
    timespec ts{0, 10'000'000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {} // the sampling signal interrupts sleeps
}

int main(int argc, char** argv) {
    const double run_ms = (argc > 1 ? std::atof(argv[1]) : 1e9) * 1e3;
    static malgrind::Profiler profiler(HostPort::info(), kRateHz);
    HostPort::start(profiler, kRateHz);

    volatile unsigned sink = 0;
    std::uint32_t flush_cycles = 0;
    const double t_end = now_ms() + run_ms;
    double next_flush = now_ms() + 250;
    while (now_ms() < t_end) {
        busy_a(&sink);
        busy_b(&sink);
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

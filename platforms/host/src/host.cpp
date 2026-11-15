#include "mulcan/platform/host.hpp"

#include <csignal>
#include <ctime>
#include <execinfo.h>
#include <sys/time.h>

namespace mulcan::platform {

namespace {
Profiler* g_profiler;

void on_sample(int) {
    const std::uint32_t t0 = HostPort::cycles();
    void* raw[kMaxDepth + 2];
    const int n = backtrace(raw, kMaxDepth + 2);
    // raw[0] = this handler, raw[1] = signal trampoline, raw[2..] = interrupted code and its callers
    std::uint32_t pcs[kMaxDepth];
    unsigned m = 0;
    for (int i = 2; i < n && m < kMaxDepth; ++i) pcs[m++] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(raw[i]));
    g_profiler->record(0, pcs, m, HostPort::cycles() - t0);
}
} // namespace

TargetInfo HostPort::info() noexcept {
    return {"Linux host", Arch::Host, 1'000'000'000, 1}; // cpu_hz = 1 GHz because cycles() is in ns
}

std::uint32_t HostPort::cycles() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint32_t>(ts.tv_sec * 1'000'000'000ull + ts.tv_nsec);
}

void HostPort::start(Profiler& profiler, std::uint16_t rate_hz) noexcept {
    g_profiler = &profiler;
    void* warm[4];
    backtrace(warm, 4); // first call loads libgcc; must not happen inside the handler
    struct sigaction sa{};
    sa.sa_handler = on_sample;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, nullptr);
    const long us = 1'000'000 / rate_hz;
    const itimerval it{{0, us}, {0, us}};
    setitimer(ITIMER_REAL, &it, nullptr);
}

} // namespace mulcan::platform

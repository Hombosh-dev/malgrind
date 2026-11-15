// Host tool self-check: frame decoding/resync and the aggregation tree.
#include "aggregate.hpp"
#include "discover.hpp"
#include "source.hpp"
#include "wire.hpp"

#include "malgrind/profiler.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;

// n frames of a 2-core target, produced by the real encoder in core/
static std::vector<std::uint8_t> make_stream(unsigned flushes) {
    Profiler p({"test", Arch::Xtensa, 240'000'000, 2}, 997);
    const std::uint32_t leaf_a[] = {0x400d1000, 0x400d2000, 0x400d3000}; // a <- b <- main
    const std::uint32_t leaf_c[] = {0x400d1800, 0x400d2000, 0x400d3000};
    std::vector<std::uint8_t> stream;
    for (unsigned i = 0; i < flushes; ++i) {
        for (int k = 0; k < 3; ++k) p.record(0, leaf_a, 3, 100);
        p.record(1, leaf_c, 3, 100);
        p.flush([&](const std::uint8_t* d, std::size_t n) { stream.insert(stream.end(), d, d + n); }, [] {}, 5000);
    }
    return stream;
}

int main() {
    const auto stream = make_stream(3); // 6 frames: core 0 and core 1 per flush

    // whole stream, and the same stream cut into odd-sized pieces
    auto whole = FrameReader().feed(stream);
    assert(whole.frames.size() == 6 && whole.text.empty());
    assert(whole.frames[0].arch == Arch::Xtensa && whole.frames[0].ncores == 2 && whole.frames[1].core == 1);
    assert(whole.frames[0].stacks.size() == 1 && whole.frames[0].stacks[0].count == 3);
    assert(whole.frames[0].stacks[0].pcs == (std::vector<std::uint32_t>{0x400d1000, 0x400d2000, 0x400d3000}));
    assert(whole.frames[0].flush_cyc == 5000 && whole.frames[0].isr_avg == 100);
    FrameReader chunked;
    std::size_t frames = 0;
    for (std::size_t i = 0; i < stream.size(); i += 7)
        frames += chunked.feed(std::span(stream).subspan(i, std::min<std::size_t>(7, stream.size() - i))).frames.size();
    assert(frames == 6);

    // log text passes through; one corrupted frame is lost, the rest still decode
    const std::string log = "I (123) app: hello\n";
    std::vector<std::uint8_t> damaged(log.begin(), log.end());
    damaged.insert(damaged.end(), stream.begin(), stream.end());
    damaged[log.size() + 20] ^= 0xFF;
    auto resynced = FrameReader().feed(damaged);
    assert(resynced.frames.size() == 5 && resynced.text.starts_with(log));

    // aggregation: no ELF, so names are the raw (call-adjusted) addresses
    Symbolizer symbols({}, {});
    Aggregator agg(symbols);
    const auto t0 = Aggregator::Clock::now();
    agg.add(whole.frames[0], t0 - 30s);  // core 0, old
    agg.add(whole.frames[1], t0 - 30s);  // core 1, old
    agg.add(whole.frames[2], t0 - 1s);   // core 0, recent
    auto contains = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };

    const auto recent = agg.snapshot_json(std::nullopt, 10s, t0);
    assert(contains(recent, R"("name":"all","value":3)"));               // only the recent frame
    const auto all_time = agg.snapshot_json(std::nullopt, 0s, t0);
    assert(contains(all_time, R"("name":"all","value":7)"));             // 3 + 1 + 3
    const auto core1 = agg.snapshot_json(1u, 0s, t0);
    assert(contains(core1, R"("name":"all","value":1)"));
    // main (0x400d3000-1) is the root of every stack and carries all of them
    assert(contains(all_time, R"("name":"0x400d2fff","value":7)"));

    const auto folded = agg.folded();
    assert(contains(folded, "core0;0x400d2fff;0x400d1fff;0x400d1000 6\n"));
    assert(contains(folded, "core1;0x400d2fff;0x400d1fff;0x400d17ff") == false); // a core-1 leaf is not call-adjusted twice

    // serial port discovery: only USB serial adapters count, result is sorted
    const auto dev = std::filesystem::temp_directory_path() / "malgrind_test_dev";
    std::filesystem::remove_all(dev);
    std::filesystem::create_directories(dev);
    for (const char* name : {"ttyUSB1", "ttyACM0", "ttyS0", "tty0", "null", "cu.usbserial-0001", "cu.Bluetooth-Incoming-Port",
                             "tty.usbserial-0001"})
        std::ofstream(dev / name);
    const auto ports = find_serial_ports(dev); // Linux and macOS (cu.*usb*, no Bluetooth, no tty.*) names
    assert(ports.size() == 3 && ports[0] == (dev / "cu.usbserial-0001").string() && ports[1] == (dev / "ttyACM0").string() &&
           ports[2] == (dev / "ttyUSB1").string());
    assert(find_serial_ports(dev / "missing").empty());
    std::filesystem::remove_all(dev);

    // project and ELF discovery in a throw-away tree
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "malgrind_test_tree";
    fs::remove_all(root);
    auto fake_elf = [](const fs::path& file, unsigned char machine) {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        const unsigned char header[20] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, machine, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof header);
    };
    std::string why;
    fs::create_directories(root / "firmware/a");
    assert(!find_project(root, why) && why.find("no PlatformIO project") != std::string::npos);
    std::ofstream(root / "firmware/a/platformio.ini");
    assert(find_project(root, why) == root / "firmware/a");
    fs::create_directories(root / "firmware/b");
    std::ofstream(root / "firmware/b/platformio.ini");
    assert(!find_project(root, why) && why.find("several") != std::string::npos);

    fake_elf(root / "firmware/a/.pio/build/esp32dev/firmware.elf", 94);   // Xtensa
    fake_elf(root / "firmware/b/.pio/build/stm/firmware.elf", 40);        // ARM, newer
    fake_elf(root / "build/app.elf", 94);                                  // Xtensa, newest
    fs::last_write_time(root / "build/app.elf", fs::file_time_type::clock::now() + 10s);
    assert(elf_machine(root / "firmware/a/.pio/build/esp32dev/firmware.elf") == 94);
    assert(find_elf(Arch::Xtensa, root) == root / "build/app.elf");         // newest of the matching ones
    assert(find_elf(Arch::ArmV7M, root) == root / "firmware/b/.pio/build/stm/firmware.elf");
    assert(!find_elf(Arch::RiscV32, root));                                  // nothing built for it
    assert(!elf_machine(root / "firmware/a/platformio.ini"));                // not an ELF
    fs::remove_all(root);

    std::puts("host ok");
}

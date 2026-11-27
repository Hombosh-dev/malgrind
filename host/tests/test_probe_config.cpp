// Unit tests for chip-agnostic OpenOCD probe configuration resolution.
#include "families/esp32.hpp"
#include "families/rp2040.hpp"
#include "families/stm32.hpp"
#include "probe_config.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

Device make_probe(std::string vid, std::string pid, std::string serial = {}, std::string name = {},
                  std::string product = {}, std::string family_hint = {}) {
    Device d;
    d.id = "probe-" + vid + "-" + pid;
    d.kind = DeviceKind::Probe;
    d.name = std::move(name);
    d.title = d.name;
    d.family_hint = std::move(family_hint);
    d.usb = os::UsbInfo{
        .vid = std::move(vid),
        .pid = std::move(pid),
        .product = std::move(product),
        .manufacturer = {},
        .serial = std::move(serial),
        .location = "1-2",
    };
    return d;
}

fs::path fresh_dir(const char* name) {
    const auto dir = fs::canonical(fs::temp_directory_path()) / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

void validation_and_error_cases() {
    Device serial_dev;
    serial_dev.kind = DeviceKind::Serial;
    serial_dev.path = "/dev/ttyUSB0";
    serial_dev.usb = os::UsbInfo{"10c4", "ea60", "CP2102", "Silicon Labs", "0001", "1-1"};
    auto res = resolve_probe_config(serial_dev, nullptr, nullptr, {}, "target/stm32f4x.cfg");
    assert(!res.config.has_value() && contains(res.error, "not a debug probe"));

    Device no_usb;
    no_usb.kind = DeviceKind::Probe;
    res = resolve_probe_config(no_usb, nullptr, nullptr, {}, "target/stm32f4x.cfg");
    assert(!res.config.has_value() && contains(res.error, "USB"));

    Device incomplete_usb = make_probe("", "374b");
    res = resolve_probe_config(incomplete_usb, nullptr, nullptr, {}, "target/stm32f4x.cfg");
    assert(!res.config.has_value() && contains(res.error, "incomplete"));

    // Unsupported VID:PID
    res = resolve_probe_config(make_probe("1234", "abcd"), nullptr, nullptr, {}, "target/stm32f4x.cfg");
    assert(!res.config.has_value() && contains(res.error, "1234:abcd"));

    // Missing target config when neither project nor override provides one
    res = resolve_probe_config(make_probe("0483", "374b", "ST001", "ST-Link"));
    assert(!res.config.has_value() && contains(res.error, "target"));

    // Generic FTDI FT2232/FT232H without family override refuses to guess JTAG pins
    for (const char* pid : {"6010", "6014"}) {
        res = resolve_probe_config(make_probe("0403", pid), nullptr, nullptr, {}, "target/stm32f4x.cfg");
        assert(!res.config.has_value() && contains(res.error, "FTDI") && contains(res.error, pid));
    }

    // Override sanitization: rejects control chars, \x1a, blank, leading '-'
    const Device stlink = make_probe("0483", "374b", "", "ST-Link");
    for (const std::string_view bad : {std::string_view{"target/a.cfg\nshutdown"},
                                       std::string_view{"target/a.cfg\r"},
                                       std::string_view{"target/a.cfg\x1a"},
                                       std::string_view{"target/a.cfg\0x", 14},
                                       std::string_view{"   "},
                                       std::string_view{"-c shutdown"}}) {
        res = resolve_probe_config(stlink, nullptr, nullptr, {}, bad);
        assert(!res.config.has_value() && contains(res.error, "invalid target config"));
    }

    // Invalid project environment returns a clear error
    ProjectInfo proj{"/tmp/p", {"a", "b"}, "a", {}, {}, {}, {}};
    res = resolve_probe_config(stlink, nullptr, &proj, "missing", "target/stm32f4x.cfg");
    assert(!res.config.has_value() && contains(res.error, "no environment 'missing'"));
}

void generic_probe_table_cases() {
    // ST-Link across all supported PIDs, case-insensitive VID/PID, with serial selection
    for (const char* pid : {"3744", "3748", "374b", "374D", "374e", "374f", "3752", "3753"}) {
        const auto res = resolve_probe_config(make_probe("0483", pid, "066EFF3238"), nullptr, nullptr, {},
                                              " target/stm32f4x.cfg ");
        assert(res.config.has_value() && res.error.empty());
        assert(res.config->package == "tool-openocd");
        assert(res.config->interface_desc == "ST-Link");
        assert(res.config->target_cfg == "target/stm32f4x.cfg");
        assert(res.config->sample_mode == ProbeSampleMode::DwtPcsr);
        assert(res.config->target_arch == malgrind::Arch::ArmV7M);
        assert((res.config->openocd_args == std::vector<std::string>{
                                                "-f",
                                                "interface/stlink.cfg",
                                                "-c",
                                                "adapter serial 066EFF3238",
                                                "-f",
                                                "target/stm32f4x.cfg",
                                            }));
    }

    // CMSIS-DAP (DAPLink)
    {
        const auto res = resolve_probe_config(make_probe("0d28", "0204"), nullptr, nullptr, {}, "target/nrf52.cfg");
        assert(res.config.has_value());
        assert(res.config->package == "tool-openocd");
        assert(res.config->interface_desc == "CMSIS-DAP (DAPLink)");
        assert(res.config->sample_mode == ProbeSampleMode::Auto);
        assert(!res.config->target_arch.has_value());
        assert((res.config->openocd_args == std::vector<std::string>{"-f", "interface/cmsis-dap.cfg", "-f", "target/nrf52.cfg"}));
    }

    // Raspberry Pi Debug Probe includes adapter speed 5000
    {
        const auto res = resolve_probe_config(make_probe("2e8a", "000c", "E6614103E7"), nullptr, nullptr, {}, "target/rp2040.cfg");
        assert(res.config.has_value());
        assert(res.config->package == "tool-openocd");
        assert(res.config->interface_desc == "Raspberry Pi Debug Probe");
        assert((res.config->openocd_args == std::vector<std::string>{
                                                "-f",
                                                "interface/cmsis-dap.cfg",
                                                "-c",
                                                "adapter speed 5000",
                                                "-c",
                                                "adapter serial E6614103E7",
                                                "-f",
                                                "target/rp2040.cfg",
                                            }));
    }

    // SEGGER J-Link (any PID)
    {
        const auto res = resolve_probe_config(make_probe("1366", "1051"), nullptr, nullptr, {}, "target/stm32h7x.cfg");
        assert(res.config.has_value());
        assert(res.config->interface_desc == "SEGGER J-Link");
        assert((res.config->openocd_args == std::vector<std::string>{"-f", "interface/jlink.cfg", "-f", "target/stm32h7x.cfg"}));
    }

    // Olimex debugger (any PID)
    {
        const auto res = resolve_probe_config(make_probe("15ba", "002a"), nullptr, nullptr, {}, "target/stm32f1x.cfg");
        assert(res.config.has_value());
        assert(res.config->interface_desc == "Olimex debugger");
        assert((res.config->openocd_args == std::vector<std::string>{
                                                "-f",
                                                "interface/ftdi/olimex-arm-usb-ocd-h.cfg",
                                                "-f",
                                                "target/stm32f1x.cfg",
                                            }));
    }

    // Unsafe serials (spaces, TCL metacharacters, control chars) are skipped rather than injected into -c
    for (const std::string_view unsafe : {"bad serial", "a;shutdown", "a$b", "a[cmd]", "bad\nline", "x\x1ay"}) {
        const auto res = resolve_probe_config(make_probe("0483", "374b", std::string(unsafe), "ST-Link"), nullptr, nullptr, {},
                                              "target/stm32f4x.cfg");
        assert(res.config.has_value());
        assert((res.config->openocd_args == std::vector<std::string>{"-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg"}));
    }
}

void family_and_project_resolution_cases() {
    const auto root = fresh_dir("malgrind_probe_config");
    std::string why;

    // 1. 303a:1001 without a project resolves Esp32Family via usb_probe_name; requires override for target
    const Device esp_builtin = make_probe("303a", "1001", "DC:54:75:C4:12:34", "ESP USB JTAG (built-in)");
    auto res = resolve_probe_config(esp_builtin);
    assert(!res.config.has_value() && contains(res.error, "target"));

    res = resolve_probe_config(esp_builtin, nullptr, nullptr, {}, "target/esp32s3.cfg");
    assert(res.config.has_value());
    assert(res.config->package == "tool-openocd-esp32");
    assert(res.config->interface_desc == "ESP USB JTAG (built-in)");
    assert(res.config->target_cfg == "target/esp32s3.cfg");
    assert(res.config->sample_mode == ProbeSampleMode::HaltResume);
    assert(res.config->target_arch == malgrind::Arch::Xtensa);
    assert((res.config->openocd_args == std::vector<std::string>{
                                            "-f",
                                            "interface/esp_usb_jtag.cfg",
                                            "-c",
                                            "adapter serial DC:54:75:C4:12:34",
                                            "-f",
                                            "target/esp32s3.cfg",
                                        }));

    // 2. Project with Arduino framework and spaces in path still resolves Esp32Family and target/esp32c3.cfg
    write_file(root / "my esp project/platformio.ini",
               "[platformio]\ndefault_envs = c3\n"
               "[env:c3]\nplatform = espressif32\nframework = arduino\nboard = esp32-c3-devkitm-1\n"
               "[env:ambig]\nplatform = espressif32\nframework = espidf\n");
    const auto proj = read_project(root / "my esp project", why);
    assert(proj);

    res = resolve_probe_config(esp_builtin, nullptr, &*proj);
    assert(res.config.has_value());
    assert(res.config->package == "tool-openocd-esp32");
    assert(res.config->target_cfg == "target/esp32c3.cfg");
    assert(res.config->sample_mode == ProbeSampleMode::HaltResume);
    assert(res.config->target_arch == malgrind::Arch::RiscV32);
    assert((res.config->openocd_args == std::vector<std::string>{
                                            "-f",
                                            "interface/esp_usb_jtag.cfg",
                                            "-c",
                                            "adapter serial DC:54:75:C4:12:34",
                                            "-f",
                                            "target/esp32c3.cfg",
                                        }));

    // 3. FTDI FT2232 with ESP32 project resolves tool-openocd-esp32 and esp32_devkitj_v1.cfg
    write_file(root / "classic/platformio.ini", "[env:esp32dev]\nplatform = espressif32\nboard = esp32dev\n");
    const auto classic_proj = read_project(root / "classic", why);
    assert(classic_proj);
    const Device ftdi = make_probe("0403", "6010", "FT12345", "FTDI FT2232/FT232H (multi-purpose USB-JTAG adapter)");
    res = resolve_probe_config(ftdi, nullptr, &*classic_proj);
    assert(res.config.has_value());
    assert(res.config->package == "tool-openocd-esp32");
    assert(res.config->target_cfg == "target/esp32.cfg");
    assert((res.config->openocd_args == std::vector<std::string>{
                                            "-f",
                                            "interface/ftdi/esp32_devkitj_v1.cfg",
                                            "-c",
                                            "adapter serial FT12345",
                                            "-f",
                                            "target/esp32.cfg",
                                        }));

    // 4. Ambiguous ESP32 project with FTDI probe: without override -> asks for target; with override -> succeeds with ESP32 FTDI cfg
    res = resolve_probe_config(ftdi, nullptr, &*proj, "ambig");
    assert(!res.config.has_value() && contains(res.error, "target"));
    res = resolve_probe_config(ftdi, nullptr, &*proj, "ambig", "target/esp32s3.cfg");
    assert(res.config.has_value());
    assert(res.config->package == "tool-openocd-esp32");
    assert(res.config->target_cfg == "target/esp32s3.cfg");
    assert((res.config->openocd_args == std::vector<std::string>{
                                            "-f",
                                            "interface/ftdi/esp32_devkitj_v1.cfg",
                                            "-c",
                                            "adapter serial FT12345",
                                            "-f",
                                            "target/esp32s3.cfg",
                                        }));

    // 5. Generic probe (J-Link) paired with ESP32 project falls back to generic interface/jlink.cfg + ESP32 target
    write_file(root / "c6/platformio.ini", "[env:dev]\nplatform = espressif32\nboard = esp32-c6-devkitm-1\n");
    const auto c6_proj = read_project(root / "c6", why);
    assert(c6_proj);
    res = resolve_probe_config(make_probe("1366", "0101"), nullptr, &*c6_proj);
    assert(res.config.has_value());
    assert(res.config->package == "tool-openocd-esp32");
    assert(res.config->interface_desc == "SEGGER J-Link");
    assert(res.config->target_cfg == "target/esp32c6.cfg");
    assert((res.config->openocd_args == std::vector<std::string>{"-f", "interface/jlink.cfg", "-f", "target/esp32c6.cfg"}));

    // 6. Explicit target override wins over inferred project target
    res = resolve_probe_config(esp_builtin, &esp32_family(), &*c6_proj, "dev", "target/esp32s2.cfg");
    assert(res.config.has_value() && res.config->target_cfg == "target/esp32s2.cfg");

    // 7. FTDI probe without project resolves family from explicit target_cfg matching probe_target_options
    res = resolve_probe_config(ftdi, nullptr, nullptr, {}, "target/esp32s3.cfg");
    assert(res.config.has_value());
    assert(res.config->package == "tool-openocd-esp32");
    assert(res.config->target_cfg == "target/esp32s3.cfg");
    assert((res.config->openocd_args == std::vector<std::string>{
                                            "-f",
                                            "interface/ftdi/esp32_devkitj_v1.cfg",
                                            "-c",
                                            "adapter serial FT12345",
                                            "-f",
                                            "target/esp32s3.cfg",
                                        }));

    // 8. ST-Link paired with STM32 project resolves Stm32Family, tool-openocd, and target/sample mode
    write_file(root / "stm32/platformio.ini",
               "[env:f4]\nplatform = ststm32\nframework = stm32cube\nboard = nucleo_f401re\n"
               "[env:f0]\nplatform = ststm32\nframework = arduino\nboard_build.mcu = stm32f030f4p6\n");
    const auto stm_proj = read_project(root / "stm32", why);
    assert(stm_proj);
    res = resolve_probe_config(make_probe("0483", "374b", "ST-NUCLEO", "ST-Link"), nullptr, &*stm_proj, "f4");
    assert(res.config.has_value());
    assert(res.config->package == "tool-openocd");
    assert(res.config->target_cfg == "target/stm32f4x.cfg");
    assert(res.config->sample_mode == ProbeSampleMode::DwtPcsr);
    assert(res.config->target_arch == malgrind::Arch::ArmV7M);
    assert((res.config->openocd_args == std::vector<std::string>{
                                            "-f",
                                            "interface/stlink.cfg",
                                            "-c",
                                            "adapter serial ST-NUCLEO",
                                            "-f",
                                            "target/stm32f4x.cfg",
                                        }));

    res = resolve_probe_config(make_probe("0483", "374b", "ST-F0", "ST-Link"), nullptr, &*stm_proj, "f0");
    assert(res.config.has_value());
    assert(res.config->target_cfg == "target/stm32f0x.cfg");
    assert(res.config->sample_mode == ProbeSampleMode::HaltResume);
    assert(res.config->target_arch == malgrind::Arch::ArmV7M);

    // 9. Raspberry Pi Debug Probe (2e8a:000c) stays family-neutral without project/override,
    //    resolves Rp2040Family for RP2040 projects/overrides, and still resolves Stm32Family for STM32 projects.
    const Device rpi_probe = make_probe("2e8a", "000c", "E6614103E7", "Raspberry Pi Debug Probe");
    assert(resolve_probe_family(rpi_probe, nullptr, {}, {}) == nullptr);
    res = resolve_probe_config(rpi_probe);
    assert(!res.config.has_value() && contains(res.error, "target"));

    res = resolve_probe_config(rpi_probe, nullptr, &*stm_proj, "f4");
    assert(res.config.has_value());
    assert(resolve_probe_family(rpi_probe, &*stm_proj, "f4", {}) == &stm32_family());
    assert(res.config->target_cfg == "target/stm32f4x.cfg");
    assert(res.config->sample_mode == ProbeSampleMode::DwtPcsr);
    assert(res.config->target_arch == malgrind::Arch::ArmV7M);

    write_file(root / "rp2040/platformio.ini",
               "[env:pico]\nplatform = raspberrypi\nframework = arduino\nboard = pico\n"
               "[env:sdk]\nplatform = https://github.com/maxgerhardt/platform-raspberrypi.git\nframework = picosdk\nboard = rpipico\n");
    const auto rp_proj = read_project(root / "rp2040", why);
    assert(rp_proj);
    assert(resolve_probe_family(rpi_probe, &*rp_proj, "pico", {}) == &rp2040_family());
    res = resolve_probe_config(rpi_probe, nullptr, &*rp_proj, "pico");
    assert(res.config.has_value());
    assert(res.config->package == "tool-openocd");
    assert(res.config->target_cfg == "target/rp2040.cfg");
    assert(res.config->sample_mode == ProbeSampleMode::HaltResume);
    assert(res.config->target_arch == malgrind::Arch::ArmV7M);
    assert((res.config->openocd_args == std::vector<std::string>{
                                            "-f",
                                            "interface/cmsis-dap.cfg",
                                            "-c",
                                            "adapter speed 5000",
                                            "-c",
                                            "adapter serial E6614103E7",
                                            "-f",
                                            "target/rp2040.cfg",
                                        }));

    assert(resolve_probe_family(rpi_probe, nullptr, {}, "target/rp2040.cfg") == &rp2040_family());
    res = resolve_probe_config(rpi_probe, nullptr, nullptr, {}, "target/rp2040.cfg");
    assert(res.config.has_value());
    assert(res.config->sample_mode == ProbeSampleMode::HaltResume);
    assert(res.config->target_arch == malgrind::Arch::ArmV7M);

    fs::remove_all(root);
}

} // namespace

int main() {
    validation_and_error_cases();
    generic_probe_table_cases();
    family_and_project_resolution_cases();
    std::puts("probe_config ok");
}

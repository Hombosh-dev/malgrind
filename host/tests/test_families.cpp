// Chip family interface, registry, ESP32, and STM32 family implementation self-checks.
#include "backup.hpp"
#include "env.hpp"
#include "families/esp32.hpp"
#include "families/family.hpp"
#include "families/stm32.hpp"
#include "os/paths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

fs::path fresh(const char* name) {
    const auto p = fs::canonical(fs::temp_directory_path()) / os::from_utf8(name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

class StubFamily final : public Family {
public:
    [[nodiscard]] std::string_view id() const override { return "stub"; }
    [[nodiscard]] std::string_view name() const override { return "Stub"; }
    [[nodiscard]] FamilyCaps caps() const override { return {}; }
    [[nodiscard]] bool handles_arch(Arch) const override { return false; }
    [[nodiscard]] std::string project_problem(const ProjectInfo&, const std::string&) const override { return {}; }
    [[nodiscard]] unsigned agent_baud() const override { return 115200; }
    [[nodiscard]] AgentCommands agent_commands(const ToolContext&, const ProjectInfo&, const std::string&,
                                               const std::string&, const RunSettings&, bool) const override {
        return {};
    }
    [[nodiscard]] std::vector<std::string> addr2line_names(Arch) const override { return {}; }
};

void registry_lookups() {
    const auto& families = all_families();
    assert(families.size() >= 2);
    const Family* esp = family_by_id("esp32");
    const Family* stm = family_by_id("stm32");
    assert(esp == &esp32_family());
    assert(stm == &stm32_family());
    assert(families[0] == esp && families[1] == stm);
    assert(family_by_id("unknown") == nullptr && family_by_id("") == nullptr);

    assert(family_for_arch(Arch::Xtensa) == esp);
    assert(family_for_arch(Arch::RiscV32) == esp);
    assert(family_for_arch(Arch::ArmV7M) == stm);
    assert(family_for_arch(Arch::Host) == nullptr);

    for (const Family* f : families) {
        const auto manifest = fs::path(MALGRIND_SOURCE_DIR) / "agent" / f->id() / "family.json";
        assert(fs::exists(manifest));
        const auto parsed = nlohmann::json::parse(slurp(manifest));
        assert(parsed.at("id").get<std::string>() == f->id());
    }
}

void base_family_defaults() {
    StubFamily stub;
    assert(!stub.caps().memcheck);
    assert(!stub.is_agent_image("/no/such/image.bin"));
    assert(stub.extra_symbol_files({}, Arch::Xtensa, {}).empty());
    assert(stub.family_hint_for_usb("303a", "1001").empty());
    assert(stub.usb_adapter_name("303a", "1001").empty());
    assert(stub.usb_probe_name("303a", "1001").empty());
    assert(stub.openocd_package() == "tool-openocd");
    assert(stub.openocd_interface_args("303a", "1001").empty());
    assert(stub.openocd_target_cfg({}, "").empty());
    assert(stub.probe_target_options().empty());
    assert(stub.probe_sample_mode() == ProbeSampleMode::Auto);
    assert(!stub.probe_target_arch("target/stm32f4x.cfg").has_value());
    assert(!stub.handles_project({}, ""));

    auto threw = [](auto&& fn) {
        try {
            fn();
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    };
    assert(threw([&] { (void)stub.identify({}, "port", {}); }));
    assert(threw([&] { stub.read_flash({}, "port", {}, "/tmp/out.bin", {}); }));
    assert(threw([&] { stub.write_flash({}, "port", "/tmp/in.bin", {}); }));
}

void esp32_metadata_and_usb() {
    const Family& esp = esp32_family();
    assert(esp.id() == "esp32" && esp.name() == "ESP32 (ESP-IDF)");
    const auto caps = esp.caps();
    assert(caps.agent && caps.backup && caps.identify && caps.identify_resets_board && caps.memcheck);
    assert(esp.handles_arch(Arch::Xtensa) && esp.handles_arch(Arch::RiscV32));
    assert(!esp.handles_arch(Arch::ArmV7M) && !esp.handles_arch(Arch::Host));
    assert(esp.agent_baud() == 921600);

    const auto xtensa_tools = esp.addr2line_names(Arch::Xtensa);
    assert(xtensa_tools == (std::vector<std::string>{"xtensa-esp32-elf-addr2line", "xtensa-esp32s3-elf-addr2line"}));
    assert(esp.addr2line_names(Arch::RiscV32) == (std::vector<std::string>{"riscv32-esp-elf-addr2line"}));
    assert(esp.addr2line_names(Arch::ArmV7M).empty());

    assert(esp.usb_adapter_name("303a", "1001") == "Espressif USB-Serial/JTAG (built-in debug port)");
    assert(esp.usb_adapter_name("10c4", "ea60").empty());
    assert(esp.family_hint_for_usb("303a", "1001") == "esp32");
    assert(esp.family_hint_for_usb("10c4", "ea60").empty());

    assert(esp.usb_probe_name("303a", "1001") == "ESP USB JTAG (built-in)");
    assert(esp.usb_probe_name("303A", "1001") == "ESP USB JTAG (built-in)");
    assert(esp.usb_probe_name("303a", "0002").empty());
    assert(esp.usb_probe_name("0483", "374b").empty());
    assert(esp.openocd_package() == "tool-openocd-esp32");
    assert((esp.openocd_interface_args("303a", "1001") == std::vector<std::string>{"-f", "interface/esp_usb_jtag.cfg"}));
    assert((esp.openocd_interface_args("0403", "6010") == std::vector<std::string>{"-f", "interface/ftdi/esp32_devkitj_v1.cfg"}));
    assert((esp.openocd_interface_args("0403", "6014") == std::vector<std::string>{"-f", "interface/ftdi/esp32_devkitj_v1.cfg"}));
    assert(esp.openocd_interface_args("0483", "374b").empty());
    assert(esp.openocd_interface_args("1366", "0101").empty());

    const auto opts = esp.probe_target_options();
    assert((opts == std::vector<ProbeTargetOption>{
                        {"target/esp32.cfg", "ESP32"},
                        {"target/esp32s3.cfg", "ESP32-S3"},
                        {"target/esp32s2.cfg", "ESP32-S2"},
                        {"target/esp32c3.cfg", "ESP32-C3"},
                        {"target/esp32c6.cfg", "ESP32-C6"},
                    }));
    assert(esp.probe_sample_mode() == ProbeSampleMode::HaltResume);
    assert(esp.probe_sample_mode("target/esp32s3.cfg") == ProbeSampleMode::HaltResume);
    assert(esp.probe_target_arch("target/esp32.cfg") == Arch::Xtensa);
    assert(esp.probe_target_arch("target/esp32s3.cfg") == Arch::Xtensa);
    assert(esp.probe_target_arch("target/esp32c3.cfg") == Arch::RiscV32);
    assert(esp.probe_target_arch("target/esp32c6.cfg") == Arch::RiscV32);
    assert(!esp.probe_target_arch("target/stm32f4x.cfg").has_value());
}

void project_matching_and_problems() {
    const auto dir = fresh("malgrind_families_proj");
    std::string why;

    // Valid ESP-IDF ini
    write(dir / "platformio.ini", "[env:a]\nplatform = espressif32@6.9.0\nframework = arduino, espidf\n");
    const auto idf_proj = read_project(dir, why);
    assert(idf_proj);
    const Family* matched = family_for_project(*idf_proj, "a", why);
    assert(matched == &esp32_family() && why.empty());
    assert(matched->project_problem(*idf_proj, "a").empty());

    // Arduino-ESP32 ini -> problem text names framework & espidf
    write(dir / "platformio.ini", "[env:a]\nplatform = espressif32\nframework = arduino\n");
    const auto arduino_proj = read_project(dir, why);
    assert(arduino_proj);
    assert(family_for_project(*arduino_proj, "a", why) == nullptr);
    assert(contains(why, "arduino") && contains(why, "espidf"));

    // STM32 ini -> matches STM32 family
    write(dir / "platformio.ini", "[env:a]\nplatform = ststm32\nframework = stm32cube\n");
    const auto stm_proj = read_project(dir, why);
    assert(stm_proj);
    assert(family_for_project(*stm_proj, "a", why) == &stm32_family() && why.empty());

    // Unsupported platform -> no family matches, combined why names both ESP32 and STM32
    write(dir / "platformio.ini", "[env:a]\nplatform = atmelavr\nframework = arduino\n");
    const auto avr_proj = read_project(dir, why);
    assert(avr_proj);
    assert(family_for_project(*avr_proj, "a", why) == nullptr);
    assert(contains(why, "atmelavr") && contains(why, "ESP32") && contains(why, "STM32"));

    // Space in project path -> rejected by ESP32 project_problem
    write(dir / "with space/platformio.ini", "[env:a]\nplatform = espressif32\nframework = espidf\n");
    const auto space_proj = read_project(dir / "with space", why);
    assert(space_proj);
    assert(family_for_project(*space_proj, "a", why) == nullptr);
    assert(contains(why, "space"));

    fs::remove_all(dir);
}

void agent_commands_preview_and_write() {
    const auto dir = fresh("malgrind_families_cmds");
    const auto proj_dir = dir / "proj";
    write(proj_dir / "platformio.ini", "[env:esp32dev]\nplatform = espressif32\nframework = espidf\n");
    write(proj_dir / "sdkconfig.esp32dev", "CONFIG_USER=1\nCONFIG_ESP_CONSOLE_UART_BAUDRATE=115200\n");

    std::string why;
    const auto project = read_project(proj_dir, why);
    assert(project);

    const ToolContext tools{{}, "/opt/pio", MALGRIND_SOURCE_DIR};
    const Family& esp = esp32_family();

    // write_files = false: preview creates nothing inside .pio/malgrind
    const auto preview = esp.agent_commands(tools, *project, "esp32dev", "/dev/ttyUSB0", {.rate_hz = 500}, false);
    assert(!fs::exists(proj_dir / ".pio"));
    assert(preview.elf == proj_dir / ".pio/malgrind/build/esp32dev/firmware.elf");
    assert(std::ranges::find(preview.build.argv, "upload") == preview.build.argv.end());
    assert(std::ranges::find(preview.upload.argv, "upload") != preview.upload.argv.end());
    assert(std::ranges::find(preview.upload.argv, "/dev/ttyUSB0") != preview.upload.argv.end());

    // write_files = true: writes sdkconfig.<env> and agent.stamp (check_memory = false by default)
    const auto real = esp.agent_commands(tools, *project, "esp32dev", "/dev/ttyUSB0", {.rate_hz = 500}, true);
    assert(real.build.argv == preview.build.argv && real.upload.argv == preview.upload.argv && real.elf == preview.elf);
    const auto sdkconfig = slurp(proj_dir / ".pio/malgrind/sdkconfig.esp32dev");
    assert(contains(sdkconfig, "CONFIG_USER=1\n") && contains(sdkconfig, "CONFIG_MALGRIND_RATE_HZ=500\n"));
    assert(contains(sdkconfig, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600\n") && !contains(sdkconfig, "115200"));
    assert(contains(sdkconfig, "# CONFIG_MALGRIND_MEMCHECK is not set\n") && !contains(sdkconfig, "CONFIG_HEAP_USE_HOOKS"));
    assert(fs::exists(proj_dir / ".pio/malgrind/agent.stamp"));

    // check_memory = true: appends sdkconfig.memcheck + CONFIG_MALGRIND_MEMCHECK=y and overrides conflicting choices
    write(proj_dir / "sdkconfig.esp32dev",
          "CONFIG_USER=1\nCONFIG_HEAP_POISONING_DISABLED=y\nCONFIG_FREERTOS_CHECK_STACKOVERFLOW_NONE=y\n");
    (void)esp.agent_commands(tools, *project, "esp32dev", "/dev/ttyUSB0", {.rate_hz = 500, .check_memory = true}, true);
    const auto mem_sdkconfig = slurp(proj_dir / ".pio/malgrind/sdkconfig.esp32dev");
    assert(contains(mem_sdkconfig, "CONFIG_USER=1\n"));
    assert(contains(mem_sdkconfig, "CONFIG_HEAP_USE_HOOKS=y\n"));
    assert(contains(mem_sdkconfig, "CONFIG_HEAP_POISONING_COMPREHENSIVE=y\n"));
    assert(contains(mem_sdkconfig, "CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y\n"));
    assert(contains(mem_sdkconfig, "CONFIG_MALGRIND_MEMCHECK=y\n"));
    assert(!contains(mem_sdkconfig, "CONFIG_HEAP_POISONING_DISABLED=y"));
    assert(!contains(mem_sdkconfig, "CONFIG_FREERTOS_CHECK_STACKOVERFLOW_NONE=y"));

    // check_memory = false after true removes memcheck config cleanly
    (void)esp.agent_commands(tools, *project, "esp32dev", "/dev/ttyUSB0", {.check_memory = false}, true);
    const auto nomem_sdkconfig = slurp(proj_dir / ".pio/malgrind/sdkconfig.esp32dev");
    assert(contains(nomem_sdkconfig, "# CONFIG_MALGRIND_MEMCHECK is not set\n"));
    assert(!contains(nomem_sdkconfig, "CONFIG_MALGRIND_MEMCHECK=y") && !contains(nomem_sdkconfig, "CONFIG_HEAP_USE_HOOKS"));

    // Missing sdkconfig.memcheck throws cleanly when check_memory = true
    const auto partial_agent = dir / "partial-agent";
    write(partial_agent / "agent/esp32/sdkconfig.agent", "CONFIG_X=1\n");
    write(partial_agent / "agent/esp32/inject.py", "");
    bool missing_memcheck_threw = false;
    try {
        write_agent_sdkconfig(*project, "esp32dev", partial_agent, 0, true);
    } catch (const std::runtime_error&) {
        missing_memcheck_threw = true;
    }
    assert(missing_memcheck_threw);

    // Missing agent files throw cleanly
    bool missing_threw = false;
    try {
        (void)esp.agent_commands({{}, "/opt/pio", dir / "missing-agent"}, *project, "esp32dev", "/dev/ttyUSB0", {}, false);
    } catch (const std::runtime_error&) {
        missing_threw = true;
    }
    assert(missing_threw);

    fs::remove_all(dir);
}

void esptool_parsing_and_agent_image() {
    assert(parse_mac("MAC: 24:0a:c4:AA:bb:0C") == "24-0a-c4-aa-bb-0c");
    assert(parse_mac("Chip is ESP32\nMAC: 24:0a:c4:aa:bb:0c\nUploading stub...\n") == "24-0a-c4-aa-bb-0c");
    assert(!parse_mac("no mac here") && !parse_mac("MAC: 24:0a:c4") && !parse_mac("MAC: zz:0a:c4:aa:bb:0c"));

    assert(parse_flash_size("Detected flash size: 4MB") == 4u * 1024 * 1024);
    assert(parse_flash_size("Device: 4016\nDetected flash size: 16MB\n") == 16u * 1024 * 1024);
    assert(parse_flash_size("Detected flash size: 512KB") == 512u * 1024);
    assert(!parse_flash_size("Detected flash size: Unknown") && !parse_flash_size("") && !parse_flash_size("Detected flash size: 4GB"));

    assert(parse_chip("Chip is ESP32-D0WD-V3 (revision v3.1)\r\nMAC: 24:0a:c4:aa:bb:0c\r\n") == "ESP32-D0WD-V3 (revision v3.1)");
    assert(!parse_chip("no chip here") && !parse_chip("Chip is   \n"));

    const auto root = fresh("malgrind_families_img");
    write(root / "agent_v1.bin", std::string("\xe9\x03junk") + std::string(kEsp32AgentMarker) + "\0more");
    write(root / "agent_v2.bin", std::string("\xe9\x03junk") + std::string(kEsp32AgentMarkerV2) + "\0more");
    write(root / "plain.bin", "\xe9\x03 only the user's firmware here");
    write(root / "other_marker.bin", "\xe9\x03junkmalgrind-agent-v3\0more");
    std::string big_v1(70000, 'a');
    big_v1.insert(65536 - 5, kEsp32AgentMarker); // straddles a 64 KiB read boundary
    write(root / "big_v1.bin", big_v1);
    std::string big_v2(70000, 'b');
    big_v2.insert(65536 - 5, kEsp32AgentMarkerV2); // straddles a 64 KiB read boundary
    write(root / "big_v2.bin", big_v2);
    std::string big_v2_tail(70000, 'c');
    big_v2_tail.insert(65536 - (kEsp32AgentMarkerV2.size() - 1), kEsp32AgentMarkerV2); // 1 byte past boundary
    write(root / "big_v2_tail.bin", big_v2_tail);

    const Family& esp = esp32_family();
    assert(esp.is_agent_image(root / "agent_v1.bin"));
    assert(esp.is_agent_image(root / "agent_v2.bin"));
    assert(esp.is_agent_image(root / "big_v1.bin"));
    assert(esp.is_agent_image(root / "big_v2.bin"));
    assert(esp.is_agent_image(root / "big_v2_tail.bin"));
    assert(!esp.is_agent_image(root / "plain.bin"));
    assert(!esp.is_agent_image(root / "other_marker.bin"));
    assert(!esp.is_agent_image(root / "missing.bin"));
    fs::remove_all(root);
}

void sdkconfig_merge_checks() {
    const auto agent = slurp(fs::path(MALGRIND_SOURCE_DIR) / "agent/esp32/sdkconfig.agent");
    assert(contains(agent, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600"));
    const auto merged = merge_sdkconfig("CONFIG_A=1\n# CONFIG_GPTIMER_ISR_IRAM_SAFE is not set\nCONFIG_ESP_CONSOLE_UART_BAUDRATE=115200\n"
                                        "CONFIG_ESP_CONSOLE_UART_DEFAULT=y\n# CONFIG_B is not set",
                                        agent);
    assert(contains(merged, "CONFIG_A=1\n") && contains(merged, "# CONFIG_B is not set\n"));
    assert(contains(merged, "CONFIG_GPTIMER_ISR_IRAM_SAFE=y\n") && contains(merged, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600\n"));
    assert(contains(merged, "# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set\n") && contains(merged, "CONFIG_ESP_CONSOLE_UART_CUSTOM=y\n"));
    assert(!contains(merged, "115200") && !contains(merged, "IRAM_SAFE is not set") && !contains(merged, "UART_DEFAULT=y"));
    assert(!contains(merged, "DEFAULT and CUSTOM are one"));
}

void esp32_extra_symbol_files() {
    ScopedEnv save_core("PLATFORMIO_CORE_DIR", std::nullopt);
    ScopedEnv save_drive("SystemDrive", std::nullopt);

    const auto root = fresh("malgrind_families_rom");
    const auto home = root / "home";
    const Family& esp = esp32_family();

    // Missing package -> empty
    fs::create_directories(home / ".platformio/packages");
    assert(esp.extra_symbol_files(home, Arch::Xtensa, "/proj/.pio/build/esp32dev/firmware.elf").empty());

    // Versioned subdir + revision selection + ignored invalid/deep files
    const auto pkg = home / ".platformio/packages/tool-esp-rom-elfs@20241011";
    const auto ver_dir = pkg / "20241011";
    write(ver_dir / "esp32_rom.elf", "elf");
    write(ver_dir / "esp32_rev300_rom.elf", "elf");
    write(ver_dir / "esp32s3_rev0_rom.elf", "elf");
    write(ver_dir / "esp32s2_rev0_rom.elf", "elf");
    write(ver_dir / "esp32c3_rev3_rom.elf", "elf");
    write(ver_dir / "esp32c3_rev101_rom.elf", "elf");
    write(ver_dir / "esp32c6_rev0_rom.elf", "elf");
    write(ver_dir / "esp32_rev999_rom.txt", "not elf");
    write(ver_dir / "esp32_revabc_rom.elf", "bad rev");
    write(ver_dir / "nested/esp32_rev999_rom.elf", "too deep");

    assert(esp.extra_symbol_files(home, Arch::ArmV7M, "/proj/firmware.elf").empty());
    assert(esp.extra_symbol_files(home, Arch::Host, "/proj/firmware.elf").empty());

    // Xtensa: default esp32 picks highest revision (300 > 0)
    const auto xt_default = esp.extra_symbol_files(home, Arch::Xtensa, "/proj/.pio/build/esp32dev/firmware.elf");
    assert(xt_default.size() == 1);
    assert(xt_default[0].start == 0x40000000u && xt_default[0].end == 0x4006ffffu);
    assert(xt_default[0].elf == ver_dir / "esp32_rev300_rom.elf");

    // Xtensa: case-insensitive esp32s3 / esp32s2 detection from firmware path
    const auto xt_s3 = esp.extra_symbol_files(home, Arch::Xtensa, "/proj/.pio/malgrind/build/ESP32S3/firmware.elf");
    assert(xt_s3.size() == 1 && xt_s3[0].elf == ver_dir / "esp32s3_rev0_rom.elf");
    const auto xt_s2 = esp.extra_symbol_files(home, Arch::Xtensa, "/proj/.pio/build/esp32s2-saola/firmware.elf");
    assert(xt_s2.size() == 1 && xt_s2[0].elf == ver_dir / "esp32s2_rev0_rom.elf");

    // RiscV32: numeric revision comparison (101 > 3), esp32c6 detection, and default esp32c3
    const auto rv_c3 = esp.extra_symbol_files(home, Arch::RiscV32, "/proj/.pio/build/esp32c3/firmware.elf");
    assert(rv_c3.size() == 1);
    assert(rv_c3[0].start == 0x40000000u && rv_c3[0].end == 0x4006ffffu);
    assert(rv_c3[0].elf == ver_dir / "esp32c3_rev101_rom.elf");
    const auto rv_c6 = esp.extra_symbol_files(home, Arch::RiscV32, "/proj/.pio/build/esp32c6/firmware.elf");
    assert(rv_c6.size() == 1 && rv_c6[0].elf == ver_dir / "esp32c6_rev0_rom.elf");
    const auto rv_default = esp.extra_symbol_files(home, Arch::RiscV32, {});
    assert(rv_default.size() == 1 && rv_default[0].elf == ver_dir / "esp32c3_rev101_rom.elf");

    // Flat package layout + <chip>_rom.elf (rev 0) via PLATFORMIO_CORE_DIR
    const auto custom_core = root / "custom_pio";
    const auto flat_pkg = custom_core / "packages/tool-esp-rom-elfs";
    write(flat_pkg / "esp32p4_rom.elf", "elf");
    ScopedEnv override_core("PLATFORMIO_CORE_DIR", os::to_utf8(custom_core));
    const auto rv_p4 = esp.extra_symbol_files(home, Arch::RiscV32, "/proj/.pio/build/esp32p4/firmware.elf");
    assert(rv_p4.size() == 1 && rv_p4[0].elf == flat_pkg / "esp32p4_rom.elf");

    fs::remove_all(root);
}

void esp32_openocd_target_inference() {
    const auto root = fresh("malgrind_families_ocd_target");
    const Family& esp = esp32_family();
    std::string why;

    // 1. board_build.mcu overrides board/env cleanly
    write(root / "mcu/platformio.ini",
          "[env:s3]\nplatform = espressif32\nboard = custom\nboard_build.mcu = esp32s3\n"
          "[env:s2]\nplatform = espressif32\nboard_build.mcu = esp32s2\n"
          "[env:c3]\nplatform = espressif32\nboard_build.mcu = esp32c3\n"
          "[env:c6]\nplatform = espressif32\nboard_build.mcu = esp32c6\n"
          "[env:classic]\nplatform = espressif32\nboard_build.mcu = esp32\n"
          "[env:h2]\nplatform = espressif32\nboard_build.mcu = esp32h2\n");
    const auto mcu_proj = read_project(root / "mcu", why);
    assert(mcu_proj);
    assert(esp.openocd_target_cfg(*mcu_proj, "s3") == "target/esp32s3.cfg");
    assert(esp.openocd_target_cfg(*mcu_proj, "s2") == "target/esp32s2.cfg");
    assert(esp.openocd_target_cfg(*mcu_proj, "c3") == "target/esp32c3.cfg");
    assert(esp.openocd_target_cfg(*mcu_proj, "c6") == "target/esp32c6.cfg");
    assert(esp.openocd_target_cfg(*mcu_proj, "classic") == "target/esp32.cfg");
    assert(esp.openocd_target_cfg(*mcu_proj, "h2").empty()); // unsupported variant does not falsely fall back to esp32
    assert(esp.openocd_target_cfg(*mcu_proj, "") == "target/esp32s3.cfg");
    assert(esp.openocd_target_cfg(*mcu_proj, "missing").empty());

    // 2. Board names (including Arduino framework and space in path)
    write(root / "with space/platformio.ini",
          "[env:a]\nplatform = espressif32\nframework = arduino\nboard = esp32-s3-devkitc-1\n"
          "[env:b]\nplatform = espressif32\nboard = lolin_s2_mini\n"
          "[env:c]\nplatform = espressif32\nboard = esp32-c3-devkitm-1\n"
          "[env:d]\nplatform = espressif32\nboard = seeed_xiao_esp32c6\n"
          "[env:e]\nplatform = espressif32\nboard = esp32dev\n"
          "[env:f]\nplatform = espressif32\nboard = esp-wrover-kit\n"
          "[env:nano]\nplatform = espressif32\nboard = arduino_nano_esp32\n"
          "[env:ambig]\nplatform = espressif32\nboard = custom_board\n");
    const auto board_proj = read_project(root / "with space", why);
    assert(board_proj);
    assert(esp.handles_project(*board_proj, "a"));
    assert(esp.handles_project(*board_proj, "ambig"));
    assert(esp.openocd_target_cfg(*board_proj, "a") == "target/esp32s3.cfg");
    assert(esp.openocd_target_cfg(*board_proj, "b") == "target/esp32s2.cfg");
    assert(esp.openocd_target_cfg(*board_proj, "c") == "target/esp32c3.cfg");
    assert(esp.openocd_target_cfg(*board_proj, "d") == "target/esp32c6.cfg");
    assert(esp.openocd_target_cfg(*board_proj, "e") == "target/esp32.cfg");
    assert(esp.openocd_target_cfg(*board_proj, "f") == "target/esp32.cfg");
    assert(esp.openocd_target_cfg(*board_proj, "nano") == "target/esp32s3.cfg");
    assert(esp.openocd_target_cfg(*board_proj, "ambig").empty());

    // 3. sdkconfig fallback & env-name inference
    write(root / "sdk/platformio.ini",
          "[env:release]\nplatform = espressif32\nframework = espidf\nboard = my_board\n"
          "[env:esp32c3]\nplatform = espressif32\n"
          "[env:esp32dev]\nplatform = espressif32\n"
          "[env:plain]\nplatform = espressif32\n");
    write(root / "sdk/sdkconfig.release", "CONFIG_IDF_TARGET_ARCH_XTENSA=y\nCONFIG_IDF_TARGET=\"esp32s3\"\n");
    const auto sdk_proj = read_project(root / "sdk", why);
    assert(sdk_proj);
    assert(esp.openocd_target_cfg(*sdk_proj, "release") == "target/esp32s3.cfg");
    assert(esp.openocd_target_cfg(*sdk_proj, "esp32c3") == "target/esp32c3.cfg");
    assert(esp.openocd_target_cfg(*sdk_proj, "esp32dev") == "target/esp32.cfg");
    assert(esp.openocd_target_cfg(*sdk_proj, "plain").empty());

    // 4. Non-espressif32 platform must not match
    write(root / "stm/platformio.ini", "[env:esp32dev]\nplatform = ststm32\nboard = esp32dev\n");
    const auto other_proj = read_project(root / "stm", why);
    assert(other_proj);
    assert(!esp.handles_project(*other_proj, "esp32dev"));
    assert(esp.openocd_target_cfg(*other_proj, "esp32dev").empty());

    fs::remove_all(root);
}

void stm32_metadata_and_usb() {
    const Family& stm = stm32_family();
    assert(stm.id() == "stm32" && stm.name() == "STM32 (Cortex-M)");
    const auto caps = stm.caps();
    assert(caps.agent && caps.backup && caps.identify && !caps.identify_resets_board && !caps.memcheck);
    assert(stm.handles_arch(Arch::ArmV7M));
    assert(!stm.handles_arch(Arch::Xtensa) && !stm.handles_arch(Arch::RiscV32) && !stm.handles_arch(Arch::Host));
    assert(stm.agent_baud() == 115200);

    assert(stm.addr2line_names(Arch::ArmV7M) == (std::vector<std::string>{"arm-none-eabi-addr2line"}));
    assert(stm.addr2line_names(Arch::Xtensa).empty());

    for (const char* pid : {"374b", "374e", "374f", "3752", "3753", "374B"}) {
        assert(stm.family_hint_for_usb("0483", pid) == "stm32");
        assert(!stm.usb_adapter_name("0483", pid).empty());
    }
    assert(stm.family_hint_for_usb("0483", "3748").empty()); // ST-Link v2 has no VCP
    assert(stm.family_hint_for_usb("10c4", "ea60").empty());

    assert(stm.openocd_package() == "tool-openocd");
    assert(stm.openocd_interface_args("0483", "374b").empty());
    assert(stm.openocd_interface_args("0403", "6010").empty());
    assert(stm.openocd_interface_args("0403", "6014").empty());

    const auto opts = stm.probe_target_options();
    assert((opts == std::vector<ProbeTargetOption>{
                        {"target/stm32f1x.cfg", "STM32F1 (Cortex-M3)"},
                        {"target/stm32f4x.cfg", "STM32F4 (Cortex-M4)"},
                        {"target/stm32l4x.cfg", "STM32L4 (Cortex-M4)"},
                        {"target/stm32h7x.cfg", "STM32H7 (Cortex-M7)"},
                        {"target/stm32g4x.cfg", "STM32G4 (Cortex-M4)"},
                        {"target/stm32f0x.cfg", "STM32F0 (Cortex-M0)"},
                    }));

    assert(stm.probe_sample_mode("") == ProbeSampleMode::Auto);
    assert(stm.probe_sample_mode("target/nrf52.cfg") == ProbeSampleMode::Auto);
    for (const char* m0 : {"target/stm32f0x.cfg", "target/stm32g0x.cfg", "target/stm32l0.cfg"}) {
        assert(stm.probe_sample_mode(m0) == ProbeSampleMode::HaltResume);
    }
    for (const char* dwt : {"target/stm32f1x.cfg", "target/stm32f3x.cfg", "target/stm32f4x.cfg",
                            "target/stm32f7x.cfg", "target/stm32g4x.cfg", "target/stm32l4x.cfg",
                            "target/stm32h7x.cfg", "target/stm32u5x.cfg"}) {
        assert(stm.probe_sample_mode(dwt) == ProbeSampleMode::DwtPcsr);
        assert(stm.probe_target_arch(dwt) == Arch::ArmV7M);
    }
    assert(stm.probe_target_arch("target/stm32l0.cfg") == Arch::ArmV7M);
    assert(!stm.probe_target_arch("target/esp32s3.cfg").has_value());
}

void stm32_project_and_target_inference() {
    ScopedEnv save_core("PLATFORMIO_CORE_DIR", std::nullopt);
    ScopedEnv save_drive("SystemDrive", std::nullopt);

    const auto root = fresh("malgrind_families_stm32_proj");
    const Family& stm = stm32_family();
    std::string why;

    // 1. Supported frameworks (stm32cube and arduino), spaces in path, and unsupported frameworks
    write(root / "with space/platformio.ini",
          "[env:cube]\nplatform = ststm32@17.2.0\nframework = stm32cube\nboard = nucleo_f401re\n"
          "[env:ard]\nplatform = ststm32\nframework = arduino\nboard = bluepill_f103c8\n"
          "[env:badfw]\nplatform = ststm32\nframework = libopencm3\nboard = blackpill_f411ce\n");
    const auto proj = read_project(root / "with space", why);
    assert(proj);
    assert(family_for_project(*proj, "cube", why) == &stm && why.empty());
    assert(family_for_project(*proj, "ard", why) == &stm && why.empty());
    assert(family_for_project(*proj, "badfw", why) == nullptr && contains(why, "stm32cube"));
    assert(stm.handles_project(*proj, "badfw")); // no_flash still works on ststm32

    // 2. Fallback board_build.mcu / board prefix rules when no board manifest exists
    write(root / "fallbacks/platformio.ini",
          "[env:f0]\nplatform = ststm32\nboard_build.mcu = stm32f030f4p6\n"
          "[env:f1]\nplatform = ststm32\nboard_build.mcu = stm32f103c8t6\n"
          "[env:bp]\nplatform = ststm32\nboard = bluepill_f103c8\n"
          "[env:f3]\nplatform = ststm32\nboard_build.mcu = stm32f303vct6\n"
          "[env:f4]\nplatform = ststm32\nboard = blackpill_f411ce\n"
          "[env:f7]\nplatform = ststm32\nboard_build.mcu = stm32f767zit6\n"
          "[env:g0]\nplatform = ststm32\nboard_build.mcu = stm32g071rbt6\n"
          "[env:g4]\nplatform = ststm32\nboard_build.mcu = stm32g474ret6\n"
          "[env:l0]\nplatform = ststm32\nboard_build.mcu = stm32l053r8t6\n"
          "[env:l4]\nplatform = ststm32\nboard_build.mcu = stm32l476rgt6\n"
          "[env:h7]\nplatform = ststm32\nboard_build.mcu = stm32h743vit6\n"
          "[env:u5]\nplatform = ststm32\nboard_build.mcu = stm32u575zit6\n"
          "[env:nuc]\nplatform = ststm32\nboard = nucleo_f446re\n"
          "[env:esp]\nplatform = espressif32\nboard = esp32dev\n");
    const auto fb = read_project(root / "fallbacks", why);
    assert(fb);
    assert(stm.openocd_target_cfg(*fb, "f0") == "target/stm32f0x.cfg");
    assert(stm.openocd_target_cfg(*fb, "f1") == "target/stm32f1x.cfg");
    assert(stm.openocd_target_cfg(*fb, "bp") == "target/stm32f1x.cfg");
    assert(stm.openocd_target_cfg(*fb, "f3") == "target/stm32f3x.cfg");
    assert(stm.openocd_target_cfg(*fb, "f4") == "target/stm32f4x.cfg");
    assert(stm.openocd_target_cfg(*fb, "f7") == "target/stm32f7x.cfg");
    assert(stm.openocd_target_cfg(*fb, "g0") == "target/stm32g0x.cfg");
    assert(stm.openocd_target_cfg(*fb, "g4") == "target/stm32g4x.cfg");
    assert(stm.openocd_target_cfg(*fb, "l0") == "target/stm32l0.cfg");
    assert(stm.openocd_target_cfg(*fb, "l4") == "target/stm32l4x.cfg");
    assert(stm.openocd_target_cfg(*fb, "h7") == "target/stm32h7x.cfg");
    assert(stm.openocd_target_cfg(*fb, "u5") == "target/stm32u5x.cfg");
    assert(stm.openocd_target_cfg(*fb, "nuc") == "target/stm32f4x.cfg");
    assert(stm.openocd_target_cfg(*fb, "esp").empty());

    // 3. Board manifest lookup under <core>/platforms/ststm32/boards/<board>.json
    const auto pio_core = root / "pio_core";
    write(pio_core / "platforms/ststm32/boards/custom_target_board.json",
          R"({"debug":{"openocd_target":"stm32g4x"}})");
    write(pio_core / "platforms/ststm32/boards/custom_cfg_board.json",
          R"({"debug":{"openocd_target":"target/stm32l4x.cfg"}})");
    write(pio_core / "platforms/ststm32/boards/custom_board_only.json",
          R"({"debug":{"openocd_board":"st_nucleo_f4"}})");
    write(pio_core / "platforms/ststm32@17.0.0/boards/versioned_board.json",
          R"({"debug":{"openocd_target":"stm32h7x"}})");
    ScopedEnv override_core("PLATFORMIO_CORE_DIR", os::to_utf8(pio_core));

    write(root / "manifests/platformio.ini",
          "[env:t1]\nplatform = ststm32\nboard = custom_target_board\n"
          "[env:t2]\nplatform = ststm32\nboard = custom_cfg_board\n"
          "[env:b1]\nplatform = ststm32\nboard = custom_board_only\n"
          "[env:v1]\nplatform = ststm32@17.0.0\nboard = versioned_board\n");
    const auto man = read_project(root / "manifests", why);
    assert(man);
    assert(stm.openocd_target_cfg(*man, "t1") == "target/stm32g4x.cfg");
    assert(stm.openocd_target_cfg(*man, "t2") == "target/stm32l4x.cfg");
    assert(stm.openocd_target_cfg(*man, "b1") == "board/st_nucleo_f4.cfg");
    assert(stm.openocd_target_cfg(*man, "v1") == "target/stm32h7x.cfg");

    fs::remove_all(root);
}

void stm32_agent_commands_and_image() {
    const fs::path src_agent = fs::path(MALGRIND_SOURCE_DIR) / "agent/stm32";
    assert(fs::exists(src_agent / "family.json"));
    assert(fs::exists(src_agent / "inject.py"));
    assert(fs::exists(src_agent / "autostart.cpp"));
    assert(fs::exists(src_agent / "README.md"));

    const auto root = fresh("malgrind_families_stm32_cmds");
    const auto proj_dir = root / "my stm proj";
    write(proj_dir / "platformio.ini", "[env:nucleo_f401re]\nplatform = ststm32\nframework = stm32cube\n");
    std::string why;
    const auto proj = read_project(proj_dir, why);
    assert(proj);

    const ToolContext tools{{}, "/opt/pio", MALGRIND_SOURCE_DIR};
    const Family& stm = stm32_family();
    const auto cmds = stm.agent_commands(tools, *proj, "nucleo_f401re", "/dev/ttyACM0", {.rate_hz = 500}, false);
    assert(!fs::exists(proj_dir / ".pio"));
    assert(cmds.elf == fs::absolute(proj_dir / ".pio/malgrind/build/nucleo_f401re/firmware.elf").lexically_normal());
    assert(std::ranges::find(cmds.build.argv, "upload") == cmds.build.argv.end());
    assert(std::ranges::find(cmds.upload.argv, "upload") != cmds.upload.argv.end());
    assert(std::ranges::find(cmds.upload.argv, "/dev/ttyACM0") != cmds.upload.argv.end());

    const auto has_env = [&](const std::string& entry) {
        return std::ranges::find(cmds.build.env, entry) != cmds.build.env.end();
    };
    const fs::path agent_root = fs::absolute(MALGRIND_SOURCE_DIR).lexically_normal();
    assert(has_env("PLATFORMIO_BUILD_DIR=" + os::to_utf8(proj_dir / ".pio/malgrind/build")));
    assert(has_env("PLATFORMIO_EXTRA_SCRIPTS=pre:" + os::to_utf8(agent_root / "agent/stm32/inject.py")));
    assert(has_env("MALGRIND_AGENT_DIR=" + os::to_utf8(agent_root / "agent/stm32")));
    assert(has_env("MALGRIND_CORE_DIR=" + os::to_utf8(agent_root / "core")));
    assert(has_env("MALGRIND_PLATFORMS_DIR=" + os::to_utf8(agent_root / "platforms")));
    assert(has_env("MALGRIND_RATE_HZ=500"));
    assert(has_env("MALGRIND_START_SAMPLING=1"));
    assert(cmds.build.env == cmds.upload.env);

    // Default rate_hz (0) resolves to kDefaultRateHz (997)
    const auto def_cmds = stm.agent_commands(tools, *proj, "nucleo_f401re", "/dev/ttyACM0", {}, true);
    assert(std::ranges::find(def_cmds.build.env, "MALGRIND_RATE_HZ=997") != def_cmds.build.env.end());

    // Missing agent files throw cleanly
    bool missing_threw = false;
    try {
        (void)stm.agent_commands({{}, "/opt/pio", root / "missing-agent"}, *proj, "nucleo_f401re", "/dev/ttyACM0", {},
                                 false);
    } catch (const std::runtime_error&) {
        missing_threw = true;
    }
    assert(missing_threw);

    // is_agent_image checks v1, v2, boundary straddling, and plain firmware
    write(root / "v1.bin", std::string("boot") + std::string(kStm32AgentMarker) + "tail");
    write(root / "v2.bin", std::string("boot") + std::string(kStm32AgentMarkerV2) + "tail");
    std::string straddle(70000, 'x');
    straddle.insert(65536 - 6, kStm32AgentMarkerV2);
    write(root / "straddle.bin", straddle);
    write(root / "plain.bin", "ordinary user firmware binary");
    assert(stm.is_agent_image(root / "v1.bin"));
    assert(stm.is_agent_image(root / "v2.bin"));
    assert(stm.is_agent_image(root / "straddle.bin"));
    assert(!stm.is_agent_image(root / "plain.bin"));
    assert(!stm.is_agent_image(root / "missing.bin"));

    fs::remove_all(root);
}

void stm32_helpers_and_mocked_flash_ops() {
    // 1. Pure helpers
    assert(parse_openocd_idcode("0xe0042000: 10076413 \n") == 0x10076413u);
    assert(parse_openocd_idcode("AP ID register 0x24770011\nIDCODE 0x2ba01477\n") == 0x2ba01477u);
    assert(parse_openocd_idcode("AP ID register 0x24770011\n") == 0x24770011u);
    assert(parse_openocd_idcode("0x10076413") == 0x10076413u);
    assert(!parse_openocd_idcode("0x00000000"));
    assert(!parse_openocd_idcode("0xffffffff"));
    assert(!parse_openocd_idcode("Error: memory read failed"));
    assert(!parse_openocd_idcode("invalid command name \"dap\""));

    assert(parse_openocd_flash_size_bytes(
               "#0 : stm32f4x.flash (stm32f4x) at 0x08000000, size 0x00080000, buswidth 0, chipwidth 0") ==
           524288u);
    assert(parse_openocd_flash_size_bytes("flash size = 512kbytes\n") == 512u * 1024u);
    assert(parse_openocd_flash_size_bytes("flash size: 256 KB") == 256u * 1024u);
    assert(parse_openocd_flash_size_bytes("flash size = 1 MB") == 1024u * 1024u);
    assert(!parse_openocd_flash_size_bytes("flash 'stm32f4x' found at 0x08000000"));
    assert(!parse_openocd_flash_size_bytes("size 0"));
    assert(!parse_openocd_flash_size_bytes("Error: flash probe failed"));

    const std::string key = format_stm32_board_key(0x10076413u, 524288u);
    assert(key == "10076413-00080000");
    assert(format_stm32_board_key(0x10076413u) == "10076413");
    assert(format_stm32_board_key(0u, 524288u).empty());
    assert(backup_path("/tmp/backups", key, "20261127-120000").has_value());

    assert((stm32_openocd_args("0483", "374b", "SN01", "target/stm32f1x.cfg") ==
            std::vector<std::string>{"-f", "interface/stlink.cfg", "-c", "adapter serial SN01", "-f", "target/stm32f1x.cfg"}));
    assert((stm32_openocd_args("0d28", "0204", "", "target/stm32f4x.cfg") ==
            std::vector<std::string>{"-f", "interface/cmsis-dap.cfg", "-f", "target/stm32f4x.cfg"}));
    assert((stm32_openocd_args("", "", "", "target/stm32l4x.cfg") ==
            std::vector<std::string>{"-f", "interface/stlink.cfg", "-f", "target/stm32l4x.cfg"}));

    // 2. Mocked OpenOCD identify, read_flash, write_flash
    ScopedEnv save_path("PATH", "");
    ScopedEnv save_core("PLATFORMIO_CORE_DIR", std::nullopt);
    ScopedEnv save_drive("SystemDrive", std::nullopt);
    ScopedEnv save_rpc("FAKE_OPENOCD_RPC_LOG", std::nullopt);
    ScopedEnv save_id("FAKE_OPENOCD_IDCODE", std::nullopt);
    ScopedEnv save_sz("FAKE_OPENOCD_FLASH_SIZE", std::nullopt);
    ScopedEnv save_mdw_fail("FAKE_OPENOCD_MDW_FAIL", std::nullopt);
    ScopedEnv save_agent("FAKE_AGENT_IMAGE", std::nullopt);
    ScopedEnv save_target("MALGRIND_STM32_TARGET", std::nullopt);

    const auto root = fresh("malgrind_families_stm32_ocd");
    const auto home = root / "home";
    const auto pkg = home / ".platformio/packages/tool-openocd";
    const auto exe = pkg / "bin" / os::exe_name("openocd");
    fs::create_directories(exe.parent_path());
    fs::copy_file(MALGRIND_FAKE_TOOL, exe, fs::copy_options::overwrite_existing);
    if constexpr (os::kind() != os::Kind::Windows) {
        fs::permissions(exe, fs::perms::owner_all);
    }
    write(pkg / "scripts/interface/stlink.cfg", "# stlink\n");

    const auto rpc_log = root / "rpc.log";
    os::set_env("FAKE_OPENOCD_RPC_LOG", os::to_utf8(rpc_log));
    os::set_env("FAKE_OPENOCD_IDCODE", "0x10076413");
    os::set_env("FAKE_OPENOCD_FLASH_SIZE", "0x00020000"); // 128 KiB
    os::set_env("MALGRIND_STM32_TARGET", "target/stm32f4x.cfg");

    const Family& stm = stm32_family();
    const ToolContext tools{home, {}, {}};

    const BoardIdentity id = stm.identify(tools, "", {});
    assert(id.key == "10076413-00020000");
    assert(id.flash_bytes == 131072u);
    assert(id.chip == "STM32F4");
    {
        const std::string rpc = slurp(rpc_log);
        assert(contains(rpc, "init\n"));
        assert(contains(rpc, "mdw 0xE0042000\n"));
        assert(contains(rpc, "flash probe 0\n"));
        assert(contains(rpc, "flash banks\n"));
        assert(!contains(rpc, "reset\n"));
    }

    // Fallback to `dap info` when `mdw` fails
    fs::remove(rpc_log);
    os::set_env("FAKE_OPENOCD_MDW_FAIL", "1");
    os::set_env("FAKE_OPENOCD_IDCODE", "0x10036410"); // STM32F1 medium-density
    const BoardIdentity dap_id = stm.identify(tools, "", {});
    assert(dap_id.key == "10036410-00020000");
    assert(dap_id.chip == "STM32F1");
    assert(contains(slurp(rpc_log), "dap info\n"));
    os::unset_env("FAKE_OPENOCD_MDW_FAIL");

    // read_flash into a path containing a space
    fs::remove(rpc_log);
    const fs::path dump_file = root / "my backup/flash dump.bin";
    fs::create_directories(dump_file.parent_path());
    stm.read_flash(tools, "", id, dump_file, {});
    assert(fs::exists(dump_file) && fs::file_size(dump_file) == id.flash_bytes);
    assert(!stm.is_agent_image(dump_file));
    {
        const std::string rpc = slurp(rpc_log);
        assert(contains(rpc, "init\n"));
        assert(contains(rpc, "halt\n"));
        assert(contains(rpc, "dump_image "));
        assert(contains(rpc, "0x08000000 131072"));
        assert(contains(rpc, "resume\n"));
    }

    // write_flash validates image file and invokes program ... 0x08000000 verify reset
    fs::remove(rpc_log);
    stm.write_flash(tools, "", dump_file, {});
    {
        const std::string rpc = slurp(rpc_log);
        assert(contains(rpc, "program "));
        assert(contains(rpc, "0x08000000 verify reset\n"));
    }

    bool missing_img_threw = false;
    try {
        stm.write_flash(tools, "", root / "no_such_image.bin", {});
    } catch (const std::runtime_error&) {
        missing_img_threw = true;
    }
    assert(missing_img_threw);

    // Agent image marker detection on read_flash output
    os::set_env("FAKE_AGENT_IMAGE", "v2");
    const fs::path agent_dump = root / "agent_dump.bin";
    stm.read_flash(tools, "", id, agent_dump, {});
    assert(stm.is_agent_image(agent_dump));

    fs::remove_all(root);
}

} // namespace

int main() {
    registry_lookups();
    base_family_defaults();
    esp32_metadata_and_usb();
    project_matching_and_problems();
    agent_commands_preview_and_write();
    esptool_parsing_and_agent_image();
    sdkconfig_merge_checks();
    esp32_extra_symbol_files();
    esp32_openocd_target_inference();
    stm32_metadata_and_usb();
    stm32_project_and_target_inference();
    stm32_agent_commands_and_image();
    stm32_helpers_and_mocked_flash_ops();
    std::puts("families ok");
}

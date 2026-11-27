// Chip family interface, registry, and ESP32 family implementation self-checks.
#include "env.hpp"
#include "families/esp32.hpp"
#include "families/family.hpp"
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
    assert(!families.empty());
    const Family* esp = family_by_id("esp32");
    assert(esp == &esp32_family());
    assert(std::ranges::find(families, esp) != families.end());
    assert(family_by_id("unknown") == nullptr && family_by_id("") == nullptr);

    assert(family_for_arch(Arch::Xtensa) == esp);
    assert(family_for_arch(Arch::RiscV32) == esp);
    assert(family_for_arch(Arch::Host) == nullptr);
    assert(family_for_arch(Arch::ArmV7M) == nullptr);

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

    // STM32 ini -> no family matches, combined why names ESP32 / ststm32
    write(dir / "platformio.ini", "[env:a]\nplatform = ststm32\nframework = stm32cube\n");
    const auto stm_proj = read_project(dir, why);
    assert(stm_proj);
    assert(family_for_project(*stm_proj, "a", why) == nullptr);
    assert(contains(why, "ststm32") && contains(why, "ESP32"));

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
    std::puts("families ok");
}

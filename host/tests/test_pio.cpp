// PlatformIO integration self-check: project parsing, the sdkconfig merge, the build invocation and process output.
#include "ops.hpp"
#include "pio.hpp"
#include "process.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }
bool any_contains(const std::vector<std::string>& v, std::string_view part) {
    return std::ranges::any_of(v, [&](const std::string& s) { return contains(s, part); });
}
fs::path fresh(const char* name) {
    // canonical: on macOS /tmp and /var are symlinks, and the current directory is reported without them
    const auto p = fs::canonical(fs::temp_directory_path()) / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}
void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
}
std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

void read_envs() {
    const auto dir = fresh("malgrind_pio_a");
    write(dir / "platformio.ini", "[platformio]\ndefault_envs = b, a\n[env:a]\nboard = x\n[env:b]\n[env]\nfoo = 1\n");
    std::string why;
    const auto p = read_project(dir, why);
    assert(p && p->dir == dir && p->envs == (std::vector<std::string>{"a", "b"}) && p->default_env == "b"); // bare [env] is no env

    write(dir / "platformio.ini", "[env:only]\n");
    assert(read_project(dir, why)->default_env == "only");

    assert(!read_project(dir / "missing", why) && contains(why, "platformio.ini"));
    write(dir / "empty/platformio.ini", "[platformio]\n");
    assert(!read_project(dir / "empty", why) && contains(why, "[env:"));

    // PlatformIO's ESP-IDF builder stops with "whitespace in project paths": say so before it does
    write(dir / "my proj/platformio.ini", "[env:a]\n");
    assert(!read_project(dir / "my proj", why) && contains(why, "space"));
    fs::remove_all(dir);
}

// pio runs with `-d <project>` and so changes directory: no path it is given may depend on where we run.
// The agent is an ESP-IDF component: other frameworks and chips build and flash fine, and then show nothing.
void unsupported_projects_are_refused() {
    const auto dir = fresh("malgrind_pio_fw");
    const auto prepare = [&](const char* ini, const char* env) {
        write(dir / "platformio.ini", ini);
        fs::create_directories(dir / "bin");
        write(dir / "bin/pio", "#!/bin/sh\n");
        setenv("PATH", (dir / "bin").c_str(), 1);
        try {
            (void)prepare_project(dir, dir, env, "/dev/ttyUSB0");
            return std::string("ok");
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
    };
    assert(prepare("[env:a]\nplatform = espressif32\nframework = espidf\n", "a") == "ok");
    assert(prepare("[env:a]\nplatform = espressif32@6.9.0\nframework = arduino, espidf\n", "a") == "ok"); // IDF with Arduino as a component
    assert(prepare("[env:a]\nboard = esp32dev\n", "a") == "ok"); // nothing said (extends, a shared section...): let the build decide
    assert(prepare("[env]\nframework = espidf\n[env:a]\nplatform = espressif32\n", "a") == "ok"); // the shared [env] section counts
    assert(contains(prepare("[env:a]\nplatform = espressif32\nframework = arduino\n", "a"), "arduino"));
    assert(contains(prepare("[env:a]\nplatform = espressif32\nframework = arduino\n[env:b]\nframework = espidf\n", "a"), "espidf")); // per env
    assert(contains(prepare("[env:a]\nplatform = ststm32\nframework = stm32cube\n", "a"), "ESP32"));
    assert(contains(prepare("[env:a]\nplatform = espressif32\n", "missing"), "no environment"));
    fs::remove_all(dir);
}

void relative_paths_become_absolute() {
    const auto dir = fresh("malgrind_pio_rel");
    write(dir / "proj/platformio.ini", "[env:esp32dev]\n");
    write(dir / "agent-copy/agent/esp32/inject.py", "");
    write(dir / "agent-copy/agent/esp32/sdkconfig.agent", "CONFIG_X=1\n");
    const auto saved = fs::current_path();
    fs::current_path(dir);

    std::string why;
    const auto p = read_project("proj", why);
    assert(p && p->dir == dir / "proj");

    const auto inv = agent_build("/opt/pio", *p, "esp32dev", "/dev/ttyUSB0", "agent-copy"); // a relative --agent-dir too
    for (const auto& entry : inv.env) {
        const auto value = entry.substr(entry.find('=') + 1);
        assert(value.starts_with("/") || value.starts_with("pre:/")); // every path in the environment is absolute
    }
    assert(fs::path(inv.argv[3]).is_absolute());

    fs::current_path(dir / "proj");
    const auto here = read_project(".", why);
    assert(here && here->dir == dir / "proj"); // neither "/." nor a trailing "/"
    fs::current_path(saved);
    fs::remove_all(dir);
}

void merge_keeps_user_keys() {
    const auto agent = slurp(fs::path(MALGRIND_SOURCE_DIR) / "agent/esp32/sdkconfig.agent");
    assert(contains(agent, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600"));
    const auto merged = merge_sdkconfig("CONFIG_A=1\n# CONFIG_GPTIMER_ISR_IRAM_SAFE is not set\nCONFIG_ESP_CONSOLE_UART_BAUDRATE=115200\n"
                                        "CONFIG_ESP_CONSOLE_UART_DEFAULT=y\n# CONFIG_B is not set",
                                        agent);
    assert(contains(merged, "CONFIG_A=1\n") && contains(merged, "# CONFIG_B is not set\n")); // user keys survive
    assert(contains(merged, "CONFIG_GPTIMER_ISR_IRAM_SAFE=y\n") && contains(merged, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600\n"));
    assert(contains(merged, "# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set\n") && contains(merged, "CONFIG_ESP_CONSOLE_UART_CUSTOM=y\n"));
    assert(!contains(merged, "115200") && !contains(merged, "IRAM_SAFE is not set") && !contains(merged, "UART_DEFAULT=y"));
    assert(!contains(merged, "DEFAULT and CUSTOM are one")); // the agent file's comments are not copied
    assert(merge_sdkconfig("", agent).ends_with("\n") && merge_sdkconfig("X", "CONFIG_Y=1\n") == "X\nCONFIG_Y=1\n");
}

void agent_build_leaves_the_project_alone() {
    const auto dir = fresh("malgrind_pio_\xc3\xa4/proj"); // non-ASCII, still one argv element and never a shell string
    write(dir / "sdkconfig.esp32dev", "CONFIG_A=1\nCONFIG_ESP_CONSOLE_UART_BAUDRATE=115200\n");
    write(dir / "platformio.ini", "[env:esp32dev]\n");
    const auto before = slurp(dir / "sdkconfig.esp32dev");
    const ProjectInfo project{dir, {"esp32dev"}, "esp32dev"};
    const fs::path agent = MALGRIND_SOURCE_DIR;
    const auto inv = agent_build("/opt/pio", project, "esp32dev", "/dev/ttyUSB0", agent);

    assert(inv.argv == (std::vector<std::string>{"/opt/pio", "run", "-d", dir.string(), "-e", "esp32dev", "-t", "upload",
                                                 "--upload-port", "/dev/ttyUSB0"}));
    assert(std::ranges::find(inv.env, "PLATFORMIO_BUILD_DIR=" + (dir / ".pio/malgrind/build").string()) != inv.env.end());
    assert(std::ranges::find(inv.env, "PLATFORMIO_EXTRA_SCRIPTS=pre:" + (agent / "agent/esp32/inject.py").string()) != inv.env.end());
    assert(std::ranges::find(inv.env, "MALGRIND_AGENT_DIR=" + (agent / "agent/esp32").string()) != inv.env.end());
    assert(std::ranges::find(inv.env, "MALGRIND_SDKCONFIG=" + (dir / ".pio/malgrind/sdkconfig.esp32dev").string()) != inv.env.end());

    const auto merged = slurp(dir / ".pio/malgrind/sdkconfig.esp32dev");
    assert(contains(merged, "CONFIG_A=1") && contains(merged, "BAUDRATE=921600") && !contains(merged, "115200"));
    assert(slurp(dir / "sdkconfig.esp32dev") == before); // the user's own file is untouched
    assert(fs::exists(dir / ".pio/malgrind/sdkconfig.esp32dev") && !fs::exists(dir / ".pio/malgrind/build")); // the seed is not in the dir pio wipes
    assert(agent_elf(project, "esp32dev") == dir / ".pio/malgrind/build/esp32dev/firmware.elf");
    fs::remove_all(dir.parent_path());
}

void plain_flash_has_no_agent() {
    const ProjectInfo project{"/tmp/proj", {"esp32dev"}, "esp32dev"};
    const auto inv = plain_flash("/opt/pio", project, "esp32dev", "/dev/ttyUSB0");
    assert(inv.argv.size() == 10 && inv.argv[0] == "/opt/pio" && inv.argv[8] == "--upload-port");
    assert(inv.env.empty() && !any_contains(inv.argv, "malgrind"));
}

void find_pio_fallback() {
    const auto home = fresh("malgrind_pio_home");
    setenv("PATH", "", 1);
    assert(!find_pio(home));
    write(home / ".platformio/penv/bin/pio", "#!/bin/sh\n");
    assert(find_pio(home) == home / ".platformio/penv/bin/pio");
    fs::remove_all(home);
}

void run_lines_streams() {
    std::vector<std::string> lines;
    const int code = run_lines({"/bin/sh", "-c", "echo $X; echo a; echo b >&2; printf 'p1\\rp2\\n'; exit 3"}, {"X=1"},
                               [&](std::string_view line) { lines.emplace_back(line); });
    assert(code == 3 && lines == (std::vector<std::string>{"1", "a", "b", "p1", "p2"})); // \r splits progress lines too
    assert(run_lines({"/no/such/program"}, {}, [](std::string_view) {}) != 0);
}

void esptool_command_line() {
    const auto home = fresh("malgrind_pio_esptool");
    write(home / "pio", "#!/usr/bin/env python3\n");
    assert(!esptool(home / "pio", home, "/dev/ttyUSB0", {"chip_id"})); // no esptool package yet
    write(home / ".platformio/packages/tool-esptoolpy/esptool.py", "");
    const auto inv = esptool(home / "pio", home, "/dev/ttyUSB0", {"read_flash", "0", "4096", "out.bin"});
    assert(inv && inv->env.empty());
    const auto script = (home / ".platformio/packages/tool-esptoolpy/esptool.py").string();
    // the interpreter is the two words of the shebang line, then the script, the connection, the command
    assert((inv->argv == std::vector<std::string>{"/usr/bin/env", "python3", script, "--port", "/dev/ttyUSB0", "--baud", "921600",
                                                  "read_flash", "0", "4096", "out.bin"}));
    write(home / "notscript", "binary");
    assert(!esptool(home / "notscript", home, "p", {}));
    fs::remove_all(home);
}

} // namespace

int main() {
    read_envs();
    unsupported_projects_are_refused();
    relative_paths_become_absolute();
    merge_keeps_user_keys();
    agent_build_leaves_the_project_alone();
    plain_flash_has_no_agent();
    find_pio_fallback();
    run_lines_streams();
    esptool_command_line();
    std::puts("pio ok");
}

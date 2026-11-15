// PlatformIO integration self-check: project parsing, the sdkconfig merge, the build invocation and process output.
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
    const auto p = fs::temp_directory_path() / name;
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
    assert(std::ranges::find(inv.env, "PLATFORMIO_BUILD_DIR=" + (dir / ".pio/malgrind").string()) != inv.env.end());
    assert(std::ranges::find(inv.env, "PLATFORMIO_EXTRA_SCRIPTS=pre:" + (agent / "agent/esp32/inject.py").string()) != inv.env.end());
    assert(std::ranges::find(inv.env, "MALGRIND_AGENT_DIR=" + (agent / "agent/esp32").string()) != inv.env.end());
    assert(std::ranges::find(inv.env, "MALGRIND_SDKCONFIG=" + (dir / ".pio/malgrind/sdkconfig.esp32dev").string()) != inv.env.end());

    const auto merged = slurp(dir / ".pio/malgrind/sdkconfig.esp32dev");
    assert(contains(merged, "CONFIG_A=1") && contains(merged, "BAUDRATE=921600") && !contains(merged, "115200"));
    assert(slurp(dir / "sdkconfig.esp32dev") == before); // the user's own file is untouched
    assert(agent_elf(project, "esp32dev") == dir / ".pio/malgrind/esp32dev/firmware.elf");
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

} // namespace

int main() {
    read_envs();
    merge_keeps_user_keys();
    agent_build_leaves_the_project_alone();
    plain_flash_has_no_agent();
    find_pio_fallback();
    run_lines_streams();
    std::puts("pio ok");
}

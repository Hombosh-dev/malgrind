// Safe flash self-check: the backup taken before the agent goes on, the restore, and the list of methods.
// pio and esptool are shell scripts that log their command line, so no board is needed.
#include "backup.hpp"
#include "ops.hpp"
#include "pty.hpp"
#include "stream.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr const char* kMac = "24-0a-c4-aa-bb-0c";

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }
std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path& file, std::string_view text, bool executable = false) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
    if (executable) fs::permissions(file, fs::perms::owner_exec, fs::perm_options::add);
}

// A home with a fake pio and a fake esptool.py (a shell script: pio's shebang names /bin/sh, so it runs as one).
struct Fake {
    fs::path root = fs::canonical(fs::temp_directory_path()) / "malgrind_flash";
    fs::path home = root / "home", backups = root / "backups", project = root / "proj", log = root / "log";

    Fake() {
        fs::remove_all(root);
        write(home / ".platformio/penv/bin/pio", "#!/bin/sh\necho \"pio $* build=$PLATFORMIO_BUILD_DIR\" >> \"$FAKE_LOG\"\n", true);
        write(home / ".platformio/packages/tool-esptoolpy/esptool.py", R"(echo "esptool $*" >> "$FAKE_LOG"
shift 4   # --port P --baud B
echo "Chip is ESP32-D0WD-V3"
echo "MAC: 24:0a:c4:AA:bb:0c"
case "$1" in
flash_id) echo "Detected flash size: 4MB" ;;
read_flash)
  [ "$FAKE_FAIL" = read_flash ] && { echo "A fatal error occurred: timeout" >&2; exit 2; }
  head -c 4194304 /dev/zero > "$4"
  [ -n "$FAKE_AGENT_IMAGE" ] && printf 'malgrind-agent-v1' | dd of="$4" bs=1 seek=1000 conv=notrunc 2>/dev/null ;;
esac
exit 0
)");
        write(project / "platformio.ini", "[env:esp32dev]\nplatform = espressif32\nframework = espidf\n");
        setenv("FAKE_LOG", log.c_str(), 1);
        setenv("PATH", "/usr/bin:/bin", 1); // not the real pio
        unsetenv("FAKE_FAIL");
        unsetenv("FAKE_AGENT_IMAGE");
    }
    ~Fake() { fs::remove_all(root); }
    [[nodiscard]] std::string calls() const { return slurp(log); }
    [[nodiscard]] Prepared prepared(const std::string& port) const { return prepare_project(home, project, "", port); }
    // backups of the board, finished ones only
    [[nodiscard]] std::size_t backup_count() const {
        std::size_t n = 0;
        for (const auto& e : fs::directory_iterator(backups / kMac, std::filesystem::directory_options::none))
            n += e.path().extension() == ".bin";
        return n;
    }
};

struct Lines {
    std::string text;
    LineFn fn() { return [this](std::string_view l) { text.append(l).append("\n"); }; }
};

void backs_up_then_flashes() {
    Fake fake;
    Pty board; // a board that is not running the agent: silent
    Lines lines;
    flash_agent(fake.prepared(board.slave), fake.home, MALGRIND_SOURCE_DIR, fake.backups, lines.fn());
    const auto calls = fake.calls();
    assert(contains(calls, "flash_id") && contains(calls, "read_flash 0 4194304"));
    assert(calls.find("read_flash") < calls.find("pio run")); // the backup comes first
    assert(contains(calls, "-t upload") && contains(calls, "build=" + (fake.project / ".pio/malgrind/build").string())); // then the agent build
    assert(fake.backup_count() == 1 && contains(lines.text, "backup saved"));
    for (const auto& e : fs::directory_iterator(fake.backups / kMac)) assert(e.path().extension() == ".bin" && fs::file_size(e.path()) == 4194304);
}

void no_backup_no_flash() {
    Fake fake;
    Pty board;
    setenv("FAKE_FAIL", "read_flash", 1);
    Lines lines;
    bool threw = false;
    try {
        flash_agent(fake.prepared(board.slave), fake.home, MALGRIND_SOURCE_DIR, fake.backups, lines.fn());
    } catch (const std::exception& e) {
        threw = contains(e.what(), "esptool failed") && contains(e.what(), "timeout");
    }
    assert(threw);
    assert(!contains(fake.calls(), "pio run")); // nothing was flashed
    assert(fake.backup_count() == 0 && fs::is_empty(fake.backups / kMac)); // and no half-written file stays
}

void board_with_agent_is_not_backed_up_again() {
    Fake fake;
    Pty board;
    std::thread agent([&] {
        std::this_thread::sleep_for(300ms);
        board.send(make_stream(1));
    });
    Lines lines;
    flash_agent(fake.prepared(board.slave), fake.home, MALGRIND_SOURCE_DIR, fake.backups, lines.fn());
    agent.join();
    assert(!contains(fake.calls(), "esptool") && contains(fake.calls(), "pio run") && !fs::exists(fake.backups));
    assert(contains(lines.text, "already runs malgrind"));
}

void our_own_image_is_not_kept() {
    Fake fake; // frames not arriving (the agent is installed but silent, or the port is wrong) but the flash holds our build
    Pty board;
    setenv("FAKE_AGENT_IMAGE", "1", 1);
    Lines lines;
    flash_agent(fake.prepared(board.slave), fake.home, MALGRIND_SOURCE_DIR, fake.backups, lines.fn());
    assert(fake.backup_count() == 0 && contains(fake.calls(), "pio run") && contains(lines.text, "no backup kept"));
}

void restore_writes_the_backup_back() {
    Fake fake;
    Pty board;
    Lines lines;
    // no backup yet: refused, and the message names the board
    bool threw = false;
    try {
        restore_flash(*find_pio(fake.home), fake.home, board.slave, fake.backups, lines.fn());
    } catch (const std::exception& e) {
        threw = contains(e.what(), "no backup") && contains(e.what(), kMac);
    }
    assert(threw && !contains(fake.calls(), "write_flash"));

    flash_agent(fake.prepared(board.slave), fake.home, MALGRIND_SOURCE_DIR, fake.backups, lines.fn());
    const auto saved = *latest_backup(fake.backups, kMac);
    restore_flash(*find_pio(fake.home), fake.home, board.slave, fake.backups, lines.fn());
    const auto calls = fake.calls();
    // the whole image goes back to address 0, header bytes untouched
    assert(contains(calls, "write_flash --flash_mode keep --flash_freq keep --flash_size keep 0 " + saved.string()));
}

void methods_say_what_is_missing() {
    const auto find = [](const std::vector<Method>& list, std::string_view id) {
        return *std::ranges::find_if(list, [&](const Method& m) { return m.id == id; });
    };
    auto m = methods({.agent = true}, true, "");
    assert(find(m, "attach").available && find(m, "attach").why.empty() && find(m, "new_run").available);
    assert(!find(m, "no_flash").available && contains(find(m, "no_flash").why, "debug probe"));

    m = methods({}, false, ""); // nothing running, no PlatformIO
    assert(!find(m, "attach").available && contains(find(m, "attach").why, "no malgrind agent"));
    assert(!find(m, "new_run").available && contains(find(m, "new_run").why, "PlatformIO"));

    m = methods({}, true, "environment 'x' uses framework 'arduino'");
    assert(!find(m, "new_run").available && contains(find(m, "new_run").why, "arduino"));

    m = methods({.busy = true}, true, ""); // somebody else has the port
    assert(!find(m, "attach").available && contains(find(m, "attach").why, "busy"));
    assert(!find(m, "new_run").available && contains(find(m, "new_run").why, "busy"));
}

} // namespace

int main() {
    backs_up_then_flashes();
    no_backup_no_flash();
    board_with_agent_is_not_backed_up_again();
    our_own_image_is_not_kept();
    restore_writes_the_backup_back();
    methods_say_what_is_missing();
    std::puts("flash ok");
}

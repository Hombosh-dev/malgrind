// Flash backup self-check: parsing esptool's output, choosing a backup, recognising our own agent image.
#include "backup.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

fs::path fresh_root(const char* name) {
    const auto root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void touch(const fs::path& file, std::string_view content = "x") {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
}

void parse_esptool_output() {
    assert(parse_mac("MAC: 24:0a:c4:AA:bb:0C") == "24-0a-c4-aa-bb-0c");
    assert(parse_mac("Chip is ESP32\nMAC: 24:0a:c4:aa:bb:0c\nUploading stub...\n") == "24-0a-c4-aa-bb-0c");
    assert(!parse_mac("no mac here") && !parse_mac("MAC: 24:0a:c4") && !parse_mac("MAC: zz:0a:c4:aa:bb:0c"));
    assert(parse_flash_size("Detected flash size: 4MB") == 4u * 1024 * 1024);
    assert(parse_flash_size("Device: 4016\nDetected flash size: 16MB\n") == 16u * 1024 * 1024);
    assert(parse_flash_size("Detected flash size: 512KB") == 512u * 1024);
    assert(!parse_flash_size("Detected flash size: Unknown") && !parse_flash_size("") && !parse_flash_size("Detected flash size: 4GB"));
}

void picks_the_newest_backup() {
    const auto root = fresh_root("malgrind_backup_a");
    assert(!latest_backup(root, "24-0a-c4-aa-bb-0c")); // nothing yet
    touch(root / "24-0a-c4-aa-bb-0c/20261116-100000.bin");
    touch(root / "24-0a-c4-aa-bb-0c/20261116-120000.bin");
    touch(root / "24-0a-c4-aa-bb-0c/20261116-110000.bin.part"); // unfinished: ignored
    touch(root / "11-22-33-44-55-66/20261117-000000.bin");      // another board
    assert(latest_backup(root, "24-0a-c4-aa-bb-0c") == root / "24-0a-c4-aa-bb-0c/20261116-120000.bin");
    assert(!latest_backup(root, "aa-aa-aa-aa-aa-aa"));
    fs::remove_all(root);
}

void mac_is_never_a_path() {
    const auto root = fresh_root("malgrind_backup_b");
    touch(root / "secret/20261116-100000.bin");
    assert(!latest_backup(root, "../b/secret") && !latest_backup(root, "secret") && !latest_backup(root, ""));
    assert(!backup_path(root, "../x", "20261116-100000"));
    assert(backup_path(root, "24-0a-c4-aa-bb-0c", "20261116-100000") == root / "24-0a-c4-aa-bb-0c/20261116-100000.bin.part");
    fs::remove_all(root);
}

void recognises_the_agent_image() {
    const auto root = fresh_root("malgrind_backup_c");
    touch(root / "agent.bin", std::string("\xe9\x03junk") + std::string(kAgentMarker) + "\0more");
    touch(root / "plain.bin", "\xe9\x03 only the user's firmware here");
    // the marker straddling a read boundary is still found
    std::string big(70000, 'a');
    big.insert(65536 - 5, kAgentMarker);
    touch(root / "big.bin", big);
    assert(is_agent_image(root / "agent.bin") && !is_agent_image(root / "plain.bin") && is_agent_image(root / "big.bin"));
    assert(!is_agent_image(root / "missing.bin"));
    fs::remove_all(root);
}

} // namespace

int main() {
    parse_esptool_output();
    picks_the_newest_backup();
    mac_is_never_a_path();
    recognises_the_agent_image();
    std::puts("backup ok");
}

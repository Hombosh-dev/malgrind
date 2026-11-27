// Run store self-check: persistence, crash safety and the id rules.
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace malgrind;
using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

const std::vector<std::uint8_t> kAb{'a', 'b'}, kCd{'c', 'd'};

fs::path fresh_root(const char* name) {
    const auto root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    return root;
}

void create_list_roundtrip() {
    const auto root = fresh_root("malgrind_store_a");
    RunStore store(root);
    {
        auto w = store.create({.name = "water-sim", .project = "/p", .env = "esp32dev", .port = "/dev/ttyUSB0"});
        w.append(0, kAb);
        w.append(250, kCd);
        w.set_target(Arch::Xtensa, 2, 160);
        w.finish(250);
    }
    const auto runs = store.list();
    assert(runs.size() == 1);
    const auto& m = runs[0];
    assert(m.name == "water-sim" && m.env == "esp32dev" && m.duration_ms == 250 && m.arch == Arch::Xtensa);
    assert(m.ncores == 2 && m.cpu_mhz == 160 && !m.has_elf && m.started.size() == 20 && m.started.back() == 'Z');
    const auto chunks = store.chunks(m.id);
    assert(chunks.size() == 2 && chunks[0].t_ms == 0 && chunks[0].bytes == kAb && chunks[1].t_ms == 250 && chunks[1].bytes == kCd);
    assert(!m.settings); // a run made without the settings panel (or before it existed) has none
    fs::remove_all(root);
}

void settings_are_kept_with_the_run() {
    const auto root = fresh_root("malgrind_store_s");
    std::string id;
    {
        RunStore store(root);
        id = store.create({.name = "tuned", .settings = RunSettings{.env = "b", .extra_args = "-v", .rate_hz = 500, .elf = "/x.elf"}}).meta().id;
    }
    RunStore again(root);
    const auto m = again.get(id);
    assert(m && m->settings && m->settings->env == "b" && m->settings->rate_hz == 500 && m->settings->extra_args == "-v" && m->settings->elf == "/x.elf");
    fs::remove_all(root);
}

void unfinished_run_has_duration() {
    const auto root = fresh_root("malgrind_store_b");
    RunStore store(root);
    {
        auto w = store.create({});
        w.append(0, kAb);
        w.append(1200, kCd); // the process dies here: finish() is never called
    }
    assert(store.list().at(0).duration_ms == 1200);
    fs::remove_all(root);
}

void truncated_tail_ignored() {
    const auto root = fresh_root("malgrind_store_c");
    RunStore store(root);
    std::string id;
    {
        auto w = store.create({});
        id = w.meta().id;
        w.append(0, kAb);
        w.append(10, kCd);
    }
    const auto file = root / id / "frames.mlg";
    fs::resize_file(file, fs::file_size(file) - 3);
    const auto chunks = store.chunks(id);
    assert(chunks.size() == 1 && chunks[0].bytes == kAb);
    fs::remove_all(root);
}

void update_and_remove() {
    const auto root = fresh_root("malgrind_store_d");
    std::string id;
    {
        RunStore store(root);
        id = store.create({.name = "old"}).meta().id;
        assert(store.update(id, "water-sim v3", "note")->name == "water-sim v3");
        assert(!store.update("missing", "x", std::nullopt));
    }
    RunStore again(root); // a new store sees the same data
    assert(again.get(id)->name == "water-sim v3" && again.get(id)->notes == "note");
    assert(!again.remove("../etc") && !again.get("a/b") && !again.get("..")); // ids are never paths
    assert(again.remove(id) && again.list().empty() && !again.remove(id));
    fs::remove_all(root);
}

void id_collision() {
    const auto root = fresh_root("malgrind_store_e");
    RunStore store(root);
    const auto a = store.create({}).meta().id, b = store.create({}).meta().id;
    assert(a != b && store.list().size() == 2);
    fs::remove_all(root);
}

void elf_copy() {
    const auto root = fresh_root("malgrind_store_f");
    const auto elf = root / "in.elf";
    fs::create_directories(root);
    std::ofstream(elf) << "ELF!";
    RunStore store(root / "runs");
    {
        auto w = store.create({});
        assert(store.elf(w.meta().id).empty());
        w.set_elf(elf);
        assert(store.get(w.meta().id)->has_elf && fs::file_size(store.elf(w.meta().id)) == 4);
    }
    fs::remove_all(root);
}

void device_id_roundtrip_and_legacy_meta() {
    const auto root = fresh_root("malgrind_store_g");
    std::string id;
    {
        RunStore store(root);
        id = store.create({.name = "dev-run", .port = "ttyUSB0", .device_id = "usb-10c4-ea60-sn-0001"}).meta().id;
    }
    RunStore again(root);
    const auto roundtrip = again.get(id);
    assert(roundtrip && roundtrip->port == "ttyUSB0" && roundtrip->device_id == "usb-10c4-ea60-sn-0001");

    // Old meta.json written before device_id and bytes existed must load cleanly and compute bytes from disk.
    const auto legacy_dir = root / "20260101-000000";
    fs::create_directories(legacy_dir);
    const std::string legacy_meta =
        R"({"id":"20260101-000000","name":"legacy","port":"/dev/ttyUSB0","started":"2026-01-01T00:00:00Z"})";
    std::ofstream(legacy_dir / "meta.json") << legacy_meta;
    std::ofstream(legacy_dir / "frames.mlg", std::ios::binary) << "12345678";
    const auto expected_legacy_bytes = fs::file_size(legacy_dir / "meta.json") + fs::file_size(legacy_dir / "frames.mlg");
    const auto legacy = again.get("20260101-000000");
    assert(legacy && legacy->port == "/dev/ttyUSB0" && legacy->device_id.empty());
    assert(legacy->bytes == expected_legacy_bytes);
    const auto all_runs = again.list();
    assert(all_runs.size() == 2 && all_runs[1].id == "20260101-000000" && all_runs[1].bytes == expected_legacy_bytes);
    fs::remove_all(root);
}

void bytes_tracked_across_writes() {
    const auto root = fresh_root("malgrind_store_bytes");
    const auto elf = root / "in.elf";
    fs::create_directories(root);
    std::ofstream(elf, std::ios::binary) << std::string(512, 'E');

    RunStore store(root / "runs");
    std::string id;
    std::uint64_t final_bytes = 0;
    {
        auto w = store.create({.name = "sized"});
        id = w.meta().id;
        const auto after_create = w.bytes();
        assert(after_create > 0 && w.meta().bytes == after_create);
        assert(store.get(id)->bytes == after_create);

        w.append(10, kAb); // 6-byte header + 2 bytes payload = 8 bytes
        assert(w.bytes() == after_create + 8);
        assert(w.meta().bytes == w.bytes());

        // Chunk > 65535 splits into two records (2 * 6 + 70000 bytes).
        const std::vector<std::uint8_t> big(70000, 0x5a);
        w.append(20, big);
        assert(w.bytes() == after_create + 8 + 12 + big.size());

        const auto before_elf = w.bytes();
        w.set_elf(elf);
        assert(w.bytes() >= before_elf + 512);
        assert(w.meta().bytes == w.bytes());

        w.finish(25);
        final_bytes = w.bytes();
        assert(w.meta().bytes == final_bytes);
    }

    const auto listed = store.list();
    assert(listed.size() == 1 && listed[0].bytes == final_bytes);
    const auto fetched = store.get(id);
    assert(fetched && fetched->bytes == final_bytes);

    {
        std::ifstream meta_in(root / "runs" / id / "meta.json");
        const auto meta_json = nlohmann::json::parse(meta_in);
        assert(meta_json.contains("bytes") && meta_json["bytes"].is_number_unsigned() && meta_json["bytes"] > 0);
    }
    fs::remove_all(root);
}

} // namespace

int main() {
    create_list_roundtrip();
    settings_are_kept_with_the_run();
    unfinished_run_has_duration();
    truncated_tail_ignored();
    update_and_remove();
    id_collision();
    elf_copy();
    device_id_roundtrip_and_legacy_meta();
    bytes_tracked_across_writes();
    std::puts("store ok");
}

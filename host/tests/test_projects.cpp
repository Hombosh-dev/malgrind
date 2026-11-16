// Project store self-check: persistence, id rules, and that a user's own folder is never touched.
#include "projects.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

fs::path fresh(const char* name) {
    const auto root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void create_list_roundtrip() {
    const auto root = fresh("malgrind_projects_a");
    ProjectStore store(root / "p");
    const auto folder = store.create({.name = "demo", .origin = Origin::Folder, .dir = "/some/where", .env = "esp32dev", .trusted = true});
    const auto none = store.create({.name = "Water simulator", .origin = Origin::None});
    assert(folder.id.starts_with("p-") && folder.created.size() == 20 && folder.id != none.id);
    const auto all = store.list();
    assert(all.size() == 2);
    const auto got = store.get(folder.id);
    assert(got && got->name == "demo" && got->origin == Origin::Folder && got->dir == "/some/where" && got->env == "esp32dev" && got->trusted);
    assert(store.get(none.id)->origin == Origin::None && !store.get(none.id)->trusted && store.get(none.id)->dir.empty());
    const nlohmann::json j = *got;
    assert(j["source"] == "folder" && j["has_source"] == true && j["trusted"] == true);
    assert(nlohmann::json(*store.get(none.id))["has_source"] == false);
    fs::remove_all(root);
}

void update_persists() {
    const auto root = fresh("malgrind_projects_b");
    std::string id;
    {
        ProjectStore store(root / "p");
        id = store.create({.name = "x", .origin = Origin::Git, .url = "https://github.com/o/r.git"}).id;
        assert(!store.get(id)->trusted); // a clone is not trusted until the user says so
        const auto after = store.update(id, {.name = "renamed", .trusted = true, .dir = "/clone/firmware/esp32"});
        assert(after && after->name == "renamed" && after->trusted && after->url == "https://github.com/o/r.git");
        assert(!store.update("missing", {.name = "y"}));
    }
    ProjectStore again(root / "p"); // a new store sees the same data
    assert(again.get(id)->name == "renamed" && again.get(id)->trusted && again.get(id)->dir == "/clone/firmware/esp32");
    fs::remove_all(root);
}

void ids_are_never_paths() {
    const auto root = fresh("malgrind_projects_c");
    ProjectStore store(root / "p");
    const auto a = store.create({.name = "a"}).id, b = store.create({.name = "b"}).id;
    assert(a != b);
    assert(!store.get("../x") && !store.get("a/b") && !store.get("..") && !store.get(""));
    assert(!store.remove("../p") && !store.update("..", {.name = "x"}) && store.clone_dir("../x").empty());
    assert(store.clone_dir(a) == root / "p" / a / "src");
    fs::remove_all(root);
}

void remove_keeps_the_users_folder() {
    const auto root = fresh("malgrind_projects_d");
    const auto mine = root / "my-firmware";
    fs::create_directories(mine);
    std::ofstream(mine / "platformio.ini") << "[env:a]\n";
    ProjectStore store(root / "p");
    const auto folder = store.create({.name = "mine", .origin = Origin::Folder, .dir = mine, .trusted = true});
    const auto git = store.create({.name = "cloned", .origin = Origin::Git});
    fs::create_directories(store.clone_dir(git.id) / "firmware");
    std::ofstream(store.clone_dir(git.id) / "firmware/platformio.ini") << "[env:a]\n";
    assert(store.remove(folder.id) && !store.get(folder.id));
    assert(fs::exists(mine / "platformio.ini")); // the user's own folder is untouched
    assert(store.remove(git.id) && !fs::exists(root / "p" / git.id)); // a clone goes with its record
    assert(!store.remove(git.id));
    fs::remove_all(root);
}

void elf_is_copied() {
    const auto root = fresh("malgrind_projects_e");
    std::ofstream(root / "in.elf") << "ELF!";
    ProjectStore store(root / "p");
    const auto p = store.create({.name = "sim", .origin = Origin::None}, root / "in.elf");
    assert(p.elf == root / "p" / p.id / "firmware.elf" && fs::file_size(p.elf) == 4);
    fs::remove(root / "in.elf"); // the project keeps its own copy
    assert(store.get(p.id)->elf == p.elf && fs::exists(p.elf));
    assert(store.create({.name = "no elf"}, root / "missing.elf").elf.empty()); // an unreadable file leaves it empty
    fs::remove_all(root);
}

} // namespace

int main() {
    create_list_roundtrip();
    update_persists();
    ids_are_never_paths();
    remove_keeps_the_users_folder();
    elf_is_copied();
    std::puts("projects ok");
}

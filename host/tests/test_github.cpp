// GitHub link parsing and cloning, with a fake `git` on PATH.
#include "github.hpp"
#include "ops.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }
std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

void accepts_repository_links() {
    const auto a = parse_github_url("https://github.com/Hombosh-dev/mcu-flamegraph");
    assert(a && a->url == "https://github.com/Hombosh-dev/mcu-flamegraph.git" && a->name == "mcu-flamegraph");
    assert(parse_github_url("https://github.com/o/r.git/")->url == "https://github.com/o/r.git");
    assert(parse_github_url("  https://github.com/o/r.js  \n")->name == "r.js");
    assert(parse_github_url("https://github.com/o/my_repo-2.0")->name == "my_repo-2.0");
}

void refuses_everything_else() {
    for (const char* bad : {"", "github.com/o/r", "http://github.com/o/r", "https://gitlab.com/o/r", "https://github.com.evil.com/o/r",
                            "https://github.com/o", "https://github.com/o/", "https://github.com//r", "https://github.com/o/r/tree/main",
                            "https://github.com/o/..", "https://github.com/o/.hidden", "https://github.com/o/a..b", "https://github.com/-o/r",
                            "file:///etc/passwd", "ext::sh -c touch% /tmp/x", "ssh://git@github.com/o/r", "git@github.com:o/r.git",
                            "--upload-pack=touch /tmp/x", "https://github.com/o/r --upload-pack=x", "https://github.com/o/r\nx",
                            "https://github.com/o/r?x=1", "https://user:pw@github.com/o/r"})
        assert(!parse_github_url(bad) && bad);
}

struct FakeGit {
    fs::path root = fs::canonical(fs::temp_directory_path()) / "malgrind_github";
    FakeGit() {
        fs::remove_all(root);
        fs::create_directories(root / "bin");
        std::ofstream(root / "bin/git") << R"(#!/bin/sh
echo "git $*" >> "$FAKE_LOG"
echo "prompt=$GIT_TERMINAL_PROMPT" >> "$FAKE_LOG"
for last; do :; done   # the destination is the last argument
[ "$FAKE_GIT" = fail ] && { echo "fatal: repository not found" >&2; exit 128; }
mkdir -p "$last"
echo "Cloning into '$last'..."
[ "$FAKE_GIT" = nopio ] && { echo readme > "$last/README.md"; exit 0; }
mkdir -p "$last/firmware/x" && printf '[env:a]\n' > "$last/firmware/x/platformio.ini"
exit 0
)";
        fs::permissions(root / "bin/git", fs::perms::owner_exec | fs::perms::owner_read, fs::perm_options::add);
        setenv("PATH", (root / "bin").string().append(":/usr/bin:/bin").c_str(), 1); // the fake git first
        setenv("FAKE_LOG", (root / "log").c_str(), 1);
        unsetenv("FAKE_GIT");
    }
    ~FakeGit() { fs::remove_all(root); }
};

void clone_finds_the_project() {
    FakeGit fake;
    std::string lines;
    const auto dest = fake.root / "clone";
    const auto project = clone_project(*parse_github_url("https://github.com/o/r"), dest, [&](std::string_view l) { lines.append(l).append("\n"); });
    assert(project == dest / "firmware/x" && fs::exists(project / "platformio.ini") && contains(lines, "Cloning into"));
    const auto log = slurp(fake.root / "log");
    // https only, shallow, never a password prompt, and the link comes after `--` so it can never be read as an option
    assert(contains(log, "-c protocol.allow=never -c protocol.https.allow=always clone --depth 1 --progress -- https://github.com/o/r.git " + dest.string()));
    assert(contains(log, "prompt=0"));
}

void clone_without_platformio_ini_leaves_nothing() {
    FakeGit fake;
    setenv("FAKE_GIT", "nopio", 1);
    const auto dest = fake.root / "clone";
    bool threw = false;
    try {
        (void)clone_project(*parse_github_url("https://github.com/o/r"), dest, [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "no platformio.ini");
    }
    assert(threw && !fs::exists(dest));
}

void failed_clone_leaves_nothing() {
    FakeGit fake;
    setenv("FAKE_GIT", "fail", 1);
    std::string lines;
    bool threw = false;
    try {
        (void)clone_project(*parse_github_url("https://github.com/o/r"), fake.root / "clone", [&](std::string_view l) { lines.append(l); });
    } catch (const std::exception& e) {
        threw = contains(e.what(), "128") && contains(e.what(), "public");
    }
    assert(threw && contains(lines, "repository not found") && !fs::exists(fake.root / "clone"));
}

void missing_git_is_explained() {
    FakeGit fake;
    setenv("PATH", "/nonexistent", 1);
    bool threw = false;
    try {
        (void)clone_project(*parse_github_url("https://github.com/o/r"), fake.root / "clone", [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "git was not found");
    }
    assert(threw);
}

} // namespace

int main() {
    accepts_repository_links();
    refuses_everything_else();
    clone_finds_the_project();
    clone_without_platformio_ini_leaves_nothing();
    failed_clone_leaves_nothing();
    missing_git_is_explained();
    std::puts("github ok");
}

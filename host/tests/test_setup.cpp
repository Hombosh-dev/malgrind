// Installing PlatformIO, with a fake Python that creates a "virtual environment" and a "pio" in it.
#include "pio.hpp"
#include "setup.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

struct Fake {
    fs::path root = fs::canonical(fs::temp_directory_path()) / "malgrind_setup";
    fs::path home = root / "home", bin = root / "bin", python = bin / "python3";
    Fake() {
        fs::remove_all(root);
        fs::create_directories(bin);
        fs::create_directories(home);
        // "-m venv DIR" makes DIR/bin/python (a copy of this script); "-m pip install ..." makes the pio next to it
        std::ofstream(python) << R"(#!/bin/sh
echo "python $*" >> "$FAKE_LOG"
case "$2" in
venv)
  # like Ubuntu without python3-venv: the interpreter is copied, then the missing ensurepip stops it (so there is no pip)
  mkdir -p "$3/bin" && cp "$0" "$3/bin/python"
  [ "$FAKE_FAIL" = venv ] && { echo "The virtual environment was not created successfully because ensurepip is not available." >&2; exit 1; }
  touch "$3/bin/pip" ;;
pip)
  [ "$FAKE_FAIL" = pip ] && { echo "ERROR: connection failed" >&2; exit 1; }
  [ "$FAKE_FAIL" = nopio ] && exit 0
  printf '#!/bin/sh\n' > "$(dirname "$0")/pio"; chmod +x "$(dirname "$0")/pio" ;;
esac
exit 0
)";
        fs::permissions(python, fs::perms::owner_exec | fs::perms::owner_read, fs::perm_options::add);
        setenv("PATH", bin.string().append(":/usr/bin:/bin").c_str(), 1);
        setenv("FAKE_LOG", (root / "log").c_str(), 1);
        unsetenv("FAKE_FAIL");
    }
    ~Fake() { fs::remove_all(root); }
};

void installs_into_the_place_find_pio_looks() {
    Fake fake;
    assert(find_python() == fake.python);
    assert(!find_pio(fake.home)); // nothing installed yet
    const auto steps = install_pio_steps(fake.python, fake.home);
    assert(steps.size() == 2 && steps[0].argv[2] == "venv" && steps[0].argv[3] == (fake.home / ".platformio/penv").string());
    assert(steps[1].argv.back() == "platformio" && steps[1].argv[0] == (fake.home / ".platformio/penv/bin/python").string());
    std::string lines;
    install_pio(fake.python, fake.home, [&](std::string_view l) { lines.append(l); });
    assert(fs::exists(fake.home / ".platformio/penv/bin/pio"));
    assert(find_pio(fake.home) == fake.home / ".platformio/penv/bin/pio"); // exactly where find_pio looks
    // a second run reuses the environment: only pip runs
    assert(install_pio_steps(fake.python, fake.home).size() == 1);
}

void a_missing_venv_module_says_what_to_install() {
    Fake fake;
    setenv("FAKE_FAIL", "venv", 1);
    std::string lines;
    bool threw = false;
    try {
        install_pio(fake.python, fake.home, [&](std::string_view l) { lines.append(l); });
    } catch (const std::exception& e) {
        threw = contains(e.what(), "python3-venv");
    }
    assert(threw && contains(lines, "ensurepip")); // the raw output was shown too
    // the half-made environment is gone, so after `apt install python3-venv` a second click installs (a stale one made pip fail forever)
    assert(!fs::exists(fake.home / ".platformio/penv"));
    unsetenv("FAKE_FAIL");
    install_pio(fake.python, fake.home, [](std::string_view) {});
    assert(fs::exists(fake.home / ".platformio/penv/bin/pio"));
}

void a_broken_environment_from_an_older_try_is_rebuilt() {
    Fake fake;
    fs::create_directories(fake.home / ".platformio/penv/bin");
    std::ofstream(fake.home / ".platformio/penv/bin/python") << "#!/bin/sh\n"; // an interpreter, but no pip
    assert(install_pio_steps(fake.python, fake.home).size() == 2); // so the venv step runs again
    std::ofstream(fake.home / ".platformio/penv/bin/pip") << "";
    assert(install_pio_steps(fake.python, fake.home).size() == 1); // a working one is reused
}

void a_network_failure_is_named() {
    Fake fake;
    setenv("FAKE_FAIL", "pip", 1);
    bool threw = false;
    try {
        install_pio(fake.python, fake.home, [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "online");
    }
    assert(threw);
}

void success_without_pio_is_not_success() {
    Fake fake;
    setenv("FAKE_FAIL", "nopio", 1);
    bool threw = false;
    try {
        install_pio(fake.python, fake.home, [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "no pio");
    }
    assert(threw);
}

void no_python_means_no_python() {
    Fake fake;
    setenv("PATH", "/nonexistent", 1);
    assert(!find_python());
}

} // namespace

int main() {
    installs_into_the_place_find_pio_looks();
    a_missing_venv_module_says_what_to_install();
    a_broken_environment_from_an_older_try_is_rebuilt();
    a_network_failure_is_named();
    success_without_pio_is_not_success();
    no_python_means_no_python();
    std::puts("setup ok");
}

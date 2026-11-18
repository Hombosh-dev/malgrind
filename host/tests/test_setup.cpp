// Installing PlatformIO and locating Python, PlatformIO core directories, virtual environments and toolchains.
#include "os/paths.hpp"
#include "os/tools.hpp"
#include "pio.hpp"
#include "setup.hpp"
#include "symbols.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)), saved_(os::get_env(name_)) {}
    ~ScopedEnv() {
        if (saved_) os::set_env(name_, *saved_);
        else os::unset_env(name_);
    }

private:
    std::string name_;
    std::optional<std::string> saved_;
};

fs::path fresh_dir(const char* name) {
    const auto p = fs::canonical(fs::temp_directory_path()) / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

struct Fake {
    ScopedEnv save_path{"PATH"};
    ScopedEnv save_core{"PLATFORMIO_CORE_DIR"};
    ScopedEnv save_log{"FAKE_LOG"};
    ScopedEnv save_fail{"FAKE_FAIL"};
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
        os::set_env("PATH", os::to_utf8(bin) + ":/usr/bin:/bin");
        os::unset_env("PLATFORMIO_CORE_DIR");
        os::set_env("FAKE_LOG", os::to_utf8(root / "log"));
        os::unset_env("FAKE_FAIL");
    }
    ~Fake() { fs::remove_all(root); }
};

void venv_bin_for_checks() {
    assert(os::venv_bin_for(os::Kind::Linux, "/a/penv") == fs::path("/a/penv/bin"));
    assert(os::venv_bin_for(os::Kind::MacOS, "/a/penv") == fs::path("/a/penv/bin"));
    assert(os::venv_bin_for(os::Kind::Windows, "C:/a/penv") == fs::path("C:/a/penv/Scripts"));
    assert(os::venv_bin("/a/penv") == os::venv_bin_for(os::kind(), "/a/penv"));
}

void install_pio_steps_for_both_layouts() {
    const auto root = fresh_dir("malgrind_setup_layouts");
    const auto posix_env = root / "posix_penv";
    const auto win_env = root / "win_penv";

    // POSIX layout: fresh -> 2 steps, half-made (no pip) -> 2 steps, complete -> 1 step
    auto steps = install_pio_steps_for(os::Kind::Linux, {"/usr/bin/python3"}, posix_env);
    assert(steps.size() == 2);
    assert((steps[0].argv == std::vector<std::string>{"/usr/bin/python3", "-m", "venv", os::to_utf8(posix_env)}));
    assert((steps[1].argv == std::vector<std::string>{os::to_utf8(posix_env / "bin/python"), "-m", "pip", "install",
                                                     "--disable-pip-version-check", "platformio"}));
    fs::create_directories(posix_env / "bin");
    std::ofstream(posix_env / "bin/python") << "";
    assert(install_pio_steps_for(os::Kind::Linux, {"/usr/bin/python3"}, posix_env).size() == 2);
    std::ofstream(posix_env / "bin/pip") << "";
    steps = install_pio_steps_for(os::Kind::Linux, {"/usr/bin/python3"}, posix_env);
    assert(steps.size() == 1);
    assert(steps[0].argv[0] == os::to_utf8(posix_env / "bin/python"));

    // Windows layout: Scripts/python.exe + Scripts/pip.exe and multi-arg `py -3` prefix
    steps = install_pio_steps_for(os::Kind::Windows, {"C:\\Windows\\py.exe", "-3"}, win_env);
    assert(steps.size() == 2);
    assert((steps[0].argv == std::vector<std::string>{"C:\\Windows\\py.exe", "-3", "-m", "venv", os::to_utf8(win_env)}));
    assert((steps[1].argv == std::vector<std::string>{os::to_utf8(win_env / "Scripts/python.exe"), "-m", "pip", "install",
                                                     "--disable-pip-version-check", "platformio"}));
    fs::create_directories(win_env / "Scripts");
    std::ofstream(win_env / "Scripts/python.exe") << "";
    assert(install_pio_steps_for(os::Kind::Windows, {"C:\\Windows\\py.exe", "-3"}, win_env).size() == 2);
    std::ofstream(win_env / "Scripts/pip.exe") << "";
    steps = install_pio_steps_for(os::Kind::Windows, {"C:\\Windows\\py.exe", "-3"}, win_env);
    assert(steps.size() == 1);
    assert(steps[0].argv[0] == os::to_utf8(win_env / "Scripts/python.exe"));

    fs::remove_all(root);
}

void platformio_core_dirs_checks() {
    ScopedEnv save_core("PLATFORMIO_CORE_DIR");
    const auto root = fresh_dir("malgrind_setup_cores");
    const auto home = root / "home";
    const auto custom = root / "custom_core";
    const auto missing = root / "missing_core";
    fs::create_directories(home);

    os::unset_env("PLATFORMIO_CORE_DIR");
    assert(os::platformio_core_dirs(home).empty());

    fs::create_directories(home / ".platformio");
    assert((os::platformio_core_dirs(home) == std::vector<fs::path>{home / ".platformio"}));

    // Non-existent $PLATFORMIO_CORE_DIR is ignored
    os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(missing));
    assert((os::platformio_core_dirs(home) == std::vector<fs::path>{home / ".platformio"}));

    // Existing $PLATFORMIO_CORE_DIR comes before home/.platformio
    fs::create_directories(custom);
    os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(custom));
    assert((os::platformio_core_dirs(home) == std::vector<fs::path>{custom, home / ".platformio"}));
    // And install_pio_steps targets the first core dir when one exists
    const auto steps = install_pio_steps({"/usr/bin/python3"}, home);
    assert(steps.size() == 2 && steps[0].argv[3] == os::to_utf8(custom / "penv"));

    // Deduplication when $PLATFORMIO_CORE_DIR points to home/.platformio
    os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(home / "." / ".platformio"));
    assert(os::platformio_core_dirs(home).size() == 1);

    // find_addr2line searches <core>/packages/toolchain-*/bin across platformio_core_dirs
    {
        ScopedEnv save_path("PATH");
        os::set_env("PATH", "/nonexistent");
        os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(custom));
        const auto tool_dir = custom / "packages/toolchain-xtensa-esp32/bin";
        fs::create_directories(tool_dir);
        const auto tool = tool_dir / os::exe_name("xtensa-esp32-elf-addr2line");
        std::ofstream(tool) << "";
        assert(find_addr2line(malgrind::Arch::Xtensa) == tool);
    }

    fs::remove_all(root);
}

void pio_python_checks() {
    ScopedEnv save_core("PLATFORMIO_CORE_DIR");
    os::unset_env("PLATFORMIO_CORE_DIR");
    const auto root = fresh_dir("malgrind_setup_piopy");
    const auto home = root / "home";
    const auto custom = root / "custom_core";
    const auto pio = root / "pio";
    fs::create_directories(home);
    std::ofstream(pio) << "#!/usr/bin/env python3\n";

    // Without penv python, POSIX falls back to parsing the shebang of `pio`
    if constexpr (os::kind() != os::Kind::Windows) {
        assert((os::pio_python(home, pio) == std::vector<std::string>{"/usr/bin/env", "python3"}));
        std::ofstream(root / "notscript") << "binary";
        assert(!os::pio_python(home, root / "notscript").has_value());
        std::ofstream(root / "empty_shebang") << "#!   \n";
        assert(!os::pio_python(home, root / "empty_shebang").has_value());
    }

    // With <core>/penv/<bin>/python[.exe], penv takes precedence over the shebang
    const auto home_py = os::venv_bin(home / ".platformio/penv") / os::exe_name("python");
    fs::create_directories(home_py.parent_path());
    std::ofstream(home_py) << "";
    assert((os::pio_python(home, pio) == std::vector<std::string>{os::to_utf8(home_py)}));

    // $PLATFORMIO_CORE_DIR takes precedence over home/.platformio
    const auto custom_py = os::venv_bin(custom / "penv") / os::exe_name("python");
    fs::create_directories(custom_py.parent_path());
    std::ofstream(custom_py) << "";
    os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(custom));
    assert((os::pio_python(home, pio) == std::vector<std::string>{os::to_utf8(custom_py)}));

    fs::remove_all(root);
}

void python_hint_checks() {
    const auto hint = os::python_hint();
    assert(!hint.empty());
    if constexpr (os::kind() == os::Kind::Linux) assert(contains(hint, "python3-venv"));
    else if constexpr (os::kind() == os::Kind::MacOS) assert(contains(hint, "Homebrew"));
    else assert(contains(hint, "python.org"));
}

void installs_into_the_place_find_pio_looks() {
    Fake fake;
    const auto py = os::find_python();
    assert(py && (*py == std::vector<std::string>{os::to_utf8(fake.python)}));
    assert(!find_pio(fake.home)); // nothing installed yet
    const auto steps = install_pio_steps(*py, fake.home);
    assert(steps.size() == 2 && steps[0].argv[2] == "venv" && steps[0].argv[3] == os::to_utf8(fake.home / ".platformio/penv"));
    assert(steps[1].argv.back() == "platformio" && steps[1].argv[0] == os::to_utf8(fake.home / ".platformio/penv/bin/python"));
    std::string lines;
    install_pio(*py, fake.home, [&](std::string_view l) { lines.append(l); });
    assert(fs::exists(fake.home / ".platformio/penv/bin/pio"));
    assert(find_pio(fake.home) == fake.home / ".platformio/penv/bin/pio"); // exactly where find_pio looks
    // a second run reuses the environment: only pip runs
    assert(install_pio_steps(*py, fake.home).size() == 1);
}

void a_missing_venv_module_says_what_to_install() {
    Fake fake;
    os::set_env("FAKE_FAIL", "venv");
    std::string lines;
    bool threw = false;
    try {
        install_pio({os::to_utf8(fake.python)}, fake.home, [&](std::string_view l) { lines.append(l); });
    } catch (const std::exception& e) {
        threw = contains(e.what(), os::python_hint());
    }
    assert(threw && contains(lines, "ensurepip")); // the raw output was shown too
    // the half-made environment is gone, so after installing venv support a second click installs (a stale one made pip fail forever)
    assert(!fs::exists(fake.home / ".platformio/penv"));

    // Multi-element python_cmd (e.g. `env python3` or Windows `py -3`) also detects venv failure cleanly
    bool multi_threw = false;
    try {
        install_pio({"/usr/bin/env", os::to_utf8(fake.python)}, fake.home, [](std::string_view) {});
    } catch (const std::exception& e) {
        multi_threw = contains(e.what(), os::python_hint());
    }
    assert(multi_threw && !fs::exists(fake.home / ".platformio/penv"));

    os::unset_env("FAKE_FAIL");
    install_pio({os::to_utf8(fake.python)}, fake.home, [](std::string_view) {});
    assert(fs::exists(fake.home / ".platformio/penv/bin/pio"));
}

void a_broken_environment_from_an_older_try_is_rebuilt() {
    Fake fake;
    fs::create_directories(fake.home / ".platformio/penv/bin");
    std::ofstream(fake.home / ".platformio/penv/bin/python") << "#!/bin/sh\n"; // an interpreter, but no pip
    assert(install_pio_steps({os::to_utf8(fake.python)}, fake.home).size() == 2); // so the venv step runs again
    std::ofstream(fake.home / ".platformio/penv/bin/pip") << "";
    assert(install_pio_steps({os::to_utf8(fake.python)}, fake.home).size() == 1); // a working one is reused
}

void a_network_failure_is_named() {
    Fake fake;
    os::set_env("FAKE_FAIL", "pip");
    bool threw = false;
    try {
        install_pio({os::to_utf8(fake.python)}, fake.home, [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "online");
    }
    assert(threw);
}

void success_without_pio_is_not_success() {
    Fake fake;
    os::set_env("FAKE_FAIL", "nopio");
    bool threw = false;
    try {
        install_pio({os::to_utf8(fake.python)}, fake.home, [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "no pio");
    }
    assert(threw);
}

void no_python_means_no_python() {
    Fake fake;
    os::set_env("PATH", "/nonexistent");
    assert(!os::find_python());

    // Falls back to `python` when `python3` is missing
    fs::remove(fake.python);
    const auto py2 = fake.bin / "python";
    std::ofstream(py2) << "#!/bin/sh\n";
    fs::permissions(py2, fs::perms::owner_exec | fs::perms::owner_read, fs::perm_options::add);
    os::set_env("PATH", os::to_utf8(fake.bin));
    assert((os::find_python() == std::vector<std::string>{os::to_utf8(py2)}));
}

} // namespace

int main() {
    venv_bin_for_checks();
    install_pio_steps_for_both_layouts();
    platformio_core_dirs_checks();
    pio_python_checks();
    python_hint_checks();
    installs_into_the_place_find_pio_looks();
    a_missing_venv_module_says_what_to_install();
    a_broken_environment_from_an_older_try_is_rebuilt();
    a_network_failure_is_named();
    success_without_pio_is_not_success();
    no_python_means_no_python();
    std::puts("setup ok");
}

// Port access self-check: reading permissions, and that the privileged command can only carry plain names.
#include "access.hpp"
#include "os/process.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <iterator>
#include <unistd.h>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

void reads_permissions() {
    const auto dir = fs::temp_directory_path() / "malgrind_access";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto port = dir / "ttyFAKE0";
    std::ofstream(port) << "x";
    fs::permissions(port, fs::perms::owner_read | fs::perms::owner_write);
    const auto mine = port_access(port);
    assert(mine.ok && mine.in_group);
    assert(mine.group == getgrgid(getegid())->gr_name); // a new file belongs to the user's own group
    if (geteuid() != 0) { // root may open anything
        fs::permissions(port, fs::perms::none);
        assert(!port_access(port).ok);
    }
    assert(!port_access(dir / "missing").ok && port_access(dir / "missing").group.empty());
    assert(!current_user().empty());
    fs::remove_all(dir);
}

void the_privileged_command_carries_names_only() {
    const auto inv = grant_access_command("/usr/bin/pkexec", "/dev/ttyUSB0", "partum", "dialout");
    assert(inv && inv->env.empty() && inv->argv.size() == 8);
    assert(inv->argv[0] == "/usr/bin/pkexec" && inv->argv[1] == "/bin/sh" && inv->argv[2] == "-c" && inv->argv[4] == "sh");
    // user, port and group are separate arguments after the script, in the order the script reads them ($1 $2 $3)
    assert(inv->argv[5] == "partum" && inv->argv[6] == "/dev/ttyUSB0" && inv->argv[7] == "dialout");
    const auto& script = inv->argv[3];
    for (const char* value : {"partum", "ttyUSB0", "dialout"}) assert(script.find(value) == std::string::npos);
    assert(script.find("setfacl") != std::string::npos && script.find("usermod") != std::string::npos);
    assert(grant_access_command("/usr/bin/pkexec", "/dev/cu.usbserial-1410", "a.b_c-d", "uucp")); // other platforms' names are fine
}

void refuses_anything_that_is_not_a_plain_name() {
    const auto p = "/usr/bin/pkexec";
    for (const char* bad : {"", "a b", "a;b", "$(id)", "-rf", "a\nb", "a'b", "a`b`", "a/b"}) {
        assert(!grant_access_command(p, "/dev/ttyUSB0", bad, "dialout") && bad);
        assert(!grant_access_command(p, "/dev/ttyUSB0", "me", bad) && bad);
    }
    for (const char* bad : {"", "ttyUSB0", "/etc/passwd", "/dev/../etc/passwd", "/dev/tty USB", "/dev/tty;id", "/dev/tty$HOME", "-x"})
        assert(!grant_access_command(p, bad, "me", "dialout") && bad);
}

// The script is what runs as root, so it is run here: a fake pkexec just executes its arguments, and fake setfacl and usermod
// log what they were given and fail on demand (FAKE_FAIL=setfacl,usermod).
struct Tools {
    fs::path root = fs::canonical(fs::temp_directory_path()) / "malgrind_access_tools";
    fs::path bin = root / "bin", log = root / "log";
    Tools() {
        fs::remove_all(root);
        fs::create_directories(bin);
        write("pkexec", "exec \"$@\"\n");
        for (const char* tool : {"setfacl", "usermod"})
            write(tool, std::string("echo \"") + tool + " $*\" >> \"$FAKE_LOG\"\ncase \",$FAKE_FAIL,\" in *," + tool + ",*) exit 1;; esac\nexit 0\n");
        setenv("PATH", (bin.string() + ":/usr/bin:/bin").c_str(), 1);
        setenv("FAKE_LOG", log.c_str(), 1);
    }
    ~Tools() { fs::remove_all(root); }
    void write(const char* name, const std::string& body) const {
        std::ofstream(bin / name) << "#!/bin/sh\n" << body;
        fs::permissions(bin / name, fs::perms::owner_all);
    }
    // runs the real command for these values; returns its exit code
    int run(const char* group, const char* fail = "") const {
        setenv("FAKE_FAIL", fail, 1);
        fs::remove(log);
        const auto inv = grant_access_command(bin / "pkexec", "/dev/ttyUSB0", "partum", group);
        assert(inv);
        return malgrind::host::os::run_lines(inv->argv, inv->env, [](std::string_view) {});
    }
    [[nodiscard]] std::string calls() const {
        std::ifstream in(log);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};

bool has(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

void the_script_does_what_it_says() {
    Tools t;
    // both fixes, with user, port and group in the places the script reads them
    assert(t.run("dialout") == 0);
    assert(has(t.calls(), "setfacl -m u:partum:rw /dev/ttyUSB0") && has(t.calls(), "usermod -aG dialout partum"));
    // a device that belongs to root: no group helps, only the ACL is set
    assert(t.run("root") == 0 && has(t.calls(), "setfacl") && !has(t.calls(), "usermod"));
    // either fix is enough: one failing does not fail the call, both failing does
    assert(t.run("dialout", "setfacl") == 0 && has(t.calls(), "usermod -aG dialout partum"));
    assert(t.run("dialout", "usermod") == 0 && has(t.calls(), "setfacl"));
    assert(t.run("dialout", "setfacl,usermod") != 0);
    assert(t.run("root", "setfacl") != 0); // root's device and no ACL: nothing was done
}

void manual_commands_name_the_fix() {
    const auto text = manual_commands("/dev/ttyUSB0", "partum", "dialout");
    assert(text.find("sudo setfacl -m u:partum:rw /dev/ttyUSB0") != std::string::npos);
    assert(text.find("sudo usermod -aG dialout partum") != std::string::npos);
}

} // namespace

int main() {
    reads_permissions();
    the_privileged_command_carries_names_only();
    refuses_anything_that_is_not_a_plain_name();
    the_script_does_what_it_says();
    manual_commands_name_the_fix();
    std::puts("access ok");
}

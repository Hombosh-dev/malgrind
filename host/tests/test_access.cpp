// Port access self-check: reading permissions, and that the privileged command can only carry plain names.
#include "access.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <grp.h>
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
    manual_commands_name_the_fix();
    std::puts("access ok");
}

#pragma once

// Whether this user may open a serial port, and how to give them access. On Linux a USB serial port belongs to a group
// ("dialout" on Debian and Ubuntu); a user who is not in it gets "Permission denied". Two fixes exist and both are applied:
// an ACL on the device (works at once, until the board is replugged) and membership of the group (permanent, after the next login).

#include "pio.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace malgrind::host {

struct PortAccess {
    bool ok = false;       // the running process may read and write the port now
    std::string group;     // the group that owns the device ("dialout"), empty if unknown
    bool in_group = false; // the running process is in that group
};
[[nodiscard]] PortAccess port_access(const std::filesystem::path& port);

[[nodiscard]] std::string current_user(); // the name of the running user, empty if the system does not know it

// One pkexec call (the system's graphical password prompt) that grants access to `port`. User, port and group reach the shell
// as positional arguments, never inside the command text. nullopt if a value is not a plain name or the port is not under /dev/.
[[nodiscard]] std::optional<Invocation> grant_access_command(const std::filesystem::path& pkexec, const std::string& port,
                                                             const std::string& user, const std::string& group);

// The same fix as two lines to type in a terminal, for a machine without pkexec.
[[nodiscard]] std::string manual_commands(const std::string& port, const std::string& user, const std::string& group);

} // namespace malgrind::host

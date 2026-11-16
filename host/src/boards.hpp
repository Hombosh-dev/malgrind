#pragma once

// What is plugged in, as far as the USB bus tells (Linux sysfs; elsewhere nothing is known and the port name is all a tile has).
// The adapter is NOT the chip: a CH343 or CP210x bridge sits in front of any MCU. The chip is learned from an agent's frame
// or by asking esptool, which resets the board, so it is never done by itself.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace malgrind::host {

struct UsbInfo {
    std::string vid, pid; // lower-case hex, "1a86"
    std::string product, manufacturer;
};

// The USB device behind a serial port such as /dev/ttyACM0. nullopt if sysfs does not say.
[[nodiscard]] std::optional<UsbInfo> usb_info_for_port(const std::filesystem::path& port, const std::filesystem::path& sys_root = "/sys");

// A readable name for a USB-serial adapter: "WCH CH340/CH343 USB-UART", "Silicon Labs CP210x USB-UART", ... else the product string.
[[nodiscard]] std::string adapter_name(const UsbInfo& info);

struct DebugProbe {
    std::string name, vid, pid;
};
// USB debug probes that are plugged in (ST-Link, J-Link, CMSIS-DAP, FTDI JTAG adapters).
[[nodiscard]] std::vector<DebugProbe> find_probes(const std::filesystem::path& sys_root = "/sys");

} // namespace malgrind::host

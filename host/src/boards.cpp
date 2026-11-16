#include "boards.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

std::string read_line(const fs::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return line;
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::optional<UsbInfo> read_usb_device(const fs::path& dir) {
    auto vid = lower(read_line(dir / "idVendor"));
    if (vid.empty()) return std::nullopt;
    return UsbInfo{std::move(vid), lower(read_line(dir / "idProduct")), read_line(dir / "product"), read_line(dir / "manufacturer"),
                   read_line(dir / "serial"), dir.filename().string()};
}

} // namespace

std::optional<UsbInfo> usb_info_for_port(const fs::path& port, const fs::path& sys_root) {
    std::error_code ec;
    // /sys/class/tty/<tty>/device is the interface (or the usb-serial port); the USB device is a few levels above it
    auto dir = fs::canonical(sys_root / "class/tty" / port.filename() / "device", ec);
    for (int level = 0; !ec && level < 4 && !dir.empty() && dir != dir.root_path(); ++level, dir = dir.parent_path())
        if (auto info = read_usb_device(dir)) return info;
    return std::nullopt;
}

std::string adapter_name(const UsbInfo& info) {
    if (info.vid == "1a86") return "WCH CH340/CH343 USB-UART";
    if (info.vid == "10c4") return "Silicon Labs CP210x USB-UART";
    if (info.vid == "0403") return "FTDI USB-UART";
    if (info.vid == "067b") return "Prolific PL2303 USB-UART";
    if (info.vid == "303a") return "Espressif USB-Serial/JTAG (built-in debug port)";
    if (info.vid == "2e8a") return "Raspberry Pi USB serial";
    return info.product.empty() ? "USB serial adapter" : info.product;
}

std::vector<DebugProbe> find_probes(const fs::path& sys_root) {
    std::vector<DebugProbe> probes;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sys_root / "bus/usb/devices", ec)) {
        const auto info = read_usb_device(entry.path());
        if (!info) continue;
        const std::string_view vid = info->vid, pid = info->pid;
        std::string name;
        if (vid == "0483" && (pid == "3744" || pid == "3748" || pid == "374b" || pid == "374d" || pid == "374e" || pid == "374f" || pid == "3752" || pid == "3753"))
            name = "ST-Link";
        else if (vid == "1366") name = "SEGGER J-Link";
        else if (vid == "0d28" && pid == "0204") name = "CMSIS-DAP (DAPLink)";
        else if (vid == "2e8a" && pid == "000c") name = "Raspberry Pi Debug Probe";
        else if (vid == "15ba") name = "Olimex debugger";
        else if (vid == "0403" && (pid == "6010" || pid == "6014")) name = "FTDI FT2232/FT232H (can be a JTAG adapter, e.g. ESP-Prog)";
        if (!name.empty()) probes.push_back({std::move(name), info->vid, info->pid});
    }
    std::ranges::sort(probes, {}, &DebugProbe::name);
    return probes;
}

} // namespace malgrind::host

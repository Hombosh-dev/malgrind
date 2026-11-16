// Board discovery self-check on a fake sysfs tree (the layout Linux uses: tty -> interface -> USB device).
#include "boards.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text << "\n";
}

// A USB device under devices/usb1/<name>, with an interface 1-x:1.0 inside it.
void usb_device(const fs::path& sys, const std::string& name, std::string_view vid, std::string_view pid, std::string_view product) {
    const auto dir = sys / "devices/pci/usb1" / name;
    write(dir / "idVendor", vid);
    write(dir / "idProduct", pid);
    write(dir / "product", product);
    write(dir / "manufacturer", "maker");
    if (name == "1-1") write(dir / "serial", "0001"); // not every adapter has one
    fs::create_directories(dir / (name + ":1.0"));
    fs::create_directories(sys / "bus/usb/devices");
    fs::create_directory_symlink(dir, sys / "bus/usb/devices" / name);
}

void tty_link(const fs::path& sys, const std::string& tty, const fs::path& target) {
    fs::create_directories(sys / "class/tty" / tty);
    fs::create_directory_symlink(target, sys / "class/tty" / tty / "device");
}

void finds_the_adapter_behind_a_port() {
    const auto sys = fs::canonical(fs::temp_directory_path()) / "malgrind_boards_a";
    fs::remove_all(sys);
    usb_device(sys, "1-1", "1A86", "55d3", "USB Single Serial"); // upper case is normalized
    usb_device(sys, "1-2", "10c4", "ea60", "CP2102 USB to UART Bridge Controller");
    tty_link(sys, "ttyACM0", sys / "devices/pci/usb1/1-1/1-1:1.0");                              // cdc_acm: the tty links to the interface
    fs::create_directories(sys / "devices/pci/usb1/1-2/1-2:1.0/ttyUSB0");
    tty_link(sys, "ttyUSB0", sys / "devices/pci/usb1/1-2/1-2:1.0/ttyUSB0");                      // usb-serial: one level deeper

    const auto acm = usb_info_for_port("/dev/ttyACM0", sys);
    assert(acm && acm->vid == "1a86" && acm->pid == "55d3" && acm->product == "USB Single Serial");
    assert(acm->serial == "0001" && acm->location == "1-1"); // two identical boards differ here
    assert(adapter_name(*acm) == "WCH CH340/CH343 USB-UART");
    const auto usb = usb_info_for_port("/dev/ttyUSB0", sys);
    assert(usb && usb->vid == "10c4" && adapter_name(*usb) == "Silicon Labs CP210x USB-UART");
    assert(usb->serial.empty() && usb->location == "1-2");
    assert(!usb_info_for_port("/dev/ttyUSB7", sys)); // not in sysfs
    assert(adapter_name({"abcd", "1", "My Gadget", ""}) == "My Gadget" && adapter_name({"abcd", "1", "", ""}) == "USB serial adapter");
    fs::remove_all(sys);
}

void finds_debug_probes() {
    const auto sys = fs::canonical(fs::temp_directory_path()) / "malgrind_boards_b";
    fs::remove_all(sys);
    usb_device(sys, "1-1", "1a86", "55d3", "USB Single Serial");     // a serial adapter is not a probe
    usb_device(sys, "1-3", "0483", "374b", "STM32 STLink");
    usb_device(sys, "1-4", "0d28", "0204", "DAPLink CMSIS-DAP");
    usb_device(sys, "1-5", "1d6b", "0002", "xHCI Host Controller");
    const auto probes = find_probes(sys);
    assert(probes.size() == 2 && probes[0].name == "CMSIS-DAP (DAPLink)" && probes[1].name == "ST-Link" && probes[1].vid == "0483");
    fs::remove_all(sys);
}

void no_sysfs_means_no_information() {
    const auto none = fs::temp_directory_path() / "malgrind_boards_none"; // macOS, or a container
    assert(!usb_info_for_port("/dev/cu.usbserial-1", none) && find_probes(none).empty());
}

} // namespace

int main() {
    finds_the_adapter_behind_a_port();
    finds_debug_probes();
    no_sysfs_means_no_information();
    std::puts("boards ok");
}

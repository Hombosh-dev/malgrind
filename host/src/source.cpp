#include "source.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace mulcan::host {

std::vector<std::string> find_serial_ports(const std::filesystem::path& dev_dir) {
    auto lower = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char ch) { return std::tolower(ch); });
        return s;
    };
    std::vector<std::string> ports;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dev_dir, ec)) {
        const auto name = entry.path().filename().string();
        const bool linux_usb = name.starts_with("ttyUSB") || name.starts_with("ttyACM");
        // macOS: call-out devices of USB adapters (cu.usbserial-*, cu.usbmodem*, cu.SLAB_USBtoUART, cu.wchusbserial*);
        // this skips cu.Bluetooth-Incoming-Port and friends
        const bool mac_usb = name.starts_with("cu.") && lower(name).find("usb") != std::string::npos;
        if (linux_usb || mac_usb) ports.push_back(entry.path().string());
    }
    std::ranges::sort(ports);
    return ports;
}

namespace {
speed_t speed_for(unsigned baud) {
#ifdef __APPLE__
    return baud; // macOS: speed_t is the baud rate itself, and it has no B460800... constants
#else
    switch (baud) {
        case 9600: return B9600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        case 1000000: return B1000000;
        case 1500000: return B1500000;
        case 2000000: return B2000000;
        default: return B0;
    }
#endif
}
} // namespace

std::optional<Source> Source::open(const std::string& port, unsigned baud) {
    if (port == "-") return Source(STDIN_FILENO, false);
    const speed_t speed = speed_for(baud);
    if (speed == B0) return std::nullopt;
    const int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return std::nullopt;
    termios tty{};
    if (tcgetattr(fd, &tty) != 0) { ::close(fd); return std::nullopt; }
    cfmakeraw(&tty);
    cfsetspeed(&tty, speed);
    tty.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { ::close(fd); return std::nullopt; }
    ioctl(fd, TIOCEXCL); // exclusive: a second program opening the port gets EBUSY instead of stealing bytes
    return Source(fd, true);
}

Source::~Source() {
    if (owns_ && fd_ >= 0) ::close(fd_);
}

std::optional<std::size_t> Source::read(std::span<std::uint8_t> out, int timeout_ms) {
    // select(), not poll(): macOS poll() does not work on device files such as serial ports
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd_, &readable);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int ready = select(fd_ + 1, &readable, nullptr, nullptr, &timeout);
    if (ready < 0) return errno == EINTR ? std::optional<std::size_t>(0) : std::nullopt;
    if (ready == 0) return 0;
    const ssize_t n = ::read(fd_, out.data(), out.size());
    if (n > 0) return static_cast<std::size_t>(n);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return std::nullopt; // EOF or device gone
}

} // namespace mulcan::host

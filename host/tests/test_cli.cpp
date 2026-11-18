// Terminal mode self-check: attach to a pty "board", read the live page over HTTP, stop, find the run in the history.
#include "cli.hpp"
#include "pty.hpp"
#include "source.hpp"
#include "store.hpp"
#include "stream.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

unsigned free_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    socklen_t len = sizeof addr;
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    close(fd);
    return ntohs(addr.sin_port);
}

// Body of GET path, or "" if nothing listens (yet).
std::string http_get(unsigned port, const std::string& path) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return {};
    }
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size()));
    std::string response;
    char buf[4096];
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) response.append(buf, static_cast<std::size_t>(n));
    close(fd);
    const auto body = response.find("\r\n\r\n");
    return body == std::string::npos ? std::string{} : response.substr(body + 4);
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void attach_shows_the_live_page_and_records(const std::string& device_flag) {
    const auto root = fs::temp_directory_path() / "malgrind_cli_a";
    fs::remove_all(root);
    fs::create_directories(root / "project");
    Pty pty;
    const unsigned port = free_port();
    int code = -1;
    std::thread cli([&] {
        code = run_cli({"attach", device_flag, pty.slave, "--no-open", "--http-port", std::to_string(port), "--data-dir",
                        (root / "runs").string(), "--project", (root / "project").string(), "--folded", (root / "out.folded").string(),
                        "--name", "terminal run"},
                       {root / "runs", {}, root});
    });

    assert(wait_for([&] { return contains(http_get(port, "/"), "<html"); })); // the page is up before any frame
    assert(contains(http_get(port, "/data.json?core=all&window=10"), R"("value":0)")); // and says "nothing yet"
    pty.send(make_stream(2)); // 8 samples
    assert(wait_for([&] { return contains(http_get(port, "/data.json?core=all&window=0"), R"("name":"all","value":8)"); }));
    assert(contains(http_get(port, "/data.json?core=1&window=0"), R"("name":"all","value":2)"));
    assert(http_get(port, "/other").empty());

    cli_request_stop(); // Ctrl-C
    cli.join();
    assert(code == 0);

    RunStore store(root / "runs"); // the run is in the same history the app shows
    const auto runs = store.list();
    assert(runs.size() == 1 && runs[0].name == "terminal run" && runs[0].duration_ms > 0 && runs[0].ncores == 2);
    std::ifstream folded(root / "out.folded");
    const std::string text((std::istreambuf_iterator<char>(folded)), {});
    assert(contains(text, "core0;0x400d2fff;0x400d1fff;0x400d1000 6\n"));
    fs::remove_all(root);
}

void exit_codes() {
    assert(run_cli({}, {}) == 2 && run_cli({"bogus"}, {}) == 2);
    assert(run_cli({"attach", "--bogus"}, {}) == 2 && run_cli({"attach", "--port"}, {}) == 2); // unknown option, missing value
    assert(run_cli({"attach", "--device"}, {}) == 2);
    assert(run_cli({"devices"}, {}) == 0);
    assert(run_cli({"attach", "--device", "usb-missing-device-123"}, {}) != 0);
    Pty pty;
    if (!exclusive_open_is_enforced()) return void(std::puts("exit_codes: busy-port check skipped, this platform does not enforce exclusive open on a pty"));
    auto other_program = Source::open(pty.slave, 921600); // the port is taken
    assert(other_program);
    assert(run_cli({"attach", "--port", pty.slave, "--no-open", "--data-dir", (fs::temp_directory_path() / "malgrind_cli_b").string()}, {}) == 1);
    fs::remove_all(fs::temp_directory_path() / "malgrind_cli_b");
}

} // namespace

int main() {
    attach_shows_the_live_page_and_records("--port");
    attach_shows_the_live_page_and_records("--device");
    exit_codes();
    std::puts("cli ok");
}

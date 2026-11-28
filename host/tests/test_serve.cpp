// serve() self-check: the JSON-lines protocol the app talks, driven through in-memory pipes and MemorySource (plus one POSIX pty check).
#include "devices.hpp"
#include "env.hpp"
#include "ops.hpp"
#include "os/paths.hpp"
#include "os/tools.hpp"
#include "pipe_stream.hpp"
#include "plan.hpp"
#include "serve.hpp"
#include "source.hpp"
#include "store.hpp"
#include "stream.hpp"

#ifndef _WIN32
#include "pty.hpp"
#endif

#include <nlohmann/json.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Harness {
    fs::path root;
    ScopedEnv save_path;
    ScopedEnv save_core{"PLATFORMIO_CORE_DIR", std::nullopt};
    ScopedEnv save_drive{"SystemDrive", std::nullopt};
    ScopedEnv save_git{"FAKE_GIT", std::nullopt};
    InPipe in_buf;
    OutPipe out_buf;
    std::istream in{&in_buf};
    std::ostream out{&out_buf};
    std::mutex devices_mutex;
    std::vector<Device> extra_devices;
    bool override_devices = false;
    std::mutex source_mutex;
    SourceFactory source_factory;
    ProbeSourceFactory probe_factory;
    MemorySource* last_source = nullptr;
    std::thread thread;
    int exit_code = -1;
    int next_id = 1;
    std::vector<json> events; // everything without an id that arrived while waiting for a reply

    explicit Harness(const char* name, bool real_serial = false, bool real_openocd = false)
        : root(fs::temp_directory_path() / os::from_utf8(name)),
          save_path("PATH", os::to_utf8(root / "empty-path")) {
        fs::remove_all(root);
        fs::create_directories(root / "home");
        ServeOptions opts{root / "runs", MALGRIND_SOURCE_DIR, root / "home", root / "backups", root / "projects",
                         [this] { return current_devices(); }};
        if (!real_serial) {
            opts.open_source = [this](const Device& dev, unsigned baud)
                -> std::variant<std::unique_ptr<ByteSource>, os::OpenFailure> {
                SourceFactory fn;
                {
                    std::scoped_lock lock(source_mutex);
                    fn = source_factory;
                }
                if (fn) return fn(dev, baud);
                auto src = std::make_unique<MemorySource>(dev.name.empty() ? dev.path : dev.name);
                {
                    std::scoped_lock lock(source_mutex);
                    last_source = src.get();
                }
                return std::unique_ptr<ByteSource>(std::move(src));
            };
        }
        if (!real_openocd) {
            opts.open_probe = [this](const ProbeConfig& cfg, const OpenOcdSourceOptions& ocd_opts)
                -> std::unique_ptr<ByteSource> {
                ProbeSourceFactory fn;
                {
                    std::scoped_lock lock(source_mutex);
                    fn = probe_factory;
                }
                if (fn) return fn(cfg, ocd_opts);
                auto src = std::make_unique<MemorySource>(ocd_opts.label.empty() ? cfg.interface_desc : ocd_opts.label);
                {
                    std::scoped_lock lock(source_mutex);
                    last_source = src.get();
                }
                return std::unique_ptr<ByteSource>(std::move(src));
            };
        }
        thread = std::thread([this, opts = std::move(opts)] { exit_code = serve(in, out, opts); });
    }
    ~Harness() {
        finish();
        fs::remove_all(root);
    }
    void set_source_factory(SourceFactory fn) {
        std::scoped_lock lock(source_mutex);
        source_factory = std::move(fn);
    }
    void set_probe_factory(ProbeSourceFactory fn) {
        std::scoped_lock lock(source_mutex);
        probe_factory = std::move(fn);
    }
    MemorySource* source() {
        std::scoped_lock lock(source_mutex);
        return last_source;
    }
    std::vector<Device> current_devices() {
        std::scoped_lock lock(devices_mutex);
        if (override_devices) return extra_devices;
        auto devs = list_devices();
        devs.insert(devs.end(), extra_devices.begin(), extra_devices.end());
        return devs;
    }
    Device plug_probe(std::string vid, std::string pid, std::string serial = "", std::string name = "Debug Probe",
                      std::string id = "", std::string family_hint = "") {
        Device d;
        d.kind = DeviceKind::Probe;
        d.path = "";
        d.name = std::move(name);
        d.title = d.name;
        d.family_hint = std::move(family_hint);
        d.usb = os::UsbInfo{.vid = std::move(vid),
                            .pid = std::move(pid),
                            .product = d.name,
                            .manufacturer = {},
                            .serial = std::move(serial),
                            .location = "1-2"};
        if (id.empty()) {
            DebugProbe dp{d.name, d.usb->vid, d.usb->pid, d.usb->serial, d.usb->location, d.usb->product, {}};
            id = probe_device_id(dp);
        }
        d.id = std::move(id);
        std::scoped_lock lock(devices_mutex);
        extra_devices.push_back(d);
        return d;
    }
    Device plug(const std::string& path, std::string id = "", std::string name = "") {
        Device d;
        d.kind = DeviceKind::Serial;
        d.path = path;
        if (name.empty()) {
            name = os::to_utf8(os::from_utf8(path).filename());
            if (name.empty()) name = path;
        }
        d.name = std::move(name);
        if (id.empty()) {
            os::PortInfo p;
            p.path = d.path;
            p.name = d.name;
            id = device_id(p);
        }
        d.id = std::move(id);
        d.title = d.name;
        d.access = os::port_access(d.path);
        std::scoped_lock lock(devices_mutex);
        extra_devices.push_back(d);
        return d;
    }
    void finish() {
        in_buf.close();
        if (thread.joinable()) thread.join();
    }
    json call(json request) {
        const int id = next_id++;
        request["id"] = id;
        in_buf.push(request.dump());
        for (;;) {
            const auto line = out_buf.next();
            assert(line && "no reply from serve");
            auto j = json::parse(*line);
            if (j.contains("id") && j["id"] == id) return j;
            events.push_back(std::move(j));
        }
    }
    void push_raw(const std::string& line) { in_buf.push(line); }
    json next_line() {
        const auto line = out_buf.next();
        assert(line);
        return json::parse(*line);
    }
    // The run list, as an array.
    json runs() { return call({{"cmd", "runs"}})["result"]; }
    // Snapshot value of the whole run (0 until the first frame).
    unsigned samples() {
        const auto r = call({{"cmd", "snapshot"}, {"window_s", 0}});
        assert(r["ok"]);
        return r["result"]["tree"].is_null() ? 0 : r["result"]["tree"]["value"].get<unsigned>();
    }
};

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

void hello_and_runs() {
    Harness h("malgrind_serve_a");
    const auto hello = h.call({{"cmd", "hello"}});
    assert(hello["ok"] && hello["result"]["version"] == MALGRIND_VERSION &&
           hello["result"]["data_dir"] == os::to_utf8(h.root / "runs"));
    assert(hello["result"].contains("pio")); // a path or null
    assert(h.runs() == json::array());
    const auto devs = h.call({{"cmd", "devices"}});
    assert(devs["ok"] && devs["result"]["devices"].is_array() && devs["result"]["can_grant_access"].is_boolean());
    assert(h.call({{"cmd", "ports"}})["ok"] == false);
    assert(h.call({{"cmd", "boards"}})["ok"] == false);
}

void bad_input() {
    Harness h("malgrind_serve_b");
    h.push_raw("not json");
    auto bad = h.next_line();
    assert(bad["ok"] == false && bad["id"].is_null() && contains(bad["error"], "JSON"));
    h.push_raw(R"({"id":7,"cmd":"nope"})");
    auto unknown = h.next_line();
    assert(unknown["id"] == 7 && unknown["ok"] == false && contains(unknown["error"], "unknown command"));
    h.push_raw(R"({"id":8})");
    assert(h.next_line()["ok"] == false); // no cmd
    assert(h.call({{"cmd", "hello"}})["ok"]); // and the loop carries on
    assert(h.call({{"cmd", "open"}, {"run", "../x"}})["ok"] == false); // a run id is never a path
}

void attach_snapshot_stop() {
    Harness h("malgrind_serve_c");
    const auto dev = h.plug("fake-port-c", "usb-10c4-ea60-sn-0001", "ttyUSB0");
    const auto attached = h.call({{"cmd", "attach"}, {"port", dev.path}, {"name", "t"}});
    assert(attached["ok"] && attached["result"]["run"]["name"] == "t");
    assert(attached["result"]["run"]["device_id"] == dev.id && attached["result"]["run"]["port"] == dev.name);
    const std::string id = attached["result"]["run"]["id"];
    assert(h.samples() == 0 && h.call({{"cmd", "functions"}, {"window_s", 0}})["result"]["functions"].empty());
    h.source()->push(make_stream(2)); // 8 samples
    assert(wait_for([&] { return h.samples() == 8; }));
    const auto fn = h.call({{"cmd", "functions"}, {"window_s", 0}, {"focus", "0x400d1fff"}})["result"];
    assert(fn["total"] == 8 && fn["callees"].size() == 2 && fn["series"].size() == 1 && fn["series"][0][1] == 8);
    assert(h.call({{"cmd", "functions"}, {"window_s", 0}})["result"]["series"].empty());
    const auto v1_snap = h.call({{"cmd", "snapshot"}, {"window_s", 0}, {"core", 1}})["result"];
    assert(v1_snap["tree"]["value"] == 2);
    assert(v1_snap["stats"]["caps"] == malgrind::kCapSamples && v1_snap["stats"]["agent_state"] == 1);
    assert(h.call({{"cmd", "memory"}})["result"] == json({{"regions", json::array()}}));
    assert(h.call({{"cmd", "tasks"}})["result"] == json({{"tasks", json::array()}}));
    assert(h.call({{"cmd", "memcheck"}})["result"] ==
           json({{"leaks", json::array()}, {"errors", json::array()}, {"dropped", 0}, {"live_bytes", 0}, {"live_allocs", 0}}));

    auto runs = h.runs();
    assert(runs.size() == 1 && runs[0]["id"] == id && runs[0]["live"] == true);
    assert(h.call({{"cmd", "delete"}, {"run", id}})["ok"] == false); // the current run cannot be deleted
    std::this_thread::sleep_for(20ms);
    assert(h.call({{"cmd", "stop"}})["ok"]);
    runs = h.runs();
    assert(runs[0]["duration_ms"].get<unsigned>() > 0 && !runs[0].contains("live") && runs[0]["ncores"] == 2);

    // history: open the stored run, the same picture comes back
    const auto opened = h.call({{"cmd", "open"}, {"run", id}});
    assert(opened["ok"] && h.samples() == 8);
    assert(h.call({{"cmd", "update"}, {"run", id}, {"name", "renamed"}, {"notes", "n"}})["result"]["name"] == "renamed");
    assert(h.call({{"cmd", "open"}, {"run", "missing"}})["ok"] == false);
    assert(h.call({{"cmd", "stop"}})["ok"] && h.samples() == 0); // closing the view
    assert(h.call({{"cmd", "delete"}, {"run", id}})["ok"] && h.runs().empty());
}

void busy_port() {
    Harness h("malgrind_serve_d");
    const auto dev = h.plug("fake-busy", "usb-10c4-ea60-sn-busy", "ttyUSB0");
    h.set_source_factory([](const Device&, unsigned) {
        return os::OpenFailure{os::OpenError::Busy, false, "busy"};
    });
    const auto probe = h.call({{"cmd", "probe"}, {"port", dev.path}});
    assert(probe["ok"] && probe["result"]["busy"] == true && probe["result"]["agent"] == false);
    const auto attach = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "x"}});
    assert(attach["ok"] == false && contains(attach["error"], "busy"));
    assert(h.runs().empty()); // a failed attach leaves no run behind
}

void probe_finds_agent() {
    Harness h("malgrind_serve_e");
    const auto dev = h.plug("fake-agent", "usb-10c4-ea60-sn-agent", "ttyUSB0");
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->push(make_stream(1));
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto probe = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(probe["ok"] && probe["result"]["busy"] == false && probe["result"]["agent"] == true);
    assert(probe["result"]["arch"] == "xtensa" && probe["result"]["ncores"] == 2);
    assert(probe["result"]["caps"] == malgrind::kCapSamples && probe["result"]["state"] == 1);
    assert(probe["result"]["family"] == "esp32" && probe["result"]["family_name"] == "ESP32 (ESP-IDF)");
    assert(h.runs().empty()); // probing records nothing

    // Silent board reports empty family.
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto silent = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(silent["ok"] && silent["result"]["agent"] == false);
    assert(silent["result"]["family"] == "" && silent["result"]["family_name"] == "");

    // Agent frame for ArmV7M reports STM32 family; an arch with no registered family reports empty family.
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        malgrind::Profiler p({"arm", malgrind::Arch::ArmV7M, 72'000'000, 1}, 997);
        const std::uint32_t pc = 0x08001000;
        p.record(0, &pc, 1, 10);
        std::vector<std::uint8_t> bytes;
        p.flush([&](const std::uint8_t* d, std::size_t n) { bytes.insert(bytes.end(), d, d + n); }, [] {}, 100);
        src->push(bytes);
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto stm_agent = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(stm_agent["ok"] && stm_agent["result"]["agent"] == true && stm_agent["result"]["arch"] == "armv7m");
    assert(stm_agent["result"]["family"] == "stm32" && stm_agent["result"]["family_name"] == "STM32 (Cortex-M)");

    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        malgrind::Profiler p({"host", malgrind::Arch::Host, 1'000'000, 1}, 997);
        const std::uint32_t pc = 0x08001000;
        p.record(0, &pc, 1, 10);
        std::vector<std::uint8_t> bytes;
        p.flush([&](const std::uint8_t* d, std::size_t n) { bytes.insert(bytes.end(), d, d + n); }, [] {}, 100);
        src->push(bytes);
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto unhandled = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(unhandled["ok"] && unhandled["result"]["agent"] == true && unhandled["result"]["arch"] == "host");
    assert(unhandled["result"]["family"] == "" && unhandled["result"]["family_name"] == "");
}

const json& method(const json& result, const char* id) {
    for (const auto& m : result["methods"])
        if (m["id"] == id) return m;
    assert(false && "method missing");
    static const json none;
    return none;
}

// A fake pio in the harness's home, so "no PlatformIO" does not hide the project's own reason.
void fake_pio(Harness& h) {
    const auto pio = os::venv_bin(h.root / "home/.platformio/penv") / os::exe_name("pio");
    fs::create_directories(pio.parent_path());
    fs::copy_file(MALGRIND_FAKE_TOOL, pio, fs::copy_options::overwrite_existing);
}

json add_project(Harness& h, json args) {
    args["cmd"] = "project_add";
    return h.call(std::move(args));
}

void probe_lists_methods() {
    Harness h("malgrind_serve_m");
    const auto dev = h.plug("fake-agent-m", "usb-10c4-ea60-sn-m1", "ttyUSB0");
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->push(make_stream(1));
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto with_agent = h.call({{"cmd", "probe"}, {"port", dev.path}});
    assert(method(with_agent["result"], "attach")["available"] == true);
    assert(method(with_agent["result"], "no_flash")["available"] == false);
    assert(contains(method(with_agent["result"], "no_flash")["why"], "debug probe"));

    // a project that cannot take the agent is named as the reason new_run is unavailable (an Arduino project here)
    fake_pio(h);
    const auto proj = h.root / "arduino";
    fs::create_directories(proj);
    std::ofstream(proj / "platformio.ini") << "[env:a]\nplatform = espressif32\nframework = arduino\n";
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto quiet_dev = h.plug("fake-quiet-m", "usb-10c4-ea60-sn-m2", "ttyUSB1");
    const auto silent = h.call({{"cmd", "probe"}, {"device", quiet_dev.id}, {"project", os::to_utf8(proj)}});
    assert(silent["ok"] && silent["result"]["agent"] == false);
    assert(method(silent["result"], "attach")["available"] == false);
    assert(method(silent["result"], "new_run")["available"] == false && contains(method(silent["result"], "new_run")["why"], "arduino"));

    // No project and no family_hint / agent -> no matching target family for new_run.
    const auto no_proj = h.call({{"cmd", "probe"}, {"device", quiet_dev.id}});
    assert(no_proj["ok"] && method(no_proj["result"], "new_run")["available"] == false);
    assert(contains(method(no_proj["result"], "new_run")["why"], "family"));

    // A supported STM32 project enables new_run; an unsupported platform reports the family mismatch from family_for_project.
    const auto stm_proj = h.root / "stm32";
    fs::create_directories(stm_proj);
    std::ofstream(stm_proj / "platformio.ini") << "[env:a]\nplatform = ststm32\nframework = stm32cube\n";
    const auto added_stm = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(stm_proj)}});
    assert(added_stm["ok"]);
    const std::string stm_id = added_stm["result"]["id"];
    const auto stm_probe = h.call({{"cmd", "probe"}, {"device", quiet_dev.id}, {"project", stm_id}});
    assert(stm_probe["ok"] && method(stm_probe["result"], "new_run")["available"] == true);

    // A supported RP2040 project also enables new_run.
    const auto rp_proj = h.root / "rp2040";
    fs::create_directories(rp_proj);
    std::ofstream(rp_proj / "platformio.ini") << "[env:pico]\nplatform = raspberrypi\nframework = arduino\n";
    const auto added_rp = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(rp_proj)}});
    assert(added_rp["ok"]);
    const std::string rp_id = added_rp["result"]["id"];
    const auto rp_probe = h.call({{"cmd", "probe"}, {"device", quiet_dev.id}, {"project", rp_id}});
    assert(rp_probe["ok"] && method(rp_probe["result"], "new_run")["available"] == true);

    const auto avr_proj = h.root / "avr";
    fs::create_directories(avr_proj);
    std::ofstream(avr_proj / "platformio.ini") << "[env:a]\nplatform = atmelavr\nframework = arduino\n";
    const auto added_avr = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(avr_proj)}});
    assert(added_avr["ok"]);
    const std::string avr_id = added_avr["result"]["id"];
    const auto avr_probe = h.call({{"cmd", "probe"}, {"device", quiet_dev.id}, {"project", avr_id}});
    assert(avr_probe["ok"] && method(avr_probe["result"], "new_run")["available"] == false);
    assert(contains(method(avr_probe["result"], "new_run")["why"], "atmelavr"));
}

void remove_agent_restores_or_says_why_not() {
    Harness h("malgrind_serve_n");
    const auto dev = h.plug("fake-port-n", "usb-10c4-ea60-sn-n1", "ttyUSB0");
    const auto bad = h.call({{"cmd", "remove_agent"}, {"device", dev.id}, {"how", "burn"}});
    assert(bad["ok"] == false && contains(bad["error"], "restore"));
    // the default is a restore: without PlatformIO (and so without esptool) the reason is given, not a rebuild
    const auto none = h.call({{"cmd", "remove_agent"}, {"device", dev.id}});
    assert(none["ok"] == false && contains(none["error"], "PlatformIO"));
    assert(h.call({{"cmd", "remove_agent"}, {"device", dev.id}, {"how", "rebuild"}})["ok"] == false); // needs dir, env
}

void projects_of_every_kind() {
    Harness h("malgrind_serve_p");
    fake_pio(h);
    const auto mine = h.root / "my-firmware";
    fs::create_directories(mine);
    std::ofstream(mine / "platformio.ini") << "[env:a]\n[env:b]\n";

    // a folder of the user's own: trusted at once, named after the folder, its environments listed
    const auto folder = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(mine)}});
    assert(folder["ok"] && folder["result"]["source"] == "folder" && folder["result"]["trusted"] == true);
    assert(folder["result"]["name"] == "my-firmware" && folder["result"]["envs"] == json::array({"a", "b"}) && folder["result"]["problem"] == "");
    assert(add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(h.root / "nothing")}})["ok"] == false);

    // no code: only a name; a file that is not an ELF is refused
    const auto none = add_project(h, {{"source", "none"}, {"name", "Water simulator"}});
    assert(none["ok"] && none["result"]["has_source"] == false && none["result"]["trusted"] == false && none["result"]["envs"].empty());
    std::ofstream(h.root / "not.elf") << "text";
    const auto bad_elf = add_project(h, {{"source", "none"}, {"elf", os::to_utf8(h.root / "not.elf")}});
    assert(bad_elf["ok"] == false && contains(bad_elf["error"], "ELF"));

    // links: only github https, nothing that git would run
    for (const char* url : {"file:///etc", "ext::sh -c id", "https://example.com/o/r", "-oProxyCommand=x"})
        assert(add_project(h, {{"source", "git"}, {"url", url}})["ok"] == false);

    // nothing can be built without source code, or before a cloned project is trusted
    const auto dev = h.plug("not-a-serial", "port-not-serial", "not-serial");
    h.set_source_factory([](const Device&, unsigned) {
        return os::OpenFailure{os::OpenError::Other, false, "not a serial port"};
    });
    const std::string none_id = none["result"]["id"];
    const auto no_source = h.call({{"cmd", "new_run"}, {"project", none_id}, {"device", dev.id}, {"name", "x"}});
    assert(no_source["ok"] == false && contains(no_source["error"], "no source code"));
    const auto probe = h.call({{"cmd", "probe"}, {"device", dev.id}, {"project", none_id}});
    assert(probe["ok"] == false); // not a serial port: the probe says so before it says anything else
    assert(h.call({{"cmd", "new_run"}, {"project", "nope"}, {"device", dev.id}, {"name", "x"}})["ok"] == false);

    // rename, environment, trust; a wrong environment is refused
    const std::string id = folder["result"]["id"];
    assert(h.call({{"cmd", "project_update"}, {"project", id}, {"name", "renamed"}, {"env", "b"}})["result"]["env"] == "b");
    assert(h.call({{"cmd", "project_update"}, {"project", id}, {"env", "zzz"}})["ok"] == false);

    // removing a project never deletes the user's folder
    assert(h.call({{"cmd", "project_remove"}, {"project", id}})["ok"] && fs::exists(mine / "platformio.ini"));
    assert(h.call({{"cmd", "project_remove"}, {"project", id}})["ok"] == false);
    assert(h.call({{"cmd", "projects"}})["result"].size() == 1);
}

void a_cloned_project_must_be_trusted() {
    Harness h("malgrind_serve_q");
    fake_pio(h);
    const auto bin = h.root / "bin";
    fs::create_directories(bin);
    fs::copy_file(MALGRIND_FAKE_TOOL, bin / os::exe_name("git"), fs::copy_options::overwrite_existing);
    os::set_env("PATH", path_with(bin));

    const auto added = add_project(h, {{"source", "git"}, {"url", "https://github.com/o/repo"}});
    assert(added["ok"] && added["result"]["name"] == "repo" && added["result"]["trusted"] == false && added["result"]["envs"] == json::array({"a"}));
    const fs::path cloned_dir = os::from_utf8(added["result"]["dir"].get<std::string>());
    assert(cloned_dir.parent_path().filename() == "firmware" && cloned_dir.filename() == "x");
    assert(added["result"]["url"] == "https://github.com/o/repo.git");
    const std::string id = added["result"]["id"];

    // untrusted: it is not built, and the probe says why
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto dev = h.plug("fake-port-q", "usb-10c4-ea60-sn-q1", "ttyUSB0");
    const auto refused = h.call({{"cmd", "new_run"}, {"project", id}, {"device", dev.id}, {"name", "x"}});
    assert(refused["ok"] == false && contains(refused["error"], "not been trusted"));
    const auto probe = h.call({{"cmd", "probe"}, {"device", dev.id}, {"project", id}});
    assert(probe["ok"] && method(probe["result"], "new_run")["available"] == false && contains(method(probe["result"], "new_run")["why"], "trusted"));

    // after the user's yes the same project is buildable as far as the checks go
    assert(h.call({{"cmd", "project_update"}, {"project", id}, {"trusted", true}})["result"]["trusted"] == true);
    const auto after = h.call({{"cmd", "probe"}, {"device", dev.id}, {"project", id}});
    assert(method(after["result"], "new_run")["available"] == true);

    // a failed clone leaves no project behind
    os::set_env("FAKE_GIT", "fail");
    const auto failed = add_project(h, {{"source", "git"}, {"url", "https://github.com/o/other"}});
    assert(failed["ok"] == false && h.call({{"cmd", "projects"}})["result"].size() == 1);
}

void runs_belong_to_projects() {
    Harness h("malgrind_serve_r");
    const auto dev = h.plug("fake-port-r1", "usb-10c4-ea60-sn-r1", "ttyUSB0");
    const auto other = h.plug("fake-port-r2", "usb-10c4-ea60-sn-r2", "ttyUSB1");
    const auto none = add_project(h, {{"source", "none"}, {"name", "Water simulator"}});
    const std::string id = none["result"]["id"];
    assert(h.call({{"cmd", "projects"}})["result"][0]["run_count"] == 0 && h.call({{"cmd", "projects"}})["result"][0]["last_run"].is_null());
    const auto run = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}, {"project", id}});
    assert(run["ok"] && run["result"]["run"]["project_id"] == id);
    assert(h.call({{"cmd", "attach"}, {"device", other.id}, {"name", "b"}, {"project", "missing"}})["ok"] == false);
    h.call({{"cmd", "stop"}});
    const auto list = h.call({{"cmd", "projects"}})["result"];
    assert(list[0]["run_count"] == 1 && list[0]["last_run"].is_string() && h.runs()[0]["project_id"] == id);
}

void setup_reports_the_machine() {
    Harness h("malgrind_serve_t");
    const auto setup = h.call({{"cmd", "setup"}});
    assert(setup["ok"] && setup["result"]["pio"].is_null() && setup["result"]["can_grant_access"].is_boolean() &&
           setup["result"]["os"].is_string() && !setup["result"]["user"].get<std::string>().empty());
    assert(setup["result"].contains("python") && setup["result"].contains("git"));
    assert(setup["result"]["python_hint"].is_string() && !setup["result"]["python_hint"].get<std::string>().empty());
    fake_pio(h);
    assert(h.call({{"cmd", "setup"}})["result"]["pio"].is_string()); // found in the home's PlatformIO folder now
}

void install_pio_refuses_what_it_cannot_do() {
    Harness h("malgrind_serve_u");
    fake_pio(h); // there is a pio already
    const auto again = h.call({{"cmd", "install_pio"}});
    assert(again["ok"] == false && contains(again["error"], "already installed"));
    fs::remove(os::venv_bin(h.root / "home/.platformio/penv") / os::exe_name("pio"));
    os::set_env("PATH", os::to_utf8(h.root / "nonexistent")); // and no Python to install it with
    const auto nopython = h.call({{"cmd", "install_pio"}});
    assert(nopython["ok"] == false && contains(nopython["error"], "Python 3 was not found"));
}

void devices_tell_access_and_reject_unknown() {
    Harness h("malgrind_serve_v");
    const auto devs = h.call({{"cmd", "devices"}});
    assert(devs["ok"] && devs["result"]["devices"].is_array() && devs["result"]["can_grant_access"].is_boolean());
    for (const auto& d : devs["result"]["devices"]) {
        assert(d.contains("id") && d.contains("kind") && d.contains("path") && d.contains("name") && d.contains("title"));
        assert(d.contains("usb") && d.contains("family_hint") && d.contains("family_hint_name"));
        assert(d["access"]["ok"].is_boolean() && d["access"].contains("group") && d["access"]["in_group"].is_boolean() &&
               d["access"]["fixable"].is_boolean());
    }
    const auto unknown = h.call({{"cmd", "grant_access"}, {"device", "/dev/ttyUSB99"}});
    assert(unknown["ok"] == false && contains(unknown["error"], "is not plugged in"));
    assert(h.call({{"cmd", "grant_access"}, {"port", "/etc/passwd"}})["ok"] == false); // only ports the engine found itself
}

void a_port_without_permission_is_reported_not_opened() {
    Harness h("malgrind_serve_w");
    fake_pio(h);
    const auto dev = h.plug("fake-denied", "usb-10c4-ea60-sn-denied", "ttyUSB0");
    h.set_source_factory([](const Device&, unsigned) {
        return os::OpenFailure{os::OpenError::Denied, false, "permission denied"};
    });
    const auto probe = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(probe["ok"] && probe["result"]["denied"] == true && probe["result"]["agent"] == false);
    assert(method(probe["result"], "attach")["available"] == false && contains(method(probe["result"], "attach")["why"], "Grant access"));
    assert(method(probe["result"], "new_run")["available"] == false && contains(method(probe["result"], "new_run")["why"], "Grant access"));
    // opening it for real names the fix too
    const auto attach = h.call({{"cmd", "attach"}, {"port", dev.path}, {"name", "x"}});
    assert(attach["ok"] == false && contains(attach["error"], "no permission") && contains(attach["error"], "Grant access"));
}

// A buildable project for the settings tests: a folder with two environments (the harness's agent files are the repository's).
std::string settings_project(Harness& h) {
    fake_pio(h);
    const auto dir = h.root / "settings-proj";
    fs::create_directories(dir);
    std::ofstream(dir / "platformio.ini") << "[env:a]\n[env:b]\n";
    const auto added = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(dir)}});
    assert(added["ok"]);
    return added["result"]["id"];
}

void devices_shape_and_id_resolution() {
    Harness h("malgrind_serve_dev");
    const auto proj_id = settings_project(h);

    Device serial;
    serial.id = "usb-10c4-ea60-sn-0001";
    serial.kind = DeviceKind::Serial;
    serial.path = "/dev/ttyUSB0";
    serial.name = "ttyUSB0";
    serial.title = "Silicon Labs CP210x USB-UART";
    serial.usb = os::UsbInfo{"10c4", "ea60", "CP2102", "Silicon Labs", "0001", "1-3"};
    serial.access = os::PortAccess{false, "dialout", false, true};
    serial.family_hint = "esp32";

    Device probe;
    probe.id = "probe-0483-374b-sn-st01";
    probe.kind = DeviceKind::Probe;
    probe.path = "";
    probe.name = "ST-Link";
    probe.title = "ST-Link";
    probe.usb = std::nullopt;
    probe.access = os::PortAccess{};
    probe.family_hint = "";

    {
        std::scoped_lock lock(h.devices_mutex);
        h.override_devices = true;
        h.extra_devices = {serial, probe};
    }

    const auto res = h.call({{"cmd", "devices"}});
    assert(res["ok"]);
    assert(res["result"]["can_grant_access"] == os::can_grant_access());
    const auto& list = res["result"]["devices"];
    assert(list.size() == 2);

    const auto& s = list[0];
    assert(s["id"] == "usb-10c4-ea60-sn-0001");
    assert(s["kind"] == "serial");
    assert(s["path"] == "/dev/ttyUSB0");
    assert(s["name"] == "ttyUSB0");
    assert(s["title"] == "Silicon Labs CP210x USB-UART");
    assert(s["usb"]["vid"] == "10c4" && s["usb"]["pid"] == "ea60");
    assert(s["usb"]["product"] == "CP2102" && s["usb"]["manufacturer"] == "Silicon Labs");
    assert(s["usb"]["serial"] == "0001" && s["usb"]["location"] == "1-3");
    assert(s["access"]["ok"] == false && s["access"]["group"] == "dialout" &&
           s["access"]["in_group"] == false && s["access"]["fixable"] == true);
    assert(s["family_hint"] == "esp32" && s["family_hint_name"] == "ESP32 (ESP-IDF)");

    const auto& p = list[1];
    assert(p["id"] == "probe-0483-374b-sn-st01");
    assert(p["kind"] == "probe");
    assert(p["path"] == "");
    assert(p["name"] == "ST-Link");
    assert(p["title"] == "ST-Link");
    assert(p["usb"].is_null());
    assert(p["access"]["ok"] == true && p["access"]["group"] == "" &&
           p["access"]["in_group"] == false && p["access"]["fixable"] == false);
    assert(p["family_hint"] == "" && p["family_hint_name"] == "");

    // Unknown device returns exact message.
    const auto missing = h.call({{"cmd", "attach"}, {"device", "usb-missing"}, {"name", "x"}});
    assert(missing["ok"] == false && missing["error"] == "device usb-missing is not plugged in");

    // Missing device/port argument.
    const auto no_arg = h.call({{"cmd", "attach"}, {"name", "x"}});
    assert(no_arg["ok"] == false && contains(no_arg["error"], "missing argument: device"));

    // Probe device is rejected by commands requiring a serial port.
    for (const json& req : {json{{"cmd", "attach"}, {"device", probe.id}, {"name", "x"}},
                            json{{"cmd", "grant_access"}, {"device", probe.id}},
                            json{{"cmd", "chip_id"}, {"device", probe.id}},
                            json{{"cmd", "remove_agent"}, {"device", probe.id}},
                            json{{"cmd", "run_plan"}, {"project", proj_id}, {"device", probe.id}}}) {
        const auto err = h.call(req);
        assert(err["ok"] == false && contains(err["error"], "not a serial port"));
    }

    // `probe` on a probe device returns probe diagnostics (here missing USB info).
    const auto probe_res = h.call({{"cmd", "probe"}, {"device", probe.id}});
    assert(probe_res["ok"] && probe_res["result"]["kind"] == "probe");
    assert(contains(probe_res["result"]["error"], "USB"));
    assert(method(probe_res["result"], "attach")["available"] == false);
    assert(method(probe_res["result"], "no_flash")["available"] == false);

    // attach {device: "<id>"} resolves device id to MemorySource and saves device_id + port on RunMeta.
    const auto attached = h.call({{"cmd", "attach"}, {"device", serial.id}, {"name", "by-id"}});
    assert(attached["ok"]);
    assert(attached["result"]["run"]["device_id"] == serial.id);
    assert(attached["result"]["run"]["port"] == "ttyUSB0");
    h.source()->push(make_stream(1));
    assert(wait_for([&] { return h.samples() == 4; }));
    assert(h.call({{"cmd", "stop"}})["ok"]);
    const auto runs = h.runs();
    assert(runs.size() == 1 && runs[0]["device_id"] == serial.id && runs[0]["port"] == "ttyUSB0");
    RunStore store(h.root / "runs");
    const auto stored = store.list().at(0);
    assert(stored.device_id == serial.id && stored.port == "ttyUSB0");
}

void run_plan_shows_what_was_detected_and_what_would_run() {
    Harness h("malgrind_serve_x");
    const auto dev = h.plug("fake-port-x", "usb-10c4-ea60-sn-x1", "ttyUSB0");
    const auto id = settings_project(h);
    const auto plan = h.call({{"cmd", "run_plan"}, {"project", id}, {"device", dev.id}, {"mode", "new_run"}})["result"];
    assert(plan["detected"]["env"] == "a" && plan["detected"]["rate_hz"] == 997 && plan["detected"]["check_memory"] == false);
    assert(plan["caps"]["memcheck"] == true && plan["envs"] == json::array({"a", "b"}));
    assert(plan["last"].is_null() && plan["error"] == "");
    assert(plan["elf_hint"] == os::to_utf8(fs::absolute(h.root / "settings-proj/.pio/malgrind/build/a/firmware.elf").lexically_normal()));
    const std::string build = plan["commands"]["build"], upload = plan["commands"]["upload"];
    assert(json(build) == plan["detected_commands"]["build"] && json(upload) == plan["detected_commands"]["upload"]); // no settings: the detected commands
    assert(contains(build, " run -d ") && !contains(build, "upload") && contains(upload, "-t upload --upload-port " + dev.path));
    assert(plan["commands"]["env"].is_array() && !plan["commands"]["env"].empty()); // the agent's environment is shown too

    // the panel's current fields change the preview, from the same function the run uses
    const auto edited = h.call({{"cmd", "run_plan"}, {"project", id}, {"device", dev.id},
                                {"settings", {{"env", "b"}, {"extra_args", "--verbose"}, {"rate_hz", 500}}}})["result"];
    assert(contains(edited["commands"]["build"], " -e b --verbose") && contains(edited["commands"]["upload"], "--verbose"));

    // a field that does not hold up is reported next to the commands, so the panel can show it while it is being edited
    const auto bad = h.call({{"cmd", "run_plan"}, {"project", id}, {"port", dev.path}, {"settings", {{"rate_hz", 9}}}});
    assert(bad["ok"] && contains(bad["result"]["error"], "between 10 and 5000") && bad["result"]["commands"].is_null());
    assert(!fs::exists(h.root / "settings-proj/.pio")); // a preview writes nothing into the project

    // attach only needs the ELF
    const auto attach = h.call({{"cmd", "run_plan"}, {"project", id}, {"device", dev.id}, {"mode", "attach"}})["result"];
    assert(attach["commands"].is_null() && attach["detected"].contains("elf"));
    assert(attach["caps"]["memcheck"] == false);
}

void new_run_checks_every_setting_before_the_board_is_touched() {
    Harness h("malgrind_serve_y");
    const auto dev = h.plug("fake-port-y", "usb-10c4-ea60-sn-y1", "ttyUSB0");
    const auto id = settings_project(h);
    for (const json& bad : {json{{"rate_hz", 9}}, json{{"env", "zzz"}}, json{{"build_command", "no-such-program-xyz"}},
                            json{{"elf", os::to_utf8(h.root / "no-such.elf")}}}) {
        const auto r = h.call({{"cmd", "new_run"}, {"project", id}, {"device", dev.id}, {"name", "x"}, {"settings", bad}});
        assert(r["ok"] == false);
    }
    assert(h.runs().empty() && !fs::exists(h.root / "settings-proj/.pio")); // nothing started, nothing written
}

void a_run_keeps_its_settings_and_the_project_offers_them_again() {
    Harness h("malgrind_serve_z");
    const auto dev = h.plug("fake-port-z", "usb-10c4-ea60-sn-z1", "ttyUSB0");
    const auto usb9 = h.plug("fake-port-9", "usb-10c4-ea60-sn-usb9", "ttyUSB9");
    const auto id = settings_project(h);
    const auto missing = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}, {"project", id},
                                 {"settings", {{"elf", os::to_utf8(h.root / "no-such.elf")}}}});
    assert(missing["ok"] == false && contains(missing["error"], "ELF does not exist") && h.runs().empty());

    const json used = {
        {"rate_hz", 700},
        {"extra_args", "-v"},
        {"upload_command", join_command({MALGRIND_FAKE_TOOL, "upload", "--port", "fake-port-7"})},
        {"build_command", join_command({MALGRIND_FAKE_TOOL, "build", "-e", "zzz"})},
        {"elf", os::to_utf8(os::from_utf8(MALGRIND_FAKE_TOOL))},
    };
    const auto run = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}, {"project", id}, {"settings", used}});
    assert(run["ok"] && run["result"]["run"]["settings"]["rate_hz"] == 700);
    h.call({{"cmd", "stop"}});
    assert(h.runs()[0]["settings"]["extra_args"] == "-v" && h.runs()[0]["settings"]["upload_command"] == used["upload_command"]); // the run keeps ALL of it
    // ...but only the board-independent part is offered again: a remembered upload line would flash the board it was typed for
    // (its --upload-port), a remembered build line its environment, a remembered ELF would label a new build with the old functions
    const auto plan = h.call({{"cmd", "run_plan"}, {"project", id}, {"device", usb9.id}})["result"];
    assert(plan["last"]["rate_hz"] == 700 && plan["last"]["extra_args"] == "-v");
    assert(plan["last"]["upload_command"] == "" && plan["last"]["build_command"] == "" && plan["last"]["elf"] == "");
    assert(contains(plan["commands"]["upload"], "--upload-port " + usb9.path)); // on another board the line names that board
    assert(h.call({{"cmd", "projects"}})["result"][0]["last_settings"]["rate_hz"] == 700);

    // a typed upload line that names no port is allowed, with a warning; one that names the chosen port is not warned about
    const auto typed = [&](const std::string& line) {
        return h.call({{"cmd", "run_plan"}, {"project", id}, {"device", usb9.id}, {"settings", {{"upload_command", line}}}})["result"];
    };
    const std::string no_port_cmd = join_command({MALGRIND_FAKE_TOOL, "upload"});
    const std::string with_port_cmd = join_command({MALGRIND_FAKE_TOOL, "upload", "--port", usb9.path});
    assert(contains(typed(no_port_cmd)["warning"], usb9.path) && typed(no_port_cmd)["error"] == "");
    assert(typed(with_port_cmd)["warning"] == "" && plan["warning"] == "");
}

void open_while_live_refused() {
    Harness h("malgrind_serve_f");
    const auto dev = h.plug("fake-port-f", "usb-10c4-ea60-sn-f1", "ttyUSB0");
    const auto id = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}})["result"]["run"]["id"];
    const auto second = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "b"}});
    assert(second["ok"] == false);
    const auto open = h.call({{"cmd", "open"}, {"run", id}});
    assert(open["ok"] == false && contains(open["error"], "stop the live run first"));
}

void diff_runs_works_while_live() {
    Harness h("malgrind_serve_diff");
    const auto dev = h.plug("fake-port-diff", "usb-10c4-ea60-sn-diff1", "ttyUSB0");

    // Record run A (4 samples: 3 on core 0, 1 on core 1).
    const std::string id_a = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "run-a"}})["result"]["run"]["id"];
    h.source()->push(make_stream(1));
    assert(wait_for([&] { return h.samples() == 4; }));
    assert(h.call({{"cmd", "stop"}})["ok"]);

    // Record run B (9 samples: 8 on core 0, 1 on core 1).
    const std::string id_b = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "run-b"}})["result"]["run"]["id"];
    h.source()->push(make_stream(1));
    {
        malgrind::Profiler p({"test", malgrind::Arch::Xtensa, 240'000'000, 2}, 997);
        const std::uint32_t pcs[] = {0x400d1900, 0x400d2000, 0x400d3000};
        for (int i = 0; i < 5; ++i) p.record(0, pcs, 3, 100);
        std::vector<std::uint8_t> extra;
        p.flush([&](const std::uint8_t* d, std::size_t n) { extra.insert(extra.end(), d, d + n); }, [] {}, 5000);
        h.source()->push(extra);
    }
    assert(wait_for([&] { return h.samples() == 9; }));
    assert(h.call({{"cmd", "stop"}})["ok"]);

    // Start a third live session and verify diff runs cleanly without interrupting it.
    const std::string id_live = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "live-c"}})["result"]["run"]["id"];
    h.source()->push(make_stream(1));
    assert(wait_for([&] { return h.samples() == 4; }));
    auto is_live_run = [&](const std::string& id) {
        for (const auto& r : h.runs())
            if (r["id"] == id) return r.contains("live") && r["live"] == true;
        return false;
    };
    assert(is_live_run(id_live));

    const auto diff = h.call({{"cmd", "diff"}, {"a", id_a}, {"b", id_b}});
    assert(diff["ok"]);
    const auto& res = diff["result"];
    assert(res["a"]["id"] == id_a && res["b"]["id"] == id_b);
    assert(res["total_a"] == 4 && res["total_b"] == 9);
    assert(!res["functions"].empty());
    assert(res["tree"]["name"] == "all" && res["tree"]["a"] == 4 && res["tree"]["b"] == 9 && res["tree"]["value"] == 9);

    const auto diff_c1 = h.call({{"cmd", "diff"}, {"a", id_a}, {"b", id_b}, {"core", 1}});
    assert(diff_c1["ok"] && diff_c1["result"]["total_a"] == 1 && diff_c1["result"]["total_b"] == 1);

    assert(h.call({{"cmd", "diff"}, {"a", "missing"}, {"b", id_b}})["ok"] == false);
    assert(h.call({{"cmd", "diff"}, {"a", id_a}})["ok"] == false);

    // Live session continues receiving frames uninterrupted.
    h.source()->push(make_stream(1));
    assert(wait_for([&] { return h.samples() == 8; }));
    assert(is_live_run(id_live));
    assert(h.call({{"cmd", "stop"}})["ok"]);
}

std::string read_binary(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    assert(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void export_and_write_file_commands() {
    Harness h("malgrind_serve_export");
    const auto dev = h.plug("fake-port-exp", "usb-10c4-ea60-sn-exp1", "ttyUSB0");
    const auto out_dir = h.root / "exports";
    fs::create_directories(out_dir);

    // No open run -> export fails clearly.
    const auto no_open = h.call({{"cmd", "export"}, {"format", "folded"}, {"path", os::to_utf8(out_dir / "none.folded")}});
    assert(no_open["ok"] == false && contains(no_open["error"], "no run is open"));

    // Unknown export format -> fails clearly.
    const auto bad_fmt = h.call({{"cmd", "export"}, {"format", "csv"}, {"path", os::to_utf8(out_dir / "bad.csv")}});
    assert(bad_fmt["ok"] == false && contains(bad_fmt["error"], "unknown export format"));

    // Record stored run A (4 samples: 3 on core 0, 1 on core 1) and close it.
    const std::string id_a = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "stored-alpha"}})["result"]["run"]["id"];
    h.source()->push(make_stream(1));
    assert(wait_for([&] { return h.samples() == 4; }));
    assert(h.call({{"cmd", "stop"}})["ok"]);

    // Start live run B; before the first frame arrives, export reports no samples.
    const std::string id_b = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "live-beta"}})["result"]["run"]["id"];
    const auto empty_live = h.call({{"cmd", "export"}, {"format", "folded"}, {"path", os::to_utf8(out_dir / "empty.folded")}});
    assert(empty_live["ok"] == false && contains(empty_live["error"], "no samples"));

    // Push 2 flushes (8 samples: 6 on core 0, 2 on core 1) to live run B.
    h.source()->push(make_stream(2));
    assert(wait_for([&] { return h.samples() == 8; }));

    // Export current live run as folded (run omitted).
    const auto live_folded_path = out_dir / "live.folded";
    const auto live_folded = h.call({{"cmd", "export"}, {"format", "folded"}, {"path", os::to_utf8(live_folded_path)}});
    assert(live_folded["ok"]);
    const auto folded_bytes = read_binary(live_folded_path);
    assert(live_folded["result"]["bytes"] == folded_bytes.size());
    assert(contains(folded_bytes, "core0;0x400d2fff;0x400d1fff;0x400d1000 6\n"));
    assert(contains(folded_bytes, "core1;0x400d2fff;0x400d1fff;0x400d1800 2\n"));

    // Export current live run as speedscope with explicit matching run id and core filter.
    const auto live_ss_path = out_dir / "live.speedscope.json";
    const auto live_ss = h.call({{"cmd", "export"},
                                 {"run", id_b},
                                 {"format", "speedscope"},
                                 {"core", 0},
                                 {"path", os::to_utf8(live_ss_path)}});
    assert(live_ss["ok"]);
    const auto ss_text = read_binary(live_ss_path);
    assert(live_ss["result"]["bytes"] == ss_text.size());
    const auto ss_json = json::parse(ss_text);
    assert(ss_json["$schema"] == "https://www.speedscope.app/file-format-schema.json");
    assert(ss_json["name"] == "live-beta");
    assert(ss_json["activeProfileIndex"] == 0);
    assert(ss_json["profiles"].size() == 1 && ss_json["profiles"][0]["name"] == "core0");
    assert(ss_json["profiles"][0]["endValue"] == 6);

    // Export stored run A while live run B stays open and unaffected.
    const auto stored_ss_path = out_dir / "stored.speedscope.json";
    const auto stored_ss = h.call({{"cmd", "export"},
                                   {"run", id_a},
                                   {"format", "speedscope"},
                                   {"path", os::to_utf8(stored_ss_path)}});
    assert(stored_ss["ok"]);
    const auto stored_json = json::parse(read_binary(stored_ss_path));
    assert(stored_json["name"] == "stored-alpha");
    assert(stored_json["profiles"].size() == 2);
    assert(stored_json["profiles"][0]["endValue"] == 3 && stored_json["profiles"][1]["endValue"] == 1);

    const auto stored_folded_path = out_dir / "stored.folded";
    const auto stored_folded = h.call({{"cmd", "export"},
                                       {"run", id_a},
                                       {"format", "folded"},
                                       {"core", 1},
                                       {"path", os::to_utf8(stored_folded_path)}});
    assert(stored_folded["ok"] && read_binary(stored_folded_path) == "core1;0x400d2fff;0x400d1fff;0x400d1800 1\n");

    // Live run B is still current and still receiving frames.
    h.source()->push(make_stream(1));
    assert(wait_for([&] { return h.samples() == 12; }));

    // Time-windowed export (`at_ms: 5000` with `window_s: 5` covering [0, 5000 ms] on stored run A).
    const auto windowed_path = out_dir / "windowed.folded";
    const auto windowed = h.call({{"cmd", "export"},
                                  {"run", id_a},
                                  {"format", "folded"},
                                  {"window_s", 5},
                                  {"at_ms", 5000},
                                  {"path", os::to_utf8(windowed_path)}});
    assert(windowed["ok"] && contains(read_binary(windowed_path), "core0;0x400d2fff;0x400d1fff;0x400d1000 3\n"));
    const auto future_empty = h.call({{"cmd", "export"},
                                      {"run", id_a},
                                      {"format", "folded"},
                                      {"window_s", 5},
                                      {"at_ms", 100000},
                                      {"path", os::to_utf8(out_dir / "future.folded")}});
    assert(future_empty["ok"] == false && contains(future_empty["error"], "no samples"));

    // Export error cases: missing run, filtered out core, unwritable path.
    assert(h.call({{"cmd", "export"}, {"run", "missing"}, {"format", "folded"}, {"path", os::to_utf8(out_dir / "m.folded")}})["ok"] == false);
    const auto no_core = h.call({{"cmd", "export"}, {"format", "speedscope"}, {"core", 9}, {"path", os::to_utf8(out_dir / "c9.json")}});
    assert(no_core["ok"] == false && contains(no_core["error"], "no samples"));
    assert(h.call({{"cmd", "export"}, {"format", "folded"}, {"path", os::to_utf8(h.root / "missing-dir/x.folded")}})["ok"] == false);

    // write_file: PNG binary round-trip (non-ASCII/null/high bytes covering all padding lengths) and SVG text round-trip.
    // Raw bytes: "\x89PNG\r\n\x1a\n\x00\x01\xff\x80" (12 bytes) -> "iVBORw0KGgoAAf+A"
    const std::string png_bytes("\x89PNG\r\n\x1a\n\x00\x01\xff\x80", 12);
    const auto png_path = out_dir / "chart.png";
    const auto wf_png = h.call({{"cmd", "write_file"}, {"path", os::to_utf8(png_path)}, {"base64", "iVBORw0KGgoAAf+A"}});
    assert(wf_png["ok"] && wf_png["result"]["bytes"] == 12);
    assert(read_binary(png_path) == png_bytes);

    // 1-byte and 2-byte padding round-trip: "\x00\xff" -> "AP8=", "\x41" -> "QQ=="
    const auto pad1_path = out_dir / "pad1.bin";
    const auto wf_pad1 = h.call({{"cmd", "write_file"}, {"path", os::to_utf8(pad1_path)}, {"base64", "AP8="}});
    assert(wf_pad1["ok"] && wf_pad1["result"]["bytes"] == 2 && read_binary(pad1_path) == std::string("\x00\xff", 2));
    const auto pad2_path = out_dir / "pad2.bin";
    const auto wf_pad2 = h.call({{"cmd", "write_file"}, {"path", os::to_utf8(pad2_path)}, {"base64", "QQ=="}});
    assert(wf_pad2["ok"] && wf_pad2["result"]["bytes"] == 1 && read_binary(pad2_path) == "A");

    // SVG round-trip: "<svg/>" -> "PHN2Zy8+"
    const auto svg_path = out_dir / "chart.svg";
    const auto wf_svg = h.call({{"cmd", "write_file"}, {"path", os::to_utf8(svg_path)}, {"base64", "PHN2Zy8+"}});
    assert(wf_svg["ok"] && wf_svg["result"]["bytes"] == 6 && read_binary(svg_path) == "<svg/>");

    // write_file rejects malformed base64 and unwritable paths.
    for (const char* bad : {"a", "abc", "YWJ!", "YW Jj", "YW_j", "====", "A===", "YQ==YQ==", "YWI=YQ==", "YW=A"}) {
        const auto err = h.call({{"cmd", "write_file"}, {"path", os::to_utf8(out_dir / "bad.bin")}, {"base64", bad}});
        assert(err["ok"] == false && contains(err["error"], "base64"));
    }
    assert(!fs::exists(out_dir / "bad.bin"));
    assert(h.call({{"cmd", "write_file"}, {"path", os::to_utf8(h.root / "missing-dir/x.bin")}, {"base64", "QQ=="}})["ok"] == false);

    assert(h.call({{"cmd", "stop"}})["ok"]);
}

void eof_finishes_run() {
    Harness h("malgrind_serve_g");
    const auto dev = h.plug("fake-port-g", "usb-10c4-ea60-sn-g1", "ttyUSB0");
    h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}});
    h.source()->push(make_stream(1));
    assert(wait_for([&] { return h.samples() == 4; }));
    std::this_thread::sleep_for(20ms);
    h.in_buf.close(); // the app window closed
    h.thread.join();
    assert(h.exit_code == 0);
    RunStore store(h.root / "runs");
    assert(store.list().size() == 1 && store.list()[0].duration_ms > 0);
}

void open_after_upload_reenumerates() {
    int calls = 0;
    int open_calls = 0;
    MemorySource* raw = nullptr;
    auto source = open_after_upload(
        "usb-1a86-7523-sn-99",
        [&]() -> std::vector<Device> {
            if (++calls == 1) return {}; // USB device still re-enumerating after reset
            Device d;
            d.id = "usb-1a86-7523-sn-99";
            d.kind = DeviceKind::Serial;
            d.path = "fake-acm1";
            d.name = "ttyACM1";
            return {d};
        },
        [&](const Device& d, unsigned) -> std::variant<std::unique_ptr<ByteSource>, os::OpenFailure> {
            if (++open_calls == 1) return os::OpenFailure{os::OpenError::NotFound, true, "re-enumerating"};
            auto mem = std::make_unique<MemorySource>(d.name);
            raw = mem.get();
            return std::unique_ptr<ByteSource>(std::move(mem));
        });
    assert(calls == 3 && open_calls == 2 && raw != nullptr);
    raw->push(make_stream(1));
    std::array<std::uint8_t, 64> buf{};
    const auto n = source->read(buf, 500);
    assert(n && *n > 0);
}

void v2_probe_memory_tasks_and_commands() {
    Harness h("malgrind_serve_v2");
    const auto dev = h.plug("fake-v2", "usb-10c4-ea60-sn-v2", "ttyUSB0");

    // Empty session defaults and live-only control errors.
    assert(h.call({{"cmd", "memory"}})["result"] == json({{"regions", json::array()}}));
    assert(h.call({{"cmd", "tasks"}})["result"] == json({{"tasks", json::array()}}));
    assert(h.call({{"cmd", "memcheck"}})["result"] ==
           json({{"leaks", json::array()}, {"errors", json::array()}, {"dropped", 0}, {"live_bytes", 0}, {"live_allocs", 0}}));
    const auto no_live_start = h.call({{"cmd", "agent_start"}, {"rate_hz", 500}});
    assert(no_live_start["ok"] == false && contains(no_live_start["error"], "no run is live"));
    const auto no_live_stop = h.call({{"cmd", "agent_stop"}});
    assert(no_live_stop["ok"] == false && contains(no_live_stop["error"], "no run is live"));

    // Probe detects a sleeping v2 HELLO beacon with its caps and state == 0.
    const std::uint32_t v2_caps = malgrind::kCapSamples | malgrind::kCapHeap | malgrind::kCapTasks;
    const malgrind::HelloPayload sleeping_hello{
        .seq = 1,
        .arch = malgrind::Arch::Xtensa,
        .ncores = 2,
        .cpu_hz = 240'000'000,
        .agent_ver = 2,
        .caps = v2_caps,
        .state = 0,
        .rate_hz = 997,
        .analyses = 0,
    };
    h.set_source_factory([&](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->push(make_hello_frame(sleeping_hello));
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto probe = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(probe["ok"] && probe["result"]["agent"] == true);
    assert(probe["result"]["arch"] == "xtensa" && probe["result"]["ncores"] == 2);
    assert(probe["result"]["caps"] == v2_caps && probe["result"]["state"] == 0);
    assert(probe["result"]["family"] == "esp32");
    assert(method(probe["result"], "attach")["available"] == true);

    // Reset factory to default MemorySource for attach.
    h.set_source_factory({});
    const auto attached = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "v2-run"}, {"settings", {{"rate_hz", 400}}}});
    assert(attached["ok"]);
    const std::string run_id = attached["result"]["run"]["id"];

    // Sleeping HELLO triggers auto-start and initializes snapshot stats before sample frames arrive.
    h.source()->push(make_hello_frame(sleeping_hello));
    assert(wait_for([&] { return decode_commands(h.source()->written()).size() == 1; }));
    {
        const auto cmds = decode_commands(h.source()->written());
        assert(cmds[0].cmd == malgrind::kCmdStart && cmds[0].rate_hz == 400 && cmds[0].analyses == v2_caps);
    }
    assert(wait_for([&] {
        const auto snap = h.call({{"cmd", "snapshot"}, {"window_s", 0}});
        return snap["ok"] && !snap["result"]["tree"].is_null();
    }));
    {
        const auto snap = h.call({{"cmd", "snapshot"}, {"window_s", 0}})["result"];
        assert(snap["tree"]["value"] == 0);
        assert(snap["stats"]["caps"] == v2_caps && snap["stats"]["agent_state"] == 0);
    }

    // Wake up and push HEAP, TASKS, and SAMPLES across two timestamps.
    auto awake_hello = sleeping_hello;
    awake_hello.seq = 2;
    awake_hello.state = 1;
    awake_hello.rate_hz = 400;
    awake_hello.analyses = v2_caps;
    h.source()->push(make_hello_frame(awake_hello));

    const malgrind::HeapRegion hr1[] = {{malgrind::kHeapInternal, 200'000, 150'000, 140'000, 100'000}};
    const malgrind::HeapRegion hr2[] = {{malgrind::kHeapInternal, 200'000, 130'000, 120'000, 90'000}};
    h.source()->push(make_heap_frame(3, 1000, hr1));
    h.source()->push(make_heap_frame(4, 3000, hr2));

    malgrind::TaskSample ts1[2]{};
    ts1[0].handle = 1;
    std::memcpy(ts1[0].name, "idle", 4);
    ts1[0].prio = 0;
    ts1[0].core = 0;
    ts1[0].state = 1;
    ts1[0].runtime = 200;
    ts1[0].stack_hwm = 768;
    ts1[1].handle = 2;
    std::memcpy(ts1[1].name, "sensor", 6);
    ts1[1].prio = 4;
    ts1[1].core = 0xff;
    ts1[1].state = 0;
    ts1[1].runtime = 800;
    ts1[1].stack_hwm = 400;
    h.source()->push(make_tasks_frame(5, 1000, 1000, ts1));

    malgrind::TaskSample ts2[2]{};
    ts2[0] = ts1[0];
    ts2[0].runtime = 300;
    ts2[1] = ts1[1];
    ts2[1].runtime = 700;
    ts2[1].stack_hwm = 600; // recovered, but min across [1000..3000] is 400 -> warn true
    h.source()->push(make_tasks_frame(6, 3000, 1000, ts2));

    h.source()->push(make_stream(1)); // 4 samples
    assert(wait_for([&] {
        if (h.samples() != 4) return false;
        const auto m = h.call({{"cmd", "memory"}, {"window_s", 0}})["result"];
        const auto t = h.call({{"cmd", "tasks"}, {"window_s", 0}})["result"];
        return !m["regions"].empty() && m["regions"][0]["series"].size() == 2 && t["tasks"].size() == 2;
    }));

    const auto mem_all = h.call({{"cmd", "memory"}, {"window_s", 0}})["result"];
    assert(mem_all["regions"][0]["name"] == "internal");
    assert(mem_all["regions"][0]["series"][0] == json::array({1000, 150'000, 140'000, 100'000, 200'000}));
    assert(mem_all["regions"][0]["series"][1] == json::array({3000, 130'000, 120'000, 90'000, 200'000}));
    const auto mem_win = h.call({{"cmd", "memory"}, {"window_s", 1}, {"at_ms", 1500}})["result"];
    assert(mem_win["regions"][0]["series"].size() == 1 && mem_win["regions"][0]["series"][0][0] == 1000);

    const auto tasks_all = h.call({{"cmd", "tasks"}, {"window_s", 0}})["result"];
    assert(tasks_all["tasks"][0]["name"] == "sensor" && std::abs(tasks_all["tasks"][0]["cpu_pct"].get<double>() - 75.0) < 1e-6);
    assert(tasks_all["tasks"][0]["core"].is_null() && tasks_all["tasks"][0]["stack_hwm"] == 400 && tasks_all["tasks"][0]["warn"] == true);
    assert(tasks_all["tasks"][1]["name"] == "idle" && std::abs(tasks_all["tasks"][1]["cpu_pct"].get<double>() - 25.0) < 1e-6);
    assert(tasks_all["tasks"][1]["core"] == 0 && tasks_all["tasks"][1]["warn"] == false);

    const auto tasks_recent = h.call({{"cmd", "tasks"}, {"window_s", 1}})["result"];
    assert(tasks_recent["tasks"][0]["name"] == "sensor" && std::abs(tasks_recent["tasks"][0]["cpu_pct"].get<double>() - 70.0) < 1e-6);
    assert(tasks_recent["tasks"][0]["stack_hwm"] == 600 && tasks_recent["tasks"][0]["warn"] == false);

    const auto fn = h.call({{"cmd", "functions"}, {"window_s", 0}, {"focus", "0x400d1fff"}})["result"];
    assert(fn["series"].size() == 1 && fn["series"][0][1] == 4);

    // Explicit agent_stop and agent_start send COMMAND frames.
    assert(h.call({{"cmd", "agent_stop"}})["ok"]);
    assert(h.call({{"cmd", "agent_start"}, {"rate_hz", 250}, {"analyses", malgrind::kCapSamples | malgrind::kCapHeap}})["ok"]);
    const auto cmds = decode_commands(h.source()->written());
    assert(cmds.size() == 3);
    assert(cmds[1].cmd == malgrind::kCmdStop);
    assert(cmds[2].cmd == malgrind::kCmdStart && cmds[2].rate_hz == 250 &&
           cmds[2].analyses == (malgrind::kCapSamples | malgrind::kCapHeap));

    const auto live_snap = h.call({{"cmd", "snapshot"}, {"window_s", 0}})["result"];
    assert(live_snap["stats"]["caps"] == v2_caps && live_snap["stats"]["agent_state"] == 1);

    // Stop, open replay, and verify memory, tasks, functions series, and snapshot match while control commands fail.
    assert(h.call({{"cmd", "stop"}})["ok"]);
    assert(h.call({{"cmd", "open"}, {"run", run_id}})["ok"]);
    assert(h.call({{"cmd", "agent_start"}, {"rate_hz", 500}})["ok"] == false);
    assert(h.call({{"cmd", "agent_stop"}})["ok"] == false);
    assert(h.call({{"cmd", "memory"}, {"window_s", 0}})["result"] == mem_all);
    assert(h.call({{"cmd", "tasks"}, {"window_s", 0}})["result"] == tasks_all);
    assert(h.call({{"cmd", "snapshot"}, {"window_s", 0}})["result"] == live_snap);
    assert(h.call({{"cmd", "functions"}, {"window_s", 0}, {"focus", "0x400d1fff"}})["result"]["series"] == fn["series"]);
    assert(h.call({{"cmd", "stop"}})["ok"]);
}

void v2_memcheck_rpc() {
    Harness h("malgrind_serve_memcheck");
    const auto dev = h.plug("fake-mc", "usb-10c4-ea60-sn-mc", "ttyUSB0");

    const json empty_mc = {
        {"leaks", json::array()},
        {"errors", json::array()},
        {"dropped", 0},
        {"live_bytes", 0},
        {"live_allocs", 0},
    };
    assert(h.call({{"cmd", "memcheck"}})["result"] == empty_mc);

    const auto attached =
        h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "mc-run"}, {"settings", {{"rate_hz", 500}}}});
    assert(attached["ok"]);
    const std::string run_id = attached["result"]["run"]["id"];
    assert(h.call({{"cmd", "memcheck"}})["result"] == empty_mc);

    const std::uint32_t mc_caps = malgrind::kCapSamples | malgrind::kCapAllocs | malgrind::kCapErrors;
    h.source()->push(make_hello_frame({
        .seq = 1,
        .arch = malgrind::Arch::Xtensa,
        .ncores = 2,
        .cpu_hz = 240'000'000,
        .agent_ver = 2,
        .caps = mc_caps,
        .state = 0,
        .rate_hz = 997,
        .analyses = 0,
    }));
    assert(wait_for([&] { return decode_commands(h.source()->written()).size() == 1; }));
    assert(decode_commands(h.source()->written())[0].analyses == mc_caps);

    const malgrind::AllocEvent allocs[] = {
        {.op = 0, .t_ms = 1000, .addr = 0x3f801000, .size = 200, .depth = 2, .pcs = {0x400d1000, 0x400d2000}},
        {.op = 0, .t_ms = 2000, .addr = 0x3f802000, .size = 100, .depth = 1, .pcs = {0x400d3000}},
        {.op = 1, .t_ms = 5000, .addr = 0x3f802000, .size = 0, .depth = 0, .pcs = {}},
        {.op = 0, .t_ms = 15000, .addr = 0x3f803000, .size = 50, .depth = 1, .pcs = {0x400d4000}},
    };
    h.source()->push(make_allocs_frame(2, allocs));

    malgrind::ErrorEvent err_oom{
        .seq = 3,
        .t_ms = 15000,
        .code = malgrind::kErrAllocFailed,
        .arg = 8192,
        .name = {},
        .depth = 1,
        .pcs = {0x400d5000},
    };
    std::memcpy(err_oom.name, "worker", 6);
    h.source()->push(make_error_frame(err_oom));

    malgrind::ErrorEvent err_drop{
        .seq = 4,
        .t_ms = 15000,
        .code = malgrind::kErrEventsDropped,
        .arg = 4,
        .name = {},
        .depth = 0,
        .pcs = {},
    };
    std::memcpy(err_drop.name, "events", 6);
    h.source()->push(make_error_frame(err_drop));

    assert(wait_for([&] {
        const auto mc = h.call({{"cmd", "memcheck"}});
        return mc["ok"] && mc["result"]["live_allocs"] == 2 && mc["result"]["dropped"] == 4 &&
               mc["result"]["errors"].size() == 1;
    }));

    // Default leak_age_s=30 at max_t_ms=15000 -> both live allocs are "possibly".
    const auto mc_default = h.call({{"cmd", "memcheck"}})["result"];
    assert(mc_default["live_bytes"] == 250 && mc_default["live_allocs"] == 2 && mc_default["dropped"] == 4);
    assert(mc_default["leaks"][0]["kind"] == "possibly" && mc_default["leaks"][0]["bytes"] == 200);
    assert(mc_default["leaks"][1]["kind"] == "possibly" && mc_default["leaks"][1]["bytes"] == 50);
    assert(mc_default["errors"][0]["kind"] == "alloc_failed" && mc_default["errors"][0]["arg"] == 8192);

    // Custom leak_age_s=10 -> 1000ms allocation (age 14s >= 10s) becomes "definitely".
    const auto mc_age10 = h.call({{"cmd", "memcheck"}, {"leak_age_s", 10}})["result"];
    assert(mc_age10["leaks"][0]["kind"] == "definitely" && mc_age10["leaks"][0]["bytes"] == 200);
    assert(mc_age10["leaks"][1]["kind"] == "possibly" && mc_age10["leaks"][1]["bytes"] == 50);

    // Scrubbing at_ms=3000 with leak_age_s=2 shows state before the free at 5000ms.
    const auto mc_scrub = h.call({{"cmd", "memcheck"}, {"at_ms", 3000}, {"leak_age_s", 2}})["result"];
    assert(mc_scrub["live_bytes"] == 300 && mc_scrub["live_allocs"] == 2 && mc_scrub["errors"].empty());
    assert(mc_scrub["leaks"][0]["kind"] == "definitely" && mc_scrub["leaks"][0]["bytes"] == 200);
    assert(mc_scrub["leaks"][1]["kind"] == "possibly" && mc_scrub["leaks"][1]["bytes"] == 100);

    // Invalid leak_age_s is rejected cleanly.
    assert(h.call({{"cmd", "memcheck"}, {"leak_age_s", -1}})["ok"] == false);
    assert(h.call({{"cmd", "memcheck"}, {"leak_age_s", "10"}})["ok"] == false);
    assert(h.call({{"cmd", "memcheck"}, {"leak_age_s", 1.5}})["ok"] == false);

    // Stop and open replay -> memcheck reports match live.
    assert(h.call({{"cmd", "stop"}})["ok"]);
    assert(h.call({{"cmd", "open"}, {"run", run_id}})["ok"]);
    assert(h.call({{"cmd", "memcheck"}, {"leak_age_s", 10}})["result"] == mc_age10);
    assert(h.call({{"cmd", "memcheck"}, {"at_ms", 3000}, {"leak_age_s", 2}})["result"] == mc_scrub);
    assert(h.call({{"cmd", "stop"}})["ok"]);
}

#ifndef _WIN32
void pty_serial_attach_and_busy() {
    Harness h("malgrind_serve_pty", /*real_serial=*/true);
    Pty pty;
    const auto dev = h.plug(pty.slave, "usb-10c4-ea60-sn-pty", "ttyPty0");
    const auto attached = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "pty"}});
    assert(attached["ok"] && attached["result"]["run"]["device_id"] == dev.id);
    pty.send(make_stream(1)); // 4 samples
    assert(wait_for([&] { return h.samples() == 4; }));
    assert(h.call({{"cmd", "stop"}})["ok"]);

    if (!exclusive_open_is_enforced()) {
        std::puts("pty_serial_attach_and_busy: busy check skipped, this platform does not enforce exclusive open on a pty");
        return;
    }
    Pty held;
    const auto held_dev = h.plug(held.slave);
    auto other_program = open_source(held.slave);
    assert(other_program);
    const auto probe = h.call({{"cmd", "probe"}, {"device", held_dev.id}});
    assert(probe["ok"] && probe["result"]["busy"] == true && probe["result"]["agent"] == false);
    const auto busy_attach = h.call({{"cmd", "attach"}, {"device", held_dev.id}, {"name", "busy"}});
    assert(busy_attach["ok"] == false && contains(busy_attach["error"], "busy"));
}
#endif

void probe_and_methods_for_debug_probes() {
    // 1. With real_openocd=true and no OpenOCD installed, missing OpenOCD is reported clearly.
    {
        Harness h("malgrind_serve_probe_noocd", false, /*real_openocd=*/true);
        {
            std::scoped_lock lock(h.devices_mutex);
            h.override_devices = true;
        }
        // No probe connected -> methods returns no_flash unavailable + aggregated probe_targets.
        const auto no_probe = h.call({{"cmd", "methods"}});
        assert(no_probe["ok"]);
        assert(method(no_probe["result"], "no_flash")["available"] == false);
        assert(contains(method(no_probe["result"], "no_flash")["why"], "debug probe"));
        assert(!no_probe["result"]["probe_targets"].empty());

        const auto stlink = h.plug_probe("0483", "374b", "ST1", "ST-Link");
        const auto p_no_cfg = h.call({{"cmd", "probe"}, {"device", stlink.id}});
        assert(p_no_cfg["ok"] && p_no_cfg["result"]["openocd"]["found"] == false);
        assert(method(p_no_cfg["result"], "no_flash")["available"] == false);
        assert(contains(p_no_cfg["result"]["error"], "target"));

        const auto p_with_cfg =
            h.call({{"cmd", "probe"}, {"device", stlink.id}, {"target_cfg", "target/stm32f4x.cfg"}});
        assert(p_with_cfg["ok"] && p_with_cfg["result"]["openocd"]["found"] == false);
        assert(p_with_cfg["result"]["openocd"]["package"] == "tool-openocd");
        assert(method(p_with_cfg["result"], "no_flash")["available"] == false);
        assert(contains(method(p_with_cfg["result"], "no_flash")["why"], "OpenOCD"));

        // Install fake openocd under home/.platformio/packages/tool-openocd -> probe becomes ready.
        const fs::path pkg_dir = h.root / "home/.platformio/packages/tool-openocd";
        fs::create_directories(pkg_dir / "bin");
        fs::create_directories(pkg_dir / "scripts/interface");
        fs::copy_file(MALGRIND_FAKE_TOOL, pkg_dir / "bin" / os::exe_name("openocd"), fs::copy_options::overwrite_existing);
        if constexpr (os::kind() != os::Kind::Windows) {
            fs::permissions(pkg_dir / "bin" / os::exe_name("openocd"), fs::perms::owner_all);
        }
        std::ofstream(pkg_dir / "scripts/interface/stlink.cfg") << "# stlink\n";

        const auto p_ready =
            h.call({{"cmd", "probe"}, {"device", stlink.id}, {"target_cfg", "target/stm32f4x.cfg"}});
        assert(p_ready["ok"] && p_ready["result"]["openocd"]["found"] == true);
        assert(p_ready["result"]["probe_config"]["target_cfg"] == "target/stm32f4x.cfg");
        assert(p_ready["result"]["probe_sample_mode"] == "dwt_pcsr");
        assert(contains(p_ready["result"]["probe_rate_hint"], "DWT PCSR"));
        assert(method(p_ready["result"], "no_flash")["available"] == true);
        assert(method(p_ready["result"], "attach")["available"] == false);

        const auto p_unknown_target =
            h.call({{"cmd", "probe"}, {"device", stlink.id}, {"target_cfg", "target/nrf52.cfg"}});
        assert(p_unknown_target["ok"] && p_unknown_target["result"]["probe_sample_mode"] == "auto");
        assert(contains(p_unknown_target["result"]["probe_rate_hint"], "Auto"));

        // `methods` resolves probe automatically from devices list, or by VID:PID.
        const auto m_auto = h.call({{"cmd", "methods"}, {"target_cfg", "target/stm32f4x.cfg"}});
        assert(m_auto["ok"] && method(m_auto["result"], "no_flash")["available"] == true);

        const auto m_vid = h.call({{"cmd", "methods"}, {"vid", "303a"}, {"pid", "1001"}, {"target_cfg", "target/esp32s3.cfg"}});
        assert(m_vid["ok"]);
        assert(m_vid["result"]["probe_sample_mode"] == "halt_resume");
        assert(contains(m_vid["result"]["probe_rate_hint"], "Halt/resume"));
        assert(m_vid["result"]["openocd"]["package"] == "tool-openocd-esp32");
        assert(m_vid["result"]["openocd"]["found"] == false); // tool-openocd-esp32 not installed
    }

    // 2. With open_probe hook enabled, ESP32 project infers target_cfg and HaltResume mode.
    {
        Harness h("malgrind_serve_probe_esp");
        const auto esp_probe = h.plug_probe("303a", "1001", "ESP1", "ESP USB JTAG (built-in)", "", "esp32");
        const fs::path proj_dir = h.root / "esp-s3";
        fs::create_directories(proj_dir);
        std::ofstream(proj_dir / "platformio.ini")
            << "[env:s3]\nplatform = espressif32\nframework = arduino\nboard = esp32-s3-devkitc-1\n";
        const auto added = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(proj_dir)}});
        assert(added["ok"]);
        const std::string proj_id = added["result"]["id"];

        const auto res = h.call({{"cmd", "probe"}, {"device", esp_probe.id}, {"project", proj_id}});
        assert(res["ok"] && res["result"]["kind"] == "probe");
        assert(res["result"]["family"] == "esp32");
        assert(res["result"]["probe_sample_mode"] == "halt_resume");
        assert(res["result"]["probe_config"]["target_cfg"] == "target/esp32s3.cfg");
        assert(method(res["result"], "no_flash")["available"] == true);
        assert(!res["result"]["probe_targets"].empty());

        // Invalid target_cfg is rejected cleanly.
        const auto bad = h.call({{"cmd", "probe"}, {"device", esp_probe.id}, {"target_cfg", "-c shutdown"}});
        assert(bad["ok"] && method(bad["result"], "no_flash")["available"] == false);
        assert(contains(bad["result"]["error"], "invalid target config"));
    }

    // 3. Raspberry Pi Debug Probe (2e8a:000c) remains family-neutral without project/override and resolves
    //    RP2040 (HaltResume) or STM32 (DwtPcsr) depending on the selected project.
    {
        Harness h("malgrind_serve_probe_rp");
        const auto rpi_probe = h.plug_probe("2e8a", "000c", "RPI1", "Raspberry Pi Debug Probe");

        const auto devs = h.call({{"cmd", "devices"}});
        assert(devs["ok"]);
        bool checked_probe_hint = false;
        for (const auto& d : devs["result"]["devices"]) {
            if (d["id"] == rpi_probe.id) {
                checked_probe_hint = true;
                assert(d["family_hint"] == "" && d["family_hint_name"] == "");
            }
        }
        assert(checked_probe_hint);

        const auto neutral = h.call({{"cmd", "probe"}, {"device", rpi_probe.id}});
        assert(neutral["ok"] && neutral["result"]["family"] == "" && neutral["result"]["family_name"] == "");
        assert(method(neutral["result"], "no_flash")["available"] == false);
        assert(contains(neutral["result"]["error"], "target"));

        const fs::path rp_dir = h.root / "rp-proj";
        fs::create_directories(rp_dir);
        std::ofstream(rp_dir / "platformio.ini")
            << "[env:pico]\nplatform = raspberrypi\nframework = arduino\nboard = pico\n";
        const auto added_rp = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(rp_dir)}});
        assert(added_rp["ok"]);

        const auto rp_res = h.call({{"cmd", "probe"}, {"device", rpi_probe.id}, {"project", added_rp["result"]["id"]}});
        assert(rp_res["ok"] && rp_res["result"]["family"] == "rp2040");
        assert(rp_res["result"]["probe_sample_mode"] == "halt_resume");
        assert(rp_res["result"]["probe_config"]["target_cfg"] == "target/rp2040.cfg");
        assert(method(rp_res["result"], "no_flash")["available"] == true);

        const fs::path stm_dir = h.root / "stm-proj";
        fs::create_directories(stm_dir);
        std::ofstream(stm_dir / "platformio.ini")
            << "[env:f4]\nplatform = ststm32\nframework = stm32cube\nboard = nucleo_f401re\n";
        const auto added_stm = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(stm_dir)}});
        assert(added_stm["ok"]);

        const auto stm_res = h.call({{"cmd", "probe"}, {"device", rpi_probe.id}, {"project", added_stm["result"]["id"]}});
        assert(stm_res["ok"] && stm_res["result"]["family"] == "stm32");
        assert(stm_res["result"]["probe_sample_mode"] == "dwt_pcsr");
        assert(stm_res["result"]["probe_config"]["target_cfg"] == "target/stm32f4x.cfg");
        assert(method(stm_res["result"], "no_flash")["available"] == true);
    }
}

void run_plan_no_flash() {
    Harness h("malgrind_serve_plan_nf");
    const auto stlink = h.plug_probe("0483", "374b", "ST2", "ST-Link");
    const auto esp_probe = h.plug_probe("303a", "1001", "ESP2", "ESP USB JTAG (built-in)", "", "esp32");

    // 1. No-code project works with no_flash without pio.
    const auto none = add_project(h, {{"source", "none"}, {"name", "NoCode"}});
    assert(none["ok"]);
    const std::string none_id = none["result"]["id"];

    const auto missing_target =
        h.call({{"cmd", "run_plan"}, {"project", none_id}, {"device", stlink.id}, {"mode", "no_flash"}});
    assert(missing_target["ok"]);
    assert(missing_target["result"]["commands"].is_null());
    assert(contains(missing_target["result"]["error"], "target"));
    assert(contains(missing_target["result"]["note"], "No firmware will be modified or flashed"));
    assert(missing_target["result"]["caps"]["memcheck"] == false);
    assert(!missing_target["result"]["probe_targets"].empty());

    const auto with_target = h.call({{"cmd", "run_plan"},
                                     {"project", none_id},
                                     {"device", stlink.id},
                                     {"mode", "no_flash"},
                                     {"settings", {{"target_cfg", "target/stm32f4x.cfg"}}}});
    assert(with_target["ok"] && with_target["result"]["error"] == "");
    assert(contains(with_target["result"]["commands"]["openocd"], "target/stm32f4x.cfg"));
    assert(contains(with_target["result"]["summary"], "No firmware will be modified or flashed"));

    // Invalid target_cfg reports error in result["error"] cleanly.
    const auto bad_target = h.call({{"cmd", "run_plan"},
                                    {"project", none_id},
                                    {"device", stlink.id},
                                    {"mode", "no_flash"},
                                    {"settings", {{"target_cfg", "bad\x1a" "cfg"}}}});
    assert(bad_target["ok"] && bad_target["result"]["commands"].is_null());
    assert(contains(bad_target["result"]["error"], "control characters"));

    // 2. Folder project infers target_cfg and plain .pio/build ELF even without pio.
    const fs::path proj_dir = h.root / "c3-proj";
    fs::create_directories(proj_dir / ".pio/build/c3");
    std::ofstream(proj_dir / "platformio.ini")
        << "[env:c3]\nplatform = espressif32\nframework = arduino\nboard = esp32-c3-devkitm-1\n";
    const unsigned char rv_hdr[20] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 243, 0};
    {
        std::ofstream out(proj_dir / ".pio/build/c3/firmware.elf", std::ios::binary);
        out.write(reinterpret_cast<const char*>(rv_hdr), sizeof rv_hdr);
    }
    const auto folder = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(proj_dir)}});
    assert(folder["ok"]);
    const std::string folder_id = folder["result"]["id"];

    const auto inferred =
        h.call({{"cmd", "run_plan"}, {"project", folder_id}, {"device", esp_probe.id}, {"mode", "no_flash"}});
    assert(inferred["ok"] && inferred["result"]["error"] == "");
    assert(inferred["result"]["detected"]["target_cfg"] == "target/esp32c3.cfg");
    assert(inferred["result"]["probe_sample_mode"] == "halt_resume");
    assert(contains(inferred["result"]["elf_hint"], "firmware.elf"));
    assert(contains(inferred["result"]["commands"]["openocd"], "target/esp32c3.cfg"));
}

void probe_run_end_to_end() {
    // 1. Injected open_probe verifies ProbeConfig/OpenOcdSourceOptions, flat samples, overhead_pct=null, and replay.
    {
        Harness h("malgrind_serve_probe_mock");
        const auto stlink = h.plug_probe("0483", "374b", "ST-MOCK", "ST-Link");
        const auto serial = h.plug("fake-serial", "usb-10c4-ea60-sn-s1", "ttyUSB0");
        const auto none = add_project(h, {{"source", "none"}, {"name", "ProbeProj"}});
        const std::string proj_id = none["result"]["id"];

        // Rejects serial device and bad settings.
        assert(h.call({{"cmd", "probe_run"}, {"device", serial.id}, {"name", "bad"}})["ok"] == false);
        assert(h.call({{"cmd", "probe_run"}, {"device", stlink.id}, {"name", "bad"}})["ok"] == false);
        assert(h.call({{"cmd", "probe_run"},
                       {"device", stlink.id},
                       {"name", "bad"},
                       {"target_cfg", "-c shutdown"}})["ok"] == false);

        ProbeConfig seen_cfg;
        OpenOcdSourceOptions seen_opts;
        MemorySource* raw_src = nullptr;
        h.set_probe_factory([&](const ProbeConfig& cfg, const OpenOcdSourceOptions& opts) {
            seen_cfg = cfg;
            seen_opts = opts;
            auto mem = std::make_unique<MemorySource>(opts.label);
            raw_src = mem.get();
            return std::unique_ptr<ByteSource>(std::move(mem));
        });

        const auto started = h.call({{"cmd", "probe_run"},
                                     {"device", stlink.id},
                                     {"project", proj_id},
                                     {"name", "flat-mock"},
                                     {"settings", {{"target_cfg", "target/stm32f4x.cfg"}}}});
        assert(started["ok"]);
        const std::string run_id = started["result"]["run"]["id"];
        assert(started["result"]["run"]["device_id"] == stlink.id);
        assert(started["result"]["run"]["settings"]["target_cfg"] == "target/stm32f4x.cfg");
        assert(seen_cfg.target_cfg == "target/stm32f4x.cfg");
        assert(seen_cfg.interface_desc == "ST-Link");

        // Push flat probe Hello (cpu_hz=0) + 2 sample flushes with kSleepPseudoPc.
        {
            std::array<std::uint8_t, 64> hbuf{};
            const std::size_t hn = malgrind::encode_hello(
                hbuf.data(), hbuf.size(),
                {.seq = 0, .arch = malgrind::Arch::ArmV7M, .ncores = 1, .cpu_hz = 0, .agent_ver = 2, .caps = malgrind::kCapSamples, .state = 1});
            raw_src->push(std::span<const std::uint8_t>(hbuf.data(), hn));

            malgrind::Profiler p({"probe", malgrind::Arch::ArmV7M, 0, 1}, 0);
            const std::uint32_t pc_a = 0x08001000;
            const std::uint32_t pc_sleep = kSleepPseudoPc;
            p.record(0, &pc_a, 1, 0);
            p.record(0, &pc_a, 1, 0);
            p.record(0, &pc_sleep, 1, 0);
            std::vector<std::uint8_t> bytes;
            p.flush([&](const std::uint8_t* d, std::size_t n) { bytes.insert(bytes.end(), d, d + n); }, [] {}, 0);
            raw_src->push(bytes);
            std::this_thread::sleep_for(30ms);
            p.record(0, &pc_a, 1, 0);
            bytes.clear();
            p.flush([&](const std::uint8_t* d, std::size_t n) { bytes.insert(bytes.end(), d, d + n); }, [] {}, 0);
            raw_src->push(bytes);
        }

        assert(wait_for([&] { return h.samples() == 4; }));
        const auto snap = h.call({{"cmd", "snapshot"}, {"window_s", 0}})["result"];
        assert(snap["stats"]["overhead_pct"].is_null());
        assert(snap["stats"]["caps"] == malgrind::kCapSamples);
        assert(snap["stats"]["arch"] == "armv7m");

        assert(h.call({{"cmd", "stop"}})["ok"]);
        const auto runs = h.runs();
        assert(runs.size() == 1 && runs[0]["id"] == run_id && runs[0]["settings"]["target_cfg"] == "target/stm32f4x.cfg");

        // Last settings remembered on project.
        const auto plan_again =
            h.call({{"cmd", "run_plan"}, {"project", proj_id}, {"device", stlink.id}, {"mode", "no_flash"}})["result"];
        assert(plan_again["last"]["target_cfg"] == "target/stm32f4x.cfg");

        // Replay matches.
        assert(h.call({{"cmd", "open"}, {"run", run_id}})["ok"]);
        assert(h.samples() == 4);
        assert(h.call({{"cmd", "stop"}})["ok"]);
    }

    // 2. Real OpenOcdSource path (open_probe unset) backed by fake_tool OpenOCD + fake addr2line + TCL RPC.
    {
        ScopedEnv save_rpc("FAKE_OPENOCD_RPC_LOG");
        ScopedEnv save_targets("FAKE_OPENOCD_TARGETS");
        ScopedEnv save_type("FAKE_OPENOCD_TARGET_TYPE");
        ScopedEnv save_req_halt("FAKE_OPENOCD_REQUIRE_HALT");
        ScopedEnv save_pc0("FAKE_OPENOCD_REG_PC_0");
        ScopedEnv save_pc1("FAKE_OPENOCD_REG_PC_1");
        ScopedEnv save_fail("FAKE_OPENOCD_FAIL");
        ScopedEnv save_path("PATH");

        Harness h("malgrind_serve_probe_real", false, /*real_openocd=*/true);
        const auto esp_probe = h.plug_probe("303a", "1001", "SN-REAL", "ESP USB JTAG (built-in)", "", "esp32");

        // Install fake tool-openocd-esp32 under home/.platformio/packages and xtensa addr2line on PATH.
        const fs::path pkg = h.root / "home/.platformio/packages/tool-openocd-esp32";
        fs::create_directories(pkg / "bin");
        fs::create_directories(pkg / "scripts/interface");
        fs::copy_file(MALGRIND_FAKE_TOOL, pkg / "bin" / os::exe_name("openocd"), fs::copy_options::overwrite_existing);
        if constexpr (os::kind() != os::Kind::Windows) {
            fs::permissions(pkg / "bin" / os::exe_name("openocd"), fs::perms::owner_all);
        }
        std::ofstream(pkg / "scripts/interface/stlink.cfg") << "# marker\n";

        const fs::path bin_dir = h.root / "tools";
        fs::create_directories(bin_dir);
        const fs::path a2l = bin_dir / os::exe_name("xtensa-esp32-elf-addr2line");
        fs::copy_file(MALGRIND_FAKE_TOOL, a2l, fs::copy_options::overwrite_existing);
        if constexpr (os::kind() != os::Kind::Windows) {
            fs::permissions(a2l, fs::perms::owner_all);
        }
        os::set_env("PATH", os::to_utf8(bin_dir));

        const fs::path elf = h.root / "app.elf";
        std::ofstream(elf, std::ios::binary) << "0x400d1000=sensor_poll\n0x400d2000=wifi_tx\n";

        const fs::path rpc_log = h.root / "rpc.log";
        os::set_env("FAKE_OPENOCD_RPC_LOG", os::to_utf8(rpc_log));
        os::set_env("FAKE_OPENOCD_TARGETS", "esp32.cpu0 esp32.cpu1");
        os::set_env("FAKE_OPENOCD_TARGET_TYPE", "esp32");
        os::set_env("FAKE_OPENOCD_REQUIRE_HALT", "1");
        os::set_env("FAKE_OPENOCD_REG_PC_0", "pc (/32): 0x400d1000,0xffffffff");
        os::set_env("FAKE_OPENOCD_REG_PC_1", "pc: 0x400d2000");

        // Startup failure leaves no orphan run behind.
        os::set_env("FAKE_OPENOCD_FAIL", "startup");
        const auto failed = h.call({{"cmd", "probe_run"},
                                    {"device", esp_probe.id},
                                    {"target_cfg", "target/esp32.cfg"},
                                    {"elf", os::to_utf8(elf)},
                                    {"name", "should-fail"}});
        assert(failed["ok"] == false && contains(failed["error"], "unable to open debug probe"));
        assert(h.runs().empty());
        os::unset_env("FAKE_OPENOCD_FAIL");

        const auto started = h.call({{"cmd", "probe_run"},
                                     {"device", esp_probe.id},
                                     {"target_cfg", "target/esp32.cfg"},
                                     {"elf", os::to_utf8(elf)},
                                     {"poll_hz", 100},
                                     {"name", "real-esp-probe"}});
        assert(started["ok"]);
        const std::string run_id = started["result"]["run"]["id"];

        assert(wait_for([&] {
            const auto fn = h.call({{"cmd", "functions"}, {"window_s", 0}})["result"];
            const std::string dump = fn.dump();
            return contains(dump, "sensor_poll") && contains(dump, "wifi_tx") && contains(dump, "[sleep]");
        }, 3000ms));

        const auto snap = h.call({{"cmd", "snapshot"}, {"window_s", 0}})["result"];
        assert(snap["stats"]["overhead_pct"].is_null());
        assert(snap["stats"]["ncores"] == 2);
        assert(snap["stats"]["caps"] == malgrind::kCapSamples);

        // Starting another run while probe_run is live is refused.
        assert(h.call({{"cmd", "probe_run"},
                       {"device", esp_probe.id},
                       {"target_cfg", "target/esp32.cfg"},
                       {"name", "dup"}})["ok"] == false);

        // Stop shuts down OpenOCD cleanly and persists the run.
        assert(h.call({{"cmd", "stop"}})["ok"]);
        {
            std::ifstream in(rpc_log, std::ios::binary);
            const std::string rpc((std::istreambuf_iterator<char>(in)), {});
            assert(contains(rpc, "shutdown\n"));
        }

        const auto runs = h.runs();
        assert(runs.size() == 1 && runs[0]["id"] == run_id && runs[0]["ncores"] == 2 && runs[0]["has_elf"] == true);

        // Stored run replays with symbolized functions and [sleep].
        assert(h.call({{"cmd", "open"}, {"run", run_id}})["ok"]);
        const auto replay_fn = h.call({{"cmd", "functions"}, {"window_s", 0}})["result"].dump();
        assert(contains(replay_fn, "sensor_poll") && contains(replay_fn, "wifi_tx") && contains(replay_fn, "[sleep]"));
        assert(h.call({{"cmd", "stop"}})["ok"]);
    }

    // 3. Real OpenOcdSource path for RP2040 with 2e8a:000c Raspberry Pi Debug Probe (dual-core HaltResume + arm-none-eabi-addr2line).
    {
        ScopedEnv save_rpc("FAKE_OPENOCD_RPC_LOG");
        ScopedEnv save_targets("FAKE_OPENOCD_TARGETS");
        ScopedEnv save_type("FAKE_OPENOCD_TARGET_TYPE");
        ScopedEnv save_req_halt("FAKE_OPENOCD_REQUIRE_HALT");
        ScopedEnv save_pc0("FAKE_OPENOCD_REG_PC_0");
        ScopedEnv save_pc1("FAKE_OPENOCD_REG_PC_1");
        ScopedEnv save_fail("FAKE_OPENOCD_FAIL");
        ScopedEnv save_path("PATH");

        Harness h("malgrind_serve_probe_rp2040", false, /*real_openocd=*/true);
        const auto rpi_probe = h.plug_probe("2e8a", "000c", "RP2040DBG", "Raspberry Pi Debug Probe");

        const fs::path pkg = h.root / "home/.platformio/packages/tool-openocd";
        fs::create_directories(pkg / "bin");
        fs::create_directories(pkg / "scripts/interface");
        fs::copy_file(MALGRIND_FAKE_TOOL, pkg / "bin" / os::exe_name("openocd"), fs::copy_options::overwrite_existing);
        if constexpr (os::kind() != os::Kind::Windows) {
            fs::permissions(pkg / "bin" / os::exe_name("openocd"), fs::perms::owner_all);
        }
        std::ofstream(pkg / "scripts/interface/stlink.cfg") << "# marker\n";

        const fs::path bin_dir = h.root / "tools";
        fs::create_directories(bin_dir);
        const fs::path a2l = bin_dir / os::exe_name("arm-none-eabi-addr2line");
        fs::copy_file(MALGRIND_FAKE_TOOL, a2l, fs::copy_options::overwrite_existing);
        if constexpr (os::kind() != os::Kind::Windows) {
            fs::permissions(a2l, fs::perms::owner_all);
        }
        os::set_env("PATH", os::to_utf8(bin_dir));

        const fs::path elf = h.root / "pico.elf";
        std::ofstream(elf, std::ios::binary) << "0x10001000=pico_core0_loop\n0x10002000=pico_core1_dsp\n";

        const fs::path rpc_log = h.root / "rp2040_rpc.log";
        os::set_env("FAKE_OPENOCD_RPC_LOG", os::to_utf8(rpc_log));
        os::set_env("FAKE_OPENOCD_TARGETS", "rp2040.core0 rp2040.core1");
        os::set_env("FAKE_OPENOCD_TARGET_TYPE", "cortex_m");
        os::set_env("FAKE_OPENOCD_REQUIRE_HALT", "1");
        os::set_env("FAKE_OPENOCD_REG_PC_0", "pc (/32): 0x10001000");
        os::set_env("FAKE_OPENOCD_REG_PC_1", "pc: 0x10002000");

        const auto started = h.call({{"cmd", "probe_run"},
                                     {"device", rpi_probe.id},
                                     {"target_cfg", "target/rp2040.cfg"},
                                     {"elf", os::to_utf8(elf)},
                                     {"poll_hz", 100},
                                     {"name", "real-rp2040-probe"}});
        assert(started["ok"]);
        const std::string run_id = started["result"]["run"]["id"];

        assert(wait_for([&] {
            const auto dump = h.call({{"cmd", "functions"}, {"window_s", 0}})["result"].dump();
            return contains(dump, "pico_core0_loop") && contains(dump, "pico_core1_dsp");
        }, 3000ms));

        const auto snap = h.call({{"cmd", "snapshot"}, {"window_s", 0}})["result"];
        assert(snap["stats"]["overhead_pct"].is_null());
        assert(snap["stats"]["arch"] == "armv7m");
        assert(snap["stats"]["ncores"] == 2);

        assert(h.call({{"cmd", "stop"}})["ok"]);
        assert(h.call({{"cmd", "open"}, {"run", run_id}})["ok"]);
        const auto replay_fn = h.call({{"cmd", "functions"}, {"window_s", 0}})["result"].dump();
        assert(contains(replay_fn, "pico_core0_loop") && contains(replay_fn, "pico_core1_dsp"));
        assert(h.call({{"cmd", "stop"}})["ok"]);
    }
}

} // namespace

int main() {
    hello_and_runs();
    bad_input();
    attach_snapshot_stop();
    busy_port();
    probe_finds_agent();
    probe_lists_methods();
    projects_of_every_kind();
    a_cloned_project_must_be_trusted();
    runs_belong_to_projects();
    devices_shape_and_id_resolution();
    setup_reports_the_machine();
    install_pio_refuses_what_it_cannot_do();
    run_plan_shows_what_was_detected_and_what_would_run();
    new_run_checks_every_setting_before_the_board_is_touched();
    a_run_keeps_its_settings_and_the_project_offers_them_again();
    devices_tell_access_and_reject_unknown();
    a_port_without_permission_is_reported_not_opened();
    remove_agent_restores_or_says_why_not();
    open_while_live_refused();
    diff_runs_works_while_live();
    export_and_write_file_commands();
    eof_finishes_run();
    open_after_upload_reenumerates();
    v2_probe_memory_tasks_and_commands();
    v2_memcheck_rpc();
    probe_and_methods_for_debug_probes();
    run_plan_no_flash();
    probe_run_end_to_end();
#ifndef _WIN32
    pty_serial_attach_and_busy();
#endif
    std::puts("serve ok");
}

#include "backup.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// "24-0a-c4-aa-bb-0c": six lowercase hex pairs
bool valid_mac(std::string_view mac) {
    if (mac.size() != 17) return false;
    for (std::size_t i = 0; i < mac.size(); ++i) {
        if (i % 3 == 2 ? mac[i] != '-' : !(is_hex(mac[i]) && !std::isupper(static_cast<unsigned char>(mac[i])))) return false;
    }
    return true;
}

} // namespace

std::optional<std::string> parse_mac(std::string_view out) {
    constexpr std::string_view key = "MAC: ";
    const auto at = out.find(key);
    if (at == std::string_view::npos) return std::nullopt;
    const auto text = out.substr(at + key.size(), 17);
    if (text.size() != 17) return std::nullopt;
    std::string mac;
    for (std::size_t i = 0; i < 17; ++i) {
        if (i % 3 == 2) {
            if (text[i] != ':') return std::nullopt;
            mac += '-';
        } else {
            if (!is_hex(text[i])) return std::nullopt;
            mac += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        }
    }
    return mac;
}

std::optional<std::uint32_t> parse_flash_size(std::string_view out) {
    constexpr std::string_view key = "Detected flash size: ";
    const auto at = out.find(key);
    if (at == std::string_view::npos) return std::nullopt;
    auto rest = out.substr(at + key.size());
    std::uint32_t n = 0;
    std::size_t digits = 0;
    while (digits < rest.size() && std::isdigit(static_cast<unsigned char>(rest[digits])) && digits < 5)
        n = n * 10 + static_cast<std::uint32_t>(rest[digits++] - '0');
    if (digits == 0) return std::nullopt;
    rest.remove_prefix(digits);
    if (rest.starts_with("MB")) return n * 1024u * 1024u;
    if (rest.starts_with("KB")) return n * 1024u;
    return std::nullopt;
}

std::optional<fs::path> backup_path(const fs::path& dir, std::string_view mac, std::string_view stamp) {
    if (!valid_mac(mac)) return std::nullopt;
    return dir / std::string(mac) / (std::string(stamp) + ".bin.part");
}

std::optional<fs::path> latest_backup(const fs::path& dir, std::string_view mac) {
    if (!valid_mac(mac)) return std::nullopt;
    std::error_code ec;
    std::optional<fs::path> best;
    for (const auto& entry : fs::directory_iterator(dir / std::string(mac), ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".bin") continue;
        if (!best || entry.path().filename() > best->filename()) best = entry.path(); // names are timestamps
    }
    return best;
}

bool is_agent_image(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    // search in blocks; the last bytes of a block are kept so a marker split between two blocks is still found
    std::vector<char> buf(1 << 16);
    std::string window;
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
        window.append(buf.data(), static_cast<std::size_t>(in.gcount()));
        if (window.find(kAgentMarker) != std::string::npos) return true;
        if (window.size() >= kAgentMarker.size()) window.erase(0, window.size() - (kAgentMarker.size() - 1));
    }
    return false;
}

} // namespace malgrind::host

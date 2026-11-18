#include "os/paths.hpp"

#include <cctype>

namespace fs = std::filesystem;

namespace malgrind::host::os {

std::string_view kind_name() noexcept {
    switch (kind()) {
        case Kind::Linux: return "linux";
        case Kind::MacOS: return "macos";
        case Kind::Windows: return "windows";
    }
    return "linux";
}

std::vector<fs::path> split_path_list(std::string_view list, char sep) {
    std::vector<fs::path> out;
    std::size_t pos = 0;
    while (pos <= list.size()) {
        const auto next = list.find(sep, pos);
        const auto part = next == std::string_view::npos ? list.substr(pos) : list.substr(pos, next - pos);
        if (!part.empty()) out.push_back(from_utf8(part));
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    return out;
}

std::string exe_name(std::string_view base) {
    if constexpr (kind() == Kind::Windows) {
        if (base.size() >= 4) {
            const auto tail = base.substr(base.size() - 4);
            if (tail[0] == '.' &&
                std::tolower(static_cast<unsigned char>(tail[1])) == 'e' &&
                std::tolower(static_cast<unsigned char>(tail[2])) == 'x' &&
                std::tolower(static_cast<unsigned char>(tail[3])) == 'e') {
                return std::string(base);
            }
        }
        return std::string(base) + ".exe";
    }
    return std::string(base);
}

} // namespace malgrind::host::os

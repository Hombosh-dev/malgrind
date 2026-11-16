#include "github.hpp"

#include <algorithm>
#include <cctype>

namespace malgrind::host {

namespace {

bool name_char(unsigned char c, bool allow_dot_underscore) {
    return std::isalnum(c) || c == '-' || (allow_dot_underscore && (c == '.' || c == '_'));
}

} // namespace

std::optional<GithubRepo> parse_github_url(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    constexpr std::string_view prefix = "https://github.com/";
    if (!text.starts_with(prefix)) return std::nullopt;
    text.remove_prefix(prefix.size());
    if (text.ends_with('/')) text.remove_suffix(1);
    if (text.ends_with(".git")) text.remove_suffix(4);
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const auto owner = text.substr(0, slash), repo = text.substr(slash + 1);
    if (owner.empty() || repo.empty() || owner.size() > 100 || repo.size() > 100) return std::nullopt;
    if (!std::ranges::all_of(owner, [](unsigned char c) { return name_char(c, false); })) return std::nullopt;
    if (!std::ranges::all_of(repo, [](unsigned char c) { return name_char(c, true); })) return std::nullopt;
    if (owner.starts_with('-') || owner.ends_with('-') || repo.starts_with('-')) return std::nullopt; // GitHub has no such names
    if (repo.starts_with('.') || repo.find("..") != std::string_view::npos) return std::nullopt;
    return GithubRepo{"https://github.com/" + std::string(owner) + "/" + std::string(repo) + ".git", std::string(repo)};
}

} // namespace malgrind::host

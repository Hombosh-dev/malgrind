#pragma once

// GitHub links the app may clone. Only https://github.com/<owner>/<repo>: git also speaks transports that run commands
// (ext::, file:, ssh with options), and a pasted link must never reach them.

#include <optional>
#include <string>
#include <string_view>

namespace malgrind::host {

struct GithubRepo {
    std::string url;  // normalized clone URL: https://github.com/<owner>/<repo>.git
    std::string name; // <repo>
};

// Accepts https://github.com/<owner>/<repo>, with an optional ".git" and an optional trailing "/" (surrounding spaces are
// ignored). Anything else, including links into a branch or a folder, returns nullopt.
[[nodiscard]] std::optional<GithubRepo> parse_github_url(std::string_view text);

} // namespace malgrind::host

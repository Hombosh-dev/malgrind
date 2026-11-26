#pragma once

#include "store.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string_view>

namespace malgrind::host {

// Compares two stored runs (replaying each with its own firmware.elf), returning differential function stats and a merged call tree.
[[nodiscard]] nlohmann::json diff_runs(const RunStore& store, std::string_view id_a, std::string_view id_b,
                                       std::optional<unsigned> core = std::nullopt);

} // namespace malgrind::host

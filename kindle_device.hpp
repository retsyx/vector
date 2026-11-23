#pragma once

#include <optional>

namespace manga {

inline constexpr int kindle_frontlight_max = 24;

std::optional<int> read_kindle_frontlight() noexcept;
bool set_kindle_frontlight(int level) noexcept;
std::optional<int> read_kindle_battery_percent() noexcept;

}  // namespace manga

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace manga {

struct LibraryEntry {
    std::filesystem::path path;
};

std::vector<LibraryEntry> list_library(const std::filesystem::path& directory);
void remove_library_archive(const std::filesystem::path& root, const std::filesystem::path& archive);

enum class TapDirection { left_advances, right_advances };

TapDirection read_tap_direction(const std::filesystem::path& settings) noexcept;
void write_tap_direction(const std::filesystem::path& settings, TapDirection direction);

struct ReadingPosition {
    std::filesystem::path archive;
    std::size_t page_index;
    // False once the reader returns to the library, which then opens in its place.
    bool reading = true;
};

std::optional<ReadingPosition> read_reading_position(const std::filesystem::path& state) noexcept;
void write_reading_position(const std::filesystem::path& state, const ReadingPosition& position);

}  // namespace manga

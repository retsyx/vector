#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
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

// Archives are keyed by their path relative to the library.
struct ReadingPositions {
    // Zero-based page last shown in each archive. Archives left on their first page have no entry.
    std::map<std::filesystem::path, std::size_t> pages;
    // The archive to reopen, or empty once the reader returns to the library, which then opens in its place.
    std::filesystem::path reading;
};

// A missing or unreadable record reads as no saved positions.
ReadingPositions read_reading_positions(const std::filesystem::path& state) noexcept;
void write_reading_positions(const std::filesystem::path& state, const ReadingPositions& positions);
// Forgets archives that are no longer among the entries of the library at root.
void forget_removed_archives(ReadingPositions& positions, const std::filesystem::path& root,
                             const std::vector<LibraryEntry>& entries);

}  // namespace manga

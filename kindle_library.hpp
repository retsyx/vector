#pragma once

#include <cstddef>
#include <cstdint>
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

// Cover thumbnails kept between launches, so the library can show them without decoding the archives again.
// Each cover file records the archive's path relative to the library, its size and modification time, and the
// thumbnail size, so a changed archive or screen reads as having no cover. Archives are full paths in the library.
class CoverCache {
public:
    CoverCache(std::filesystem::path directory, std::filesystem::path root);

    // Returns no pixels when no current cover of this size is saved.
    std::vector<std::uint8_t> read(const std::filesystem::path& archive, int width, int height) const noexcept;
    void write(const std::filesystem::path& archive, int width, int height,
               const std::vector<std::uint8_t>& pixels) const;
    void remove(const std::filesystem::path& archive) const noexcept;
    // Deletes the covers of archives that are no longer among the entries.
    void forget_removed_archives(const std::vector<LibraryEntry>& entries) const noexcept;

private:
    std::filesystem::path file(const std::filesystem::path& archive) const;

    std::filesystem::path directory_;
    std::filesystem::path root_;
};

}  // namespace manga

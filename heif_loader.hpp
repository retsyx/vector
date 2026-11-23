#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct zip;

namespace manga {

struct Image {
    std::uint32_t width;
    std::uint32_t height;
    // Tightly packed RGBA8 with straight alpha, starting at the top-left pixel.
    std::vector<std::uint8_t> pixels;
};

// Decodes the primary still image from HEIF bytes, including HEIC and AVIF.
// The result owns its pixels. Invalid or unsupported images throw std::runtime_error.
Image decode_heif(const std::uint8_t* data, std::size_t size);

class ZipBook {
public:
    // Indexes HEIF, HEIC, AVIF, PNG and JPEG pages in natural filename order.
    // Opening an invalid ZIP throws std::runtime_error.
    explicit ZipBook(const std::filesystem::path& path);

    std::size_t page_count() const noexcept;
    const std::string& page_name(std::size_t page_index) const;
    // Page indices are zero-based. Each call reads and decodes one page in memory.
    Image load_page(std::size_t page_index) const;

private:
    struct ArchiveCloser {
        void operator()(zip* archive) const noexcept;
    };

    struct Page {
        std::string name;
        std::uint64_t entry_index;
    };

    std::unique_ptr<zip, ArchiveCloser> archive_;
    std::vector<Page> pages_;
    mutable std::mutex archive_mutex_;
};

}  // namespace manga

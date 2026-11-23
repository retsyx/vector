#pragma once

#include "heif_loader.hpp"

namespace manga {

struct PageRect {
    int x;
    int y;
    int width;
    int height;
};

// Fits a page to a pixel viewport while keeping its original aspect ratio.
PageRect fit_page(std::uint32_t source_width, std::uint32_t source_height,
                  int viewport_width, int viewport_height) noexcept;

class Reader {
public:
    // Page indices are zero-based. An empty archive or invalid starting page throws.
    explicit Reader(const std::filesystem::path& archive, std::size_t initial_page = 0);

    std::size_t page_count() const noexcept;
    std::size_t page_index() const noexcept;
    const std::string& page_name() const;
    const Image& image() const noexcept;

    bool previous();
    bool next();
    // Returns false when there is no page change. On decode failure, the current page remains open.
    bool go_to(std::size_t page);

private:
    ZipBook book_;
    std::size_t page_index_;
    Image image_;
};

}  // namespace manga

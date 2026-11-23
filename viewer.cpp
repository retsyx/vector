#include "viewer.hpp"

#include <algorithm>
#include <stdexcept>

namespace manga {

PageRect fit_page(std::uint32_t source_width, std::uint32_t source_height,
                  int viewport_width, int viewport_height) noexcept {
    if (source_width == 0 || source_height == 0 || viewport_width <= 0 || viewport_height <= 0) {
        return {};
    }

    const auto scaled_width = std::uint64_t{source_width} * static_cast<unsigned>(viewport_height);
    const auto scaled_height = std::uint64_t{source_height} * static_cast<unsigned>(viewport_width);
    int width = viewport_width;
    int height = viewport_height;
    if (scaled_width >= scaled_height) {
        height = std::max(1, static_cast<int>((scaled_height + source_width / 2) / source_width));
    } else {
        width = std::max(1, static_cast<int>((scaled_width + source_height / 2) / source_height));
    }
    return {(viewport_width - width) / 2, (viewport_height - height) / 2, width, height};
}

Reader::Reader(const std::filesystem::path& archive, std::size_t initial_page)
    : book_(archive), page_index_(initial_page) {
    if (book_.page_count() == 0) {
        throw std::runtime_error("The archive has no supported image pages");
    }
    image_ = book_.load_page(initial_page);
}

std::size_t Reader::page_count() const noexcept {
    return book_.page_count();
}

std::size_t Reader::page_index() const noexcept {
    return page_index_;
}

const std::string& Reader::page_name() const {
    return book_.page_name(page_index_);
}

const Image& Reader::image() const noexcept {
    return image_;
}

bool Reader::previous() {
    return page_index_ > 0 && go_to(page_index_ - 1);
}

bool Reader::next() {
    return go_to(page_index_ + 1);
}

bool Reader::go_to(std::size_t page) {
    if (page == page_index_ || page >= book_.page_count()) {
        return false;
    }
    image_ = book_.load_page(page);
    page_index_ = page;
    return true;
}

}  // namespace manga

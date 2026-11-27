// Renders the viewer's Kindle screens to PNG files with the viewer's own drawing code.
// An in-memory 8-bit framebuffer the size of a 6" Kindle screen stands in for FBInk.
// build-screenshots.sh extracts the drawing code from viewer_kindle.cpp into viewer_drawing.inc.
#include "kindle_device.hpp"
#include "kindle_library.hpp"
#include "kindle_menu.hpp"
#include "viewer.hpp"

#include <fbink.h>
#include <png.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace screen {

constexpr int width = 1072;
constexpr int height = 1448;
std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height, 255);

}  // namespace screen

extern "C" {

int fbink_open(void) { return 3; }
int fbink_close(int) { return 0; }
int fbink_init(int, const FBInkConfig*) { return 0; }
int fbink_reinit(int, const FBInkConfig*) { return 0; }
uint32_t fbink_get_last_marker(void) { return 0; }
int fbink_wait_for_complete(int, uint32_t) { return 0; }

void fbink_get_state(const FBInkConfig*, FBInkState* state) {
    *state = FBInkState{};
    state->view_width = state->screen_width = screen::width;
    state->view_height = state->screen_height = screen::height;
}

// The viewer only draws 8-bit grayscale buffers at non-negative offsets.
int fbink_print_raw_data(int, const unsigned char* data, const int w, const int h, const size_t len, short x_off,
                         short y_off, const FBInkConfig*) {
    if (w <= 0 || h <= 0 || len != static_cast<size_t>(w) * static_cast<size_t>(h) || x_off < 0 || y_off < 0) {
        std::cerr << "Unexpected raw data: " << w << 'x' << h << ", " << len << " bytes at " << x_off << ','
                  << y_off << '\n';
        return -1;
    }
    for (int row = 0; row < h && y_off + row < screen::height; ++row) {
        std::copy_n(data + static_cast<std::size_t>(row) * w, std::max(0, std::min(w, screen::width - x_off)),
                    screen::pixels.data() + static_cast<std::size_t>(y_off + row) * screen::width + x_off);
    }
    return 0;
}

}  // extern "C"

namespace {

#include "viewer_drawing.inc"

void save(const std::filesystem::path& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        throw std::runtime_error("Cannot write " + path.string());
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
        throw std::runtime_error("Cannot encode " + path.string());
    }
    png_init_io(png, file);
    png_set_IHDR(png, info, screen::width, screen::height, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int row = 0; row < screen::height; ++row) {
        png_write_row(png, screen::pixels.data() + static_cast<std::size_t>(row) * screen::width);
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    if (std::fclose(file) != 0) {
        throw std::runtime_error("Cannot write " + path.string());
    }
    std::cout << "Wrote " << path.string() << '\n';
}

std::size_t page_index(const char* text) {
    const auto page = std::stoul(text);
    if (page == 0) {
        throw std::invalid_argument("Page numbers start at 1");
    }
    return page - 1;
}

void render(const std::filesystem::path& library, const std::filesystem::path& output,
            const std::filesystem::path& archive, std::size_t reading_page, std::size_t scrubber_page) {
    const auto root = std::filesystem::canonical(library);

    // As run() configures FBInk.
    FBInkConfig configuration{};
    configuration.ignore_alpha = true;
    configuration.no_viewport = true;
    configuration.wfm_mode = WFM_GC16;
    configuration.is_flashing = true;
    const Framebuffer framebuffer;
    const auto fd = framebuffer.fd;
    const auto state = display_state(fd, configuration);
    const auto width = static_cast<int>(state.view_width);
    const auto height = static_cast<int>(state.view_height);

    // The first page of the library, as browse_library fills it.
    {
        const auto entries = manga::list_library(root);
        const manga::KindleMenu menu(state);
        constexpr auto per_page = manga::KindleMenu::covers_per_page;
        const auto last_page = entries.empty() ? 0 : (entries.size() - 1) / per_page;
        manga::KindleMenuPage display;
        display.title = "Manga Library";
        display.subtitle = root.filename().string();
        if (last_page > 0) {
            display.subtitle += "  1/" + std::to_string(last_page + 1);
        }
        display.back = "Exit";
        display.top_right = entries.empty() ? "" : "Delete";
        display.bottom_right = last_page > 0 ? "Next" : "";
        display.scroll_forward = last_page > 0;
        const auto size = menu.thumbnail_size();
        for (std::size_t index = 0; index < std::min(per_page, entries.size()); ++index) {
            display.thumbnails.push_back(archive_thumbnail(entries[index].path, size.first, size.second));
        }
        menu.draw(fd, configuration, display);
        save(output / "library.png");
    }

    manga::Reader reader(root / archive, reading_page);
    draw_page(fd, configuration, reader, false);
    save(output / "reading.png");

    // The reading bar of a book opened from the library.
    {
        const auto header = manga::reading_header_height(width, height);
        std::optional<manga::KindleReadingHeader> dropdown;
        dropdown.emplace();
        dropdown->button = "Library";
        dropdown->slider = manga::KindleMenuSlider{"Light", 0, manga::kindle_frontlight_max, 14};
        dropdown->status = reading_status(78);
        draw_reading_dropdown(fd, configuration, reader, state, header, dropdown);
        save(output / "reading-bar.png");
    }

    // The page scrubber after a drag to another page, as scrub_pages draws it.
    {
        draw_page(fd, configuration, reader, false);
        const manga::KindlePageScrubber scrubber{scrubber_page, reader.page_count(), true, {}};
        reader.go_to(scrubber_page);
        draw_scrubber_preview(fd, configuration, state, reader);
        draw_scrubber_controls(fd, configuration, state, scrubber);
        save(output / "scrubber.png");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "Usage: render LIBRARY OUTPUT ARCHIVE READING_PAGE SCRUBBER_PAGE\n";
        return EXIT_FAILURE;
    }
    try {
        render(argv[1], argv[2], argv[3], page_index(argv[4]), page_index(argv[5]));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "render: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

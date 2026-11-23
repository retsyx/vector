#pragma once

#include <fbink.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manga {

struct KindleMenuSlider {
    std::string label;
    int minimum;
    int maximum;
    int value;
};

struct KindleReadingHeader {
    std::optional<KindleMenuSlider> slider;
    std::string status;
    std::string message;
};

struct KindleMenuPage {
    std::string title;
    std::string subtitle;
    std::string back;
    std::string top_right;
    std::vector<std::string> rows;
    // Cover grid tiles in reading order. An empty tile draws a placeholder.
    std::vector<std::vector<std::uint8_t>> thumbnails;
    std::string details;
    std::string bottom_left;
    std::string bottom_center;
    std::string bottom_right;
    std::string message;
};

enum class MenuActionKind { none, back, top_right, row, bottom_left, bottom_center, bottom_right };

struct MenuAction {
    MenuActionKind kind = MenuActionKind::none;
    std::size_t row = 0;
};

class KindleMenu {
public:
    static constexpr std::size_t covers_per_page = 6;

    explicit KindleMenu(const FBInkState& state);
    std::size_t rows_per_page() const;
    std::pair<int, int> thumbnail_size() const;
    void draw(int fd, const FBInkConfig& configuration, const KindleMenuPage& page) const;
    MenuAction hit(const KindleMenuPage& page, double x, double y) const;

private:
    int width_;
    int height_;
};

int reading_header_height(int width, int height) noexcept;
bool reading_header_menu_hit(int width, int header, int x, int y) noexcept;
// Screen pixels. The track accepts taps and drags. The step buttons accept taps.
std::optional<int> reading_header_slider_value(const KindleReadingHeader& overlay, int width, int header,
                                               int start_x, int start_y, int end_x, bool tap);
void paint_reading_header(std::vector<std::uint8_t>& pixels, int width, int header,
                          std::size_t page, std::size_t page_count, const KindleReadingHeader& overlay);

}  // namespace manga

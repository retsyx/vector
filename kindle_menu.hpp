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
    std::string button;
    std::optional<KindleMenuSlider> slider;
    std::string status;
    std::string message;
};

struct KindlePageScrubber {
    // Zero-based.
    std::size_t page;
    std::size_t page_count;
    // Later pages lie toward the left, matching the side that advances the page. The reading direction
    // toggle shows this setting.
    bool left_advances;
    std::string message;
};

enum class ScrubberActionKind { none, cancel, go, select, toggle_direction };

struct ScrubberAction {
    ScrubberActionKind kind = ScrubberActionKind::none;
    std::size_t page = 0;
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
    // Vertical swipes over the cover grid scroll in the enabled directions.
    bool scroll_back = false;
    bool scroll_forward = false;
};

enum class MenuActionKind {
    none, back, top_right, row, bottom_left, bottom_center, bottom_right, scroll_back, scroll_forward
};

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
    MenuAction swipe(const KindleMenuPage& page, double start_x, double start_y, double end_x, double end_y) const;

private:
    int width_;
    int height_;
};

int reading_header_height(int width, int height) noexcept;
bool reading_header_button_hit(int width, int header, int x, int y) noexcept;
// Screen pixels. The track accepts taps and drags. The step buttons accept taps.
std::optional<int> reading_header_slider_value(const KindleReadingHeader& overlay, int width, int header,
                                               int start_x, int start_y, int end_x, bool tap);
void paint_reading_header(std::vector<std::uint8_t>& pixels, int width, int header,
                          std::size_t page, std::size_t page_count, const KindleReadingHeader& overlay);

// Height of the scrubber controls along the bottom edge, or 0 when the screen is too small for them.
int page_scrubber_height(int width, int height) noexcept;
// Screen pixels. A tap above the controls goes to the selected page. The track accepts taps and drags.
// The arrows and buttons accept taps.
ScrubberAction page_scrubber_action(const KindlePageScrubber& scrubber, int width, int height,
                                    int start_x, int start_y, int end_x, bool tap);
// Paints the controls into a buffer of width by panel pixels.
void paint_page_scrubber(std::vector<std::uint8_t>& pixels, int width, int panel,
                         const KindlePageScrubber& scrubber);

}  // namespace manga

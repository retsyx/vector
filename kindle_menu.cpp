#include "kindle_menu.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace manga {
namespace {

constexpr int glyph_side = 8;
constexpr std::uint8_t ink = 20;
constexpr std::uint8_t muted = 80;
constexpr std::uint8_t border = 172;
constexpr std::uint8_t paper = 255;

// Printable ASCII from Daniel Hepper's public-domain font8x8_basic,
// based on the public-domain IBM VGA font and Marcel Sondaar's font table.
// https://github.com/dhepper/font8x8
constexpr std::uint8_t glyphs[][glyph_side] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+0020
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},  // U+0021
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+0022
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},  // U+0023
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},  // U+0024
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},  // U+0025
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},  // U+0026
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+0027
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},  // U+0028
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},  // U+0029
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},  // U+002A
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},  // U+002B
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},  // U+002C
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},  // U+002D
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},  // U+002E
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},  // U+002F
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},  // U+0030
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},  // U+0031
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},  // U+0032
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},  // U+0033
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},  // U+0034
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},  // U+0035
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},  // U+0036
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},  // U+0037
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},  // U+0038
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},  // U+0039
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},  // U+003A
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},  // U+003B
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},  // U+003C
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},  // U+003D
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},  // U+003E
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},  // U+003F
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},  // U+0040
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},  // U+0041
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},  // U+0042
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},  // U+0043
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},  // U+0044
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},  // U+0045
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},  // U+0046
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},  // U+0047
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},  // U+0048
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  // U+0049
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},  // U+004A
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},  // U+004B
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},  // U+004C
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},  // U+004D
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},  // U+004E
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},  // U+004F
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},  // U+0050
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},  // U+0051
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},  // U+0052
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},  // U+0053
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  // U+0054
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},  // U+0055
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},  // U+0056
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},  // U+0057
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},  // U+0058
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},  // U+0059
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},  // U+005A
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},  // U+005B
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},  // U+005C
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},  // U+005D
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},  // U+005E
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},  // U+005F
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+0060
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},  // U+0061
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},  // U+0062
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},  // U+0063
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00},  // U+0064
    {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},  // U+0065
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00},  // U+0066
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},  // U+0067
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},  // U+0068
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  // U+0069
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},  // U+006A
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},  // U+006B
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  // U+006C
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},  // U+006D
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},  // U+006E
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},  // U+006F
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},  // U+0070
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},  // U+0071
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},  // U+0072
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},  // U+0073
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},  // U+0074
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},  // U+0075
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},  // U+0076
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},  // U+0077
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},  // U+0078
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},  // U+0079
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},  // U+007A
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},  // U+007B
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},  // U+007C
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},  // U+007D
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+007E
};

struct Rect {
    int x;
    int y;
    int width;
    int height;

    bool contains(int px, int py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    Rect inset(int distance) const {
        return {x + distance, y + distance, width - 2 * distance, height - 2 * distance};
    }
};

struct Layout {
    int scale;
    int margin;
    int row_height;
    int row_start;
    int row_width;
    Rect back;
    Rect top_right;
    Rect title;
    Rect subtitle;
    Rect divider;
    Rect message;
    std::array<Rect, 3> footer;

    std::size_t capacity() const {
        return static_cast<std::size_t>(std::max(0, (message.y - row_start) / row_height));
    }

    Rect row(std::size_t index) const {
        return {margin, row_start + static_cast<int>(index) * row_height, row_width, row_height};
    }
};

Layout layout_for(int width, int height) {
    const int scale = std::clamp(std::min(width, height) / 240, 1, 6);
    const int unit = scale * glyph_side;
    const int margin = std::max(unit, width / 24);
    const int content_width = width - 2 * margin;
    const int header_height = 3 * unit;
    const int back_width = std::min(6 * unit, content_width / 3);
    const int title_x = margin + back_width + unit / 2;
    const int divider_y = margin + 4 * unit + unit / 2;
    const int footer_y = height - margin - 3 * unit;
    const int gutter = unit / 2;
    const int button_width = (content_width - 2 * gutter) / 3;
    const int middle_x = margin + button_width + gutter;
    const int right_x = middle_x + button_width + gutter;
    return {
        scale,
        margin,
        3 * unit,
        divider_y + unit / 2,
        content_width,
        {margin, margin, back_width, header_height},
        {width - margin - back_width, margin, back_width, header_height},
        {title_x, margin, width - margin - title_x, header_height},
        {margin, margin + header_height, content_width, unit + unit / 2},
        {margin, divider_y, content_width, scale},
        {margin, footer_y - 2 * unit, content_width, 2 * unit},
        {{{margin, footer_y, button_width, 3 * unit},
          {middle_x, footer_y, button_width, 3 * unit},
          {right_x, footer_y, width - margin - right_x, 3 * unit}}},
    };
}

constexpr int cover_columns = 3;
constexpr int cover_rows = 2;
static_assert(KindleMenu::covers_per_page == cover_columns * cover_rows);

struct CoverGrid {
    int left;
    int top;
    int gap;
    int cell_width;
    int cell_height;
    int cover_width;
    int cover_height;

    Rect cover(std::size_t index) const {
        const int column = static_cast<int>(index % cover_columns);
        const int row = static_cast<int>(index / cover_columns);
        const int x = left + column * (cell_width + gap) + (cell_width - cover_width) / 2;
        const int y = top + row * (cell_height + gap) + (cell_height - cover_height) / 2;
        return {x, y, cover_width, cover_height};
    }
};

CoverGrid cover_grid(const Layout& layout) {
    const int height = layout.message.y - layout.row_start;
    const int gap = std::min({6 * layout.scale, layout.row_width / 6, height / 4});
    const int cell_width = (layout.row_width - (cover_columns - 1) * gap) / cover_columns;
    const int cell_height = (height - (cover_rows - 1) * gap) / cover_rows;
    const int cover_width = std::min(cell_width, cell_height * 2 / 3);
    return {
        layout.margin + (layout.row_width - cover_columns * cell_width - (cover_columns - 1) * gap) / 2,
        layout.row_start + (height - cover_rows * cell_height - (cover_rows - 1) * gap) / 2,
        gap,
        cell_width,
        cell_height,
        cover_width,
        cover_width * 3 / 2,
    };
}

struct ReadingHeaderLayout {
    int scale;
    Rect button;
    Rect page;
    Rect status;
    Rect light;
    Rect decrease;
    Rect slider;
    Rect increase;
    int slider_left;
    int slider_right;
};

ReadingHeaderLayout layout_reading_header(int width, int header) {
    const int margin = std::max(8, width / 24);
    const int row = header / 3;
    const int scale = std::clamp(row / 24, 1, 5);
    const int button_width = std::min(width / 2 - margin, 8 * scale * glyph_side);
    const int content_width = width - 2 * margin;
    const int half_row = row / 2;
    const int slider_y = row + 2 * half_row;
    const int slider_height = header - slider_y - 1;
    const int step_width = std::min(slider_height, content_width / 4);
    const int slider_width = content_width - 2 * step_width;
    const int inset = std::min(6 * scale, slider_width / 4);
    return {
        scale,
        {margin, row / 8, button_width, row * 3 / 4},
        {margin + button_width, 0, content_width - button_width, row},
        {margin, row, content_width, half_row},
        {margin, row + half_row, content_width, half_row},
        {margin, slider_y, step_width, slider_height},
        {margin + step_width, slider_y, slider_width, slider_height},
        {width - margin - step_width, slider_y, step_width, slider_height},
        margin + step_width + inset,
        width - margin - step_width - 1 - inset,
    };
}

int dimension(std::uint32_t pixels) {
    if (pixels == 0 || pixels > static_cast<std::uint32_t>(std::numeric_limits<short>::max())) {
        throw std::runtime_error("Invalid Kindle menu viewport");
    }
    return static_cast<int>(pixels);
}

std::uint32_t take_codepoint(std::string_view& text) {
    const auto first = static_cast<unsigned char>(text.front());
    text.remove_prefix(1);
    if (first < 0x80) {
        return first;
    }
    int tail_count = 0;
    std::uint32_t value = first;
    if (first >= 0xC2 && first <= 0xDF) {
        tail_count = 1;
        value &= 0x1F;
    } else if (first >= 0xE0 && first <= 0xEF) {
        tail_count = 2;
        value &= 0x0F;
    } else if (first >= 0xF0 && first <= 0xF4) {
        tail_count = 3;
        value &= 0x07;
    } else {
        return '?';
    }
    for (int index = 0; index < tail_count; ++index) {
        if (text.empty() || (static_cast<unsigned char>(text.front()) & 0xC0) != 0x80) {
            return '?';
        }
        value = (value << 6) | (static_cast<unsigned char>(text.front()) & 0x3F);
        text.remove_prefix(1);
    }
    return value;
}

std::string display_text(std::string_view input) {
    constexpr std::string_view latin1 =
        "AAAAAAACEEEEIIII"
        "DNOOOOOxOUUUUYTs"
        "aaaaaaaceeeeiiii"
        "dnooooo/ouuuuyty";
    std::string output;
    while (!input.empty()) {
        const auto value = take_codepoint(input);
        if (value >= 32 && value <= 126) {
            output.push_back(static_cast<char>(value));
        } else if (value < 32 || value == 0x7F || value == 0xA0) {
            output.push_back(' ');
        } else if (value >= 0xC0 && value <= 0xFF) {
            output.push_back(latin1[value - 0xC0]);
        } else {
            output.push_back('?');
        }
    }
    return output;
}

std::string elide(std::string text, std::size_t cells) {
    if (text.size() <= cells) {
        return text;
    }
    if (cells < 5) {
        text.resize(cells);
        return text;
    }
    const std::size_t prefix = (cells - 2) / 2;
    const std::size_t suffix = cells - prefix - 3;
    return text.substr(0, prefix) + "..." + text.substr(text.size() - suffix);
}

class Frame {
public:
    Frame(int width, int height, std::vector<std::uint8_t>& pixels)
        : width_(width), height_(height), pixels_(pixels) {}

    void fill(Rect rect, std::uint8_t color) {
        const int left = std::max(0, rect.x);
        const int top = std::max(0, rect.y);
        const int right = std::min(width_, rect.x + rect.width);
        const int bottom = std::min(height_, rect.y + rect.height);
        if (left >= right || top >= bottom) {
            return;
        }
        for (int y = top; y < bottom; ++y) {
            const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                                static_cast<std::size_t>(left);
            std::fill_n(pixels_.data() + offset, right - left, color);
        }
    }

    void text(Rect region, std::string_view label, int scale, std::uint8_t color, bool centered = false) {
        const int cell = glyph_side * scale;
        if (region.width < cell || region.height < cell) {
            return;
        }
        const auto line = elide(display_text(label), static_cast<std::size_t>(region.width / cell));
        const int left = centered ? region.x + (region.width - static_cast<int>(line.size()) * cell) / 2
                                  : region.x;
        const int top = region.y + (region.height - cell) / 2;
        int x = left;
        for (char character : line) {
            const auto& glyph = glyphs[static_cast<unsigned char>(character) - 32];
            for (int row = 0; row < glyph_side; ++row) {
                for (int column = 0; column < glyph_side; ++column) {
                    if (glyph[row] & (1U << column)) {
                        fill({x + column * scale, top + row * scale, scale, scale}, color);
                    }
                }
            }
            x += cell;
        }
    }

    void button(Rect rect, std::string_view label, int scale, bool primary) {
        if (label.empty()) {
            return;
        }
        fill(rect, ink);
        if (!primary) {
            fill(rect.inset(std::max(1, scale / 2)), paper);
        }
        text(rect.inset(scale), label, std::max(1, scale - 1), primary ? paper : ink, true);
    }

    void thumbnail(Rect rect, const std::vector<std::uint8_t>* thumbnail) {
        if (rect.width == 0) {
            return;
        }
        const auto width = static_cast<std::size_t>(rect.width);
        const auto height = static_cast<std::size_t>(rect.height);
        if (thumbnail && thumbnail->size() == width * height) {
            for (std::size_t row = 0; row < height; ++row) {
                const auto destination = (static_cast<std::size_t>(rect.y) + row) * width_ + rect.x;
                std::copy_n(thumbnail->data() + row * width, width, pixels_.data() + destination);
            }
            return;
        }

        fill(rect, border);
        fill(rect.inset(1), paper);
        const auto line = rect.inset(rect.width / 4);
        const auto weight = std::max(1, rect.height / 24);
        fill({line.x, rect.y + rect.height / 2 - 2 * weight, line.width, weight}, border);
        fill({line.x, rect.y + rect.height / 2 + weight, line.width, weight}, border);
    }

    void show(int fd, const FBInkConfig& configuration) const {
        FBInkConfig render = configuration;
        render.col = render.row = render.hoffset = render.voffset = 0;
        render.halign = render.valign = NONE;
        render.ignore_alpha = true;
        render.is_cleared = false;
        render.no_refresh = false;
        render.scaled_width = render.scaled_height = 0;
        const int result = fbink_print_raw_data(fd, pixels_.data(), width_, height_, pixels_.size(),
                                                0, 0, &render);
        if (result < 0) {
            throw std::runtime_error("Draw Kindle menu (FBInk error " + std::to_string(result) + ")");
        }
    }

private:
    int width_;
    int height_;
    std::vector<std::uint8_t>& pixels_;
};

}  // namespace

KindleMenu::KindleMenu(const FBInkState& state)
    : width_(dimension(state.view_width)), height_(dimension(state.view_height)) {
    if (rows_per_page() == 0 || layout_for(width_, height_).footer[0].width <= 0) {
        throw std::runtime_error("The Kindle viewport is too small for the library menu");
    }
}

std::size_t KindleMenu::rows_per_page() const {
    return layout_for(width_, height_).capacity();
}

std::pair<int, int> KindleMenu::thumbnail_size() const {
    const auto grid = cover_grid(layout_for(width_, height_));
    return {grid.cover_width, grid.cover_height};
}

void KindleMenu::draw(int fd, const FBInkConfig& configuration, const KindleMenuPage& page) const {
    const auto layout = layout_for(width_, height_);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), paper);
    Frame frame(width_, height_, pixels);
    frame.button(layout.back, page.back, layout.scale, false);
    Rect title = layout.title;
    if (page.back.empty()) {
        title = {layout.margin, title.y, layout.row_width, title.height};
    }
    if (!page.top_right.empty()) {
        title.width = layout.top_right.x - title.x - layout.scale * glyph_side / 2;
        frame.button(layout.top_right, page.top_right, std::max(1, layout.scale - 1), false);
    }
    const int title_scale = layout.scale + (page.top_right.empty() ? 1 : 0);
    frame.text(title, page.title, title_scale, ink);
    frame.text(layout.subtitle, page.subtitle, std::max(1, layout.scale - 1), muted);
    frame.fill(layout.divider, ink);

    if (!page.thumbnails.empty()) {
        const auto grid = cover_grid(layout);
        const auto visible_covers = std::min(page.thumbnails.size(), covers_per_page);
        for (std::size_t index = 0; index < visible_covers; ++index) {
            const auto tile = grid.cover(index);
            const auto& thumbnail = page.thumbnails[index];
            if (tile.width > 0 && !thumbnail.empty()) {
                frame.fill(tile.inset(-1), border);
            }
            frame.thumbnail(tile, &thumbnail);
        }
    } else {
        const auto visible_rows = std::min(page.rows.size(), layout.capacity());
        for (std::size_t index = 0; index < visible_rows; ++index) {
            const auto row = layout.row(index);
            frame.text(row.inset(2 * layout.scale), page.rows[index], layout.scale, ink);
            frame.fill({row.x, row.y + row.height - 1, row.width, 1}, border);
        }
        if (!page.details.empty()) {
            const int scale = std::max(1, layout.scale - 1);
            const auto columns = static_cast<std::size_t>(layout.row_width / (glyph_side * scale));
            const auto details = elide(display_text(page.details), columns * (layout.capacity() - visible_rows));
            for (std::size_t offset = 0; offset < details.size(); offset += columns) {
                const auto row = layout.row(visible_rows + offset / columns);
                frame.text(row, std::string_view(details).substr(offset, columns), scale, ink);
            }
        }
    }

    frame.text(layout.message, page.message, std::max(1, layout.scale - 1), muted);
    frame.button(layout.footer[0], page.bottom_left, layout.scale, false);
    frame.button(layout.footer[1], page.bottom_center, layout.scale, true);
    frame.button(layout.footer[2], page.bottom_right, layout.scale, false);
    frame.show(fd, configuration);
}

MenuAction KindleMenu::hit(const KindleMenuPage& page, double x, double y) const {
    if (!(x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0)) {
        return {};
    }
    const int px = std::min(static_cast<int>(x * width_), width_ - 1);
    const int py = std::min(static_cast<int>(y * height_), height_ - 1);
    const auto layout = layout_for(width_, height_);
    if (!page.back.empty() && layout.back.contains(px, py)) {
        return {MenuActionKind::back};
    }
    if (!page.top_right.empty() && layout.top_right.contains(px, py)) {
        return {MenuActionKind::top_right};
    }
    if (!page.bottom_left.empty() && layout.footer[0].contains(px, py)) {
        return {MenuActionKind::bottom_left};
    }
    if (!page.bottom_center.empty() && layout.footer[1].contains(px, py)) {
        return {MenuActionKind::bottom_center};
    }
    if (!page.bottom_right.empty() && layout.footer[2].contains(px, py)) {
        return {MenuActionKind::bottom_right};
    }
    if (!page.thumbnails.empty()) {
        const auto grid = cover_grid(layout);
        const auto visible_covers = std::min(page.thumbnails.size(), covers_per_page);
        for (std::size_t index = 0; index < visible_covers; ++index) {
            if (grid.cover(index).contains(px, py)) {
                return {MenuActionKind::row, index};
            }
        }
        return {};
    }
    if (py >= layout.row_start) {
        const auto index = static_cast<std::size_t>((py - layout.row_start) / layout.row_height);
        if (index < std::min(page.rows.size(), layout.capacity()) && !page.rows[index].empty() &&
            layout.row(index).contains(px, py)) {
            return {MenuActionKind::row, index};
        }
    }
    return {};
}

int reading_header_height(int width, int height) noexcept {
    return width >= 160 && height >= 240 ? std::clamp(height / 5, 120, 320) : 0;
}

bool reading_header_button_hit(int width, int header, int x, int y) noexcept {
    return x >= 0 && x < width && y >= 0 && y < header / 3;
}

std::optional<int> reading_header_slider_value(const KindleReadingHeader& overlay, int width, int header,
                                               int start_x, int start_y, int end_x, bool tap) {
    if (!overlay.slider || overlay.slider->minimum >= overlay.slider->maximum || width < 160 || header < 120) {
        return std::nullopt;
    }
    const auto& control = *overlay.slider;
    const auto layout = layout_reading_header(width, header);
    const bool decrease = layout.decrease.contains(start_x, start_y);
    const bool increase = layout.increase.contains(start_x, start_y);
    if (tap && (decrease || increase)) {
        const auto value = std::clamp(control.value, control.minimum, control.maximum);
        if (decrease) {
            return value > control.minimum ? value - 1 : value;
        }
        return value < control.maximum ? value + 1 : value;
    }
    if (!layout.slider.contains(start_x, start_y)) {
        return std::nullopt;
    }
    const auto position = std::clamp(end_x, layout.slider_left, layout.slider_right);
    const auto travel = layout.slider_right - layout.slider_left;
    const auto range = std::int64_t{control.maximum} - control.minimum;
    const auto value = control.minimum + ((position - layout.slider_left) * range + travel / 2) / travel;
    return static_cast<int>(value);
}

void paint_reading_header(std::vector<std::uint8_t>& pixels, int width, int header,
                          std::size_t page, std::size_t page_count, const KindleReadingHeader& overlay) {
    const auto layout = layout_reading_header(width, header);
    const auto scale = layout.scale;
    const auto readout_scale = std::max(1, scale - 1);
    Frame frame(width, header, pixels);
    frame.button(layout.button, overlay.button, scale + 1, false);
    frame.text(layout.page, std::to_string(page) + " / " + std::to_string(page_count), scale, muted, true);
    frame.fill({layout.status.x, layout.status.y - 1, layout.status.width, 1}, border);
    frame.text(layout.status, overlay.status, readout_scale, muted, true);

    if (overlay.slider && overlay.slider->minimum < overlay.slider->maximum) {
        const auto& control = *overlay.slider;
        const auto value = std::clamp(control.value, control.minimum, control.maximum);
        const auto range = std::int64_t{control.maximum} - control.minimum;
        const auto relative_value = std::int64_t{value} - control.minimum;
        const auto travel = layout.slider_right - layout.slider_left;
        const auto position = layout.slider_left + static_cast<int>((relative_value * travel + range / 2) / range);
        const auto center_y = layout.slider.y + layout.slider.height / 2;
        const auto half_knob = 4 * scale;
        const Rect knob{position - half_knob, center_y - 5 * scale, 2 * half_knob + 1, 10 * scale};
        const auto label = control.label + "  " + std::to_string(value) + " / " + std::to_string(control.maximum);
        frame.text(layout.light, overlay.message.empty() ? label : overlay.message, readout_scale, ink, true);
        const auto step_padding = std::min(3 * scale, layout.decrease.width / 8);
        const auto step_scale = std::max(1, std::min(scale, layout.decrease.width / (2 * glyph_side)));
        frame.button(layout.decrease.inset(step_padding), "-", step_scale + 1, false);
        frame.button(layout.increase.inset(step_padding), "+", step_scale + 1, false);
        frame.fill({layout.slider_left, center_y - scale, travel + 1, 2 * scale}, border);
        frame.fill({layout.slider_left, center_y - scale, position - layout.slider_left + 1, 2 * scale}, ink);
        frame.fill(knob, ink);
        frame.fill(knob.inset(scale), paper);
    } else {
        frame.text(layout.light, "Light unavailable", readout_scale, muted, true);
    }
    frame.fill({0, header - 1, width, 1}, border);
}

}  // namespace manga

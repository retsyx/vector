#include "kindle_device.hpp"
#include "kindle_library.hpp"
#include "kindle_menu.hpp"
#include "kindle_power.hpp"
#include "viewer.hpp"

#include <fbink.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr unsigned fraction_scale = 256;

struct Options {
    std::filesystem::path archive;
    std::filesystem::path library;
    std::filesystem::path settings;
    std::filesystem::path state;
    std::size_t first_page = 0;
    bool interactive = false;
    bool touch = false;
};

void print_usage(std::ostream& output) {
    output << "Usage: manga_kindle [--page N] [--interactive | --touch] archive.zip\n"
              "       manga_kindle --library DIRECTORY [--settings FILE] [--state FILE]\n"
              "Page numbers start at 1. Interactive commands: n (next), p (previous), q (quit).\n"
              "In the library, choose a book or open Settings to change tap direction.\n"
              "Swipe up or down over the library covers to scroll through them.\n"
              "While reading, tap the top edge or swipe down from it to show the reading bar.\n"
              "Swipe up from the bottom edge to scrub through the pages.\n";
}

std::size_t parse_page(std::string_view text) {
    std::size_t page = 0;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, page);
    if (result.ec != std::errc{} || result.ptr != end || page == 0) {
        throw std::invalid_argument("Expected a positive page number after --page");
    }
    return page - 1;
}

Options parse_options(int argc, char** argv) {
    Options options;
    bool parse_flags = true;
    bool has_page = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (parse_flags && argument == "--") {
            parse_flags = false;
        } else if (parse_flags && argument == "--page") {
            if (++index == argc) {
                throw std::invalid_argument("Missing page number after --page");
            }
            options.first_page = parse_page(argv[index]);
            has_page = true;
        } else if (parse_flags && (argument == "--library" || argument == "--settings" || argument == "--state")) {
            if (++index == argc || std::string_view(argv[index]).empty()) {
                throw std::invalid_argument("Missing path after " + std::string(argument));
            }
            if (argument == "--library") {
                options.library = argv[index];
            } else if (argument == "--settings") {
                options.settings = argv[index];
            } else {
                options.state = argv[index];
            }
        } else if (parse_flags && argument == "--interactive") {
            options.interactive = true;
        } else if (parse_flags && argument == "--touch") {
            options.touch = true;
        } else if (parse_flags && !argument.empty() && argument.front() == '-') {
            throw std::invalid_argument("Unknown option: " + std::string(argument));
        } else if (options.archive.empty()) {
            options.archive = argument;
        } else {
            throw std::invalid_argument("Expected one ZIP archive");
        }
    }
    if (!options.library.empty()) {
        if (!options.archive.empty() || options.interactive || options.touch || has_page) {
            throw std::invalid_argument("Use --library by itself or with --settings and --state");
        }
        if (options.settings.empty()) {
            options.settings = options.library / ".manga-viewer-settings";
        }
        if (options.state.empty()) {
            options.state = options.settings;
            options.state += ".position";
        }
        if (options.settings.lexically_normal() == options.state.lexically_normal()) {
            throw std::invalid_argument("Use different files for settings and reading position");
        }
    } else if (!options.settings.empty() || !options.state.empty()) {
        throw std::invalid_argument("--settings and --state require --library");
    } else if (options.archive.empty()) {
        throw std::invalid_argument("Missing ZIP archive");
    }
    if (options.interactive && options.touch) {
        throw std::invalid_argument("Choose either --interactive or --touch");
    }
    return options;
}

struct Sample {
    std::size_t first;
    std::size_t second;
    unsigned fraction;
};

Sample sample_axis(int destination, int destination_size, std::uint32_t source_size) {
    // Map destination pixel centers to source pixel centers, clamped at the edges.
    const auto offset = ((2 * std::int64_t{destination} + 1) * source_size * (fraction_scale / 2)) /
                            destination_size -
                        fraction_scale / 2;
    const auto last = std::int64_t{source_size - 1};
    const auto position = std::clamp(offset, std::int64_t{0}, last * fraction_scale);
    const auto first = position / fraction_scale;
    return {static_cast<std::size_t>(first), static_cast<std::size_t>(std::min(first + 1, last)),
            static_cast<unsigned>(position % fraction_scale)};
}

unsigned grayscale_on_white(const std::uint8_t* pixel) {
    const unsigned luminance = (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2] + 128u) / 256u;
    const unsigned alpha = pixel[3];
    return (luminance * alpha + 255u * (255u - alpha) + 127u) / 255u;
}

unsigned interpolate(unsigned first, unsigned second, unsigned fraction) {
    return (first * (fraction_scale - fraction) + second * fraction + fraction_scale / 2) / fraction_scale;
}

std::vector<std::uint8_t> compose_page(const manga::Image& image, int width, int height, int rows) {
    const auto page = manga::fit_page(image.width, image.height, width, height);
    if (page.width == 0 || page.height == 0) {
        throw std::runtime_error("Cannot fit the page to the Kindle viewport");
    }

    const auto stride = static_cast<std::size_t>(width);
    std::vector<std::uint8_t> frame(stride * static_cast<std::size_t>(rows), 255);
    if (page.y >= rows) {
        return frame;
    }
    std::vector<Sample> columns;
    columns.reserve(static_cast<std::size_t>(page.width));
    for (int column = 0; column < page.width; ++column) {
        columns.push_back(sample_axis(column, page.width, image.width));
    }
    const auto source_stride = std::size_t{image.width} * 4;
    for (int row = 0; row < std::min(page.height, rows - page.y); ++row) {
        const auto vertical = sample_axis(row, page.height, image.height);
        const auto* top = image.pixels.data() + vertical.first * source_stride;
        const auto* bottom = image.pixels.data() + vertical.second * source_stride;
        auto* output = frame.data() + static_cast<std::size_t>(page.y + row) * stride + page.x;
        for (const auto& horizontal : columns) {
            const auto left = horizontal.first * 4;
            const auto right = horizontal.second * 4;
            const auto upper = interpolate(grayscale_on_white(top + left), grayscale_on_white(top + right),
                                           horizontal.fraction);
            const auto lower = interpolate(grayscale_on_white(bottom + left), grayscale_on_white(bottom + right),
                                           horizontal.fraction);
            *output++ = static_cast<std::uint8_t>(interpolate(upper, lower, vertical.fraction));
        }
    }
    return frame;
}

std::vector<std::uint8_t> archive_thumbnail(const std::filesystem::path& archive, int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    try {
        const manga::ZipBook book(archive);
        if (book.page_count() != 0) {
            return compose_page(book.load_page(0), width, height, height);
        }
    } catch (const std::exception& error) {
        std::cerr << "Could not load cover for " << archive.filename().string() << ": " << error.what() << '\n';
    }
    return {};
}

void require_fbink(int result, const char* action) {
    if (result < 0) {
        throw std::runtime_error(std::string(action) + " (FBInk error " + std::to_string(result) + ")");
    }
}

struct Framebuffer {
    const int fd = fbink_open();

    ~Framebuffer() {
        if (fd >= 0) {
            fbink_close(fd);
        }
    }
};

FBInkState display_state(int fd, const FBInkConfig& configuration) {
    require_fbink(fbink_reinit(fd, &configuration), "Check framebuffer layout");
    FBInkState state{};
    fbink_get_state(&configuration, &state);
    constexpr auto max_dimension = static_cast<unsigned>(std::numeric_limits<short>::max());
    if (state.view_width == 0 || state.view_height == 0 ||
        state.view_width > max_dimension || state.view_height > max_dimension) {
        throw std::runtime_error("Invalid FBInk viewport dimensions");
    }
    return state;
}

FBInkState draw_page(int fd, const FBInkConfig& configuration, const manga::Reader& reader,
                     bool wait_for_update = true) {
    const auto state = display_state(fd, configuration);
    const auto width = static_cast<int>(state.view_width);
    const auto height = static_cast<int>(state.view_height);
    const auto frame = compose_page(reader.image(), width, height, height);
    require_fbink(fbink_print_raw_data(fd, frame.data(), width, height, frame.size(), 0, 0, &configuration),
                  "Draw page on the Kindle");
    const auto marker = fbink_get_last_marker();
    if (wait_for_update && configuration.is_flashing && marker != 0 && !state.unreliable_wait_for) {
        require_fbink(fbink_wait_for_complete(fd, marker), "Wait for the Kindle display update");
    }
    std::cout << "Page " << reader.page_index() + 1 << '/' << reader.page_count() << "  " << reader.page_name()
              << std::endl;
    return state;
}

void draw_reading_dropdown(int fd, const FBInkConfig& configuration, const manga::Reader& reader,
                           const FBInkState& state, int header,
                           const std::optional<manga::KindleReadingHeader>& dropdown) {
    const auto width = static_cast<int>(state.view_width);
    // Only refresh the area covered by the dropdown, leaving the page geometry intact.
    auto frame = dropdown ? std::vector<std::uint8_t>(static_cast<std::size_t>(width) * header, 255)
                          : compose_page(reader.image(), width, static_cast<int>(state.view_height), header);
    if (dropdown) {
        manga::paint_reading_header(frame, width, header, reader.page_index() + 1, reader.page_count(), *dropdown);
    }
    require_fbink(fbink_print_raw_data(fd, frame.data(), width, header, frame.size(), 0, 0, &configuration),
                  "Update the Kindle reading dropdown");
}

// Fits the page to the screen above the scrubber controls, outlined inside a narrow margin.
void draw_scrubber_preview(int fd, const FBInkConfig& configuration, const FBInkState& state,
                           const manga::Reader& reader) {
    constexpr std::uint8_t outline = 172;
    const auto width = static_cast<int>(state.view_width);
    const auto height = static_cast<int>(state.view_height);
    const auto top = height - manga::page_scrubber_height(width, height);
    const auto gap = std::max(2, width / 48);
    const auto area_width = width - 2 * gap;
    const auto area_height = top - 2 * gap;
    const auto& image = reader.image();
    const auto page = compose_page(image, area_width, area_height, area_height);
    const auto fitted = manga::fit_page(image.width, image.height, area_width, area_height);

    // Copy the page without the white bars that center it in the area, and outline it.
    const auto stride = static_cast<std::size_t>(width);
    std::vector<std::uint8_t> frame(stride * static_cast<std::size_t>(top), 255);
    for (int row = -1; row <= fitted.height; ++row) {
        auto* output = frame.data() + static_cast<std::size_t>(gap + fitted.y + row) * stride + gap + fitted.x;
        if (row < 0 || row == fitted.height) {
            std::fill_n(output - 1, fitted.width + 2, outline);
        } else {
            const auto* input = page.data() + static_cast<std::size_t>(fitted.y + row) * area_width + fitted.x;
            output[-1] = output[fitted.width] = outline;
            std::copy_n(input, fitted.width, output);
        }
    }
    require_fbink(fbink_print_raw_data(fd, frame.data(), width, top, frame.size(), 0, 0, &configuration),
                  "Draw the Kindle page preview");
}

void draw_scrubber_controls(int fd, const FBInkConfig& configuration, const FBInkState& state,
                            const manga::KindlePageScrubber& scrubber) {
    const auto width = static_cast<int>(state.view_width);
    const auto height = static_cast<int>(state.view_height);
    const auto panel = manga::page_scrubber_height(width, height);
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(width) * static_cast<std::size_t>(panel), 255);
    manga::paint_page_scrubber(frame, width, panel, scrubber);
    require_fbink(fbink_print_raw_data(fd, frame.data(), width, panel, frame.size(), 0,
                                       static_cast<short>(height - panel), &configuration),
                  "Draw the Kindle page scrubber");
}

struct Point {
    double x;
    double y;
};

struct Gesture {
    Point start;
    Point end;
    bool moved = false;

    bool is_tap() const { return !moved; }
};

// A contact that is still down, or the completed gesture once it lifts.
struct Touch {
    Gesture gesture;
    bool lifted;
};

bool read_axis(int fd, unsigned axis, input_absinfo& info) {
    return ioctl(fd, EVIOCGABS(axis), &info) == 0 && info.maximum > info.minimum;
}

double normalize_axis(int value, const input_absinfo& axis) {
    const auto clamped = std::clamp(value, axis.minimum, axis.maximum);
    return (static_cast<double>(clamped) - axis.minimum) / (static_cast<double>(axis.maximum) - axis.minimum);
}

class StopSignals {
public:
    StopSignals() {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        if (sigprocmask(SIG_BLOCK, &signals, &previous_mask_) < 0) {
            throw std::system_error(errno, std::generic_category(), "Block Kindle viewer exit signals");
        }
        fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
        if (fd < 0) {
            const int error = errno;
            sigprocmask(SIG_SETMASK, &previous_mask_, nullptr);
            throw std::system_error(error, std::generic_category(), "Watch Kindle viewer exit signals");
        }
    }

    ~StopSignals() {
        signalfd_siginfo signal;
        while (read(fd, &signal, sizeof(signal)) == sizeof(signal)) {
        }
        close(fd);
        sigprocmask(SIG_SETMASK, &previous_mask_, nullptr);
    }

    StopSignals(const StopSignals&) = delete;
    StopSignals& operator=(const StopSignals&) = delete;

    int fd;

private:
    sigset_t previous_mask_;
};

struct InputDevices {
    std::size_t count = 0;
    FBInkInputDevice* entries = fbink_input_scan(INPUT_TOUCHSCREEN | INPUT_POWER_BUTTON, 0, 0, &count);

    ~InputDevices() {
        if (entries) {
            for (std::size_t index = 0; index < count; ++index) {
                if (entries[index].fd >= 0) {
                    close(entries[index].fd);
                }
            }
            std::free(entries);
        }
    }
};

bool is_power_press(const input_event& event) {
    return event.type == EV_KEY && event.code == KEY_POWER && event.value == 1;
}

class Touchscreen {
public:
    Touchscreen(int power_listener, int signal_listener) : events_{{power_listener, POLLIN, 0},
                                                                  {signal_listener, POLLIN, 0}} {
        touch_ = find_touch(false);
        if (!touch_) {
            touch_ = find_touch(true);
        }
        if (!touch_) {
            throw std::runtime_error("Could not find a Kindle touchscreen with supported input events");
        }
        for (std::size_t index = 0; index < devices_.count; ++index) {
            const auto& device = devices_.entries[index];
            if (&device != touch_ && device.fd >= 0 && (device.type & INPUT_POWER_BUTTON)) {
                events_.push_back({device.fd, POLLIN, 0});
            }
        }
        events_.push_back({touch_->fd, POLLIN, 0});
    }

    void acquire() {
        // A grab is device-wide. On a shared touchscreen and power device, use passive input.
        if (touch_->type & INPUT_POWER_BUTTON) {
            std::cout << "Reading shared Kindle touch and power input without grabbing it" << std::endl;
            return;
        }
        if (ioctl(touch_->fd, EVIOCGRAB, 1) < 0) {
            throw std::system_error(errno, std::generic_category(), "Acquire the Kindle touchscreen");
        }
    }

    void release() noexcept {
        // Closing the event descriptor also drops any exclusive grab.
        if (touch_->fd >= 0) {
            close(touch_->fd);
            touch_->fd = -1;
        }
    }

    Touchscreen(const Touchscreen&) = delete;
    Touchscreen& operator=(const Touchscreen&) = delete;

    bool power_transition_pending() {
        return wait_for_events(events_.size() - 1, 0) && stop_for_system_event();
    }

    std::optional<Gesture> wait_for_gesture() {
        while (const auto touch = wait_for_touch()) {
            if (touch->lifted) {
                return touch->gesture;
            }
        }
        return std::nullopt;
    }

    // Reports a contact that is still down once the queued input is read, so a slow
    // caller sees the latest position. Reports a lifted contact as soon as it is read.
    std::optional<Touch> wait_for_touch() {
        constexpr int idle_timeout_ms = 10 * 60 * 1000;
        std::optional<Touch> latest;
        while (wait_for_events(events_.size(), latest ? 0 : idle_timeout_ms)) {
            if (stop_for_system_event()) {
                return std::nullopt;
            }
            const auto touch_events = events_.back().revents;
            if (touch_events == 0) {
                continue;
            }
            if ((touch_events & POLLIN) == 0) {
                throw std::runtime_error("The Kindle touchscreen became unavailable");
            }
            std::array<input_event, 16> batch;
            const auto capacity = touch_->type & INPUT_POWER_BUTTON ? sizeof(batch) : sizeof(input_event);
            const auto received = read(touch_->fd, batch.data(), capacity);
            if (received < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            constexpr auto event_size = static_cast<ssize_t>(sizeof(input_event));
            if (received <= 0 || received % event_size != 0) {
                throw std::runtime_error("Could not read the Kindle touch event");
            }
            const auto end = batch.begin() + received / event_size;
            if (std::any_of(batch.begin(), end, is_power_press)) {
                close_for_power_press();
                return std::nullopt;
            }
            std::optional<Touch> lifted;
            for (auto event = batch.begin(); event != end; ++event) {
                if (auto report = consume(*event); report && !lifted) {
                    (report->lifted ? lifted : latest) = report;
                }
            }
            if (lifted) {
                return lifted;
            }
        }
        return latest;
    }

private:
    FBInkInputDevice* find_touch(bool shared) {
        if (devices_.entries) {
            for (std::size_t index = 0; index < devices_.count; ++index) {
                auto& device = devices_.entries[index];
                if (device.fd >= 0 && (device.type & INPUT_TOUCHSCREEN) &&
                    static_cast<bool>(device.type & INPUT_POWER_BUTTON) == shared && configure(device.fd)) {
                    return &device;
                }
            }
        }
        return nullptr;
    }

    void close_for_power_press() {
        release();
        std::cout << "Closing for Kindle power button" << std::endl;
    }

    bool wait_for_events(std::size_t count, int timeout) {
        int ready;
        do {
            ready = poll(events_.data(), count, timeout);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            throw std::system_error(errno, std::generic_category(), "Wait for Kindle input or sleep");
        }
        return ready != 0;
    }

    bool stop_for_system_event() {
        if (events_[0].revents) {
            release();
            char event[256];
            const auto bytes = read(events_[0].fd, event, sizeof(event));
            if (bytes > 0) {
                std::cout << "Closing for Kindle sleep: " << std::string_view(event, static_cast<std::size_t>(bytes))
                          << std::endl;
            } else {
                std::cerr << "The Kindle sleep listener closed.\n";
            }
            return true;
        }
        if (events_[1].revents) {
            release();
            return true;
        }
        for (std::size_t index = 2; index + 1 < events_.size(); ++index) {
            auto& descriptor = events_[index];
            if (descriptor.revents & POLLIN) {
                std::array<input_event, 16> batch;
                const auto bytes = read(descriptor.fd, batch.data(), sizeof(batch));
                if (bytes > 0) {
                    const auto end = batch.begin() + bytes / static_cast<ssize_t>(sizeof(input_event));
                    if (std::any_of(batch.begin(), end, is_power_press)) {
                        close_for_power_press();
                        return true;
                    }
                }
            }
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                descriptor.fd = -1;
            }
        }
        return false;
    }

    bool configure(int fd) {
        input_absinfo slots{};
        multitouch_ = ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slots) == 0 && slots.minimum == 0 &&
                      read_axis(fd, ABS_MT_POSITION_X, axis_x_) && read_axis(fd, ABS_MT_POSITION_Y, axis_y_);
        slot_ = slots.value;
        if (!multitouch_ && !(read_axis(fd, ABS_X, axis_x_) && read_axis(fd, ABS_Y, axis_y_))) {
            return false;
        }
        other_contacts_.assign(multitouch_ ? static_cast<std::size_t>(slots.maximum) : 0, false);
        x_ = axis_x_.value;
        y_ = axis_y_.value;
        return true;
    }

    void update_contact(bool down) {
        if (active_ == down) {
            return;
        }
        active_ = down;
        released_ = !down;
        if (down) {
            gesture_.reset();
            blocked_ = std::find(other_contacts_.begin(), other_contacts_.end(), true) != other_contacts_.end();
        }
    }

    std::optional<Touch> report_contact() {
        constexpr double tap_slop = 0.02;
        if (active_ || released_) {
            const Point point{normalize_axis(x_, axis_x_), normalize_axis(y_, axis_y_)};
            if (active_ && !gesture_) {
                gesture_ = Gesture{point, point};
            } else if (gesture_) {
                gesture_->end = point;
                gesture_->moved = gesture_->moved || std::abs(point.x - gesture_->start.x) >= tap_slop ||
                                  std::abs(point.y - gesture_->start.y) >= tap_slop;
            }
        }
        if (!released_) {
            if (!gesture_ || blocked_) {
                return std::nullopt;
            }
            return Touch{*gesture_, false};
        }
        released_ = false;
        auto completed = std::exchange(gesture_, std::nullopt);
        if (!completed || blocked_) {
            return std::nullopt;
        }
        return Touch{*completed, true};
    }

    std::optional<Touch> consume(const input_event& event) {
        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
            active_ = released_ = false;
            gesture_.reset();
            resync_ = true;
        } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
            if (resync_) {
                if (!configure(touch_->fd)) {
                    throw std::runtime_error("Could not resynchronize the Kindle touchscreen");
                }
                resync_ = false;
            } else {
                return report_contact();
            }
        } else if (!resync_ && multitouch_ && event.type == EV_ABS) {
            if (event.code == ABS_MT_SLOT) {
                slot_ = event.value;
            } else if (slot_ == 0) {
                switch (event.code) {
                case ABS_MT_POSITION_X: x_ = event.value; break;
                case ABS_MT_POSITION_Y: y_ = event.value; break;
                case ABS_MT_TRACKING_ID: update_contact(event.value >= 0); break;
                }
            } else if (event.code == ABS_MT_TRACKING_ID && slot_ > 0 &&
                       static_cast<std::size_t>(slot_) <= other_contacts_.size()) {
                const bool down = event.value >= 0;
                other_contacts_[static_cast<std::size_t>(slot_ - 1)] = down;
                blocked_ = blocked_ || (down && (active_ || released_));
            }
        } else if (!resync_ && !multitouch_) {
            if (event.type == EV_KEY && event.code == BTN_TOUCH) {
                update_contact(event.value != 0);
            } else if (event.type == EV_ABS && event.code == ABS_X) {
                x_ = event.value;
            } else if (event.type == EV_ABS && event.code == ABS_Y) {
                y_ = event.value;
            }
        }
        return std::nullopt;
    }

    InputDevices devices_;
    std::vector<pollfd> events_;
    FBInkInputDevice* touch_ = nullptr;
    bool multitouch_ = false;
    input_absinfo axis_x_{};
    input_absinfo axis_y_{};
    int slot_ = 0;
    int x_ = 0;
    int y_ = 0;
    bool active_ = false;
    bool released_ = false;
    bool resync_ = false;
    bool blocked_ = false;
    std::vector<bool> other_contacts_;
    std::optional<Gesture> gesture_;
};

struct ScreenSnapshot {
    const int fd;
    const FBInkConfig restore_configuration;
    FBInkDump dump{};

    ScreenSnapshot(int framebuffer, const FBInkConfig& configuration)
        : fd(framebuffer), restore_configuration(configuration) {
        require_fbink(fbink_dump(fd, &dump), "Save the original Kindle screen");
    }

    void restore() {
        if (fbink_restore(fd, &restore_configuration, &dump) < 0) {
            std::cerr << "Could not restore the original Kindle screen.\n";
        }
    }

    ~ScreenSnapshot() {
        fbink_free_dump_data(&dump);
    }
};

bool turn_page(manga::Reader& reader, bool next) {
    try {
        return next ? reader.next() : reader.previous();
    } catch (const std::runtime_error& error) {
        std::cerr << "Cannot turn page: " << error.what() << '\n';
        return false;
    }
}

Point screen_point(Point point, const FBInkState& state) {
    if (state.touch_swap_axes) {
        std::swap(point.x, point.y);
    }
    if (state.touch_mirror_x) {
        point.x = 1.0 - point.x;
    }
    if (state.touch_mirror_y) {
        point.y = 1.0 - point.y;
    }
    return point;
}

Gesture screen_gesture(Gesture gesture, const FBInkState& state) {
    gesture.start = screen_point(gesture.start, state);
    gesture.end = screen_point(gesture.end, state);
    return gesture;
}

// A normal exit may restore the original screen. Power and sleep events leave it to Kindle.
template<typename View>
void run_touch_session(int fd, const FBInkConfig& configuration, View view) {
    if ((fbink_features() & FBINK_FEATURE_INPUT) == 0) {
        throw std::runtime_error("This FBInk build has input detection disabled");
    }

    const StopSignals signals;
    const manga::KindlePowerListener power;
    Touchscreen touchscreen(power.fd(), signals.fd);
    if (touchscreen.power_transition_pending()) {
        return;
    }
    touchscreen.acquire();
    ScreenSnapshot screen(fd, configuration);
    if (touchscreen.power_transition_pending()) {
        touchscreen.release();
        return;
    }
    const bool closed = view(touchscreen);
    touchscreen.release();
    if (closed && !touchscreen.power_transition_pending()) {
        screen.restore();
    }
}

std::optional<manga::MenuAction> wait_for_menu_action(Touchscreen& touchscreen, const FBInkState& state,
                                                    const manga::KindleMenu& menu,
                                                    const manga::KindleMenuPage& display) {
    while (const auto touch = touchscreen.wait_for_gesture()) {
        const auto gesture = screen_gesture(*touch, state);
        const auto action = gesture.is_tap()
                                ? menu.hit(display, gesture.end.x, gesture.end.y)
                                : menu.swipe(display, gesture.start.x, gesture.start.y, gesture.end.x, gesture.end.y);
        if (action.kind != manga::MenuActionKind::none) {
            return action;
        }
    }
    return std::nullopt;
}

std::string reading_status(std::optional<int> battery) {
    const auto now = std::time(nullptr);
    std::tm local{};
    std::array<char, 32> clock{};
    const bool has_time = now != static_cast<std::time_t>(-1) && localtime_r(&now, &local) &&
                          std::strftime(clock.data(), clock.size(), "%Y-%m-%d  %H:%M", &local) != 0;
    std::string status = has_time ? clock.data() : "Time unavailable";
    return status + "   Battery " + (battery ? std::to_string(*battery) + '%' : "unavailable");
}

bool open_page(manga::Reader& reader, std::size_t page) {
    try {
        reader.go_to(page);
    } catch (const std::runtime_error& error) {
        std::cerr << "Cannot open page " << page + 1 << ": " << error.what() << '\n';
    }
    return reader.page_index() == page;
}

// Previews pages above a scrubber until a page is chosen or the scrubber is cancelled,
// which returns to the starting page. The reader follows the preview. Returns false when
// a system event or the idle timeout ends the session.
bool scrub_pages(manga::Reader& reader, int fd, const FBInkConfig& configuration, Touchscreen& touchscreen,
                 const FBInkState& state, bool left_advances) {
    const auto width = static_cast<int>(state.view_width);
    const auto height = static_cast<int>(state.view_height);
    const auto edge = std::clamp(height / 15, 40, 108);
    const auto original = reader.page_index();
    manga::KindlePageScrubber scrubber{original, reader.page_count(), left_advances, {}};
    draw_scrubber_preview(fd, configuration, state, reader);
    draw_scrubber_controls(fd, configuration, state, scrubber);
    while (const auto touch = touchscreen.wait_for_touch()) {
        if (touchscreen.power_transition_pending()) {
            return false;
        }
        const auto gesture = screen_gesture(touch->gesture, state);
        const bool tap = touch->lifted && gesture.is_tap();
        const auto action = manga::page_scrubber_action(
            scrubber, width, height, std::min(static_cast<int>(gesture.start.x * width), width - 1),
            std::min(static_cast<int>(gesture.start.y * height), height - 1),
            std::min(static_cast<int>(gesture.end.x * width), width - 1), tap);
        switch (action.kind) {
        case manga::ScrubberActionKind::select:
            if (action.page != scrubber.page) {
                // Move the knob first so it keeps up while the page decodes.
                scrubber.page = action.page;
                scrubber.message.clear();
                draw_scrubber_controls(fd, configuration, state, scrubber);
                const bool opened = open_page(reader, scrubber.page);
                if (touchscreen.power_transition_pending()) {
                    return false;
                }
                if (opened) {
                    draw_scrubber_preview(fd, configuration, state, reader);
                } else {
                    scrubber.message = "Could not load page " + std::to_string(scrubber.page + 1);
                    scrubber.page = reader.page_index();
                    draw_scrubber_controls(fd, configuration, state, scrubber);
                }
            }
            continue;
        case manga::ScrubberActionKind::go: return true;
        case manga::ScrubberActionKind::cancel: open_page(reader, original); return true;
        case manga::ScrubberActionKind::none: break;
        }
        const auto down = (gesture.end.y - gesture.start.y) * height;
        const auto across = std::abs(gesture.end.x - gesture.start.x) * width;
        if (touch->lifted && !tap && down > across && down >= std::max(24.0, edge / 2.0)) {
            open_page(reader, original);
            return true;
        }
    }
    return false;
}

template<typename OnDisplayed>
bool read_touch_book(manga::Reader& reader, int fd, FBInkConfig configuration, Touchscreen& touchscreen,
                     manga::TapDirection direction, bool has_library,
                     OnDisplayed displayed) {
    if (touchscreen.power_transition_pending()) {
        return false;
    }
    auto state = draw_page(fd, configuration, reader, false);
    displayed(reader.page_index());
    configuration.is_flashing = false;
    std::optional<manga::KindleReadingHeader> dropdown;
    std::optional<int> battery;
    const bool left_advances = direction == manga::TapDirection::left_advances;
    while (const auto touch = touchscreen.wait_for_gesture()) {
        if (touchscreen.power_transition_pending()) {
            return false;
        }
        const auto gesture = screen_gesture(*touch, state);
        const auto& point = gesture.end;
        const auto width = static_cast<int>(state.view_width);
        const auto height = static_cast<int>(state.view_height);
        const auto header = manga::reading_header_height(width, height);
        const auto edge = std::clamp(height / 15, 40, 108);
        const auto start_y = gesture.start.y * height;
        const auto down = point.y * height - start_y;
        const auto across = std::abs(point.x - gesture.start.x) * width;
        const bool tap = gesture.is_tap();
        const bool vertical = std::abs(down) > across;

        if (dropdown) {
            const bool outside_tap = tap && point.y * height >= header;
            const bool swipe_up = !tap && vertical && start_y < header && -down >= std::max(22.0, edge / 3.0);
            if (outside_tap || swipe_up) {
                dropdown.reset();
                draw_reading_dropdown(fd, configuration, reader, state, header, dropdown);
                continue;
            }

            const auto end_x = std::min(static_cast<int>(point.x * width), width - 1);
            const auto level = tap || across > std::abs(down)
                                   ? manga::reading_header_slider_value(*dropdown, width, header,
                                         std::min(static_cast<int>(gesture.start.x * width), width - 1),
                                         static_cast<int>(start_y), end_x, tap)
                                   : std::nullopt;
            if (level) {
                if (*level != dropdown->slider->value) {
                    if (manga::set_kindle_frontlight(*level)) {
                        dropdown->slider->value = *level;
                        dropdown->message.clear();
                    } else {
                        dropdown->message = "Could not adjust the light";
                    }
                    if (touchscreen.power_transition_pending()) {
                        return false;
                    }
                    dropdown->status = reading_status(battery);
                    draw_reading_dropdown(fd, configuration, reader, state, header, dropdown);
                }
                continue;
            }
            if (tap && manga::reading_header_button_hit(width, header, end_x, static_cast<int>(point.y * height))) {
                return true;
            }
            continue;
        }
        const bool top_tap = tap && point.y * height < edge;
        const bool top_swipe = !tap && vertical && start_y < edge * 0.75 && down >= std::max(24.0, edge / 2.0);
        if (header > 0 && (top_tap || top_swipe)) {
            const auto light = manga::read_kindle_frontlight();
            if (touchscreen.power_transition_pending()) {
                return false;
            }
            battery = manga::read_kindle_battery_percent();
            if (touchscreen.power_transition_pending()) {
                return false;
            }
            dropdown.emplace();
            dropdown->button = has_library ? "Library" : "Exit";
            if (light) {
                dropdown->slider = manga::KindleMenuSlider{"Light", 0, manga::kindle_frontlight_max, *light};
            }
            dropdown->status = reading_status(battery);
            draw_reading_dropdown(fd, configuration, reader, state, header, dropdown);
            continue;
        }
        const bool bottom_swipe = !tap && vertical && start_y > height - edge * 0.75 &&
                                  -down >= std::max(24.0, edge / 2.0);
        if (bottom_swipe && manga::page_scrubber_height(width, height) > 0) {
            if (!scrub_pages(reader, fd, configuration, touchscreen, state, left_advances) ||
                touchscreen.power_transition_pending()) {
                return false;
            }
            state = draw_page(fd, configuration, reader, false);
            displayed(reader.page_index());
            continue;
        }
        if (!tap) {
            continue;
        }
        if (turn_page(reader, (point.x < 0.5) == left_advances)) {
            if (touchscreen.power_transition_pending()) {
                break;
            }
            state = draw_page(fd, configuration, reader, false);
            displayed(reader.page_index());
        }
    }
    return false;
}

bool browse_library(const Options& options, int fd, FBInkConfig configuration, Touchscreen& touchscreen) {
    using manga::MenuActionKind;
    using manga::TapDirection;

    const auto root = std::filesystem::canonical(options.library);
    auto entries = manga::list_library(root);
    auto direction = manga::read_tap_direction(options.settings);
    auto saved = manga::read_reading_position(options.state);
    enum class LibraryView { picker, settings, delete_picker };
    auto view = LibraryView::picker;
    std::size_t page = 0;
    std::optional<std::size_t> pending_deletion;
    std::string message;
    std::pair<int, int> thumbnail_size{};
    std::map<std::filesystem::path, std::vector<std::uint8_t>> thumbnails;
    constexpr auto per_page = manga::KindleMenu::covers_per_page;
    constexpr auto max_cached_thumbnails = 3 * per_page;

    const auto open_book = [&](const std::filesystem::path& archive) {
        const auto relative = archive.lexically_relative(root);
        const auto first = saved && saved->archive == relative ? saved->page_index : 0;
        std::unique_ptr<manga::Reader> reader;
        try {
            try {
                reader = std::make_unique<manga::Reader>(archive, first);
            } catch (const std::out_of_range&) {
                reader = std::make_unique<manga::Reader>(archive);
            }
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            message = "Could not open that archive";
            return true;
        }
        return read_touch_book(*reader, fd, configuration, touchscreen, direction, true, [&](std::size_t index) {
            if (saved && saved->archive == relative && saved->page_index == index) {
                return;
            }
            manga::ReadingPosition position{relative, index};
            try {
                manga::write_reading_position(options.state, position);
                saved = std::move(position);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                message = "Could not save reading position";
            }
        });
    };

    const auto delete_archive = [&](std::size_t index) {
        const auto entry = entries.begin() + static_cast<std::ptrdiff_t>(index);
        const auto relative = entry->path.lexically_relative(root);
        try {
            manga::remove_library_archive(root, entry->path);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            message = "Could not delete archive";
            return;
        }
        thumbnails.erase(entry->path);
        entries.erase(entry);
        message = "Archive deleted";

        if (saved && saved->archive == relative) {
            saved.reset();
            const auto position = manga::read_reading_position(options.state);
            if (position && position->archive == relative && unlink(options.state.c_str()) < 0 && errno != ENOENT) {
                std::cerr << "Could not clear saved reading position: "
                          << std::error_code(errno, std::generic_category()).message() << '\n';
                message = "Deleted. Could not clear saved position";
            }
        }
    };

    if (saved && !touchscreen.power_transition_pending()) {
        const auto match = std::find_if(entries.begin(), entries.end(), [&](const manga::LibraryEntry& entry) {
            return entry.path.lexically_relative(root) == saved->archive;
        });
        if (match != entries.end()) {
            if (!open_book(match->path)) {
                return false;
            }
        } else {
            message = "Previous archive is unavailable";
        }
    }

    while (!touchscreen.power_transition_pending()) {
        const auto state = display_state(fd, configuration);
        const manga::KindleMenu menu(state);
        const auto last_page = entries.empty() ? 0 : (entries.size() - 1) / per_page;
        page = std::min(page, last_page);
        const auto start = page * per_page;

        manga::KindleMenuPage display;
        display.message = message;
        if (pending_deletion) {
            display.title = "Delete archive?";
            display.subtitle = "Remove this file from your Kindle?";
            display.details = entries.at(*pending_deletion).path.lexically_relative(root).string();
            display.bottom_left = "Cancel";
            display.bottom_right = "Delete";
        } else if (view == LibraryView::settings) {
            display.title = "Settings";
            display.subtitle = "Tap side that advances the page";
            display.back = "Library";
            display.rows = {direction == TapDirection::left_advances ? "[x] Left side" : "[ ] Left side",
                            direction == TapDirection::right_advances ? "[x] Right side" : "[ ] Right side"};
        } else {
            const bool selecting_deletion = view == LibraryView::delete_picker;
            display.title = selecting_deletion ? "Delete an archive" : "Manga Library";
            display.subtitle = selecting_deletion ? "Choose an archive to delete" : root.filename().string();
            if (last_page > 0) {
                display.subtitle += "  " + std::to_string(page + 1) + "/" + std::to_string(last_page + 1);
            }
            display.back = selecting_deletion ? "Library" : "Exit";
            display.top_right = !selecting_deletion && !entries.empty() ? "Delete" : "";
            display.bottom_left = page > 0 ? "Previous" : "";
            display.bottom_center = selecting_deletion ? "" : "Settings";
            display.bottom_right = page < last_page ? "Next" : "";
            display.scroll_back = page > 0;
            display.scroll_forward = page < last_page;
            const auto size = menu.thumbnail_size();
            if (size != thumbnail_size) {
                thumbnail_size = size;
                thumbnails.clear();
            }
            for (std::size_t index = start; index < std::min(start + per_page, entries.size()); ++index) {
                const auto& archive = entries[index].path;
                auto thumbnail = thumbnails.find(archive);
                if (thumbnail == thumbnails.end()) {
                    if (touchscreen.power_transition_pending()) {
                        return false;
                    }
                    if (thumbnails.size() == max_cached_thumbnails) {
                        thumbnails.clear();
                    }
                    auto pixels = archive_thumbnail(archive, size.first, size.second);
                    thumbnail = thumbnails.emplace(archive, std::move(pixels)).first;
                }
                display.thumbnails.push_back(thumbnail->second);
            }
            if (entries.empty() && display.message.empty()) {
                display.message = "No ZIP or CBZ files found";
            }
        }

        if (touchscreen.power_transition_pending()) {
            return false;
        }
        menu.draw(fd, configuration, display);
        configuration.is_flashing = false;
        const auto action = wait_for_menu_action(touchscreen, state, menu, display);
        if (!action || touchscreen.power_transition_pending()) {
            return false;
        }

        message.clear();
        if (pending_deletion) {
            if (action->kind == MenuActionKind::bottom_right) {
                delete_archive(*pending_deletion);
            }
            pending_deletion.reset();
            view = LibraryView::picker;
            continue;
        }
        if (view == LibraryView::settings) {
            if (action->kind == MenuActionKind::back) {
                view = LibraryView::picker;
            } else if (action->kind == MenuActionKind::row) {
                const auto chosen = action->row == 0 ? TapDirection::left_advances : TapDirection::right_advances;
                try {
                    manga::write_tap_direction(options.settings, chosen);
                    direction = chosen;
                    message = "Saved";
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                    message = "Could not save settings";
                }
            }
            continue;
        }

        switch (action->kind) {
        case MenuActionKind::back:
            if (view == LibraryView::picker) {
                return true;
            }
            view = LibraryView::picker;
            break;
        case MenuActionKind::top_right: view = LibraryView::delete_picker; break;
        case MenuActionKind::bottom_left:
        case MenuActionKind::scroll_back: --page; break;
        case MenuActionKind::bottom_right:
        case MenuActionKind::scroll_forward: ++page; break;
        case MenuActionKind::bottom_center: view = LibraryView::settings; break;
        case MenuActionKind::row:
            if (view == LibraryView::delete_picker) {
                pending_deletion = start + action->row;
            } else if (!open_book(entries.at(start + action->row).path)) {
                return false;
            }
            break;
        case MenuActionKind::none: break;
        }
    }
    return false;
}

void run(const Options& options) {
    std::optional<manga::Reader> reader;
    if (options.library.empty()) {
        reader.emplace(options.archive, options.first_page);
    }
    if ((fbink_features() & FBINK_FEATURE_IMAGE) == 0) {
        throw std::runtime_error("This FBInk build has image support disabled");
    }

    FBInkConfig configuration{};
    configuration.ignore_alpha = true;
    configuration.no_viewport = true;
    configuration.wfm_mode = WFM_GC16;
    configuration.is_flashing = true;
    const Framebuffer framebuffer;
    require_fbink(framebuffer.fd, "Open the Kindle framebuffer");
    require_fbink(fbink_init(framebuffer.fd, &configuration), "Initialize the Kindle framebuffer");
    if (!options.library.empty()) {
        run_touch_session(framebuffer.fd, configuration, [&](Touchscreen& touchscreen) {
            return browse_library(options, framebuffer.fd, configuration, touchscreen);
        });
        return;
    }
    if (options.touch) {
        run_touch_session(framebuffer.fd, configuration, [&](Touchscreen& touchscreen) {
            return read_touch_book(*reader, framebuffer.fd, configuration, touchscreen,
                                   manga::TapDirection::left_advances, false, [](std::size_t) {});
        });
        return;
    }
    draw_page(framebuffer.fd, configuration, *reader);

    if (!options.interactive) {
        return;
    }
    configuration.is_flashing = false;
    std::cout << "n next, p previous, q quit\n" << std::flush;
    for (std::string command; std::getline(std::cin, command) && command != "q";) {
        if (command == "n" || command == "p") {
            if (turn_page(*reader, command == "n")) {
                draw_page(framebuffer.fd, configuration, *reader);
            }
        } else if (!command.empty()) {
            std::cerr << "Enter n, p, or q.\n";
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        print_usage(std::cout);
        return EXIT_SUCCESS;
    }
    try {
        run(parse_options(argc, argv));
        return EXIT_SUCCESS;
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        print_usage(std::cerr);
    } catch (const std::exception& error) {
        std::cerr << "manga_kindle: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}

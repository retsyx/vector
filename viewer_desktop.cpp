#include "viewer.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

enum class Action { none, previous, next, first, last, fullscreen, quit };

[[noreturn]] void fail_sdl(const char* action) {
    throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
}

struct VideoSession {
    VideoSession() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fail_sdl("Start SDL video");
        }
    }

    ~VideoSession() { SDL_Quit(); }
};

using Texture = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;

Texture make_texture(SDL_Renderer* renderer, const manga::Image& image) {
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    Texture texture(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                      width, height),
                    SDL_DestroyTexture);
    if (!texture || !SDL_UpdateTexture(texture.get(), nullptr, image.pixels.data(), width * 4) ||
        !SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND)) {
        fail_sdl("Upload page texture");
    }
    return texture;
}

void draw_page(SDL_Renderer* renderer, SDL_Texture* texture, const manga::Image& image) {
    int viewport_width = 0;
    int viewport_height = 0;
    if (!SDL_GetRenderOutputSize(renderer, &viewport_width, &viewport_height)) {
        fail_sdl("Get window size");
    }
    if (viewport_width <= 0 || viewport_height <= 0) {
        return;
    }

    const auto page = manga::fit_page(image.width, image.height, viewport_width, viewport_height);
    const SDL_FRect destination{static_cast<float>(page.x), static_cast<float>(page.y),
                                static_cast<float>(page.width), static_cast<float>(page.height)};
    if (!SDL_SetRenderDrawColor(renderer, 37, 37, 37, 255) || !SDL_RenderClear(renderer) ||
        !SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255) ||
        !SDL_RenderFillRect(renderer, &destination) ||
        !SDL_RenderTexture(renderer, texture, nullptr, &destination) || !SDL_RenderPresent(renderer)) {
        fail_sdl("Draw page");
    }
}

Action horizontal_action(bool left, bool right_to_left) {
    return left == right_to_left ? Action::next : Action::previous;
}

Action event_action(const SDL_Event& event, SDL_Window* window, bool right_to_left) {
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        return Action::quit;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
        int width = 0;
        if (SDL_GetWindowSize(window, &width, nullptr) && width > 0) {
            return horizontal_action(event.button.x < static_cast<float>(width) / 2, right_to_left);
        }
    }
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return Action::none;
    }

    switch (event.key.key) {
    case SDLK_LEFT: return horizontal_action(true, right_to_left);
    case SDLK_RIGHT: return horizontal_action(false, right_to_left);
    case SDLK_PAGEUP: return Action::previous;
    case SDLK_PAGEDOWN:
    case SDLK_SPACE: return Action::next;
    case SDLK_HOME: return Action::first;
    case SDLK_END: return Action::last;
    case SDLK_F:
    case SDLK_F11: return Action::fullscreen;
    case SDLK_ESCAPE:
        return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) ? Action::fullscreen : Action::quit;
    default: return Action::none;
    }
}

bool navigate(manga::Reader& reader, Action action) {
    switch (action) {
    case Action::previous: return reader.previous();
    case Action::next: return reader.next();
    case Action::first: return reader.go_to(0);
    case Action::last: return reader.go_to(reader.page_count() - 1);
    default: return false;
    }
}

void set_page_title(SDL_Window* window, const std::string& archive_name, const manga::Reader& reader) {
    const auto title = archive_name + " | " + std::to_string(reader.page_index() + 1) + "/" +
                       std::to_string(reader.page_count()) + " | Arrows: pages, F: fullscreen";
    SDL_SetWindowTitle(window, title.c_str());
}

int view_book(const std::filesystem::path& path, bool right_to_left) {
    manga::Reader reader(path);
    const VideoSession video;
    const auto archive_name = path.filename().string();
    int window_width = 900;
    int window_height = 1000;
    SDL_Rect usable_bounds{};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable_bounds)) {
        window_width = std::clamp(usable_bounds.w * 9 / 10, 1, window_width);
        window_height = std::clamp(usable_bounds.h * 9 / 10, 1, window_height);
    }
    const std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow(archive_name.c_str(), window_width, window_height,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY),
        SDL_DestroyWindow);
    if (!window) {
        fail_sdl("Open viewer window");
    }
    const std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateRenderer(window.get(), nullptr), SDL_DestroyRenderer);
    if (!renderer) {
        fail_sdl("Create page renderer");
    }

    auto texture = make_texture(renderer.get(), reader.image());
    set_page_title(window.get(), archive_name, reader);
    draw_page(renderer.get(), texture.get(), reader.image());

    for (;;) {
        SDL_Event event{};
        if (!SDL_WaitEvent(&event)) {
            fail_sdl("Wait for input");
        }
        const auto action = event_action(event, window.get(), right_to_left);
        if (action == Action::quit) {
            return EXIT_SUCCESS;
        }
        if (action == Action::fullscreen) {
            const bool is_fullscreen = (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_FULLSCREEN) != 0;
            if (!SDL_SetWindowFullscreen(window.get(), !is_fullscreen)) {
                std::cerr << "Switch fullscreen: " << SDL_GetError() << '\n';
            }
            continue;
        }

        bool page_changed = false;
        try {
            page_changed = navigate(reader, action);
        } catch (const std::exception& error) {
            // Keep displaying the last decoded page and surface the failing entry.
            std::cerr << error.what() << '\n';
            const auto title = archive_name + " | " + error.what();
            SDL_SetWindowTitle(window.get(), title.c_str());
        }
        if (page_changed || event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
            texture = make_texture(renderer.get(), reader.image());
            set_page_title(window.get(), archive_name, reader);
            draw_page(renderer.get(), texture.get(), reader.image());
        } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                   event.type == SDL_EVENT_WINDOW_EXPOSED) {
            draw_page(renderer.get(), texture.get(), reader.image());
        }
    }
}

void print_usage(std::ostream& output) {
    output << "Usage: manga_viewer [--rtl] archive.zip\n"
              "Use arrow keys or click either side to turn pages. Home and End jump to the first and last page.\n"
              "F toggles fullscreen. Esc exits fullscreen or closes the viewer.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        print_usage(std::cout);
        return EXIT_SUCCESS;
    }
    const bool right_to_left = argc == 3 && std::string_view(argv[1]) == "--rtl";
    if (argc != 2 && !right_to_left) {
        print_usage(std::cerr);
        return EXIT_FAILURE;
    }

    try {
        return view_book(argv[right_to_left ? 2 : 1], right_to_left);
    } catch (const std::exception& error) {
        std::cerr << "manga_viewer: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

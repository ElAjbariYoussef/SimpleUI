#include "Platform/Linux/LinuxPlatform.h"

#include <SDL3/SDL.h>

#include <utility>

namespace sui {
namespace {

std::string describe(const char* context) {
    return std::string(context) + ": " + SDL_GetError();
}

}  // namespace

bool LinuxPlatform::initialize(const WindowConfig& config, std::string& error) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error = describe("SDL_Init(VIDEO)");
        return false;
    }

    SDL_WindowFlags flags = 0;
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (config.maximized) {
        flags |= SDL_WINDOW_MAXIMIZED;
    }
    if (config.borderless) {
        flags |= SDL_WINDOW_BORDERLESS;
    }
    if (config.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    // SDL3 takes no x/y position; that moved to SDL_SetWindowPosition.
    window_ = SDL_CreateWindow(config.title.c_str(), config.width, config.height,
                              flags);
    if (!window_) {
        error = describe("SDL_CreateWindow");
        SDL_Quit();
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        error = describe("SDL_CreateRenderer");
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
        return false;
    }

    return true;
}

void LinuxPlatform::shutdown() {
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

void LinuxPlatform::clear() {
    SDL_SetRenderDrawColor(renderer_, 24, 24, 27, 255);
    SDL_RenderClear(renderer_);
}

void LinuxPlatform::present() {
    SDL_RenderPresent(renderer_);
}

void LinuxPlatform::windowSize(int& width, int& height) const {
    width = 0;
    height = 0;
    SDL_GetWindowSize(window_, &width, &height);
}

bool LinuxPlatform::pumpEvents() {
    input_.clear();

    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                running_ = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running_ = false;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                // A drag while a button is held keeps targeting the element
                // under the cursor, so it reports the same event as a click.
                if (event.motion.state & SDL_BUTTON_LMASK) {
                    input_.push_back(InputEvent{InputKind::PointerDown, event.motion.x,
                                                event.motion.y, SDL_BUTTON_LEFT});
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                input_.push_back(InputEvent{InputKind::PointerDown, event.button.x,
                                            event.button.y, event.button.button});
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                input_.push_back(InputEvent{InputKind::PointerUp, event.button.x,
                                            event.button.y, event.button.button});
                break;
            default:
                break;
        }
    }

    return running_;
}

}  // namespace sui
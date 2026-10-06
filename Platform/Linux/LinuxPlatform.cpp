#include "Platform/Linux/LinuxPlatform.h"

#include <SDL3/SDL.h>

#include <utility>

namespace sui {
namespace {

std::string describe(const char* context) {
    return std::string(context) + ": " + SDL_GetError();
}

// SDL reports the held buttons as a bitmask; the engine wants a single index, with
// 0 meaning none, so that a move can say whether it is part of a drag.
int heldButton(Uint32 state) {
    if (state & SDL_BUTTON_LMASK) {
        return SDL_BUTTON_LEFT;
    }
    if (state & SDL_BUTTON_MMASK) {
        return SDL_BUTTON_MIDDLE;
    }
    if (state & SDL_BUTTON_RMASK) {
        return SDL_BUTTON_RIGHT;
    }
    return 0;
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

    // Alpha has to reach the framebuffer as blending, not as a stored alpha byte.
    // SDL's default is no blending, which silently turns every translucent fill --
    // any `opacity`, any shadow, any semi-transparent background -- into an opaque
    // one, so this has to be set before the first frame.
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    // Text is rasterised by the engine at its exact pixel size, so the renderer's
    // own sampler has no business blending it. Nearest sampling plus pixel-aligned
    // destinations is what keeps glyph edges crisp; SDL defaults to linear, which
    // turns every glyph soft.
    SDL_SetDefaultTextureScaleMode(renderer_, SDL_SCALEMODE_NEAREST);
    setViewportSize(config.width, config.height);

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
    SDL_GetWindowSizeInPixels(window_, &width, &height);
}

void LinuxPlatform::setViewportSize(int width, int height) {
    // The logical size is the drawable size, so the mapping is 1:1 and the engine's
    // coordinates *are* framebuffer pixels. Letterbox at an exactly matching size is
    // a no-op, and it keeps the bars out of the way if the two ever drift apart.
    SDL_SetRenderLogicalPresentation(renderer_, width, height,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

// SDL reports the pointer in window points; layout, hit tests and paint all work in
// framebuffer pixels, so the two are reconciled once here rather than at every use.
float LinuxPlatform::pointerScale() const {
    int pointWidth = 0;
    int pointHeight = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    SDL_GetWindowSize(window_, &pointWidth, &pointHeight);
    SDL_GetWindowSizeInPixels(window_, &pixelWidth, &pixelHeight);
    if (pointWidth <= 0) {
        return 1.0f;
    }
    return static_cast<float>(pixelWidth) / static_cast<float>(pointWidth);
}

bool LinuxPlatform::pumpEvents() {
    input_.clear();

    const float scale = pointerScale();
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
                // Every move is reported, because it is the only thing that drives
                // `:hover` and a move with no button down is the common case. `button`
                // carries which button is held, which is what tells a plain hover from
                // a drag; the engine needs both to arrive as the same kind of event.
                input_.push_back(InputEvent{InputKind::PointerMove, event.motion.x * scale,
                                            event.motion.y * scale,
                                            heldButton(event.motion.state)});
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                input_.push_back(InputEvent{InputKind::PointerDown, event.button.x * scale,
                                            event.button.y * scale, event.button.button});
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                input_.push_back(InputEvent{InputKind::PointerUp, event.button.x * scale,
                                            event.button.y * scale, event.button.button});
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                // Deltas travel in notches (SDL's float x/y); the engine turns them
                // into pixels when it scrolls. The cursor position rides along so the
                // wheel lands on the element it points at.
                input_.push_back(InputEvent{InputKind::Wheel, event.wheel.mouse_x * scale,
                                            event.wheel.mouse_y * scale, 0, event.wheel.x,
                                            event.wheel.y});
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                // Nothing is hovered once the pointer is out of the window, and the
                // engine is never told about a motion again until it comes back. The
                // coordinate is off-screen so the hit test finds no target and
                // `:hover` clears, which is what a browser does on the same event.
                input_.push_back(InputEvent{InputKind::PointerMove, -1.0f, -1.0f, 0});
                break;
            default:
                break;
        }
    }

    return running_;
}

}  // namespace sui
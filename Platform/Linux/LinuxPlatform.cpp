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

// SDL's system-cursor set mirrors the CSS keywords almost one-to-one. Only the
// values SDL has no shape for are folded; everything else maps straight across.
SDL_SystemCursor systemCursorFor(std::string_view name) {
    struct Entry {
        std::string_view name;
        SDL_SystemCursor id;
    };
    static constexpr Entry table[] = {
        {"default", SDL_SYSTEM_CURSOR_DEFAULT},
        {"text", SDL_SYSTEM_CURSOR_TEXT},
        {"wait", SDL_SYSTEM_CURSOR_WAIT},
        {"crosshair", SDL_SYSTEM_CURSOR_CROSSHAIR},
        {"progress", SDL_SYSTEM_CURSOR_PROGRESS},
        {"nwse-resize", SDL_SYSTEM_CURSOR_NWSE_RESIZE},
        {"nesw-resize", SDL_SYSTEM_CURSOR_NESW_RESIZE},
        {"ew-resize", SDL_SYSTEM_CURSOR_EW_RESIZE},
        {"ns-resize", SDL_SYSTEM_CURSOR_NS_RESIZE},
        {"move", SDL_SYSTEM_CURSOR_MOVE},
        {"all-scroll", SDL_SYSTEM_CURSOR_ALL_SCROLL},
        {"not-allowed", SDL_SYSTEM_CURSOR_NOT_ALLOWED},
        {"pointer", SDL_SYSTEM_CURSOR_POINTER},
        {"no-drop", SDL_SYSTEM_CURSOR_NO_DROP},
        {"n-resize", SDL_SYSTEM_CURSOR_N_RESIZE},
        {"s-resize", SDL_SYSTEM_CURSOR_S_RESIZE},
        {"e-resize", SDL_SYSTEM_CURSOR_E_RESIZE},
        {"w-resize", SDL_SYSTEM_CURSOR_W_RESIZE},
        {"ne-resize", SDL_SYSTEM_CURSOR_NE_RESIZE},
        {"nw-resize", SDL_SYSTEM_CURSOR_NW_RESIZE},
        {"se-resize", SDL_SYSTEM_CURSOR_SE_RESIZE},
        {"sw-resize", SDL_SYSTEM_CURSOR_SW_RESIZE},
        {"context-menu", SDL_SYSTEM_CURSOR_CONTEXT_MENU},
        {"help", SDL_SYSTEM_CURSOR_HELP},
        {"cell", SDL_SYSTEM_CURSOR_CELL},
        {"vertical-text", SDL_SYSTEM_CURSOR_VERTICAL_TEXT},
        {"alias", SDL_SYSTEM_CURSOR_ALIAS},
        {"copy", SDL_SYSTEM_CURSOR_COPY},
        {"col-resize", SDL_SYSTEM_CURSOR_COL_RESIZE},
        {"row-resize", SDL_SYSTEM_CURSOR_ROW_RESIZE},
        {"grab", SDL_SYSTEM_CURSOR_GRAB},
        {"grabbing", SDL_SYSTEM_CURSOR_GRABBING},
        {"zoom-in", SDL_SYSTEM_CURSOR_ZOOM_IN},
        {"zoom-out", SDL_SYSTEM_CURSOR_ZOOM_OUT},
    };
    for (const auto& entry : table) {
        if (name == entry.name) {
            return entry.id;
        }
    }
    // "auto", an unknown keyword, an empty name: the platform's own arrow.
    return SDL_SYSTEM_CURSOR_DEFAULT;
}

// SDL3 puts the window id in a different union member per event family, so one
// helper routes every event we care about. Returns 0 when the event names no
// window at all (quit, or something outside the window families).
SDL_WindowID sdlWindowOf(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_QUIT:
            return 0;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            return event.key.windowID;
        case SDL_EVENT_TEXT_EDITING:
            return event.edit.windowID;
        case SDL_EVENT_TEXT_INPUT:
            return event.text.windowID;
        case SDL_EVENT_MOUSE_MOTION:
            return event.motion.windowID;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            return event.button.windowID;
        case SDL_EVENT_MOUSE_WHEEL:
            return event.wheel.windowID;
        default:
            break;
    }
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
        return event.window.windowID;
    }
    return 0;
}

}  // namespace

bool LinuxPlatform::initialize(const WindowConfig& config, std::string& error) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error = describe("SDL_Init(VIDEO)");
        return false;
    }
    // The entry window keeps the identity every un-threaded call site assumes.
    if (!createWindow(nextId_++, config, error)) {
        SDL_Quit();
        return false;
    }
    return true;
}

// The SDL half of opening a window, shared by startup and `openWindow`. Everything
// here that could fail leaves nothing half-built behind.
bool LinuxPlatform::createWindow(WindowId id, const WindowConfig& config, std::string& error) {
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
    SDL_Window* window =
        SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    if (!window) {
        error = describe("SDL_CreateWindow");
        return false;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        error = describe("SDL_CreateRenderer");
        SDL_DestroyWindow(window);
        return false;
    }

    // Alpha has to reach the framebuffer as blending, not as a stored alpha byte.
    // SDL's default is no blending, which silently turns every translucent fill --
    // any `opacity`, any shadow, any semi-transparent background -- into an opaque
    // one, so this has to be set before the first frame.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Text is rasterised by the engine at its exact pixel size, so the renderer's
    // own sampler has no business blending it. Nearest sampling plus pixel-aligned
    // destinations is what keeps glyph edges crisp; SDL defaults to linear, which
    // turns every glyph soft.
    SDL_SetDefaultTextureScaleMode(renderer, SDL_SCALEMODE_NEAREST);
    // The logical size is the drawable size, so the mapping is 1:1 and the engine's
    // coordinates *are* framebuffer pixels. Letterbox at an exactly matching size is
    // a no-op, and it keeps the bars out of the way if the two ever drift apart.
    SDL_SetRenderLogicalPresentation(renderer, config.width, config.height,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    // Enables SDL_EVENT_TEXT_INPUT, which is what carries typed characters to a
    // focused field. Without it only raw key events arrive and IME/text entry is
    // lost. It is per window, so every window that shows a form needs its own.
    SDL_StartTextInput(window);

    windows_.push_back(Window{id, SDL_GetWindowID(window), window, renderer});
    return true;
}

void LinuxPlatform::destroyWindow(Window& window) {
    if (window.renderer) {
        SDL_DestroyRenderer(window.renderer);
        window.renderer = nullptr;
    }
    if (window.window) {
        SDL_StopTextInput(window.window);
        SDL_DestroyWindow(window.window);
        window.window = nullptr;
    }
}

void LinuxPlatform::shutdown() {
    for (SDL_Cursor* cursor : cursors_) {
        if (cursor) {
            SDL_DestroyCursor(cursor);
        }
    }
    cursors_.clear();
    cursorName_.clear();
    for (Window& window : windows_) {
        destroyWindow(window);
    }
    windows_.clear();
    closed_.clear();
    SDL_Quit();
}

WindowId LinuxPlatform::openWindow(const WindowConfig& config, std::string& error) {
    if (windows_.empty()) {
        error = "the platform is shut down";
        return kInvalidWindow;
    }
    const WindowId id = nextId_++;
    if (!createWindow(id, config, error)) {
        // The id is burnt either way: an id that names no window must never come
        // to mean a different one later.
        return kInvalidWindow;
    }
    return id;
}

void LinuxPlatform::closeWindow(WindowId id) {
    for (std::size_t i = 0; i < windows_.size(); ++i) {
        if (windows_[i].id != id) {
            continue;
        }
        destroyWindow(windows_[i]);
        windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

bool LinuxPlatform::hasWindow(WindowId id) const {
    return find(id) != nullptr;
}

WindowId LinuxPlatform::primaryWindow() const {
    return windows_.empty() ? kInvalidWindow : windows_.front().id;
}

std::vector<WindowId> LinuxPlatform::windows() const {
    std::vector<WindowId> ids;
    ids.reserve(windows_.size());
    for (const Window& window : windows_) {
        ids.push_back(window.id);
    }
    return ids;
}

LinuxPlatform::Window* LinuxPlatform::find(WindowId id) {
    for (Window& window : windows_) {
        if (window.id == id) {
            return &window;
        }
    }
    return nullptr;
}

const LinuxPlatform::Window* LinuxPlatform::find(WindowId id) const {
    for (const Window& window : windows_) {
        if (window.id == id) {
            return &window;
        }
    }
    return nullptr;
}

WindowId LinuxPlatform::engineIdFor(std::uint32_t sdlId) const {
    for (const Window& window : windows_) {
        if (window.sdlId == sdlId) {
            return window.id;
        }
    }
    return kInvalidWindow;
}

SDL_Window* LinuxPlatform::window(WindowId id) const {
    const Window* found = find(id);
    return found ? found->window : nullptr;
}

SDL_Renderer* LinuxPlatform::renderer(WindowId id) const {
    const Window* found = find(id);
    return found ? found->renderer : nullptr;
}

void LinuxPlatform::clear(WindowId id) {
    SDL_Renderer* target = renderer(id);
    if (!target) {
        return;
    }
    SDL_SetRenderDrawColor(target, 24, 24, 27, 255);
    SDL_RenderClear(target);
}

void LinuxPlatform::present(WindowId id) {
    if (SDL_Renderer* target = renderer(id)) {
        SDL_RenderPresent(target);
    }
}

void LinuxPlatform::windowSize(WindowId id, int& width, int& height) const {
    width = 0;
    height = 0;
    if (SDL_Window* target = window(id)) {
        SDL_GetWindowSizeInPixels(target, &width, &height);
    }
}

void LinuxPlatform::setViewportSize(WindowId id, int width, int height) {
    if (SDL_Renderer* target = renderer(id)) {
        SDL_SetRenderLogicalPresentation(target, width, height,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }
}

void LinuxPlatform::setWindowTitle(WindowId id, std::string_view title) {
    if (SDL_Window* target = window(id)) {
        SDL_SetWindowTitle(target, std::string(title).c_str());
    }
}

// SDL reports the pointer in window points; layout, hit tests and paint all work in
// framebuffer pixels, so the two are reconciled once here rather than at every use.
float LinuxPlatform::pointerScale(WindowId id) const {
    SDL_Window* target = window(id);
    if (!target) {
        return 1.0f;
    }
    int pointWidth = 0;
    int pointHeight = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    SDL_GetWindowSize(target, &pointWidth, &pointHeight);
    SDL_GetWindowSizeInPixels(target, &pixelWidth, &pixelHeight);
    if (pointWidth <= 0) {
        return 1.0f;
    }
    return static_cast<float>(pixelWidth) / static_cast<float>(pointWidth);
}

bool LinuxPlatform::pumpEvents() {
    input_.clear();
    closed_.clear();

    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            // The app is going away entirely, unlike one window closing.
            running_ = false;
            continue;
        }
        // Every event names the window it came from, and an event for a window the
        // engine does not own is not this engine's business. Key and text events carry
        // it too, which is what lets a keystroke reach the field in *that* window.
        const WindowId window = engineIdFor(sdlWindowOf(event));
        if (window == kInvalidWindow) {
            continue;
        }
        // Pointer coordinates are in this window's points; every other window's
        // scale is its own business.
        const float scale = pointerScale(window);

        switch (event.type) {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // Closing one window closes one window. The page it was showing is
                // the application's to drop, which it learns from `closedWindows`.
                closed_.push_back(window);
                closeWindow(window);
                continue;
            case SDL_EVENT_MOUSE_MOTION:
                // Every move is reported, because it is the only thing that drives
                // `:hover` and a move with no button down is the common case. `button`
                // carries which button is held, which is what tells a plain hover from
                // a drag; the engine needs both to arrive as the same kind of event.
                input_.push_back(InputEvent{InputKind::PointerMove, event.motion.x * scale,
                                            event.motion.y * scale,
                                            heldButton(event.motion.state), 0.0f, 0.0f,
                                            0, {}, 0, 0, window});
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                input_.push_back(InputEvent{InputKind::PointerDown, event.button.x * scale,
                                            event.button.y * scale, event.button.button,
                                            0.0f, 0.0f, 0, {}, 0, 0, window});
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                input_.push_back(InputEvent{InputKind::PointerUp, event.button.x * scale,
                                            event.button.y * scale, event.button.button,
                                            0.0f, 0.0f, 0, {}, 0, 0, window});
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                // Deltas travel in notches (SDL's float x/y); the engine turns them
                // into pixels when it scrolls. The cursor position rides along so the
                // wheel lands on the element it points at.
                input_.push_back(InputEvent{InputKind::Wheel, event.wheel.mouse_x * scale,
                                            event.wheel.mouse_y * scale, 0, event.wheel.x,
                                            event.wheel.y, 0, {}, 0, 0, window});
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                // Nothing is hovered once the pointer is out of the window, and the
                // engine is never told about a motion again until it comes back. The
                // coordinate is off-screen so the hit test finds no target and
                // `:hover` clears, which is what a browser does on the same event.
                input_.push_back(InputEvent{InputKind::PointerMove, -1.0f, -1.0f, 0, 0.0f,
                                            0.0f, 0, {}, 0, 0, window});
                break;
            case SDL_EVENT_KEY_DOWN: {
                InputEvent ev{InputKind::KeyDown};
                ev.key = static_cast<int>(event.key.key);
                ev.window = window;
                input_.push_back(std::move(ev));
                break;
            }
            case SDL_EVENT_KEY_UP: {
                InputEvent ev{InputKind::KeyUp};
                ev.key = static_cast<int>(event.key.key);
                ev.window = window;
                input_.push_back(std::move(ev));
                break;
            }
            case SDL_EVENT_TEXT_INPUT: {
                InputEvent ev{InputKind::TextInput};
                if (event.text.text) {
                    ev.text = event.text.text;
                }
                ev.window = window;
                input_.push_back(std::move(ev));
                break;
            }
            case SDL_EVENT_TEXT_EDITING: {
                InputEvent ev{InputKind::TextEdit};
                ev.window = window;
                if (event.edit.text) {
                    ev.text = event.edit.text;
                }
                ev.editStart = event.edit.start;
                ev.editLength = event.edit.length;
                input_.push_back(std::move(ev));
                break;
            }
            default:
                break;
        }
    }

    return running_;
}

void LinuxPlatform::setCursor(std::string_view name) {
    if (name == cursorName_) {
        return;
    }
    cursorName_ = std::string(name);

    // `cursor: none` has no shape in SDL's system set; hide the pointer instead.
    if (name == "none") {
        SDL_HideCursor();
        return;
    }
    SDL_ShowCursor();

    const SDL_SystemCursor id = systemCursorFor(name);
    const std::size_t index = static_cast<std::size_t>(id);
    if (index >= cursors_.size()) {
        cursors_.resize(index + 1, nullptr);
    }
    if (!cursors_[index]) {
        cursors_[index] = SDL_CreateSystemCursor(id);
    }
    // A null result means the toolkit could not build that shape (a headless video
    // driver, say); leaving the previous cursor in place is better than clearing it.
    if (cursors_[index]) {
        SDL_SetCursor(cursors_[index]);
    }
}

}  // namespace sui
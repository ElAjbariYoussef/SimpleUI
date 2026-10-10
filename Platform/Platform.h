// Platform abstraction over the windowing/video calls the engine needs. Keeps
// SDL details out of Core so Windows/Linux/macOS differences stay contained.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;

namespace sui {

// Pointer and keyboard input, in window coordinates, forwarded to the engine as
// SDL-free structs so the DOM/event code stays platform independent.
enum class InputKind : std::uint8_t {
    PointerDown,
    PointerUp,
    PointerMove,
    Wheel,
    KeyDown,
    KeyUp,
    TextInput,
    TextEdit
};

struct InputEvent {
    InputKind kind = InputKind::PointerDown;
    float x = 0.0f;
    float y = 0.0f;
    int button = 0;  // 1 = left, 2 = middle, 3 = right
    float dx = 0.0f;  // wheel delta per axis, in notches; only filled for Wheel
    float dy = 0.0f;
    // Platform key code (SDL's keycode), filled for the key events. `text` holds
    // the UTF-8 produced by a key press, which is what a text field inserts.
    int key = 0;
    std::string text;
    int editStart = 0;
    int editLength = 0;

    InputEvent() = default;
    InputEvent(InputKind k, float px = 0.0f, float py = 0.0f, int b = 0, float ddx = 0.0f,
               float ddy = 0.0f, int kk = 0, std::string t = {}, int start = 0, int len = 0)
        : kind(k), x(px), y(py), button(b), dx(ddx), dy(ddy), key(kk), text(std::move(t)),
          editStart(start), editLength(len) {}
};

struct WindowConfig {
    std::string title = "SimpleUI";
    int width = 1280;
    int height = 720;
    bool maximized = false;
    bool resizable = true;
    bool borderless = false;
    bool hidden = false;
};

class Platform {
public:
    Platform() = default;
    virtual ~Platform() = default;

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    virtual bool initialize(const WindowConfig& config, std::string& error) = 0;
    virtual void shutdown() = 0;

    virtual SDL_Window* window() const = 0;
    virtual SDL_Renderer* renderer() const = 0;

    virtual void clear() = 0;
    virtual void present() = 0;

    // The drawable size in *pixels*, not in window points, because every coordinate
    // the engine works in -- layout, hit tests, glyph bitmaps -- is a pixel. On a
    // display where the two differ the pointer events below are converted to match.
    virtual void windowSize(int& width, int& height) const = 0;

    // Tells the renderer to map its coordinates onto a drawable this size. Without it
    // the logical canvas stays whatever it was created at, so a resize only ever
    // letterboxes the old frame instead of relaying the page out to the new size.
    virtual void setViewportSize(int width, int height) = 0;

    // Writes the current backbuffer to a BMP file; used by the headless
    // screenshot mode for verifying layout without a visible window.
    virtual bool saveScreenshot(const std::string& path) const;

    // Drains the event queue; returns false once the app should quit.
    virtual bool pumpEvents() = 0;

    // Pointer events collected by the last pumpEvents(), in arrival order.
    virtual const std::vector<InputEvent>& inputEvents() const = 0;
};

// Factory for the host platform; compiled from Platform/<os>/.
std::unique_ptr<Platform> makePlatform();

}  // namespace sui
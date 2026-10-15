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
struct SDL_Cursor;

namespace sui {

// Identifies one open window, for as long as it stays open. `kInvalidWindow` is
// "no window", which is what an event the engine cannot attribute to a window of
// its own carries -- and what the engine reads as "the primary window".
using WindowId = std::uint32_t;
inline constexpr WindowId kInvalidWindow = 0;

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
    // Which window the event arrived on. Appended last and defaulted, so every
    // existing construction site keeps compiling; `kInvalidWindow` reads as the
    // primary window, which is what a single-window app means by it.
    WindowId window = kInvalidWindow;

    InputEvent() = default;
    InputEvent(InputKind k, float px = 0.0f, float py = 0.0f, int b = 0, float ddx = 0.0f,
               float ddy = 0.0f, int kk = 0, std::string t = {}, int start = 0, int len = 0,
               WindowId w = kInvalidWindow)
        : kind(k), x(px), y(py), button(b), dx(ddx), dy(ddy), key(kk), text(std::move(t)),
          editStart(start), editLength(len), window(w) {}
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

    // --- Windows ---
    //
    // An app opens one window per page it shows side by side, so a window is
    // created and destroyed independently of any other. Ids stay stable for a
    // window's lifetime; `kInvalidWindow` means none.
    virtual WindowId openWindow(const WindowConfig& config, std::string& error) = 0;
    virtual void closeWindow(WindowId id) = 0;
    virtual bool hasWindow(WindowId id) const = 0;
    // The window `initialize` created, which is also the one every convenience
    // below means. `kInvalidWindow` once it is closed.
    virtual WindowId primaryWindow() const = 0;
    // Windows closed since the last pumpEvents(), oldest first. The application
    // reads this to drop the page each one was showing.
    virtual const std::vector<WindowId>& closedWindows() const = 0;

    virtual SDL_Window* window(WindowId id) const = 0;
    virtual SDL_Renderer* renderer(WindowId id) const = 0;

    virtual void clear(WindowId id) = 0;
    virtual void present(WindowId id) = 0;

    // The drawable size in *pixels*, not in window points, because every coordinate
    // the engine works in -- layout, hit tests, glyph bitmaps -- is a pixel. On a
    // display where the two differ the pointer events below are converted to match.
    virtual void windowSize(WindowId id, int& width, int& height) const = 0;

    // Tells the renderer to map its coordinates onto a drawable this size. Without it
    // the logical canvas stays whatever it was created at, so a resize only ever
    // letterboxes the old frame instead of relaying the page out to the new size.
    virtual void setViewportSize(WindowId id, int width, int height) = 0;

    // The primary window's conveniences. Every engine call site that has not been
    // threaded with a window id goes through these, which is what keeps the
    // single-window path reading exactly as it did.
    SDL_Window* window() const { return window(primaryWindow()); }
    SDL_Renderer* renderer() const { return renderer(primaryWindow()); }
    void clear() { clear(primaryWindow()); }
    void present() { present(primaryWindow()); }
    void windowSize(int& width, int& height) const {
        windowSize(primaryWindow(), width, height);
    }
    void setViewportSize(int width, int height) {
        setViewportSize(primaryWindow(), width, height);
    }

    // Writes the current backbuffer to a BMP file; used by the headless
    // screenshot mode for verifying layout without a visible window.
    virtual bool saveScreenshot(const std::string& path) const;

    // Drains the event queue; returns false once the app should quit.
    virtual bool pumpEvents() = 0;

    // Sets a window's title bar text. A page swap re-reads it from the new
    // page's <window title>, which is the only place a title comes from.
    virtual void setWindowTitle(WindowId id, std::string_view title) = 0;

    // The primary window's convenience.
    void setWindowTitle(std::string_view title) { setWindowTitle(primaryWindow(), title); }

    // Sets the pointer shape shown while it is over the window. `name` is a CSS
    // cursor keyword ("pointer", "text", "col-resize", ...). A platform folds the
    // values its toolkit has no cursor for onto the nearest one it does; "none"
    // hides the pointer outright.
    virtual void setCursor(std::string_view name) = 0;

    // Pointer events collected by the last pumpEvents(), in arrival order.
    virtual const std::vector<InputEvent>& inputEvents() const = 0;
};

// Factory for the host platform; compiled from Platform/<os>/.
std::unique_ptr<Platform> makePlatform();

}  // namespace sui
#pragma once

#include "Platform/Platform.h"

#include <map>

namespace sui {

class LinuxPlatform final : public Platform {
public:
    bool initialize(const WindowConfig& config, std::string& error) override;
    void shutdown() override;

    WindowId openWindow(const WindowConfig& config, std::string& error) override;
    void closeWindow(WindowId id) override;
    bool hasWindow(WindowId id) const override;
    WindowId primaryWindow() const override;
    std::vector<WindowId> windows() const override;
    const std::vector<WindowId>& closedWindows() const override { return closed_; }

    bool minimizeToTray(WindowId id, std::string& error) override;
    void restoreFromTray(WindowId id) override;
    bool isMinimizedToTray(WindowId id) const override;

    SDL_Window* window(WindowId id) const override;
    SDL_Renderer* renderer(WindowId id) const override;

    void clear(WindowId id) override;
    void present(WindowId id) override;

    void windowSize(WindowId id, int& width, int& height) const override;
    void setViewportSize(WindowId id, int width, int height) override;

    bool pumpEvents() override;
    const std::vector<InputEvent>& inputEvents() const override { return input_; }

    void setWindowTitle(WindowId id, std::string_view title) override;
    void setCursor(std::string_view name) override;

private:
    // One open window: the SDL pair plus the id the engine addresses it by.
    struct Window {
        WindowId id = kInvalidWindow;
        // SDL's own window id, which is what an event carries; `id` is the
        // engine's. Kept side by side so an event can be routed without asking
        // SDL about a pointer we may already have destroyed.
        std::uint32_t sdlId = 0;
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
    };

    // Creates the SDL pair and registers it. `id` is taken as given so the entry
    // window keeps the identity the rest of startup expects.
    bool createWindow(WindowId id, const WindowConfig& config, std::string& error);
    void destroyWindow(Window& window);

    // One window's tray icon. The node's address is the click callback's
    // userdata, which is stable for as long as the icon lives (std::map keeps
    // nodes in place across insertions and erasures of other nodes).
    struct Tray {
        LinuxPlatform* platform = nullptr;
        SDL_Tray* tray = nullptr;
        WindowId window = kInvalidWindow;
    };
    // Click callbacks can run off the main thread, so they only post a custom
    // event; pumpEvents does the window work on the main thread. Linux's
    // SDLCALL is empty, so the plain signature matches SDL_TrayClickCallback.
    static bool trayClicked(void* userdata, SDL_Tray* tray);
    Window* find(WindowId id);
    const Window* find(WindowId id) const;
    // The engine id for an SDL window id, or `kInvalidWindow` for one the engine
    // does not own.
    WindowId engineIdFor(std::uint32_t sdlId) const;
    // Framebuffer pixels per window point, which is 1 everywhere except a display
    // with a scale factor.
    float pointerScale(WindowId id) const;

    std::vector<Window> windows_;
    std::vector<WindowId> closed_;
    // Ids are never reused, so an event for a window that just closed cannot be
    // attributed to whatever opened in its place.
    WindowId nextId_ = 1;
    bool running_ = true;
    std::vector<InputEvent> input_;
    // One lazily created cursor per SDL system-cursor id, indexed by that id, so a
    // hover that flips between two shapes does not create a cursor per motion.
    std::vector<SDL_Cursor*> cursors_;
    // The keyword already in effect, kept so an unchanged hover does not re-issue
    // SDL_SetCursor on every motion event.
    std::string cursorName_;
    // Tray icons by window, the shared lazily built glyph every icon uses, and
    // the custom SDL event types the menu callbacks post. 0 means "not
    // registered" (the platform failed to reserve them).
    std::map<WindowId, Tray> trays_;
    SDL_Surface* trayIcon_ = nullptr;
    std::uint32_t trayRestoreEvent_ = 0;
};

}  // namespace sui
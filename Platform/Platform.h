// Platform abstraction over the windowing/video calls the engine needs. Keeps
// SDL details out of Core so Windows/Linux/macOS differences stay contained.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct SDL_Window;
struct SDL_Renderer;

namespace sui {

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

    virtual void windowSize(int& width, int& height) const = 0;

    // Drains the event queue; returns false once the app should quit.
    virtual bool pumpEvents() = 0;
};

// Factory for the host platform; compiled from Platform/<os>/.
std::unique_ptr<Platform> makePlatform();

}  // namespace sui
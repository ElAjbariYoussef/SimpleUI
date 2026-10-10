#pragma once

#include "Platform/Platform.h"

namespace sui {

class LinuxPlatform final : public Platform {
public:
    bool initialize(const WindowConfig& config, std::string& error) override;
    void shutdown() override;

    SDL_Window* window() const override { return window_; }
    SDL_Renderer* renderer() const override { return renderer_; }

    void clear() override;
    void present() override;

    void windowSize(int& width, int& height) const override;
    void setViewportSize(int width, int height) override;

    bool pumpEvents() override;
    const std::vector<InputEvent>& inputEvents() const override { return input_; }

    void setCursor(std::string_view name) override;

private:
    // Framebuffer pixels per window point, which is 1 everywhere except a display
    // with a scale factor.
    float pointerScale() const;

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool running_ = true;
    std::vector<InputEvent> input_;
    // One lazily created cursor per SDL system-cursor id, indexed by that id, so a
    // hover that flips between two shapes does not create a cursor per motion.
    std::vector<SDL_Cursor*> cursors_;
    // The keyword already in effect, kept so an unchanged hover does not re-issue
    // SDL_SetCursor on every motion event.
    std::string cursorName_;
};

}  // namespace sui
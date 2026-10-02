// Draw API the layout/paint stages emit into. Backends sit behind this so the
// engine is not coupled to SDL draw calls directly.
#pragma once

#include <SDL3/SDL_rect.h>

#include <cstdint>

struct SDL_Renderer;

namespace sui {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

class Renderer {
public:
    explicit Renderer(SDL_Renderer* renderer) : renderer_(renderer) {}

    void fillRect(const SDL_FRect& rect, const Color& color);
    void strokeRect(const SDL_FRect& rect, const Color& color, float thickness = 1.0f);

private:
    SDL_Renderer* renderer_ = nullptr;
};

}  // namespace sui
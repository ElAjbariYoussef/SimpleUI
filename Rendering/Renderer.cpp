#include "Rendering/Renderer.h"

#include <SDL3/SDL.h>

namespace sui {

void Renderer::fillRect(const SDL_FRect& rect, const Color& color) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer_, &rect);
}

void Renderer::strokeRect(const SDL_FRect& rect, const Color& color,
                          float thickness) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);

    const SDL_FRect top{rect.x, rect.y, rect.w, thickness};
    const SDL_FRect bottom{rect.x, rect.y + rect.h - thickness, rect.w, thickness};
    const SDL_FRect left{rect.x, rect.y, thickness, rect.h};
    const SDL_FRect right{rect.x + rect.w - thickness, rect.y, thickness, rect.h};

    SDL_RenderFillRect(renderer_, &top);
    SDL_RenderFillRect(renderer_, &bottom);
    SDL_RenderFillRect(renderer_, &left);
    SDL_RenderFillRect(renderer_, &right);
}

}  // namespace sui
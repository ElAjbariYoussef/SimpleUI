#include "Platform/Platform.h"

#include "Platform/Linux/LinuxPlatform.h"

#include <SDL3/SDL.h>

namespace sui {

// Only Linux exists today; the header-per-OS layout is what Platform/Windows and
// Platform/macOS will follow.
std::unique_ptr<Platform> makePlatform() {
    return std::make_unique<LinuxPlatform>();
}

bool Platform::saveScreenshot(const std::string& path) const {
    int width = 0;
    int height = 0;
    windowSize(width, height);
    if (width <= 0 || height <= 0) {
        return false;
    }
    SDL_Surface* surface = SDL_RenderReadPixels(renderer(), nullptr);
    if (!surface) {
        return false;
    }
    const bool ok = SDL_SaveBMP(surface, path.c_str());
    SDL_DestroySurface(surface);
    return ok;
}

}  // namespace sui

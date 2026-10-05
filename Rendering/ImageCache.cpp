#include "Rendering/ImageCache.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_surface.h>

#include <cstdint>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#include "stb/stb_image.h"

namespace sui {
namespace {

// Decoded pixels arrive as RGBA whatever the file's own format was, which is the only
// layout `SDL_CreateTexture` below asks for.
struct Decoded {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

bool decode(const std::string& bytes, Decoded& out) {
    int w = 0, h = 0, channels = 0;
    std::uint8_t* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h,
        &channels, 4);
    if (!pixels) {
        return false;
    }
    out.width = w;
    out.height = h;
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    out.rgba.assign(pixels, pixels + count);
    stbi_image_free(pixels);
    return w > 0 && h > 0;
}

}  // namespace

ImageCache::~ImageCache() {
    clear();
}

void ImageCache::clear() {
    for (auto& [href, texture] : textures_) {
        SDL_DestroyTexture(texture);
    }
    textures_.clear();
    failed_.clear();
}

SDL_Texture* ImageCache::texture(SDL_Renderer* renderer, std::string_view href) {
    const std::string key(href);
    if (const auto found = textures_.find(key); found != textures_.end()) {
        return found->second;
    }
    if (failed_.count(key) != 0) {
        return nullptr;
    }

    Decoded image;
    const auto bytes = resources_->readText(key);
    if (!bytes || !decode(*bytes, image)) {
        // The decoder leaves a reason on its own error state, which is more useful than
        // a bare "no image"; a missing file is reported as missing rather than corrupt.
        if (bytes) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "cannot decode image %s: %s", key.c_str(),
                        stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        }
        failed_[key] = true;
        return nullptr;
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        image.width, image.height, SDL_PIXELFORMAT_RGBA32, image.rgba.data(),
        image.width * 4);
    if (!surface) {
        failed_[key] = true;
        return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (!texture) {
        failed_[key] = true;
        return nullptr;
    }
    // Backgrounds are almost always scaled to something other than their own size, and
    // a smooth ramp or a photo is better sampled than blocked, unlike the glyph
    // textures which are drawn 1:1 and want the opposite.
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    textures_[key] = texture;
    return texture;
}

}  // namespace sui

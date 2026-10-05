// Decoded image textures, keyed by the href they came from.
//
// A stylesheet may name the same image from several rules and an image is almost
// never small, so decoding is done once and the texture shared. The cache owns the
// textures and hands out borrowed pointers, which keeps every decoded pixel in one
// place for SDL to release.
#pragma once

#include <SDL3/SDL_render.h>

#include <string>
#include <string_view>
#include <unordered_map>

#include "Core/Resources/Resources.h"

namespace sui {

class ImageCache {
public:
    explicit ImageCache(Resources& resources) : resources_(&resources) {}
    ~ImageCache();

    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // Decodes and uploads on first use, then returns the same texture. A path that
    // fails to load caches its failure too, so a broken `url()` costs one attempt
    // rather than one per painted element per frame.
    SDL_Texture* texture(SDL_Renderer* renderer, std::string_view href);

    void clear();

private:
    Resources* resources_;
    std::unordered_map<std::string, SDL_Texture*> textures_;
    std::unordered_map<std::string, bool> failed_;
};

}  // namespace sui

// Decoded image textures, keyed by the href they came from.
//
// A stylesheet may name the same image from several rules and an image is almost
// never small, so decoding is done once and the texture shared. The cache owns the
// textures and hands out borrowed pointers, which keeps every decoded pixel in one
// place for SDL to release.
#pragma once

#include <SDL3/SDL_render.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Core/Resources/Resources.h"

namespace sui {

// Decoded pixels arrive as RGBA whatever the file's own format was, which is the only
// layout textures ask for — and the only layout CPU mask sampling asks for.
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

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

    // The decoded bytes behind `texture()`, retained for CPU sampling (mask
    // images). Shares the same failure cache; the pointer stays valid while
    // the entry lives, which is the cache's own lifetime.
    const DecodedImage* decoded(std::string_view href);

    void clear();

private:
    Resources* resources_;
    std::unordered_map<std::string, SDL_Texture*> textures_;
    std::map<std::string, DecodedImage> decoded_;
    std::unordered_map<std::string, bool> failed_;
};

}  // namespace sui

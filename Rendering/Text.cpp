#define STB_TRUETYPE_IMPLEMENTATION
#include "Rendering/Text.h"

#include <stb/stb_truetype.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace sui {
namespace {

int pxFromFloat(float size) {
    return static_cast<int>(std::lround(size));
}

}  // namespace

TextFont::TextFont() = default;

TextFont::~TextFont() {
    if (font_) {
        delete static_cast<stbtt_fontinfo*>(font_);
        font_ = nullptr;
    }
    delete[] fontData_;
    fontData_ = nullptr;
    fontDataSize_ = 0;
}

bool TextFont::loadFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    auto data = std::make_unique<unsigned char[]>(size);
    if (!in.read(reinterpret_cast<char*>(data.get()),
                 static_cast<std::streamsize>(size))) {
        return false;
    }
    return loadFromMemory(data.release(), static_cast<int>(size));
}

bool TextFont::loadFromMemory(const unsigned char* data, int size) {
    auto info = new stbtt_fontinfo();
    if (!stbtt_InitFont(info, data, 0)) {
        delete info;
        delete[] data;
        return false;
    }
    if (font_) {
        delete static_cast<stbtt_fontinfo*>(font_);
    }
    delete[] fontData_;
    font_ = info;
    fontData_ = const_cast<unsigned char*>(data);
    fontDataSize_ = static_cast<std::size_t>(size);
    cache_.clear();
    return true;
}

GlyphMetrics TextFont::metrics(unsigned int codepoint, float size) const {
    GlyphMetrics gm;
    if (!font_ || size <= 0.0f) {
        return gm;
    }
    const int sizePx = pxFromFloat(size);
    (void)sizePx;
    const FontSizeKey key{codepoint, pxFromFloat(size)};
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }

    auto* info = static_cast<stbtt_fontinfo*>(font_);
    int advance = 0;
    int lsb = 0;
    stbtt_GetCodepointHMetrics(info, static_cast<int>(codepoint), &advance, &lsb);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(info, static_cast<int>(codepoint),
                                stbtt_ScaleForPixelHeight(info, size),
                                stbtt_ScaleForPixelHeight(info, size), &x0, &y0,
                                &x1, &y1);
    const float scale = stbtt_ScaleForPixelHeight(info, size);
    gm.advance = static_cast<float>(advance) * scale;
    gm.width = static_cast<float>(x1 - x0);
    gm.height = static_cast<float>(y1 - y0);
    gm.bearingX = static_cast<float>(lsb) * scale;
    gm.bearingY = static_cast<float>(-y1) * scale;  // approximate
    gm.x0 = x0;
    gm.y0 = y0;
    gm.x1 = x1;
    gm.y1 = y1;
    cache_.emplace(key, gm);
    return gm;
}

SDL_Texture* TextFont::renderText(SDL_Renderer* renderer, std::string_view text,
                                  float size, SDL_Color color) {
    if (!font_ || !renderer || text.empty() || size <= 0.0f) {
        return nullptr;
    }
    const float scale = stbtt_ScaleForPixelHeight(
        static_cast<stbtt_fontinfo*>(font_), size);
    const int sizePx = pxFromFloat(size);

    // Measure
    float totalW = 0.0f;
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(static_cast<stbtt_fontinfo*>(font_), &ascent, &descent,
                          &lineGap);
    const float fontH = static_cast<float>(ascent - descent + lineGap) * scale;
    for (char c : text) {
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(static_cast<stbtt_fontinfo*>(font_),
                                   static_cast<unsigned char>(c), &advance, &lsb);
        totalW += static_cast<float>(advance) * scale;
        if (text.find(c) == std::string_view::npos) {
            // no kerning needed for simple cases
        }
    }
    const int w = static_cast<int>(std::ceil(totalW));
    const int h = static_cast<int>(std::ceil(fontH));
    if (w <= 0 || h <= 0) {
        return nullptr;
    }

    // Create surface
    SDL_Surface* surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        return nullptr;
    }
    // Clear to transparent
    const SDL_PixelFormatDetails* pf = SDL_GetPixelFormatDetails(surf->format);
    SDL_FillSurfaceRect(surf, nullptr, SDL_MapRGBA(pf, nullptr, 0, 0, 0, 0));

    float x = 0.0f;
    for (char c : text) {
        unsigned int cp = static_cast<unsigned char>(c);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(static_cast<stbtt_fontinfo*>(font_), cp,
                                    scale, scale, &x0, &y0, &x1, &y1);
        int wGlyph = x1 - x0;
        int hGlyph = y1 - y0;
        if (wGlyph > 0 && hGlyph > 0) {
            std::vector<unsigned char> bmp(wGlyph * hGlyph);
            stbtt_MakeCodepointBitmap(static_cast<stbtt_fontinfo*>(font_),
                                      bmp.data(), wGlyph, hGlyph, wGlyph, scale,
                                      scale, cp);
            // Blit grayscale to RGBA
            for (int gy = 0; gy < hGlyph; ++gy) {
                for (int gx = 0; gx < wGlyph; ++gx) {
                    const unsigned char a = bmp[gy * wGlyph + gx];
                    if (a == 0) {
                        continue;
                    }
                    const int px = static_cast<int>(x) + x0 + gx;
                    const int py = static_cast<int>(ascent * scale) + y0 + gy;
                    if (px < 0 || py < 0 || px >= surf->w || py >= surf->h) {
                        continue;
                    }
                    Uint8* p = static_cast<Uint8*>(surf->pixels) +
                               py * surf->pitch + px * 4;
                    p[0] = color.r;
                    p[1] = color.g;
                    p[2] = color.b;
                    p[3] = static_cast<Uint8>((a * color.a) / 255);
                }
            }
        }
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(static_cast<stbtt_fontinfo*>(font_), cp,
                                   &advance, &lsb);
        x += static_cast<float>(advance) * scale;
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_DestroySurface(surf);
    return tex;
}

}  // namespace sui
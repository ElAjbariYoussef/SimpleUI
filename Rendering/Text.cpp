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

// Decodes one UTF-8 sequence, advancing i. Invalid bytes decode as U+FFFD so
// malformed text still renders instead of vanishing.
unsigned int decodeUtf8(std::string_view text, std::size_t& i) {
    const auto byteAt = [&](std::size_t k) {
        return static_cast<unsigned char>(text[k]);
    };
    const unsigned char c = byteAt(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int extra = 0;
    unsigned int cp = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07u;
    } else {
        ++i;
        return 0xFFFDu;
    }
    if (i + static_cast<std::size_t>(extra) >= text.size()) {
        ++i;
        return 0xFFFDu;
    }
    for (int k = 1; k <= extra; ++k) {
        const unsigned char cc = byteAt(i + static_cast<std::size_t>(k));
        if ((cc & 0xC0) != 0x80) {
            ++i;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

// CSS `font-size` names the em square, not the font's ascent-to-descent extent, so the
// scale has to come from the em. `stbtt_ScaleForPixelHeight` would map the full font
// height onto the size instead, which for DejaVu Sans (2384 units of ascent+descent on
// a 2048 em) draws every glyph 16% smaller than the stylesheet asked for.
float emScale(const stbtt_fontinfo* info, float size) {
    return stbtt_ScaleForMappingEmToPixels(info, size);
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
    std::vector<unsigned char> data(size);
    if (!in.read(reinterpret_cast<char*>(data.data()),
                 static_cast<std::streamsize>(size))) {
        return false;
    }
    // loadFromMemory copies, so the buffer can go out of scope here.
    return loadFromMemory(data.data(), static_cast<int>(size));
}

bool TextFont::loadFromMemory(const unsigned char* data, int size) {
    if (!data || size <= 0) {
        return false;
    }
    // stbtt_fontinfo caches pointers into the buffer, so it has to be
    // initialized against the copy we keep alive, not the caller's memory.
    auto copy = std::make_unique<unsigned char[]>(static_cast<std::size_t>(size));
    std::memcpy(copy.get(), data, static_cast<std::size_t>(size));

    auto info = std::make_unique<stbtt_fontinfo>();
    if (!stbtt_InitFont(info.get(), copy.get(), 0)) {
        return false;
    }

    delete static_cast<stbtt_fontinfo*>(font_);
    delete[] fontData_;
    font_ = info.release();
    fontData_ = copy.release();
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
    const float scale = emScale(info, size);
    stbtt_GetCodepointBitmapBox(info, static_cast<int>(codepoint), scale, scale, &x0, &y0,
                                &x1, &y1);
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

TextFont::VerticalMetrics TextFont::verticalMetrics(float size) const {
    VerticalMetrics vm;
    if (!font_ || size <= 0.0f) {
        return vm;
    }
    const float scale = emScale(static_cast<stbtt_fontinfo*>(font_), size);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(static_cast<stbtt_fontinfo*>(font_), &ascent, &descent, &lineGap);
    vm.ascent = static_cast<float>(ascent) * scale;
    vm.descent = static_cast<float>(-descent) * scale;
    vm.lineGap = static_cast<float>(lineGap) * scale;
    vm.height = vm.ascent + vm.descent + vm.lineGap;
    return vm;
}

float TextFont::measureText(std::string_view text, float size) const {
    if (!font_ || size <= 0.0f) {
        return 0.0f;
    }
    float total = 0.0f;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned int cp = decodeUtf8(text, i);
        total += metrics(cp, size).advance;
    }
    return total;
}

SDL_Texture* TextFont::renderText(SDL_Renderer* renderer, std::string_view text,
                                  float size, SDL_Color color) const {
    if (!font_ || !renderer || text.empty() || size <= 0.0f) {
        return nullptr;
    }
    auto* info = static_cast<stbtt_fontinfo*>(font_);
    const float scale = emScale(info, size);
    const VerticalMetrics vm = verticalMetrics(size);
    const int ascentPx = static_cast<int>(std::ceil(vm.ascent));

    const int w = static_cast<int>(std::ceil(measureText(text, size)));
    const int h = static_cast<int>(std::ceil(vm.height));
    if (w <= 0 || h <= 0) {
        return nullptr;
    }

    SDL_Surface* surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        return nullptr;
    }
    const SDL_PixelFormatDetails* pf = SDL_GetPixelFormatDetails(surf->format);
    SDL_FillSurfaceRect(surf, nullptr, SDL_MapRGBA(pf, nullptr, 0, 0, 0, 0));

    auto* pixels = static_cast<Uint8*>(surf->pixels);
    float penX = 0.0f;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned int cp = decodeUtf8(text, i);
        const GlyphMetrics gm = metrics(cp, size);
        const int x0 = gm.x0;
        const int y0 = gm.y0;
        const int gw = gm.x1 - x0;
        const int gh = gm.y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> bmp(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh));
            stbtt_MakeCodepointBitmap(info, bmp.data(), gw, gh, gw, scale, scale,
                                      static_cast<int>(cp));
            for (int gy = 0; gy < gh; ++gy) {
                for (int gx = 0; gx < gw; ++gx) {
                    const unsigned char a = bmp[static_cast<std::size_t>(gy) * gw + gx];
                    if (a == 0) {
                        continue;
                    }
                    const int px = static_cast<int>(std::floor(penX)) + x0 + gx;
                    const int py = ascentPx + y0 + gy;
                    if (px < 0 || py < 0 || px >= w || py >= h) {
                        continue;
                    }
                    Uint8* p = pixels + py * surf->pitch + px * 4;
                    p[0] = color.r;
                    p[1] = color.g;
                    p[2] = color.b;
                    p[3] = static_cast<Uint8>((a * color.a) / 255);
                }
            }
        }
        penX += gm.advance;
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    // The bitmap is already at its final pixel size, so it must be sampled 1:1.
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface(surf);
    return tex;
}

}  // namespace sui
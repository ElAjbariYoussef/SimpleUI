#define STB_TRUETYPE_IMPLEMENTATION
#include "Rendering/Text.h"

#include <stb/stb_truetype.h>

#include <hb-ot.h>
#include <hb.h>

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
    clearHarfBuzz();
    if (font_) {
        delete static_cast<stbtt_fontinfo*>(font_);
        font_ = nullptr;
    }
    delete[] fontData_;
    fontData_ = nullptr;
    fontDataSize_ = 0;
}

void TextFont::clearHarfBuzz() {
    if (hbFont_) {
        hb_font_destroy(static_cast<hb_font_t*>(hbFont_));
        hbFont_ = nullptr;
    }
    if (hbFace_) {
        hb_face_destroy(static_cast<hb_face_t*>(hbFace_));
        hbFace_ = nullptr;
    }
    if (hbBlob_) {
        hb_blob_destroy(static_cast<hb_blob_t*>(hbBlob_));
        hbBlob_ = nullptr;
    }
}

// The shaper reads the same bytes stb_truetype rasterizes: the blob borrows
// our copy (kept alive as long), the face parses its OpenType tables, and the
// OpenType metric funcs answer from those tables, so shaping needs no
// FreeType and can never disagree with stb about a glyph id.
void TextFont::initHarfBuzz() {
    clearHarfBuzz();
    if (!fontData_ || fontDataSize_ == 0) {
        return;
    }
    hb_blob_t* blob = hb_blob_create(
        reinterpret_cast<const char*>(fontData_), static_cast<unsigned int>(fontDataSize_),
        HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* face = hb_face_create(blob, 0);
    hb_font_t* font = hb_font_create(face);
    hb_ot_font_set_funcs(font);
    hbBlob_ = blob;
    hbFace_ = face;
    hbFont_ = font;
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
    initHarfBuzz();
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

float TextFont::measureText(std::string_view text, float size, bool kern,
                          float stretch) const {
    if (!font_ || size <= 0.0f) {
        return 0.0f;
    }
    if (hbFont_) {
        float total = 0.0f;
        for (const ShapedGlyph& glyph : shapeText(text, size, kern, stretch)) {
            total += glyph.advance;
        }
        return total;
    }
    float total = 0.0f;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned int cp = decodeUtf8(text, i);
        total += metrics(cp, size).advance;
    }
    return total;
}

bool TextFont::needsShaping(std::string_view text) {
    for (unsigned char c : text) {
        if (c >= 0x80) {
            return true;
        }
    }
    return false;
}

bool TextFont::glyphBitmapBox(unsigned int glyphId, float size, float stretch, int& x0,
                              int& y0, int& width, int& height) const {
    x0 = 0;
    y0 = 0;
    width = 0;
    height = 0;
    if (!font_ || size <= 0.0f) {
        return false;
    }
    auto* info = static_cast<stbtt_fontinfo*>(font_);
    const float scale = emScale(info, size);
    int x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(info, static_cast<int>(glyphId), scale * stretch, scale, &x0, &y0,
                            &x1, &y1);
    width = x1 - x0;
    height = y1 - y0;
    return width > 0 && height > 0;
}

std::size_t TextFont::glyphCount(std::string_view text, bool kern,
                              float stretch) const {
    if (hbFont_) {
        return shapeText(text, 16.0f, kern, stretch).size();
    }
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::vector<ShapedGlyph> TextFont::shapeText(std::string_view text, float size,
                                              bool kern, float stretch) const {
    std::vector<ShapedGlyph> out;
    if (!hbFont_ || text.empty() || size <= 0.0f) {
        return out;
    }
    hb_font_t* font = static_cast<hb_font_t*>(hbFont_);
    // 26.6 fixed point: advances come back in px after one divide.
    // `font-stretch` is a horizontal scale of the font itself, so advances and
    // offsets widen or narrow with the glyphs rather than just spacing them.
    const int scaled = static_cast<int>(size * 64.0f);
    hb_font_set_scale(font, static_cast<int>(size * 64.0f * stretch), scaled);
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0,
                       static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buffer);
    if (kern) {
        hb_shape(font, buffer, nullptr, 0);
    } else {
        hb_feature_t noKern;
        hb_feature_from_string("kern=0", -1, &noKern);
        hb_shape(font, buffer, &noKern, 1);
    }
    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    for (unsigned int i = 0; i < count; ++i) {
        ShapedGlyph glyph;
        glyph.glyph = infos[i].codepoint;
        glyph.cluster = infos[i].cluster;
        glyph.advance = static_cast<float>(positions[i].x_advance) / 64.0f;
        glyph.xOffset = static_cast<float>(positions[i].x_offset) / 64.0f;
        glyph.yOffset = static_cast<float>(positions[i].y_offset) / 64.0f;
        out.push_back(glyph);
    }
    hb_buffer_destroy(buffer);
    return out;
}

SDL_Texture* TextFont::renderText(SDL_Renderer* renderer, std::string_view text,
                                  float size, SDL_Color color, bool bold,
                                  bool italic, bool kern, float stretch) const {
    if (!font_ || !renderer || text.empty() || size <= 0.0f) {
        return nullptr;
    }
    auto* info = static_cast<stbtt_fontinfo*>(font_);
    const float scale = emScale(info, size);
    const VerticalMetrics vm = verticalMetrics(size);
    const int ascentPx = static_cast<int>(std::ceil(vm.ascent));

    // A synthetic italic is a shear about the baseline: a glyph pixel `d` above the
    // baseline moves right by `slant * d`. That leans tops right and descenders
    // left, so the surface needs padding on both sides to keep them. Bold is a
    // one-pixel copy smeared to the right.
    constexpr float kItalicSlant = 0.21f;
    const int italicLeft = italic ? static_cast<int>(std::ceil(kItalicSlant * vm.descent)) : 0;
    const int italicRight = italic ? static_cast<int>(std::ceil(kItalicSlant * vm.ascent)) : 0;
    const int boldRight = bold ? 1 : 0;

    const int h = static_cast<int>(std::ceil(vm.height));
    const int w = static_cast<int>(std::ceil(measureText(text, size, kern, stretch))) +
                  italicLeft +
                  italicRight + boldRight;
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
    // Coverage is accumulated as the maximum alpha so overlapping strokes of the
    // same glyph (bold's smear) do not darken or erase one another.
    auto put = [&](int px, int py, unsigned char coverage) {
        if (px < 0 || py < 0 || px >= w || py >= h) {
            return;
        }
        const unsigned char aa = static_cast<unsigned char>((coverage * color.a) / 255);
        Uint8* p = pixels + py * surf->pitch + px * 4;
        if (aa > p[3]) {
            p[0] = color.r;
            p[1] = color.g;
            p[2] = color.b;
            p[3] = aa;
        }
    };

    float penX = 0.0f;
    const bool shaped = hbFont_ != nullptr;
    const std::vector<ShapedGlyph> glyphs =
        shaped ? shapeText(text, size, kern, stretch) : std::vector<ShapedGlyph>{};
    if (shaped && !glyphs.empty()) {
        // Shaped blit: glyph ids with advances and mark offsets from the
        // shaper, rasterized by stb from the same font bytes. stb takes the
        // axes apart, so a stretched run widens its glyphs to match the
        // stretched advances the shaper handed back.
        const float scaleX = scale * stretch;
        for (const ShapedGlyph& glyph : glyphs) {
            int x0 = 0, y0 = 0, gw = 0, gh = 0;
            if (glyphBitmapBox(glyph.glyph, size, stretch, x0, y0, gw, gh)) {
                std::vector<unsigned char> bmp(static_cast<std::size_t>(gw) *
                                               static_cast<std::size_t>(gh));
                stbtt_MakeGlyphBitmap(info, bmp.data(), gw, gh, gw, scaleX, scale,
                                       static_cast<int>(glyph.glyph));
                for (int gy = 0; gy < gh; ++gy) {
                    const int destY = ascentPx + y0 + gy -
                                      static_cast<int>(std::lround(glyph.yOffset));
                    const float above = static_cast<float>(-(y0 + gy));
                    const int shear =
                        italic ? static_cast<int>(std::lround(kItalicSlant * above)) : 0;
                    for (int gx = 0; gx < gw; ++gx) {
                        const unsigned char a = bmp[static_cast<std::size_t>(gy) * gw + gx];
                        if (a == 0) {
                            continue;
                        }
                        const int px = italicLeft +
                                       static_cast<int>(std::floor(penX + glyph.xOffset)) +
                                       x0 + gx + shear;
                        put(px, destY, a);
                        if (bold) {
                            put(px + 1, destY, a);
                        }
                    }
                }
            }
            penX += glyph.advance;
        }
    } else {
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
                const int destY = ascentPx + y0 + gy;
                // Distance above the baseline; used by the italic shear.
                const float above = static_cast<float>(-(y0 + gy));
                const int shear = italic ? static_cast<int>(std::lround(kItalicSlant * above)) : 0;
                for (int gx = 0; gx < gw; ++gx) {
                    const unsigned char a = bmp[static_cast<std::size_t>(gy) * gw + gx];
                    if (a == 0) {
                        continue;
                    }
                    const int px = italicLeft + static_cast<int>(std::floor(penX)) + x0 + gx + shear;
                    put(px, destY, a);
                    if (bold) {
                        put(px + 1, destY, a);
                    }
                }
            }
        }
        penX += gm.advance;
        }
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    // The bitmap is already at its final pixel size, so it must be sampled 1:1.
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface(surf);
    return tex;
}

}  // namespace sui
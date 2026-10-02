#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sui {

// Cached glyph metrics for a codepoint at a given font size. stb_truetype gives
// per-glyph advance; we track horizontal advance and vertical metrics.
struct GlyphMetrics {
    float advance = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float bearingX = 0.0f;
    float bearingY = 0.0f;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

class TextFont {
public:
    TextFont();
    ~TextFont();

    TextFont(const TextFont&) = delete;
    TextFont& operator=(const TextFont&) = delete;

    // Loads a TTF from memory or file. For now, we try to load from a bundled
    // default; returns false if no font is available (rendering will fallback).
    bool loadFromFile(const std::string& path);
    bool loadFromMemory(const unsigned char* data, int size);

    // Returns glyph metrics for codepoint at size (px). Values are in pixels.
    GlyphMetrics metrics(unsigned int codepoint, float size) const;

    // Renders a string to a texture for a given color and size. Caller owns the
    // returned texture (SDL_DestroyTexture).
    SDL_Texture* renderText(SDL_Renderer* renderer, std::string_view text,
                            float size, SDL_Color color);

    bool hasFont() const noexcept { return font_ != nullptr; }

private:
    struct FontSizeKey {
        unsigned int cp;
        int sizePx;  // integer px for caching
        bool operator==(const FontSizeKey& o) const noexcept {
            return cp == o.cp && sizePx == o.sizePx;
        }
    };

    struct FontSizeKeyHash {
        std::size_t operator()(const FontSizeKey& k) const noexcept {
            // simple combine
            return (std::size_t(k.cp) * 131542391ULL) ^ std::size_t(k.sizePx);
        }
    };

    void* font_ = nullptr;  // stbtt_fontinfo*
    unsigned char* fontData_ = nullptr;
    std::size_t fontDataSize_ = 0;
    mutable std::unordered_map<FontSizeKey, GlyphMetrics, FontSizeKeyHash> cache_;
};

}  // namespace sui
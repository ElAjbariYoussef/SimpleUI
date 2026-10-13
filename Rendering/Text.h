#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sui {

// One shaped glyph: the font's glyph id, the source byte index it came from,
// and its pen advance plus mark offsets, all in px.
struct ShapedGlyph {
    unsigned int glyph = 0;
    unsigned int cluster = 0;
    float advance = 0.0f;
    float xOffset = 0.0f;
    float yOffset = 0.0f;
};

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

    // Vertical metrics of the font at the given size.
    struct VerticalMetrics {
        float ascent = 0.0f;
        float descent = 0.0f;
        float lineGap = 0.0f;
        float height = 0.0f;  // ascent + descent + lineGap
    };
    VerticalMetrics verticalMetrics(float size) const;

    // Advance width of a UTF-8 string at the given size. Runs needing shaping
    // (anything past ASCII) go through HarfBuzz so ligatures, kerning, and
    // mark positioning measure what renders.
    float measureText(std::string_view text, float size) const;

    // Shapes a run with HarfBuzz: glyph ids with clusters, advances, and mark
    // offsets. Empty when there is no font. Direction stays left-to-right;
    // bidirectional reordering is not shaped yet.
    std::vector<ShapedGlyph> shapeText(std::string_view text, float size) const;

    // Whether a run needs the shaper: anything past ASCII, where ligatures,
    // marks, and non-Latin scripts live.
    static bool needsShaping(std::string_view text);

    // Glyphs in a run: the shaped count when shaping, else codepoints. Feeds
    // letter-spacing, which applies per rendered glyph.
    std::size_t glyphCount(std::string_view text) const;

    // Renders a string to a texture for a given color and size. Caller owns the
    // returned texture (SDL_DestroyTexture). The texture is exactly
    // (measureText(text, size), verticalMetrics(size).height) pixels.
    SDL_Texture* renderText(SDL_Renderer* renderer, std::string_view text, float size,
                            SDL_Color color, bool bold = false, bool italic = false) const;

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
    // HarfBuzz shaping state over the same bytes: blob, face, and font with
    // the OpenType metric funcs, so no FreeType is needed. Null until shaped.
    void* hbBlob_ = nullptr;  // hb_blob_t*
    void* hbFace_ = nullptr;  // hb_face_t*
    void* hbFont_ = nullptr;  // hb_font_t*
    void initHarfBuzz();
    void clearHarfBuzz();
    mutable std::unordered_map<FontSizeKey, GlyphMetrics, FontSizeKeyHash> cache_;
};

}  // namespace sui
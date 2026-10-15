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
    // Set when `font-variant: small-caps` shrank this glyph: it rasterizes an
    // uppercase letter at small-cap size to match its shrunken advance.
    bool small = false;
};

// Synthetic small caps shrink uppercase-substituted glyphs to this of their
// size, which is what the-sized-down capitals of print read as.
inline constexpr float kSmallCapScale = 0.8f;

// A Latin lowercase codepoint's uppercase, or 0 when it has none here. Only
// ASCII and Latin-1 are mapped, so other scripts render unchanged; the length
// rule in the shaper (same byte length or no substitution) keeps clusters
// aligned without a locale.
inline unsigned int smallCapUpper(unsigned int cp) noexcept {
    if (cp >= 'a' && cp <= 'z') {
        return cp - 32u;
    }
    if ((cp >= 0xE0u && cp <= 0xF6u) || (cp >= 0xF8u && cp <= 0xFEu)) {
        return cp - 32u;
    }
    return 0;
}

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
    // mark positioning measure what renders. `kern` follows `font-kerning`.
    float measureText(std::string_view text, float size, bool kern = true,
                   float stretch = 1.0f, bool smallCaps = false) const;

    // Shapes a run with HarfBuzz: glyph ids with clusters, advances, and mark
    // offsets. Empty when there is no font. Direction stays left-to-right;
    // bidirectional reordering is not shaped yet.
    std::vector<ShapedGlyph> shapeText(std::string_view text, float size, bool kern = true,
                                       float stretch = 1.0f,
                                       bool smallCaps = false) const;

    // Whether a run contains complex script content (anything past ASCII),
    // where ligatures, marks, and non-Latin scripts live. Informational: all
    // runs shape when the shaper is ready, ASCII included for kerning.
    static bool needsShaping(std::string_view text);

    // Glyphs in a run: the shaped count when shaping, else codepoints. Feeds
    // letter-spacing, which applies per rendered glyph.
    std::size_t glyphCount(std::string_view text, bool kern = true,
                           float stretch = 1.0f) const;

    // The pixel box one shaped glyph rasterizes to at a stretch: origin inside the
    // em, plus size. stb takes the axes apart, so an expanded run widens its
    // glyphs to match its stretched advances rather than just spreading them.
    // False for a glyph that draws nothing (a space, or no font).
    bool glyphBitmapBox(unsigned int glyphId, float size, float stretch, int& x0, int& y0,
                        int& width, int& height, bool smallCaps = false) const;

    // Renders a string to a texture for a given color and size. Caller owns the
    // returned texture (SDL_DestroyTexture). The texture is exactly
    // (measureText(text, size), verticalMetrics(size).height) pixels.
    SDL_Texture* renderText(SDL_Renderer* renderer, std::string_view text, float size,
                            SDL_Color color, bool bold = false, bool italic = false,
                            bool kern = true, float stretch = 1.0f,
                            bool smallCaps = false) const;

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
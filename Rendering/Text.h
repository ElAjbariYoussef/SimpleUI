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

// Which face a measurement or raster uses. `family` is a CSS `font-family`
// list: each comma-separated family is tried in order (quoted names may hold
// commas), and within the first family that has a loaded face, weight and
// italic pick among its faces following the CSS fallback order. When no
// listed family is loaded, or the list is empty, the default face answers.
struct FontQuery {
    std::string_view family;
    int weight = 400;
    bool italic = false;
};

class TextFont {
public:
    TextFont();
    ~TextFont();

    TextFont(const TextFont&) = delete;
    TextFont& operator=(const TextFont&) = delete;

    // Loads a TTF from memory or file as the default face. For now, we try to
    // load from a bundled default; returns false if no font is available
    // (rendering will fallback).
    bool loadFromFile(const std::string& path);
    bool loadFromMemory(const unsigned char* data, int size);

    // Adds a named face from a file (one @font-face source that resolved and
    // decoded). Faces never move once added, so references into them stay
    // valid across further loads.
    bool addFace(std::string family, const std::string& path, int weight, bool italic);
    // Adds a named face from bytes: @font-face sources arrive through the
    // resource layer, which hands over memory rather than paths.
    bool addFaceFromMemory(std::string family, const unsigned char* data, int size, int weight,
                           bool italic);
    bool hasFamily(std::string_view name) const noexcept;
    // Whether a face with exactly this family, weight, and style is loaded;
    // @font-face loading uses it to skip sources it already added.
    bool hasFace(std::string_view family, int weight, bool italic) const noexcept;
    // The family a query resolves to, or empty for the default face.
    std::string matchedFamily(const FontQuery& query) const;

    // The face a query resolves to after fallback. `family` is the matched
    // @font-face family, empty when the default face answered; weight and
    // italic describe whichever face that is.
    struct FaceMatch {
        std::string family;
        int weight = 400;
        bool italic = false;
    };
    FaceMatch matchFace(const FontQuery& query) const;

    // Returns glyph metrics for codepoint at size (px). Values are in pixels.
    GlyphMetrics metrics(FontQuery query, unsigned int codepoint, float size) const;

    // Vertical metrics of the face at the given size.
    struct VerticalMetrics {
        float ascent = 0.0f;
        float descent = 0.0f;
        float lineGap = 0.0f;
        float height = 0.0f;  // ascent + descent + lineGap
    };
    VerticalMetrics verticalMetrics(FontQuery query, float size) const;

    // Advance width of a UTF-8 string at the given size. Runs needing shaping
    // (anything past ASCII) go through HarfBuzz so ligatures, kerning, and
    // mark positioning measure what renders. `kern` follows `font-kerning`.
    float measureText(FontQuery query, std::string_view text, float size, bool kern = true,
                   float stretch = 1.0f, bool smallCaps = false) const;

    // Shapes a run with HarfBuzz: glyph ids with clusters, advances, and mark
    // offsets. Empty when there is no font. Direction stays left-to-right;
    // bidirectional reordering is not shaped yet.
    std::vector<ShapedGlyph> shapeText(FontQuery query, std::string_view text, float size, bool kern = true,
                                       float stretch = 1.0f,
                                       bool smallCaps = false) const;

    // Whether a run contains complex script content (anything past ASCII),
    // where ligatures, marks, and non-Latin scripts live. Informational: all
    // runs shape when the shaper is ready, ASCII included for kerning.
    static bool needsShaping(std::string_view text);

    // Glyphs in a run: the shaped count when shaping, else codepoints. Feeds
    // letter-spacing, which applies per rendered glyph.
    std::size_t glyphCount(FontQuery query, std::string_view text, bool kern = true,
                           float stretch = 1.0f) const;

    // The pixel box one shaped glyph rasterizes to at a stretch: origin inside the
    // em, plus size. stb takes the axes apart, so an expanded run widens its
    // glyphs to match its stretched advances rather than just spreading them.
    // False for a glyph that draws nothing (a space, or no font).
    bool glyphBitmapBox(FontQuery query, unsigned int glyphId, float size, float stretch, int& x0, int& y0,
                        int& width, int& height, bool smallCaps = false) const;

    // Renders a string to a texture for a given color and size. Caller owns the
    // returned texture (SDL_DestroyTexture). The texture is exactly
    // (measureText(text, size), verticalMetrics(size).height) pixels.
    SDL_Texture* renderText(SDL_Renderer* renderer, FontQuery query, std::string_view text, float size,
                            SDL_Color color, bool bold = false, bool italic = false,
                            bool kern = true, float stretch = 1.0f,
                            bool smallCaps = false) const;

    bool hasFont() const noexcept { return !faces_.empty() && faces_[0]->info != nullptr; }

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

    struct Face {
        std::string family;
        int weight = 400;
        bool italic = false;
        void* info = nullptr;  // stbtt_fontinfo*
        unsigned char* data = nullptr;
        std::size_t dataSize = 0;
        void* hbBlob = nullptr;  // hb_blob_t*
        void* hbFace = nullptr;  // hb_face_t*
        void* hbFont = nullptr;  // hb_font_t*
        mutable std::unordered_map<FontSizeKey, GlyphMetrics, FontSizeKeyHash> cache;
        ~Face();
    };

    static bool sameFamily(std::string_view a, std::string_view b) noexcept;
    const Face& faceFor(const FontQuery& query) const noexcept;
    const Face* bestFaceFor(std::string_view family, const FontQuery& query) const noexcept;
    const Face* resolveFace(const FontQuery& query, std::string* matchedFamily) const noexcept;
    const Face* defaultFace() const noexcept;
    static void initFaceHarfBuzz(Face& face);
    static void clearFaceHarfBuzz(Face& face);
    std::vector<std::unique_ptr<Face>> faces_;
};

}  // namespace sui
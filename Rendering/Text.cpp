#define STB_TRUETYPE_IMPLEMENTATION
#include "Rendering/Text.h"

#include <stb/stb_truetype.h>

#include <hb-ot.h>
#include <hb.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
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
// Encodes one codepoint as UTF-8, returning the byte count.
int encodeUtf8(unsigned int cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

// A small-caps substitution for one decoded codepoint: its uppercase when that
// encodes to the same byte length (so clusters stay aligned), or 0 to leave it.
// Only the mapped Latin ranges substitute; every other script renders unchanged.
bool smallCapSub(unsigned int cp, std::size_t length, unsigned int& out) {
    const unsigned int upper = smallCapUpper(cp);
    if (!upper) {
        return false;
    }
    char encoded[4] = {0, 0, 0, 0};
    if (encodeUtf8(upper, encoded) != static_cast<int>(length)) {
        return false;
    }
    out = upper;
    return true;
}


float emScale(const stbtt_fontinfo* info, float size) {
    return stbtt_ScaleForMappingEmToPixels(info, size);
}

}  // namespace

TextFont::TextFont() = default;

TextFont::~TextFont() = default;

TextFont::Face::~Face() {
    clearFaceHarfBuzz(*this);
    delete static_cast<stbtt_fontinfo*>(info);
    delete[] data;
}

bool TextFont::sameFamily(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char x = static_cast<unsigned char>(a[i]);
        unsigned char y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') {
            x = static_cast<unsigned char>(x + ('a' - 'A'));
        }
        if (y >= 'A' && y <= 'Z') {
            y = static_cast<unsigned char>(y + ('a' - 'A'));
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

namespace {

// The CSS fallback order for weight, as a distance: exact matches are zero,
// and the side the spec searches first outranks the other. Below 400 that is
// lighter faces; above 500, heavier; in the 400..500 window it is faces up to
// 500 first, then lighter, then heavier than 500.
int weightDistance(int wanted, int have) {
    if (have == wanted) {
        return 0;
    }
    if (wanted > 500) {
        return have > wanted ? have - wanted : 1000 + (wanted - have);
    }
    if (wanted < 400) {
        return have < wanted ? wanted - have : 1000 + (have - wanted);
    }
    if (have > wanted && have <= 500) {
        return have - wanted;
    }
    return have < wanted ? 1000 + (wanted - have) : 2000 + (have - wanted);
}

// A CSS family name never keeps its surrounding space or quotes, but quoted
// names may hold spaces and commas that must not split the list.
std::string_view trimFamily(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string_view dequoteFamily(std::string_view name) {
    if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') &&
        name.back() == name.front()) {
        name.remove_prefix(1);
        name.remove_suffix(1);
    }
    return name;
}

std::vector<std::string_view> splitFontFamilies(std::string_view list) {
    std::vector<std::string_view> families;
    std::size_t start = 0;
    char quote = 0;
    bool escaped = false;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const char c = list[i];
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == ',') {
            families.push_back(trimFamily(list.substr(start, i - start)));
            start = i + 1;
        }
    }
    families.push_back(trimFamily(list.substr(start)));
    return families;
}

}  // namespace

const TextFont::Face* TextFont::defaultFace() const noexcept {
    if (faces_.empty() || !faces_[0] || !faces_[0]->info) {
        return nullptr;
    }
    return faces_[0].get();
}

// The best face of one family: style is one flag here (normal or italic), so
// it is only exact and fallback; weight follows the CSS order through
// weightDistance. A tie keeps the face loaded first.
const TextFont::Face* TextFont::bestFaceFor(std::string_view family,
                                            const FontQuery& query) const noexcept {
    const Face* best = nullptr;
    int bestStyle = 0;
    int bestWeight = 0;
    for (const auto& face : faces_) {
        if (!face || !sameFamily(face->family, family)) {
            continue;
        }
        const int style = face->italic == query.italic ? 0 : 1;
        const int weight = weightDistance(query.weight, face->weight);
        if (best == nullptr || style < bestStyle ||
            (style == bestStyle && weight < bestWeight)) {
            best = face.get();
            bestStyle = style;
            bestWeight = weight;
        }
    }
    return best;
}

const TextFont::Face* TextFont::resolveFace(const FontQuery& query,
                                            std::string* matchedFamily) const noexcept {
    const Face* best = nullptr;
    if (!query.family.empty()) {
        for (std::string_view raw : splitFontFamilies(query.family)) {
            const std::string_view family = dequoteFamily(trimFamily(raw));
            if (family.empty()) {
                continue;
            }
            const Face* candidate = bestFaceFor(family, query);
            if (candidate) {
                best = candidate;
                break;
            }
        }
    }
    if (!best) {
        best = defaultFace();
    }
    if (matchedFamily) {
        *matchedFamily = best ? best->family : std::string();
    }
    return best;
}

const TextFont::Face& TextFont::faceFor(const FontQuery& query) const noexcept {
    static const Face empty;
    const Face* face = resolveFace(query, nullptr);
    return face ? *face : empty;
}

bool TextFont::hasFamily(std::string_view name) const noexcept {
    for (const auto& face : faces_) {
        if (face && sameFamily(face->family, name)) {
            return true;
        }
    }
    return false;
}

bool TextFont::hasFace(std::string_view family, int weight, bool italic) const noexcept {
    for (const auto& face : faces_) {
        if (face && face->weight == weight && face->italic == italic &&
            sameFamily(face->family, family)) {
            return true;
        }
    }
    return false;
}

std::string TextFont::matchedFamily(const FontQuery& query) const {
    return matchFace(query).family;
}

TextFont::FaceMatch TextFont::matchFace(const FontQuery& query) const {
    FaceMatch out;
    std::string family;
    const Face* face = resolveFace(query, &family);
    if (!face) {
        return out;
    }
    out.family = std::move(family);
    out.weight = face->weight;
    out.italic = face->italic;
    return out;
}

void TextFont::clearFaceHarfBuzz(Face& face) {
    if (face.hbFont) {
        hb_font_destroy(static_cast<hb_font_t*>(face.hbFont));
        face.hbFont = nullptr;
    }
    if (face.hbFace) {
        hb_face_destroy(static_cast<hb_face_t*>(face.hbFace));
        face.hbFace = nullptr;
    }
    if (face.hbBlob) {
        hb_blob_destroy(static_cast<hb_blob_t*>(face.hbBlob));
        face.hbBlob = nullptr;
    }
}

// The shaper reads the same bytes stb_truetype rasterizes: the blob borrows
// our copy (kept alive as long), the face parses its OpenType tables, and the
// OpenType metric funcs answer from those tables, so shaping needs no
// FreeType and can never disagree with stb about a glyph id.
void TextFont::initFaceHarfBuzz(Face& face) {
    clearFaceHarfBuzz(face);
    if (!face.data || face.dataSize == 0) {
        return;
    }
    hb_blob_t* blob = hb_blob_create(
        reinterpret_cast<const char*>(face.data), static_cast<unsigned int>(face.dataSize),
        HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* hbface = hb_face_create(blob, 0);
    hb_font_t* font = hb_font_create(hbface);
    hb_ot_font_set_funcs(font);
    face.hbBlob = blob;
    face.hbFace = hbface;
    face.hbFont = font;
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

bool TextFont::addFace(std::string family, const std::string& path, int weight, bool italic) {
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
    return addFaceFromMemory(std::move(family), data.data(), static_cast<int>(size), weight,
                             italic);
}

bool TextFont::addFaceFromMemory(std::string family, const unsigned char* data, int size,
                                 int weight, bool italic) {
    if (!data || size <= 0) {
        return false;
    }
    auto copy = std::make_unique<unsigned char[]>(static_cast<std::size_t>(size));
    std::memcpy(copy.get(), data, static_cast<std::size_t>(size));
    auto info = std::make_unique<stbtt_fontinfo>();
    if (!stbtt_InitFont(info.get(), copy.get(), 0)) {
        return false;
    }
    auto face = std::make_unique<Face>();
    face->family = std::move(family);
    face->weight = weight;
    face->italic = italic;
    face->info = info.release();
    face->data = copy.release();
    face->dataSize = static_cast<std::size_t>(size);
    initFaceHarfBuzz(*face);
    faces_.push_back(std::move(face));
    return true;
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

    if (faces_.empty() || !faces_[0]) {
        faces_.push_back(std::make_unique<Face>());
    }
    Face& face = *faces_[0];
    delete static_cast<stbtt_fontinfo*>(face.info);
    delete[] face.data;
    face.info = info.release();
    face.data = copy.release();
    face.dataSize = static_cast<std::size_t>(size);
    face.cache.clear();
    initFaceHarfBuzz(face);
    return true;
}

GlyphMetrics TextFont::metrics(FontQuery query, unsigned int codepoint, float size) const {
    GlyphMetrics gm;
    const Face& face = faceFor(query);
    auto* faceInfo = static_cast<stbtt_fontinfo*>(face.info);
    if (!faceInfo || size <= 0.0f) {
        return gm;
    }
    const int sizePx = pxFromFloat(size);
    (void)sizePx;
    const FontSizeKey key{codepoint, pxFromFloat(size)};
    auto it = face.cache.find(key);
    if (it != face.cache.end()) {
        return it->second;
    }

    auto* info = faceInfo;
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
    face.cache.emplace(key, gm);
    return gm;
}

TextFont::VerticalMetrics TextFont::verticalMetrics(FontQuery query, float size) const {
    VerticalMetrics vm;
    const Face& face = faceFor(query);
    auto* faceInfo = static_cast<stbtt_fontinfo*>(face.info);
    if (!faceInfo || size <= 0.0f) {
        return vm;
    }
    const float scale = emScale(faceInfo, size);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(faceInfo, &ascent, &descent, &lineGap);
    vm.ascent = static_cast<float>(ascent) * scale;
    vm.descent = static_cast<float>(-descent) * scale;
    vm.lineGap = static_cast<float>(lineGap) * scale;
    vm.height = vm.ascent + vm.descent + vm.lineGap;
    return vm;
}

float TextFont::measureText(FontQuery query, std::string_view text, float size, bool kern,
                          float stretch, bool smallCaps) const {
    const Face& face = faceFor(query);
    auto* faceInfo = static_cast<stbtt_fontinfo*>(face.info);
    if (!faceInfo || size <= 0.0f) {
        return 0.0f;
    }
    if (face.hbFont) {
        float total = 0.0f;
        for (const ShapedGlyph& glyph : shapeText(query, text, size, kern, stretch, smallCaps)) {
            total += glyph.advance;
        }
        return total;
    }
    float total = 0.0f;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const unsigned int cp = decodeUtf8(text, i);
        unsigned int shaped = cp;
        float cap = 1.0f;
        if (smallCaps) {
            unsigned int upper = 0;
            if (smallCapSub(cp, i - start, upper)) {
                shaped = upper;
                cap = kSmallCapScale;
            }
        }
        total += metrics(query, shaped, size).advance * cap;
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

bool TextFont::glyphBitmapBox(FontQuery query, unsigned int glyphId, float size, float stretch, int& x0,
                              int& y0, int& width, int& height,
                              bool smallCaps) const {
    x0 = 0;
    y0 = 0;
    width = 0;
    height = 0;
    const Face& face = faceFor(query);
    auto* info = static_cast<stbtt_fontinfo*>(face.info);
    if (!info || size <= 0.0f) {
        return false;
    }
    const float cap = smallCaps ? kSmallCapScale : 1.0f;
    const float scale = emScale(info, size) * cap;
    int x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(info, static_cast<int>(glyphId), scale * stretch, scale, &x0, &y0,
                            &x1, &y1);
    width = x1 - x0;
    height = y1 - y0;
    return width > 0 && height > 0;
}

std::size_t TextFont::glyphCount(FontQuery query, std::string_view text, bool kern,
                              float stretch) const {
    if (faceFor(query).hbFont) {
        return shapeText(query, text, 16.0f, kern, stretch).size();
    }
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::vector<ShapedGlyph> TextFont::shapeText(FontQuery query, std::string_view text, float size,
                                              bool kern, float stretch,
                                              bool smallCaps) const {
    std::vector<ShapedGlyph> out;
    const Face& face = faceFor(query);
    auto* faceHb = static_cast<hb_font_t*>(face.hbFont);
    if (!faceHb || text.empty() || size <= 0.0f) {
        return out;
    }
    // Small caps shape the uppercase: every Latin lowercase becomes its capital
    // at the same byte length (anything that would change length stays as-is),
    // so clusters still index the text the caller holds. The flags mark which
    // bytes were lowered, and their advances shrink to match the smaller caps.
    std::string shaped;
    std::vector<bool> lowered;
    if (smallCaps) {
        shaped.reserve(text.size());
        std::size_t i = 0;
        while (i < text.size()) {
            const std::size_t start = i;
            const unsigned int cp = decodeUtf8(text, i);
            const std::size_t length = i - start;
            unsigned int upper = 0;
            const bool flag = smallCapSub(cp, length, upper);
            if (flag) {
                char encoded[4] = {0, 0, 0, 0};
                const int bytes = encodeUtf8(upper, encoded);
                shaped.append(encoded, static_cast<std::size_t>(bytes));
            } else {
                shaped.append(text.substr(start, length));
            }
            for (std::size_t k = 0; k < length; ++k) {
                lowered.push_back(flag);
            }
        }
        text = shaped;
    }
    hb_font_t* font = faceHb;
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
        if (!lowered.empty() && infos[i].cluster < lowered.size() &&
            lowered[infos[i].cluster]) {
            glyph.small = true;
            glyph.advance *= kSmallCapScale;
            glyph.xOffset *= kSmallCapScale;
        }
        out.push_back(glyph);
    }
    hb_buffer_destroy(buffer);
    return out;
}

SDL_Texture* TextFont::renderText(SDL_Renderer* renderer, FontQuery query, std::string_view text,
                                  float size, SDL_Color color, bool bold,
                                  bool italic, bool kern, float stretch,
                                  bool smallCaps) const {
    const Face& face = faceFor(query);
    auto* info = static_cast<stbtt_fontinfo*>(face.info);
    if (!info || !renderer || text.empty() || size <= 0.0f) {
        return nullptr;
    }
    // Synthetic styling stands in for faces a family does not carry, so it
    // only applies when the matched face lacks the weight (600 and up reads
    // bold) or the slant the request asked for.
    bold = bold && face.weight < 600;
    italic = italic && !face.italic;
    const float scale = emScale(info, size);
    const VerticalMetrics vm = verticalMetrics(query, size);
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
    const int w =
        static_cast<int>(std::ceil(measureText(query, text, size, kern, stretch, smallCaps))) +
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
    const bool shaped = face.hbFont != nullptr;
    const std::vector<ShapedGlyph> glyphs =
        shaped ? shapeText(query, text, size, kern, stretch, smallCaps) : std::vector<ShapedGlyph>{};
    if (shaped && !glyphs.empty()) {
        // Shaped blit: glyph ids with advances and mark offsets from the
        // shaper, rasterized by stb from the same font bytes. stb takes the
        // axes apart, so a stretched run widens its glyphs to match the
        // stretched advances the shaper handed back.
        const float scaleX = scale * stretch;
        for (const ShapedGlyph& glyph : glyphs) {
            int x0 = 0, y0 = 0, gw = 0, gh = 0;
            if (glyphBitmapBox(query, glyph.glyph, size, stretch, x0, y0, gw, gh, glyph.small)) {
                std::vector<unsigned char> bmp(static_cast<std::size_t>(gw) *
                                               static_cast<std::size_t>(gh));
                // A small cap rasterizes at its shrunken scale, matching the box
                // above and the advance the shaper handed back.
                const float cap = glyph.small ? kSmallCapScale : 1.0f;
                stbtt_MakeGlyphBitmap(info, bmp.data(), gw, gh, gw, scaleX * cap, scale * cap,
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
        const std::size_t start = i;
        const unsigned int cp = decodeUtf8(text, i);
        unsigned int shaped = cp;
        float cap = 1.0f;
        if (smallCaps) {
            unsigned int upper = 0;
            if (smallCapSub(cp, i - start, upper)) {
                shaped = upper;
                cap = kSmallCapScale;
            }
        }
        const GlyphMetrics gm = metrics(query, shaped, size);
        // A substituted cap shrinks its box and raster to match the advance the
        // measurer handed back, the way the shaped path does.
        const int x0 = static_cast<int>(std::lround(gm.x0 * cap));
        const int y0 = static_cast<int>(std::lround(gm.y0 * cap));
        const int gw = static_cast<int>(std::lround((gm.x1 - gm.x0) * cap));
        const int gh = static_cast<int>(std::lround((gm.y1 - gm.y0) * cap));
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> bmp(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh));
            stbtt_MakeCodepointBitmap(info, bmp.data(), gw, gh, gw, scale * cap, scale * cap,
                                      static_cast<int>(shaped));
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
        penX += gm.advance * cap;
        }
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    // The bitmap is already at its final pixel size, so it must be sampled 1:1.
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface(surf);
    return tex;
}

}  // namespace sui
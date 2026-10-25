#include "Layout/BlockLayout.h"

#include "CSS/StyleResolver.h"
#include "Rendering/Text.h"
#include "SUI/DOM/Element.h"

#include <fribidi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <string_view>

namespace sui {
namespace {

bool isBlockLevel(const ComputedStyle& s) {
    // CSS puts `display: flex` in the block formatting context; only
    // `inline-flex` behaves like an inline-level box. A `display: table` box is
    // block-level too (an `inline-table` is not).
    return s.display == ComputedStyle::Block || s.display == ComputedStyle::Flex ||
           s.display == ComputedStyle::Grid || s.display == ComputedStyle::Table;
}

// CSS clamps by max-* first and then by min-*, so min-width and min-height win
// when the two conflict.
float applyMinMax(float value, const UsedBox& box) {
    if (box.maxWidth > 0.0f) {
        value = std::min(value, box.maxWidth);
    }
    if (box.minWidth > 0.0f) {
        value = std::max(value, box.minWidth);
    }
    return std::max(0.0f, value);
}

// Metrics for a style at its own size; degrades to an approximation when no
// font is loaded so layout stays usable.
struct Metrics {
    float ascent = 0.0f;
    float lineHeight = 0.0f;
    float spaceWidth = 0.0f;
};

Metrics metricsFor(const TextFont* font, const ComputedStyle& s) {
    Metrics m;
    const sui::FontQuery query{s.fontFamily, s.fontWeight,
                               s.fontStyle != ComputedStyle::FontNormal};
    if (font && font->hasFont() && s.fontSize > 0.0f) {
        const TextFont::VerticalMetrics vm = font->verticalMetrics(query, s.fontSize);
        m.ascent = vm.ascent;
        m.lineHeight = std::max(s.fontSize * s.lineHeight, vm.height);
        m.spaceWidth = font->measureText(query, " ", s.fontSize, s.fontKerning, s.fontStretch,
                                s.fontVariant == ComputedStyle::FontVariant::SmallCaps);
    } else {
        m.ascent = s.fontSize * 0.8f;
        m.lineHeight = s.fontSize * s.lineHeight;
        m.spaceWidth = s.fontSize * 0.25f;
    }
    return m;
}

// U+00AD SOFT HYPHEN: invisible unless a break is taken at it.
constexpr std::string_view kSoftHyphen = "\xC2\xAD";

bool isSoftHyphenAt(std::string_view t, std::size_t i) {
    return i + 1 < t.size() && t[i] == '\xC2' &&
           static_cast<unsigned char>(t[i + 1]) == 0xAD;
}

std::string transformText(const ComputedStyle& s, std::string_view t) {
    // A soft hyphen is invisible unless a break is taken at it, so it never
    // measures and never paints: dropped here, which is the one place both text
    // measurement and every emitted run go through.
    const bool hasSoftHyphen = t.find(kSoftHyphen) != std::string_view::npos;
    if (s.textTransform == ComputedStyle::TransformNone && !hasSoftHyphen) {
        return std::string(t);
    }
    std::string out;
    out.resize(t.size());
    std::size_t j = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (hasSoftHyphen && isSoftHyphenAt(t, i)) {
            ++i;
            continue;
        }
        const char c = t[i];
        char nc = c;
        if (s.textTransform == ComputedStyle::TransformUppercase) {
            nc = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else if (s.textTransform == ComputedStyle::TransformLowercase) {
            nc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (s.textTransform == ComputedStyle::TransformCapitalize) {
            if (j == 0 && std::isalpha(static_cast<unsigned char>(c))) {
                nc = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            } else {
                nc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        out[j++] = nc;
    }
    out.resize(j);
    return out;
}

// Vertical-align as a baseline offset in pixels, positive downward. Only the
// baseline-relative keywords are modelled: `super`/`sub` raise and lower the box,
// and `middle` centres it on half the x-height. `top`/`bottom` are handled where
// the line box is known (see `Flow::atomic`); for text, whose run already spans
// the line height, they land where the default does.
float verticalAlignShift(const ComputedStyle& s, const Metrics* parent = nullptr,
                         const Metrics* child = nullptr) {
    switch (s.verticalAlign) {
        case ComputedStyle::VAlignSub: return s.fontSize * 0.16f;
        case ComputedStyle::VAlignSuper: return -s.fontSize * 0.33f;
        case ComputedStyle::VAlignMiddle: return -s.fontSize * 0.25f;
        case ComputedStyle::VAlignTextTop:
        case ComputedStyle::VAlignTop:
            if (parent && child) {
                return child->ascent - parent->ascent;
            }
            return 0.0f;
        case ComputedStyle::VAlignTextBottom:
        case ComputedStyle::VAlignBottom:
            if (parent && child) {
                return parent->lineHeight - child->lineHeight;
            }
            return 0.0f;
        default: return 0.0f;
    }
}

// An SVG viewport length: plain numbers read as px, percentages resolve
// like CSS, and anything else falls back. Negative lengths clamp to zero.
Length svgDimension(const Element& el, const char* name, float fallback) {
    const std::string_view raw = el.attribute(name);
    if (raw.empty()) {
        return Length(fallback);
    }
    std::string text(raw);
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
            text.back() == '\r' || text.back() == '\f')) {
        text.pop_back();
    }
    bool percent = false;
    if (!text.empty() && text.back() == '%') {
        percent = true;
        text.pop_back();
    }
    char* stop = nullptr;
    const float value = std::strtof(text.c_str(), &stop);
    if (!stop || *stop != '\0' || !(value >= 0.0f)) {
        return Length(fallback);
    }
    if (percent) {
        return Length(Length::Percent, value);
    }
    return Length(value);
}

float textWidth(const TextFont* font, const ComputedStyle& s, std::string_view t) {
    if (t.empty()) {
        return 0.0f;
    }
    const sui::FontQuery query{s.fontFamily, s.fontWeight,
                               s.fontStyle != ComputedStyle::FontNormal};
    std::string tt = transformText(s, t);
    float w = 0.0f;
    std::size_t glyphCount = 0;
    for (unsigned char c : tt) {
        if ((c & 0xC0) != 0x80) {
            ++glyphCount;
        }
    }
    if (font && font->hasFont() && s.fontSize > 0.0f) {
        w = font->measureText(query, tt, s.fontSize, s.fontKerning, s.fontStretch,
                     s.fontVariant == ComputedStyle::FontVariant::SmallCaps);
        // Letter-spacing applies per rendered glyph: ligatures render fewer.
        glyphCount = font->glyphCount(query, tt, s.fontKerning, s.fontStretch);
    } else {
        w = static_cast<float>(glyphCount) * s.fontSize * 0.5f;
    }
    if (s.letterSpacing.unit != sui::Length::Auto && glyphCount > 0) {
        const float ls = s.letterSpacing.resolve(s.fontSize, s.fontSize, 16.0f);
        w += ls * static_cast<float>(glyphCount - 1);
    }
    // A synthetic italic leans the glyph tops to the right, so the run claims the
    // extra width rather than letting the last letter overlap what follows.
    if (s.fontStyle != ComputedStyle::FontNormal && !tt.empty()) {
        w += s.fontSize * 0.21f;
    }
    return w;
}

bool isFloated(const ComputedStyle& s) {
    return s.floatType != ComputedStyle::FloatNone;
}

// A vertical margin strut: the largest positive and most negative margins seen
// by one adjoining margin chain. Their sum is the collapsed margin.
struct MarginStrut {
    float positive = 0.0f;
    float negative = 0.0f;

    void add(float value) {
        if (value >= 0.0f) {
            positive = std::max(positive, value);
        } else {
            negative = std::min(negative, value);
        }
    }

    float value() const noexcept { return positive + negative; }
};

// The HTML `colspan` / `rowspan` value, clamped to a sane range. An absent or
// malformed value falls back to one cell.
int spanAttribute(Element& el, const char* name) {
    const std::string_view text = el.attribute(name);
    if (text.empty()) {
        return 1;
    }
    int value = 0;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return 1;
        }
        value = value * 10 + (c - '0');
        if (value > 1000) {
            return 1000;
        }
    }
    return value < 1 ? 1 : value;
}

// A positive integer attribute, falling back to `fallback` when absent or
// malformed, used for a textarea's `rows` / `cols`.
int intAttribute(Element& el, const char* name, int fallback) {
    const std::string_view text = el.attribute(name);
    if (text.empty()) {
        return fallback;
    }
    int value = 0;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return fallback;
        }
        value = value * 10 + (c - '0');
        if (value > 10000) {
            return fallback;
        }
    }
    return value < 1 ? fallback : value;
}

// A floated box, recorded as its margin box in window coordinates so any line or
// block can ask how much room is left beside it.
struct FloatBox {
    float top = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    bool isLeft = true;
};

// The floated boxes belonging to one containing block. Browsers place a float at
// the position of the line or block it belongs to and let everything that follows
// wrap beside it, so `available` answers, for a vertical slice, how far the left
// inset reaches and where the right edge sits. Coordinates are absolute, matching
// the layout engine's line and box coordinates.
class FloatContext {
public:
    void available(float yTop, float yBottom, float& leftEdge, float& rightEdge) const {
        for (const FloatBox& f : floats_) {
            // Half-open intervals: a float exactly touching the slice edge does
            // not overlap it.
            if (f.bottom <= yTop || f.top >= yBottom) {
                continue;
            }
            if (f.isLeft) {
                leftEdge = std::max(leftEdge, f.right);
            } else {
                rightEdge = std::min(rightEdge, f.left);
            }
        }
    }

    // The lowest float edge a `clear` box has to drop below; `baseline` is what
    // to return when nothing is cleared.
    float clearBottom(ComputedStyle::ClearType clear, float baseline) const {
        float bottom = baseline;
        for (const FloatBox& f : floats_) {
            const bool matches =
                clear == ComputedStyle::ClearBoth ||
                (clear == ComputedStyle::ClearLeft && f.isLeft) ||
                (clear == ComputedStyle::ClearRight && !f.isLeft);
            if (matches) {
                bottom = std::max(bottom, f.bottom);
            }
        }
        return bottom;
    }

    // The next float bottom strictly below `y`, so a float that does not fit
    // beside the others can try again lower down.
    float nextBottom(float y) const {
        float next = y;
        bool found = false;
        for (const FloatBox& f : floats_) {
            if (f.bottom > y + kEpsilon && (!found || f.bottom < next)) {
                next = f.bottom;
                found = true;
            }
        }
        return found ? next : y;
    }

    float bottom(float baseline) const {
        float bottom = baseline;
        for (const FloatBox& f : floats_) {
            bottom = std::max(bottom, f.bottom);
        }
        return bottom;
    }

    void add(const FloatBox& f) { floats_.push_back(f); }

private:
    static constexpr float kEpsilon = 0.01f;
    std::vector<FloatBox> floats_;
};

class Layout {
public:
    Layout(const StyleResolver& resolver, const TextFont* font, float viewportWidth,
           float viewportHeight)
        : resolver_(resolver),
          viewportWidth_(viewportWidth),
          viewportHeight_(viewportHeight),
          font_(font) {}

    // A padding box: the containing block an out-of-flow box resolves against.
    struct Box {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };

    // An out-of-flow box waiting for its containing block to be final.
    struct Pending {
        Element* el;
        Element* owner;  // nearest positioned ancestor, null for the viewport
        bool fixed;
    };

    // A relatively positioned box, already in its final place apart from the
    // offsets.
    struct Relative {
        Element* el;
        ComputedStyle style;
    };

    // A sticky box, also in flow, whose offsets come from the scroll position
    // of its nearest scrolling ancestor rather than from the insets.
    struct Sticky {
        Element* el;
        ComputedStyle style;
        Element* containingBlock;
    };

    // Exposed for Flow, which needs the same answers run() uses.
    bool isLaidOut(Element& el) const { return laidOut(el); }
    ComputedStyle resolveStyle(Element& el) const { return resolver_.resolve(el); }
    bool hasFirstLineRules() const { return resolver_.hasFirstLineRules(); }
    bool hasFirstLetterRules() const { return resolver_.hasFirstLetterRules(); }
    PseudoFont firstLineFont(Element& el, const ComputedStyle& base) const {
        return resolver_.firstLineFont(el, base);
    }
    PseudoFont firstLetterFont(Element& el, const ComputedStyle& base) const {
        return resolver_.firstLetterFont(el, base);
    }

    // A percentage height has to resolve against a containing block that is
    // usually itself content-sized, which is not knowable while laying out. The
    // viewport is used instead: that is the only definite height in a typical
    // desktop page, and it is what an author writing `height: 50%` expects.
    UsedBox usedBox(const ComputedStyle& s, float availableWidth) const {
        return resolveBox(s, availableWidth, viewportHeight_, resolver_.rootFontSize());
    }

    void applyTextAreaSize(Element& el, ComputedStyle& s) const {
        if (el.tag() != "textarea") {
            return;
        }
        // Without a CSS size a textarea behaves like a replaced element: `cols`
        // characters wide by `rows` lines tall, measured in its own font.
        const int cols = intAttribute(el, "cols", 20);
        const int rows = intAttribute(el, "rows", 2);
        const sui::FontQuery query{s.fontFamily, s.fontWeight,
                                   s.fontStyle != ComputedStyle::FontNormal};
        const float advance = (font_ && font_->hasFont()) ? font_->measureText(query, "0", s.fontSize, s.fontKerning, s.fontStretch,
                         s.fontVariant == ComputedStyle::FontVariant::SmallCaps)
                                                          : s.fontSize * 0.5f;
        const Metrics m = metricsFor(font_, s);
        if (s.width.isAuto()) {
            s.width = Length(static_cast<float>(cols) * advance);
        }
        if (s.height.isAuto()) {
            s.height = Length(static_cast<float>(rows) * m.lineHeight);
        }
    }

    void applySelectSize(Element& el, ComputedStyle& s) const {
        if (el.tag() != "select") {
            return;
        }
        const sui::FontQuery query{s.fontFamily, s.fontWeight,
                                   s.fontStyle != ComputedStyle::FontNormal};
        // A closed combo sizes like a replaced element: wide enough for the
        // longest option plus the drop arrow, one line tall.
        if (s.width.isAuto()) {
            float text = 0.0f;
            if (font_ && font_->hasFont()) {
                for (Element* option : el.selectOptions()) {
                    text = std::max(text, font_->measureText(query, option->optionLabel(), s.fontSize, s.fontKerning, s.fontStretch,
                         s.fontVariant == ComputedStyle::FontVariant::SmallCaps));
                }
            } else {
                std::size_t longest = 0;
                for (Element* option : el.selectOptions()) {
                    longest = std::max(longest, option->optionLabel().size());
                }
                text = static_cast<float>(longest) * s.fontSize * 0.5f;
            }
            constexpr float kArrowRoom = 24.0f;
            s.width = Length(text + kArrowRoom);
        }
        if (s.height.isAuto()) {
            // An open list is as tall as its visible rows; a dropdown is one
            // line tall.
            const int rows = std::max(el.listboxRows(), 1);
            s.height = Length(static_cast<float>(rows) * metricsFor(font_, s).lineHeight);
        }
    }

    void applyMeterProgressSize(Element& el, ComputedStyle& s) const {
        if (el.tag() != "meter" && el.tag() != "progress") {
            return;
        }
        // Without a CSS size a gauge behaves like a replaced element: ten
        //ems wide and one tall, measured in its own font.
        if (s.width.isAuto()) {
            s.width = Length(10.0f * s.fontSize);
        }
        if (s.height.isAuto()) {
            s.height = Length(s.fontSize);
        }
    }

    void applySvgSize(Element& el, ComputedStyle& s) const {
        if (el.tag() != "svg") {
            return;
        }
        // Without a CSS size an svg behaves like a replaced element: its
        // width/height attributes win (percentages resolve like CSS), each
        // defaulting to the 300-by-150 viewport.
        if (s.width.isAuto()) {
            s.width = svgDimension(el, "width", 300.0f);
        }
        if (s.height.isAuto()) {
            s.height = svgDimension(el, "height", 150.0f);
        }
    }

    // Lays out `el` with the given available width. Returns the outer height
    // consumed, margins included.
    // A block formatting context root separates its children's margins from
    // its own. Floats, out-of-flow boxes, scroll containers, flex/grid/table
    // containers, and layout/paint containment all establish one.
    bool isBfcRoot(const ComputedStyle& s) const {
        if (isFloated(s) || s.position == ComputedStyle::Absolute ||
            s.position == ComputedStyle::Fixed) {
            return true;
        }
        if (s.overflowX != ComputedStyle::Visible ||
            s.overflowY != ComputedStyle::Visible) {
            return true;
        }
        if (s.isFlexContainer() || s.isGridContainer() || s.isTableContainer()) {
            return true;
        }
        if (s.contentVisibility == ComputedStyle::ContentHidden) {
            return true;
        }
        return (s.contain & (ComputedStyle::ContainLayout |
                             ComputedStyle::ContainPaint)) != 0;
    }

    // Whether a text node produces a line box in this context. Collapsible
    // whitespace between blocks does not, preserved whitespace does.
    bool textCreatesLine(std::string_view text, const ComputedStyle& s) const {
        bool onlySpace = true;
        for (unsigned char c : text) {
            if (!std::isspace(c)) {
                onlySpace = false;
                break;
            }
        }
        if (!onlySpace) {
            return true;
        }
        if (text.empty()) {
            return false;
        }
        return s.whiteSpace != ComputedStyle::WrapText &&
               s.whiteSpace != ComputedStyle::WrapNone;
    }

    // Generated marker boxes sit before an item's first child and prevent its
    // top margin from adjoining that child's.
    bool hasGeneratedBox(const Element& el, const ComputedStyle& s) const {
        return el.tag() == "li" && s.listStyleType != ComputedStyle::LSTNone;
    }

    // The first in-flow block-level child, or null when inline content (or a
    // preserved blank) comes first. Floats and out-of-flow boxes are skipped.
    Element* firstBlockChild(Element& el, const ComputedStyle& parent) const {
        for (const auto& child : el.children()) {
            if (!child) {
                continue;
            }
            if (child->isTextNode()) {
                if (textCreatesLine(child->text(), parent)) {
                    return nullptr;
                }
                continue;
            }
            if (!laidOut(*child)) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::None || !cs.visible) {
                continue;
            }
            if (isFloated(cs) || cs.position == ComputedStyle::Absolute ||
                cs.position == ComputedStyle::Fixed) {
                continue;
            }
            return isBlockLevel(cs) ? child.get() : nullptr;
        }
        return nullptr;
    }

    // The last in-flow block-level child, or null when inline content follows.
    Element* lastBlockChild(Element& el, const ComputedStyle& parent) const {
        const auto& children = el.children();
        for (std::size_t i = children.size(); i-- > 0;) {
            const auto& child = children[i];
            if (!child) {
                continue;
            }
            if (child->isTextNode()) {
                if (textCreatesLine(child->text(), parent)) {
                    return nullptr;
                }
                continue;
            }
            if (!laidOut(*child)) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::None || !cs.visible) {
                continue;
            }
            if (isFloated(cs) || cs.position == ComputedStyle::Absolute ||
                cs.position == ComputedStyle::Fixed) {
                continue;
            }
            return isBlockLevel(cs) ? child.get() : nullptr;
        }
        return nullptr;
    }

    // A box's top margin collapses with its first in-flow block child's when no
    // border, padding, cleared top, or inline content separates them.
    bool canCollapseTop(Element& el, const ComputedStyle& s, float width) const {
        if (!isBlockLevel(s) || isBfcRoot(s) ||
            s.clearType != ComputedStyle::ClearNone || hasGeneratedBox(el, s) ||
            !pureBlockFlow(el, s)) {
            return false;
        }
        const UsedBox b = usedBox(s, width);
        if (b.borderWidth.top > 0.0f || b.padding.top > 0.0f) {
            return false;
        }
        Element* first = firstBlockChild(el, s);
        if (!first) {
            return false;
        }
        const ComputedStyle cs = resolver_.resolve(*first);
        return cs.clearType == ComputedStyle::ClearNone;
    }

    // A box's bottom margin collapses with its last in-flow block child's when
    // no border, padding, definite height, min-height, or trailing inline
    // content separates them.
    bool canCollapseBottom(Element& el, const ComputedStyle& s,
                           const UsedBox& b) const {
        if (!isBlockLevel(s) || isBfcRoot(s) || hasGeneratedBox(el, s) ||
            !pureBlockFlow(el, s)) {
            return false;
        }
        if (b.borderWidth.bottom > 0.0f || b.padding.bottom > 0.0f ||
            b.height > 0.0f || b.minHeight > 0.0f) {
            return false;
        }
        if (s.height.isAuto() && s.aspectRatio() > 0.0f) {
            return false;
        }
        return lastBlockChild(el, s) != nullptr;
    }

    // The used content width of a child, for resolving its descendants'
    // percentage margins while precomputing margin struts.
    float childContentWidth(const ComputedStyle& cs, float containingWidth) const {
        const UsedBox b = usedBox(cs, containingWidth);
        const float width = b.width > 0.0f
                                ? b.width
                                : std::max(0.0f, containingWidth - b.horizontalChrome());
        return applyMinMax(width, b);
    }

    // An empty block's top and bottom margins collapse through it. A block is
    // empty for this purpose only when it has no height, border, padding,
    // inline content, floats, or non-through block children.
    bool collapsesThrough(Element& el, const ComputedStyle& s, const UsedBox& b,
                          float width) const {
        const MarginKey key{&el, width};
        auto cached = throughCache_.find(key);
        if (cached != throughCache_.end()) {
            return cached->second;
        }
        const bool result = [&]() -> bool {
        if (!isBlockLevel(s) || isBfcRoot(s) ||
            s.contentVisibility == ComputedStyle::ContentHidden ||
            s.clearType != ComputedStyle::ClearNone || hasGeneratedBox(el, s)) {
            return false;
        }
        if (b.borderWidth.top > 0.0f || b.borderWidth.bottom > 0.0f ||
            b.padding.top > 0.0f || b.padding.bottom > 0.0f) {
            return false;
        }
        if (b.height > 0.0f || b.minHeight > 0.0f ||
            (s.height.isAuto() && s.aspectRatio() > 0.0f)) {
            return false;
        }
        for (const auto& child : el.children()) {
            if (!child) {
                continue;
            }
            if (child->isTextNode()) {
                if (textCreatesLine(child->text(), s)) {
                    return false;
                }
                continue;
            }
            if (!laidOut(*child)) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::None || !cs.visible) {
                continue;
            }
            if (isFloated(cs)) {
                return false;
            }
            if (cs.position == ComputedStyle::Absolute ||
                cs.position == ComputedStyle::Fixed) {
                continue;
            }
            if (!isBlockLevel(cs)) {
                return false;
            }
            const UsedBox cb = usedBox(cs, width);
            if (!collapsesThrough(*child, cs, cb, childContentWidth(cs, width))) {
                return false;
            }
        }
        return true;
        }();
        throughCache_.emplace(key, result);
        return result;
    }

    // The collapsed top margin of a box: its own top margin plus the adjoining
    // chain down its first in-flow block descendants. A through box contributes
    // both of its margins.
    MarginStrut collapsedTop(Element& el, float width) const {
        const MarginKey key{&el, width};
        auto cached = topCache_.find(key);
        if (cached != topCache_.end()) {
            return cached->second;
        }
        const MarginStrut result = [&]() -> MarginStrut {
        const ComputedStyle s = resolver_.resolve(el);
        const UsedBox b = usedBox(s, width);
        MarginStrut out;
        out.add(b.margin.top);
        if (collapsesThrough(el, s, b, width)) {
            out.add(b.margin.bottom);
            for (const auto& child : el.children()) {
                if (!child || child->isTextNode() || !laidOut(*child)) {
                    continue;
                }
                const ComputedStyle cs = resolver_.resolve(*child);
                if (cs.display == ComputedStyle::None || !cs.visible ||
                    isFloated(cs) || cs.position == ComputedStyle::Absolute ||
                    cs.position == ComputedStyle::Fixed || !isBlockLevel(cs)) {
                    continue;
                }
                const MarginStrut inner =
                    collapsedTop(*child, childContentWidth(cs, width));
                out.add(inner.positive);
                out.add(inner.negative);
            }
            return out;
        }
        if (!canCollapseTop(el, s, width)) {
            return out;
        }
        for (const auto& child : el.children()) {
            if (!child) {
                continue;
            }
            if (child->isTextNode()) {
                if (textCreatesLine(child->text(), s)) {
                    break;
                }
                continue;
            }
            if (!laidOut(*child)) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::None || !cs.visible ||
                isFloated(cs) || cs.position == ComputedStyle::Absolute ||
                cs.position == ComputedStyle::Fixed) {
                continue;
            }
            if (!isBlockLevel(cs)) {
                break;
            }
            const float cw = childContentWidth(cs, width);
            const MarginStrut inner = collapsedTop(*child, cw);
            out.add(inner.positive);
            out.add(inner.negative);
            if (!collapsesThrough(*child, cs, usedBox(cs, width), cw)) {
                break;
            }
        }
        return out;
        }();
        topCache_.emplace(key, result);
        return result;
    }

    // The mirror strut for the bottom edge.
    MarginStrut collapsedBottom(Element& el, float width) const {
        const MarginKey key{&el, width};
        auto cached = bottomCache_.find(key);
        if (cached != bottomCache_.end()) {
            return cached->second;
        }
        const MarginStrut result = [&]() -> MarginStrut {
        const ComputedStyle s = resolver_.resolve(el);
        const UsedBox b = usedBox(s, width);
        MarginStrut out;
        out.add(b.margin.bottom);
        if (collapsesThrough(el, s, b, width)) {
            out.add(b.margin.top);
            const auto& children = el.children();
            for (std::size_t i = children.size(); i-- > 0;) {
                const auto& child = children[i];
                if (!child || child->isTextNode() || !laidOut(*child)) {
                    continue;
                }
                const ComputedStyle cs = resolver_.resolve(*child);
                if (cs.display == ComputedStyle::None || !cs.visible ||
                    isFloated(cs) || cs.position == ComputedStyle::Absolute ||
                    cs.position == ComputedStyle::Fixed || !isBlockLevel(cs)) {
                    continue;
                }
                const MarginStrut inner =
                    collapsedBottom(*child, childContentWidth(cs, width));
                out.add(inner.positive);
                out.add(inner.negative);
            }
            return out;
        }
        if (!canCollapseBottom(el, s, b)) {
            return out;
        }
        const auto& children = el.children();
        for (std::size_t i = children.size(); i-- > 0;) {
            const auto& child = children[i];
            if (!child) {
                continue;
            }
            if (child->isTextNode()) {
                if (textCreatesLine(child->text(), s)) {
                    break;
                }
                continue;
            }
            if (!laidOut(*child)) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::None || !cs.visible ||
                isFloated(cs) || cs.position == ComputedStyle::Absolute ||
                cs.position == ComputedStyle::Fixed) {
                continue;
            }
            if (!isBlockLevel(cs)) {
                break;
            }
            const float cw = childContentWidth(cs, width);
            const MarginStrut inner = collapsedBottom(*child, cw);
            out.add(inner.positive);
            out.add(inner.negative);
            if (!collapsesThrough(*child, cs, usedBox(cs, width), cw)) {
                break;
            }
        }
        return out;
        }();
        bottomCache_.emplace(key, result);
        return result;
    }

    // Repositions an all-block child list with collapsed sibling margins. The
    // first child's top and the last child's bottom are already outside the
    // parent when suppress* is set, so they are not counted inside.
    float collapseBlockMargins(Element& el, float contentY, float contentWidth,
                               bool suppressFirstTop, bool suppressLastBottom) {
        MarginStrut pending;
        float prevBottom = contentY;
        bool first = true;
        bool any = false;
        for (const auto& child : el.children()) {
            if (!child || child->isTextNode() || !laidOut(*child)) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::None || !cs.visible ||
                isFloated(cs) || cs.position == ComputedStyle::Absolute ||
                cs.position == ComputedStyle::Fixed) {
                continue;
            }
            if (!isBlockLevel(cs)) {
                continue;
            }
            const UsedBox cb = usedBox(cs, contentWidth);
            const float cw = childContentWidth(cs, contentWidth);
            const bool through = collapsesThrough(*child, cs, cb, cw);
            const MarginStrut top = collapsedTop(*child, contentWidth);
            const MarginStrut bottom = collapsedBottom(*child, contentWidth);
            if (through) {
                pending.add(top.positive);
                pending.add(top.negative);
                pending.add(bottom.positive);
                pending.add(bottom.negative);
                const float desired = prevBottom + pending.value();
                shiftSubtree(*child, 0.0f, desired - child->layoutY());
                continue;
            }
            float desired = 0.0f;
            if (first) {
                if (suppressFirstTop) {
                    desired = contentY;
                } else {
                    MarginStrut gap = pending;
                    gap.add(top.positive);
                    gap.add(top.negative);
                    desired = contentY + gap.value();
                }
            } else {
                MarginStrut gap = pending;
                gap.add(top.positive);
                gap.add(top.negative);
                desired = prevBottom + gap.value();
            }
            shiftSubtree(*child, 0.0f, desired - child->layoutY());
            prevBottom = desired + child->layoutHeight();
            pending = bottom;
            first = false;
            any = true;
        }
        float height = any ? prevBottom - contentY : 0.0f;
        if (!suppressLastBottom) {
            height += pending.value();
        }
        return std::max(0.0f, height);
    }

    // True when the container's children form a pure block flow, so its own
    // first/last margins may join theirs. Line boxes, inline-level boxes,
    // floats, and clearance all keep the container's contents from collapsing
    // with its edges.
    bool pureBlockFlow(Element& el, const ComputedStyle& s) const {
        auto cached = pureCache_.find(&el);
        if (cached != pureCache_.end()) {
            return cached->second;
        }
        const bool result = [&]() -> bool {
        if (hasFloatChild(el)) {
            return false;
        }
        for (const auto& child : el.children()) {
            if (!child) {
                continue;
            }
            if (child->isTextNode()) {
                if (textCreatesLine(child->text(), s)) {
                    return false;
                }
                continue;
            }
            if (!laidOut(*child)) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::None || !cs.visible) {
                continue;
            }
            if (isFloated(cs) || cs.position == ComputedStyle::Absolute ||
                cs.position == ComputedStyle::Fixed) {
                continue;
            }
            if (!isBlockLevel(cs) || cs.clearType != ComputedStyle::ClearNone) {
                return false;
            }
        }
        return true;
        }();
        pureCache_.emplace(&el, result);
        return result;
    }

    float run(Element& el, float availableWidth, float originX, float originY,
              bool collapseMargins = false) {
        // The inline probe below may narrow `availableWidth` to a shrink-to-fit
        // width. Auto margins still resolve against the containing block, so keep
        // the offered width for that decision.
        const float containingWidth = availableWidth;
        ComputedStyle s = resolver_.resolve(el);
        applyTextAreaSize(el, s);
        applySelectSize(el, s);
        applyMeterProgressSize(el, s);
        applySvgSize(el, s);
        const UsedBox box = usedBox(s, availableWidth);
        el.clearTextRuns();
        if (s.display == ComputedStyle::None || !s.visible) {
            el.setLayoutX(originX);
            el.setLayoutY(originY);
            el.setLayoutWidth(0.0f);
            el.setLayoutHeight(0.0f);
            return 0.0f;
        }

        // Auto margins in normal flow: a block-level box with a definite width
        // that does not fill its containing block splits the leftover space.
        // `margin: 0 auto` is how a page centres a column; one auto side pushes
        // the box to the other. Vertical auto margins stay zero in flow, as CSS
        // specifies.
        UsedBox placed = box;
        const float chromeW = box.horizontalChrome();
        // An `auto` width normally fills the line, so it has nothing to split;
        // once `max-width` clamps it below the line, though, the usual
        // `max-width: 60ch; margin: 0 auto` container pattern needs the
        // margins distributed just like a definite width.
        const bool clampedAuto = s.width.isAuto() && box.maxWidth > 0.0f &&
                                 box.maxWidth + chromeW < availableWidth;
        // A table with an auto width is sized by its columns during content
        // layout, so the width and margins are both settled in the second pass
        // below. `clampedAuto` cannot stand in for it: the max-width would
        // centre the cap, not the narrower table the columns actually need.
        const bool autoTable = s.display == ComputedStyle::Table && box.width <= 0.0f;
        if (!autoTable && isBlockLevel(s) && !isFloated(s) &&
            s.position != ComputedStyle::Absolute &&
            s.position != ComputedStyle::Fixed && availableWidth < kUnbounded &&
            (box.width > 0.0f || clampedAuto) &&
            (s.margin.left.isAuto() || s.margin.right.isAuto())) {
            const float usedContent = box.width > 0.0f
                                          ? applyMinMax(box.width, box)
                                          : applyMinMax(
                                                std::max(0.0f, availableWidth - chromeW),
                                                box);
            const float borderW = usedContent + box.borderWidth.left +
                                  box.borderWidth.right + box.padding.left +
                                  box.padding.right;
            const float fixed = (s.margin.left.isAuto() ? 0.0f : box.margin.left) +
                                (s.margin.right.isAuto() ? 0.0f : box.margin.right);
            const float space = std::max(0.0f, availableWidth - borderW - fixed);
            if (s.margin.left.isAuto() && s.margin.right.isAuto()) {
                placed.margin.left = space * 0.5f;
                placed.margin.right = space * 0.5f;
            } else if (s.margin.left.isAuto()) {
                placed.margin.left = space;
            } else {
                placed.margin.right = space;
            }
        }

        // A block in normal flow folds its first/last child's adjoining margins
        // into its own. The caller positions it by this collapsed top margin, and
        // its outer height carries the collapsed bottom margin.
        const bool collapsible =
            collapseMargins && isBlockLevel(s) && !isBfcRoot(s) && !isFloated(s) &&
            s.position != ComputedStyle::Absolute &&
            s.position != ComputedStyle::Fixed && pureBlockFlow(el, s);
        const float flowTop =
            collapsible ? collapsedTop(el, availableWidth).value() : box.margin.top;
        const float flowBottom =
            collapsible ? collapsedBottom(el, availableWidth).value() : box.margin.bottom;

        const float contentX = originX + placed.margin.left + placed.borderWidth.left +
                               placed.padding.left;
        const float contentY = originY + flowTop + box.borderWidth.top + box.padding.top;

        // An inline-level box with auto width shrinks to fit its contents, the
        // way a browser sizes a button or an image. The probe below measures
        // the unwrapped width; run() then repeats the layout at the real width.
        if (box.width <= 0.0f && s.display != ComputedStyle::Block &&
            s.contentVisibility != ComputedStyle::ContentHidden) {
            availableWidth =
                std::min(availableWidth, probeWidth(el, box) + box.horizontalChrome());
            el.clearTextRuns();  // the probe's runs are throwaway measurements
        }

        // A positioned element is the containing block for its own out-of-flow
        // descendants, so the flag moves with whatever is being laid out and is put
        // back afterwards.
        Element* const savedOwner = cbOwner_;
        if (s.position != ComputedStyle::Static) {
            cbOwner_ = &el;
        }

        float contentWidth = 0.0f;
        float contentHeight = layoutContents(el, s, box, availableWidth, contentX, contentY,
                                             contentWidth, collapsible);
        cbOwner_ = savedOwner;

        if (box.height > 0.0f) {
            contentHeight = box.height;
        } else if (s.aspectRatio() > 0.0f && contentWidth > 0.0f && s.height.isAuto()) {
            contentHeight = contentWidth / s.aspectRatio();
        }
        if (box.maxHeight > 0.0f) {
            contentHeight = std::min(contentHeight, box.maxHeight);
        }
        if (box.minHeight > 0.0f) {
            contentHeight = std::max(contentHeight, box.minHeight);
        }
        contentWidth = applyMinMax(contentWidth, box);

        const float borderW = contentWidth + box.borderWidth.left + box.borderWidth.right +
                              box.padding.left + box.padding.right;
        const float borderH = contentHeight + box.borderWidth.top + box.borderWidth.bottom +
                              box.padding.top + box.padding.bottom;

        // The table's width is now known. Resolve its horizontal auto margins
        // against the measured width, then move the finished subtree by the
        // leading margin. The content was laid out from the containing block's
        // edge because the width was still unknown, so shifting is what keeps
        // the cells, captions, rows, and text runs under the table.
        float autoLeft = 0.0f;
        if (autoTable && !isFloated(s) &&
            s.position != ComputedStyle::Absolute &&
            s.position != ComputedStyle::Fixed && containingWidth < kUnbounded &&
            (s.margin.left.isAuto() || s.margin.right.isAuto())) {
            const float fixed = (s.margin.left.isAuto() ? 0.0f : box.margin.left) +
                                (s.margin.right.isAuto() ? 0.0f : box.margin.right);
            const float space = std::max(0.0f, containingWidth - borderW - fixed);
            if (s.margin.left.isAuto() && s.margin.right.isAuto()) {
                autoLeft = space * 0.5f;
            } else if (s.margin.left.isAuto()) {
                autoLeft = space;
            }
        }

        el.setLayoutX(originX + placed.margin.left);
        el.setLayoutY(originY + flowTop);
        el.setLayoutWidth(borderW);
        el.setLayoutHeight(borderH);
        recordBoxes(el, contentX, contentY, contentWidth, contentHeight, box);
        shiftSubtree(el, autoLeft, 0.0f);

        if (s.position != ComputedStyle::Static) {
            if (s.position == ComputedStyle::Absolute ||
                s.position == ComputedStyle::Fixed) {
                // An out-of-flow box takes no space in its parent and is placed
                // once the flow has fixed its containing block.
                pending_.push_back({&el, cbOwner_, s.position == ComputedStyle::Fixed});
                return 0.0f;
            }
            if (s.position == ComputedStyle::Sticky) {
                // Stays in the flow, but its final place depends on how far its
                // scrollport has moved, so it is resolved once every flow
                // position is known.
                sticky_.push_back({&el, s, blockAncestor(el)});
            } else {
                relative_.push_back({&el, s});
            }
        }
        return borderH + flowTop + flowBottom;
    }

    // Out-of-flow boxes are placed after the in-flow layout, because their
    // containing block is the nearest positioned ancestor's padding box and that
    // is not known until the flow has placed it. Relative boxes are offset in the
    // same pass: they are already in their final place, so moving them first means
    // an absolute box anchored to one reads the moved position.
    void placeOutOfFlow() {
        for (const Relative& item : relative_) {
            Element& el = *item.el;
            const float em = item.style.fontSize;
            const float rem = resolver_.rootFontSize();
            el.setLayoutX(el.layoutX() +
                          offset(item.style.left, item.style.right, 0.0f, em, rem));
            el.setLayoutY(el.layoutY() +
                          offset(item.style.top, item.style.bottom, 0.0f, em, rem));
        }
        relative_.clear();
        // Insertion order puts a box before its own out-of-flow descendants, since
        // a parent's placeholder is only pushed once its contents are laid out.
        // Walking backwards therefore places containing blocks first.
        for (std::size_t i = pending_.size(); i-- > 0;) {
            placeAbsolute(*pending_[i].el, containingBlockOf(pending_[i]));
        }
        pending_.clear();
        placeSticky();
    }

    // The nearest block-level ancestor, whose content box is the range a sticky
    // box may move through while it clings to its scrollport.
    Element* blockAncestor(Element& el) const {
        for (Element* a = el.parent(); a; a = a->parent()) {
            if (!a->isVisual()) {
                continue;
            }
            // Only a genuinely inline box fails to be a block container; an
            // inline-block, inline-flex or inline-grid still boxes its children.
            const ComputedStyle as = resolver_.resolve(*a);
            if (as.display == ComputedStyle::None || as.display == ComputedStyle::Inline) {
                continue;
            }
            return a;
        }
        return nullptr;
    }

    // The nearest ancestor the given axis can scroll through; a sticky offset
    // holds against that box's scrollport. Without one the page has no scroller
    // on the axis and the box stays put.
    Element* scrollAncestor(Element& el, bool vertical) const {
        for (Element* a = el.parent(); a; a = a->parent()) {
            if (!a->isVisual()) {
                continue;
            }
            const ComputedStyle as = resolver_.resolve(*a);
            const auto overflow = vertical ? as.overflowY : as.overflowX;
            if (as.display != ComputedStyle::None && overflow != ComputedStyle::Visible) {
                return a;
            }
        }
        return nullptr;
    }

    // Moves a sticky box and everything it contains. Its padding and content
    // boxes, which the painter clips to, and the text runs move too, so the
    // whole subtree lands where the box is drawn.
    void shiftSubtree(Element& el, float dx, float dy) {
        if (dx == 0.0f && dy == 0.0f) {
            return;
        }
        el.setLayoutX(el.layoutX() + dx);
        el.setLayoutY(el.layoutY() + dy);
        el.setPaddingX(el.paddingX() + dx);
        el.setPaddingY(el.paddingY() + dy);
        el.setContentX(el.contentX() + dx);
        el.setContentY(el.contentY() + dy);
        for (Element::TextRun& run : el.textRuns()) {
            run.x += dx;
            run.y += dy;
        }
        for (const auto& child : el.children()) {
            if (child && !child->isTextNode()) {
                shiftSubtree(*child, dx, dy);
            }
        }
    }

    // Reads the current scroll offsets of every sticky box's scrollport and
    // shifts the box so each given inset clears the port edge: `top` and
    // `left` pull the box down or right once the leading edge would pass it,
    // `bottom` and `right` pull the other way once the trailing edge arrives.
    // The box never leaves its containing block. The frame loop re-runs layout
    // whenever the wheel moves a box, so every scroll notch lands here with
    // fresh offsets.
    void placeSticky() {
        for (const Sticky& item : sticky_) {
            Element& el = *item.el;
            const ComputedStyle& s = item.style;
            Element* cb = item.containingBlock;
            const float em = s.fontSize;
            const float rem = resolver_.rootFontSize();
            const float cbH = cb ? cb->contentHeight() : viewportHeight_;
            const float cbW = cb ? cb->contentWidth() : viewportWidth_;
            const float cbTop = cb ? cb->contentY() : 0.0f;
            const float cbLeft = cb ? cb->contentX() : 0.0f;
            const float cbBottom = cb ? cb->contentY() + cb->contentHeight() : viewportHeight_;
            const float cbRight = cb ? cb->contentX() + cb->contentWidth() : viewportWidth_;
            const float y = el.layoutY();
            const float x = el.layoutX();
            const float h = el.layoutHeight();
            const float w = el.layoutWidth();

            float dy = 0.0f;
            if (Element* scroller = scrollAncestor(el, /*vertical=*/true)) {
                const float st = scroller->scrollTop();
                const float viewTop = scroller->paddingY();
                const float viewBottom = viewTop + scroller->paddingHeight();
                if (!s.top.isAuto()) {
                    // Keeping the leading edge clear of the port's top pulls the
                    // box down; it never rises above its flow spot.
                    dy = std::max(dy, viewTop + s.top.resolve(cbH, em, rem) + st - y);
                } else if (!s.bottom.isAuto()) {
                    // Keeping the trailing edge clear of the port's bottom pulls
                    // the box up; it never sinks below where the flow put it.
                    dy = std::min(dy,
                                  viewBottom - s.bottom.resolve(cbH, em, rem) + st - y - h);
                }
                // A box taller than the scrollport has to fit inside it.
                dy = std::min(dy, std::max(0.0f, viewBottom + st - y - h));
            }
            float dx = 0.0f;
            if (Element* scroller = scrollAncestor(el, /*vertical=*/false)) {
                const float sl = scroller->scrollLeft();
                const float viewLeft = scroller->paddingX();
                const float viewRight = viewLeft + scroller->paddingWidth();
                if (!s.left.isAuto()) {
                    dx = std::max(dx, viewLeft + s.left.resolve(cbW, em, rem) + sl - x);
                } else if (!s.right.isAuto()) {
                    dx = std::min(dx,
                                  viewRight - s.right.resolve(cbW, em, rem) + sl - x - w);
                }
                dx = std::min(dx, std::max(0.0f, viewRight + sl - x - w));
            }

            // A sticky box may cross its containing block while it clings, but it
            // never leaves it: each axis is bounded by that block's content box.
            if (cb) {
                dy = std::min(dy, std::max(0.0f, cbBottom - y - h));
                dy = std::max(dy, cbTop - y);
                dx = std::min(dx, std::max(0.0f, cbRight - x - w));
                dx = std::max(dx, cbLeft - x);
            }
            shiftSubtree(el, dx, dy);
        }
        sticky_.clear();
    }

    Box containingBlockOf(const Pending& item) const {
        if (item.fixed || !item.owner) {
            return {0.0f, 0.0f, viewportWidth_, viewportHeight_};
        }
        Element& owner = *item.owner;
        const UsedBox chrome = usedBox(resolver_.resolve(owner), owner.layoutWidth());
        const float insetX = chrome.borderWidth.left + chrome.padding.left;
        const float insetY = chrome.borderWidth.top + chrome.padding.top;
        return {owner.layoutX() + insetX,
                owner.layoutY() + insetY,
                std::max(0.0f, owner.layoutWidth() - insetX - chrome.borderWidth.right -
                                         chrome.padding.right),
                std::max(0.0f, owner.layoutHeight() - insetY - chrome.borderWidth.bottom -
                                         chrome.padding.bottom)};
    }

    // `left` wins over `right`, and `top` over `bottom`, which is the one case CSS
    // resolves by leaving the direction over-specified.
    static float offset(const Length& leading, const Length& trailing, float base,
                        float emBase, float remBase) {
        if (!leading.isAuto()) {
            return leading.resolve(base, emBase, remBase);
        }
        if (!trailing.isAuto()) {
            return -trailing.resolve(base, emBase, remBase);
        }
        return 0.0f;
    }

    // An auto margin in the over-constrained axis centres the box, which is why a
    // single centred overlay only needs `margin: auto` and two offsets.
    static float centred(float free, float extent, float margin) {
        return std::max(0.0f, (free - extent - margin) * 0.5f);
    }

    void placeAbsolute(Element& el, const Box& cb) {
        const ComputedStyle s = resolver_.resolve(el);
        const float em = s.fontSize;
        const float rem = resolver_.rootFontSize();
        const UsedBox box = usedBox(s, cb.width);

        const float leftUsed =
            s.left.isAuto() ? 0.0f : s.left.resolve(cb.width, em, rem);
        const float rightUsed =
            s.right.isAuto() ? 0.0f : s.right.resolve(cb.width, em, rem);

        // Both horizontal edges given means the size comes from them; otherwise the
        // box shrink-wraps its content the way a browser-sized replaced element
        // does, never past the containing block.
        float borderW = 0.0f;
        if (!s.left.isAuto() && !s.right.isAuto()) {
            borderW = cb.width - leftUsed - rightUsed - box.margin.left - box.margin.right;
        } else if (box.width > 0.0f) {
            borderW = box.width + box.horizontalChrome();
        } else {
            borderW = std::min(probeWidth(el, box) + box.horizontalChrome(), cb.width);
        }
        borderW = std::max(applyMinMax(borderW, box), 0.0f);

        // An `auto` margin is only distributed when the axis is over-constrained,
        // which is to say when both of its edges are given. Everywhere else it
        // resolves to zero, the same as any other unset margin.
        const bool bothHorizontal = !s.left.isAuto() && !s.right.isAuto();
        const float marginLeft =
            !s.margin.left.isAuto()
                ? box.margin.left
                : (bothHorizontal
                       ? centred(cb.width - leftUsed - rightUsed, borderW,
                                 box.margin.right)
                       : 0.0f);
        float x = cb.x + marginLeft;
        if (!s.left.isAuto()) {
            x = cb.x + leftUsed + marginLeft;
        } else if (!s.right.isAuto()) {
            x = cb.x + cb.width - rightUsed - box.margin.right - borderW;
        }

        // With both vertical edges given the height is whatever is left over, so it
        // is known without measuring. Otherwise the content decides.
        const float topUsed =
            s.top.isAuto() ? 0.0f : s.top.resolve(cb.height, em, rem);
        const float bottomUsed =
            s.bottom.isAuto() ? 0.0f : s.bottom.resolve(cb.height, em, rem);
        float borderH = 0.0f;
        const bool bothVertical = !s.top.isAuto() && !s.bottom.isAuto();
        if (bothVertical) {
            borderH = cb.height - topUsed - bottomUsed - box.margin.top -
                      box.margin.bottom;
            borderH = std::max(0.0f, borderH);
        } else if (box.height > 0.0f) {
            borderH = box.height + box.verticalChrome();
        } else {
            // Measured once to learn the height, then again at the final place so
            // the descendants land where the box actually is.
            borderH = measureAtWidth(el, s, borderW) - box.verticalChrome();
        }

        const float marginTop =
            !s.margin.top.isAuto()
                ? box.margin.top
                : (bothVertical
                       ? centred(cb.height - topUsed - bottomUsed, borderH, box.margin.bottom)
                       : 0.0f);
        float y = cb.y + marginTop;
        if (!s.top.isAuto()) {
            y = cb.y + topUsed + marginTop;
        } else if (!s.bottom.isAuto()) {
            y = cb.y + cb.height - bottomUsed - box.margin.bottom - borderH;
        }
        if (borderH <= 0.0f) {
            borderH = std::max(0.0f, borderH);
        }

        // The offsets and margins are already baked into the placement, so the
        // box lays out as though it were at the origin of its final position.
        runFixed(el, s, borderW, x, y, bothVertical ? borderH : 0.0f);
    }

    float measureAtWidth(Element& el, const ComputedStyle& s, float borderW) {
        UsedBox box = usedBox(s, borderW);
        box.margin = {0.0f, 0.0f, 0.0f, 0.0f};
        el.clearTextRuns();
        const float contentWidth = std::max(
            0.0f, borderW - box.borderWidth.left - box.borderWidth.right -
                     box.padding.left - box.padding.right);
        float width = 0.0f;
        const float height = layoutContents(el, s, box, contentWidth, 0.0f, 0.0f, width);
        el.setLayoutX(0.0f);
        el.setLayoutY(0.0f);
        el.setLayoutWidth(borderW);
        el.setLayoutHeight(height + verticalEdges(box.padding, box.borderWidth));
        return el.layoutHeight();
    }

    // Lays out an already-placed out-of-flow box. The offsets and auto margins were
    // resolved by the caller, so this one pass puts the box and its descendants in
    // their final positions.
    // `definiteHeight` is non-zero when both vertical offsets pinned the height, in
    // which case the content does not get a say.
    void runFixed(Element& el, const ComputedStyle& s, float borderW, float x, float y,
                  float definiteHeight = 0.0f) {
        UsedBox box = usedBox(s, borderW);
        box.margin = {0.0f, 0.0f, 0.0f, 0.0f};
        el.clearTextRuns();
        const float contentX = x + box.borderWidth.left + box.padding.left;
        const float contentY = y + box.borderWidth.top + box.padding.top;
        const float contentWidth = std::max(
            0.0f, borderW - box.borderWidth.left - box.borderWidth.right -
                     box.padding.left - box.padding.right);
        Element* const savedOwner = cbOwner_;
        cbOwner_ = &el;
        float width = 0.0f;
        float height = layoutContents(el, s, box, contentWidth, contentX, contentY, width);
        cbOwner_ = savedOwner;
        if (definiteHeight > 0.0f) {
            height = definiteHeight - verticalEdges(box.padding, box.borderWidth);
        } else if (box.height > 0.0f) {
            height = box.height;
        }
        width = applyMinMax(width, box);
        el.setLayoutX(x);
        el.setLayoutY(y);
        el.setLayoutWidth(
            std::max(borderW, width + horizontalEdges(box.padding, box.borderWidth)));
        el.setLayoutHeight(height + verticalEdges(box.padding, box.borderWidth));
        recordBoxes(el, contentX, contentY, width, height, box);
    }

    // The padding and content boxes, in window coordinates. `overflow` crops to the
    // padding box and `background-clip: content-box` is measured against the content
    // box, and the two are only one padding-width apart.
    void recordBoxes(Element& el, float x, float y, float width, float height,
                     const UsedBox& box) {
        el.setPaddingX(x - box.padding.left);
        el.setPaddingY(y - box.padding.top);
        el.setPaddingWidth(std::max(0.0f, width + box.padding.left + box.padding.right));
        el.setPaddingHeight(std::max(0.0f, height + box.padding.top + box.padding.bottom));
        el.setContentX(x);
        el.setContentY(y);
        el.setContentWidth(std::max(0.0f, width));
        el.setContentHeight(std::max(0.0f, height));
    }

private:
    std::vector<Pending> pending_;
    std::vector<Relative> relative_;
    std::vector<Sticky> sticky_;

    // The padding box an out-of-flow box resolves against. Set while laying out a
    // positioned element, so its own out-of-flow descendants anchor to it.
    Element* cbOwner_ = nullptr;

    // The floats currently in effect for in-flow descendants. A block container
    // that declares floats owns this context; its in-flow children inherit it so
    // their line boxes wrap around the floats.
    FloatContext* floats_ = nullptr;

    bool laidOut(Element& el) const {
        auto found = laidOutCache_.find(&el);
        if (found != laidOutCache_.end()) {
            return found->second;
        }
        bool result = false;
        if (el.isVisual()) {
            const ComputedStyle cs = resolver_.resolve(el);
            result = cs.display != ComputedStyle::None && cs.visible;
        }
        laidOutCache_.emplace(&el, result);
        return result;
    }

    bool hasBlockChild(Element& el) const {
        for (const auto& child : el.children()) {
            if (child && !child->isTextNode() && laidOut(*child)) {
                const ComputedStyle cs = resolver_.resolve(*child);
                // A floated child is taken out of flow, so it does not force the
                // container into block layout; it is placed while its siblings flow.
                if (isBlockLevel(cs) && !isFloated(cs)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool hasFloatChild(Element& el) const {
        auto found = floatChildCache_.find(&el);
        if (found != floatChildCache_.end()) {
            return found->second;
        }
        bool result = false;
        for (const auto& child : el.children()) {
            if (child && !child->isTextNode() && laidOut(*child) &&
                isFloated(resolver_.resolve(*child))) {
                result = true;
                break;
            }
        }
        floatChildCache_.emplace(&el, result);
        return result;
    }

    // Places a floated child's margin box against the container's left or right
    // edge at `startY`, lowering it past any floats that leave too little room.
    // `contentWidth`/`contentX` describe the containing block's content box. The
    // float is laid out with the width CSS gives it: its used width, or a
    // shrink-to-fit width capped at the container.
    void placeFloat(Element& child, const ComputedStyle& cs, float contentWidth,
                    float contentX, float startY) {
        if (!floats_) {
            return;
        }
        FloatContext& ctx = *floats_;
        const bool left = cs.floatType == ComputedStyle::FloatLeft;
        const UsedBox box = usedBox(cs, contentWidth);

        // The float's own contents do not wrap around the floats beside it, so it
        // lays out with no active context (it may still declare its own).
        FloatContext* const saved = floats_;
        floats_ = nullptr;

        child.clearTextRuns();
        float borderW = box.width;
        if (borderW <= 0.0f) {
            // Shrink to fit: the natural width, capped at the container.
            borderW = std::min(contentWidth, probeWidth(child, box));
        }
        borderW += box.horizontalChrome();
        const float outerW = borderW + box.margin.left + box.margin.right;

        // Lower the float until the slice at its top has room for it. Each step
        // drops it to the bottom of the nearest float that got in the way.
        float y = startY;
        float leftEdge = contentX;
        float rightEdge = contentX + contentWidth;
        for (int guard = 0; guard < 1000; ++guard) {
            leftEdge = contentX;
            rightEdge = contentX + contentWidth;
            ctx.available(y, y + 0.01f, leftEdge, rightEdge);
            if (rightEdge - leftEdge >= outerW - 0.01f) {
                break;
            }
            const float next = ctx.nextBottom(y);
            if (next <= y + 0.01f) {
                break;
            }
            y = next;
        }
        const float x = left ? leftEdge : std::max(contentX, rightEdge - outerW);

        run(child, borderW, x, y);
        floats_ = saved;

        const float top = y;
        const float bottom = y + box.margin.top + child.layoutHeight() + box.margin.bottom;
        ctx.add({top, bottom, x, x + outerW, left});
    }

    float layoutBlockContainer(Element& el, const ComputedStyle& s, const UsedBox& box,
                               float availableWidth, float contentX, float contentY,
                               float& outHeight, float& outWidth,
                               bool allowParentChild) {
        float contentWidth = applyMinMax(
            box.width > 0.0f ? box.width
                             : std::max(0.0f, availableWidth - box.horizontalChrome()),
            box);
        if (box.width <= 0.0f) {
            // Auto width still has to fit fixed-width children. Floats are
            // shrink-to-fit, so they never widen the container.
            for (const auto& child : el.children()) {
                if (!child || child->isTextNode() || !laidOut(*child)) {
                    continue;
                }
                const ComputedStyle childStyle = resolver_.resolve(*child);
                if (isFloated(childStyle)) {
                    continue;
                }
                const UsedBox childBox = usedBox(childStyle, contentWidth);
                if (childBox.width > 0.0f) {
                    contentWidth = applyMinMax(
                        std::max(contentWidth, childBox.width + childBox.horizontalChrome()),
                        box);
                }
            }
        }

        // Margin collapsing needs a pure block flow: no line boxes, floats,
        // clearance, or inline-level boxes between the blocks. Otherwise the
        // existing additive flow is kept, and run() had already left this
        // container's own margins additive too.
        const bool allBlockFlow = pureBlockFlow(el, s);
        const bool suppressFirstTop =
            allowParentChild && canCollapseTop(el, s, contentWidth);
        const bool suppressLastBottom =
            allowParentChild && canCollapseBottom(el, s, box);

        FloatContext* const ctx = floats_;
        float cursorY = 0.0f;
        for (const auto& child : el.children()) {
            if (!child || child->isTextNode() || !laidOut(*child)) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (isFloated(cs)) {
                // A float takes no flow space; it sits beside whatever follows.
                if (ctx) {
                    placeFloat(*child, cs, contentWidth, contentX, contentY + cursorY);
                }
                continue;
            }
            if (ctx && cs.clearType != ComputedStyle::ClearNone) {
                // `clear` drops the box below the floats on the named side.
                const float clearY = ctx->clearBottom(cs.clearType, contentY);
                cursorY = std::max(cursorY, clearY - contentY);
            }
            const bool collapseChild = allBlockFlow && isBlockLevel(cs);
            cursorY += run(*child, contentWidth, contentX, contentY + cursorY,
                           collapseChild);
        }
        if (allBlockFlow) {
            outHeight = collapseBlockMargins(el, contentY, contentWidth,
                                             suppressFirstTop, suppressLastBottom);
        } else {
            outHeight = cursorY;
        }
        outWidth = contentWidth;
        return outHeight;
    }

    // Zeroes a subtree past a `line-clamp` cap: no box, no runs, nothing to
    // paint or hit. Layout never ran here, so whatever geometry lingers is
    // from an earlier pass and must go.
    static void clearSubtree(Element& el) {
        el.clearTextRuns();
        el.setLayoutX(0.0f);
        el.setLayoutY(0.0f);
        el.setLayoutWidth(0.0f);
        el.setLayoutHeight(0.0f);
        for (const auto& child : el.children()) {
            if (child) {
                clearSubtree(*child);
            }
        }
    }

    // Widest the element's own contents want when nothing wraps. The layout
    // this writes is discarded: run() repeats it at the real width.
    float probeWidth(Element& el, const UsedBox& box) {
        float width = 0.0f;
        layoutContents(el, resolver_.resolve(el), box, kUnbounded, 0.0f, 0.0f, width);
        return width;
    }

    // The natural content size on one axis, used as a flex item's automatic
    // basis. Hidden content has no natural content, so its placeholder is the
    // authored size or contain-intrinsic-size rather than the unbounded line.
    float measureNatural(Element& el, const ComputedStyle& s, bool row) {
        if (s.contentVisibility == ComputedStyle::ContentHidden) {
            const float em = s.fontSize;
            const float rem = resolver_.rootFontSize();
            if (row) {
                return s.width.isAuto()
                           ? std::max(0.0f, s.containIntrinsicWidth.resolve(
                                               kUnbounded, em, rem))
                           : std::max(0.0f, s.width.resolve(kUnbounded, em, rem));
            }
            return s.height.isAuto()
                       ? std::max(0.0f, s.containIntrinsicHeight.resolve(
                                           kUnbounded, em, rem))
                       : std::max(0.0f, s.height.resolve(kUnbounded, em, rem));
        }
        float width = 0.0f;
        const float height = layoutContents(el, s, usedBox(s, kUnbounded), kUnbounded, 0.0f,
                                            0.0f, width);
        return row ? width : height;
    }

    // The width an auto-width block wants before it has to wrap. Inline content
    // measures itself with an unbounded probe, while stacked block children
    // contribute the widest one plus its chrome and margins. Used for a flex
    // item's cross size in a column, where the item may be auto-width.
    float measureMaxContentWidth(Element& el, const ComputedStyle& s) {
        if (s.contentVisibility == ComputedStyle::ContentHidden) {
            const float em = s.fontSize;
            const float rem = resolver_.rootFontSize();
            return s.width.isAuto()
                       ? std::max(0.0f, s.containIntrinsicWidth.resolve(
                                            kUnbounded, em, rem))
                       : std::max(0.0f, s.width.resolve(kUnbounded, em, rem));
        }
        const UsedBox box = usedBox(s, kUnbounded);
        if (box.width > 0.0f) {
            return box.width;
        }
        if (s.isFlexContainer() || s.isGridContainer() || s.isTableContainer()) {
            float width = 0.0f;
            layoutContents(el, s, box, kUnbounded, 0.0f, 0.0f, width);
            return std::max(0.0f, width);
        }
        if (!hasBlockChild(el)) {
            return std::max(0.0f, probeWidth(el, box));
        }
        float widest = 0.0f;
        for (const auto& child : el.children()) {
            if (!child || child->isTextNode() || !laidOut(*child)) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::None || !cs.visible ||
                cs.position == ComputedStyle::Absolute ||
                cs.position == ComputedStyle::Fixed) {
                continue;
            }
            const UsedBox cb = usedBox(cs, kUnbounded);
            float childWidth = 0.0f;
            if (cb.width > 0.0f) {
                childWidth = cb.width;
            } else if (isBlockLevel(cs)) {
                childWidth = measureMaxContentWidth(*child, cs);
            } else {
                childWidth = std::max(0.0f, probeWidth(*child, cb));
            }
            const float outer = childWidth + cb.horizontalChrome() +
                                (cs.margin.left.isAuto() ? 0.0f : cb.margin.left) +
                                (cs.margin.right.isAuto() ? 0.0f : cb.margin.right);
            widest = std::max(widest, outer);
        }
        return widest;
    }

    // The width below which the content cannot shrink without overflow. An
    // inline probe at zero width wraps at every opportunity, so its widest line
    // is the longest unbreakable run. Block children stack, so the widest one
    // wins rather than their sum.
    float measureMinContent(Element& el, const ComputedStyle& s) {
        if (s.contentVisibility == ComputedStyle::ContentHidden ||
            s.isFlexContainer() || s.isGridContainer() || s.isTableContainer()) {
            // Their intrinsic sizing has its own min/max rules; the table
            // algorithm uses the max-content probe as a safe floor here.
            return measureNatural(el, s, true);
        }
        if (hasBlockChild(el)) {
            float widest = 0.0f;
            for (const auto& child : el.children()) {
                if (!child || child->isTextNode() || !laidOut(*child)) {
                    continue;
                }
                const ComputedStyle cs = resolver_.resolve(*child);
                if (!isBlockLevel(cs) || isFloated(cs)) {
                    continue;
                }
                const UsedBox box = usedBox(cs, 0.0f);
                const float outer =
                    box.width > 0.0f
                        ? box.width + box.horizontalChrome()
                        : measureMinContent(*child, cs) + box.horizontalChrome();
                widest = std::max(widest, outer);
            }
            return widest;
        }
        ComputedStyle probe = s;
        probe.width = decltype(probe.width){};
        float width = 0.0f;
        layoutContents(el, probe, usedBox(probe, 0.0f), 0.0f, 0.0f, 0.0f, width);
        return std::max(0.0f, width);
    }

    // Lays out `el` with a style the caller has already decided, rather than the
    // one the cascade produces. Flex sizing needs this: the algorithm picks the
    // item's main size, and the item has to lay out at that size.
    void runWith(Element& el, const ComputedStyle& sized, float availableWidth, float originX,
                 float originY) {
        const UsedBox box = usedBox(sized, availableWidth);
        el.clearTextRuns();
        if (sized.display == ComputedStyle::None || !sized.visible) {
            el.setLayoutX(originX);
            el.setLayoutY(originY);
            el.setLayoutWidth(0.0f);
            el.setLayoutHeight(0.0f);
            return;
        }
        const float contentX =
            originX + box.margin.left + box.borderWidth.left + box.padding.left;
        const float contentY = originY + box.margin.top + box.borderWidth.top + box.padding.top;

        Element* const savedOwner = cbOwner_;
        if (sized.position != ComputedStyle::Static) {
            cbOwner_ = &el;
        }
        float width = 0.0f;
        float height = layoutContents(el, sized, box, availableWidth, contentX, contentY, width);
        cbOwner_ = savedOwner;
        if (box.height > 0.0f) {
            height = box.height;
        }
        width = applyMinMax(width, box);
        el.setLayoutX(originX + box.margin.left);
        el.setLayoutY(originY + box.margin.top);
        el.setLayoutWidth(width + horizontalEdges(box.padding, box.borderWidth));
        el.setLayoutHeight(height + verticalEdges(box.padding, box.borderWidth));
        recordBoxes(el, contentX, contentY, width, height, box);
    }

    float layoutContents(Element& el, const ComputedStyle& s, const UsedBox& box,
                         float availableWidth, float contentX, float contentY,
                         float& outWidth, bool allowParentChild = false) {
        if (s.contentVisibility == ComputedStyle::ContentHidden) {
            // Hidden content contributes no boxes: drop everything below this
            // element so a previous pass cannot leave runs at old positions,
            // then use the CSS size or contain-intrinsic-size placeholder.
            el.clearLayoutSubtree();
            const float em = s.fontSize;
            const float rem = resolver_.rootFontSize();
            if (box.width > 0.0f) {
                outWidth = box.width;
            } else if (isBlockLevel(s)) {
                outWidth = std::max(0.0f, availableWidth - box.horizontalChrome());
            } else {
                outWidth = std::max(
                    0.0f, s.containIntrinsicWidth.resolve(availableWidth, em, rem));
            }
            return std::max(
                0.0f, s.containIntrinsicHeight.resolve(viewportHeight_, em, rem));
        }
        if (s.isFlexContainer()) {
            return layoutFlexContainer(el, s, box, availableWidth, contentX, contentY,
                                       outWidth);
        }
        if (s.isGridContainer()) {
            return layoutGridContainer(el, s, box, availableWidth, contentX, contentY,
                                       outWidth);
        }
        if (s.isTableContainer()) {
            return layoutTable(el, s, box, availableWidth, contentX, contentY, outWidth);
        }

        // A container that declares floats owns a context for them; in-flow
        // descendants inherit it so their lines wrap beside the floats. A
        // container without floats simply passes the ancestor's context through.
        // A floated `::first-letter` is synthetic, so the rule makes the
        // container own a context even though no child element is floated.
        bool syntheticFirstLetterFloat = false;
        if (resolver_.hasFirstLetterRules() && el.tag() != "input" &&
            el.tag() != "textarea" && el.tag() != "select") {
            const PseudoFont letter = resolver_.firstLetterFont(el, s);
            syntheticFirstLetterFloat =
                letter.floatSet && letter.floatType != ComputedStyle::FloatNone;
        }
        const bool ownsFloats = hasFloatChild(el) || syntheticFirstLetterFloat;
        FloatContext floatCtx;
        FloatContext* const savedFloats = floats_;
        if (ownsFloats) {
            floats_ = &floatCtx;
        }

        float height = 0.0f;
        if (hasBlockChild(el)) {
            layoutBlockContainer(el, s, box, availableWidth, contentX, contentY, height,
                                 outWidth, allowParentChild);
        } else {
            height = layoutInlineContainer(el, s, box, availableWidth, contentX, contentY,
                                           outWidth);
        }

        // A block box contains the floats it declares, so they extend its height
        // rather than spilling over whatever follows it.
        if (ownsFloats) {
            height = std::max(height, floatCtx.bottom(contentY) - contentY);
            floats_ = savedFloats;
        }
        return height;
    }

    // Flexbox. A container lays its children out along one axis; leftover space goes
    // to items that can grow, and the items are aligned on the other axis. The main
    // axis is the inline axis for a row and the block axis for a column.
    struct FlexItem {
        Element* el = nullptr;
        ComputedStyle style;
        UsedBox box;
        float mainSize = 0.0f;  // content box along the main axis
        float crossSize = 0.0f; // cross extent, borders included
        float mainPos = 0.0f;   // where it lands, offsets included
        float crossPos = 0.0f;
        // Set when min/max pinned the item, so it stops absorbing free space.
        bool frozen = false;
    };

    // One line's worth of items, sized and placed independently of other lines.
    struct FlexLine {
        std::vector<std::size_t> items;
        float crossSize = 0.0f;
        float crossPos = 0.0f;
    };

    // Which edge list belongs to the main and cross axes of this container.
    static float mainChrome(const UsedBox& b, bool row) {
        return row ? horizontalEdges(b.padding, b.borderWidth)
                   : verticalEdges(b.padding, b.borderWidth);
    }
    static float crossChrome(const UsedBox& b, bool row) {
        return row ? verticalEdges(b.padding, b.borderWidth)
                   : horizontalEdges(b.padding, b.borderWidth);
    }
    // Margins measured along one axis, excluding padding and border.
    static float mainMargins(const UsedBox& b, bool row) {
        return row ? horizontalEdges(b.margin, {})
                   : verticalEdges(b.margin, {});
    }
    static float crossMargins(const UsedBox& b, bool row) {
        return row ? verticalEdges(b.margin, {})
                   : horizontalEdges(b.margin, {});
    }

    // min/max clamp on whichever axis this container's main axis is. CSS clamps by
    // max-* first and then by min-*, so min-* wins when they conflict.
    float clampMain(float value, const UsedBox& box, bool row) const {
        const float min = row ? box.minWidth : box.minHeight;
        const float max = row ? box.maxWidth : box.maxHeight;
        if (max > 0.0f) {
            value = std::min(value, max);
        }
        if (min > 0.0f) {
            value = std::max(value, min);
        }
        return std::max(0.0f, value);
    }

    // The same clamp on the cross axis: height for a row, width for a column.
    float clampCross(float value, const UsedBox& box, bool row) const {
        const float min = row ? box.minHeight : box.minWidth;
        const float max = row ? box.maxHeight : box.maxWidth;
        if (max > 0.0f) {
            value = std::min(value, max);
        }
        if (min > 0.0f) {
            value = std::max(value, min);
        }
        return std::max(0.0f, value);
    }

    // Hands any space still unclaimed — or still oversubscribed after a clamp — to
    // the items that are not frozen. CSS calls this the second flexing pass.
    static void redistribute(std::vector<FlexItem>& items, const FlexLine& l, bool row,
                             float gap, float mainAvailable) {
        float occupied = gap * static_cast<float>(l.items.size() - 1);
        for (const std::size_t idx : l.items) {
            const FlexItem& item = items[idx];
            occupied += item.mainSize + mainChrome(item.box, row) +
                        mainMargins(item.box, row);
        }
        const float freeSpace = mainAvailable - occupied;
        if (std::fabs(freeSpace) <= 0.5f) {
            return;
        }
        if (freeSpace > 0.0f) {
            float grow = 0.0f;
            for (const std::size_t idx : l.items) {
                if (!items[idx].frozen) {
                    grow += items[idx].style.flex.grow;
                }
            }
            if (grow <= 0.0f) {
                return;
            }
            for (const std::size_t idx : l.items) {
                if (!items[idx].frozen) {
                    items[idx].mainSize +=
                        freeSpace * (items[idx].style.flex.grow / grow);
                }
            }
            return;
        }
        float shrinkBase = 0.0f;
        for (const std::size_t idx : l.items) {
            if (!items[idx].frozen) {
                shrinkBase += items[idx].mainSize * items[idx].style.flex.shrink;
            }
        }
        if (shrinkBase <= 0.0f) {
            return;
        }
        for (const std::size_t idx : l.items) {
            if (items[idx].frozen) {
                continue;
            }
            const float share =
                items[idx].mainSize * items[idx].style.flex.shrink / shrinkBase;
            items[idx].mainSize = std::max(0.0f, items[idx].mainSize + freeSpace * share);
        }
    }

    // The items a flex or grid container positions: its visual children in
    // `order`-modified document order (equal orders keep document order), with
    // the whitespace-only text nodes between them dropped.
    std::vector<Element*> flexChildren(Element& el) const {
        std::vector<std::pair<int, Element*>> ordered;
        for (const auto& child : el.children()) {
            if (child && !child->isTextNode() && laidOut(*child)) {
                ordered.emplace_back(resolver_.resolve(*child).order, child.get());
            }
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const std::pair<int, Element*>& a,
                            const std::pair<int, Element*>& b) {
                             return a.first < b.first;
                         });
        std::vector<Element*> out;
        out.reserve(ordered.size());
        for (const auto& entry : ordered) {
            out.push_back(entry.second);
        }
        return out;
    }

    float layoutFlexContainer(Element& el, const ComputedStyle& s, const UsedBox& box,
                              float availableWidth, float contentX, float contentY,
                              float& outWidth) {
        const bool row = s.flexDirection == ComputedStyle::Row ||
                         s.flexDirection == ComputedStyle::RowReverse;
        // A container with a definite main-axis size hands the items exactly that
        // much room; an auto one shares whatever the parent offered.
        const float mainAvailable =
            row ? (box.width > 0.0f ? box.width
                                    : std::max(0.0f, availableWidth - box.horizontalChrome()))
                : (box.height > 0.0f
                       ? box.height
                       : std::max(0.0f, viewportHeight_ -
                                            verticalEdges(box.padding, box.borderWidth)));
        const float crossDefinite = row ? box.height : box.width;
        // A row's items are spaced by the column gap and its lines by the row
        // gap; a column container swaps the pair.
        const float mainGap = std::max(0.0f, row ? s.columnGap : s.rowGap);
        const float crossGap = std::max(0.0f, row ? s.rowGap : s.columnGap);

        std::vector<FlexItem> items;
        items.reserve(8);
        for (Element* child : flexChildren(el)) {
            FlexItem item;
            item.el = child;
            item.style = resolver_.resolve(*child);
            item.box = usedBox(item.style, row ? availableWidth : viewportHeight_);
            items.push_back(item);
        }
        if (items.empty()) {
            outWidth = row ? box.width : box.height;
            return box.height > 0.0f ? box.height : 0.0f;
        }

        // 1. Resolve the flexible length of every item. `flex-basis` wins; then the
        //    item's own main-axis size; then the size its content naturally wants.
        for (FlexItem& item : items) {
            float basis = 0.0f;
            if (item.style.flex.basis.isAuto()) {
                basis = row ? item.box.width : item.box.height;
                // A zero basis is only a signal to measure when it came from `auto`:
                // `flex: 1` sets it to a deliberate zero, and content must not
                // override that.
                if (basis <= 0.0f) {
                    item.el->clearTextRuns();
                    basis = measureNatural(*item.el, item.style, row);
                    item.el->clearTextRuns();
                }
            } else {
                basis = item.style.flex.basis.resolve(mainAvailable, item.style.fontSize,
                                                      resolver_.rootFontSize());
            }
            item.mainSize = std::max(0.0f, basis);
        }

        // 2. Collect items into lines. A no-wrap container is always one line, and
        //    is allowed to overflow; a wrapping one breaks before an item that would
        //    not fit.
        const bool wrap = s.flexWrap != ComputedStyle::NoWrap;
        std::vector<FlexLine> lines;
        FlexLine line;
        float used = 0.0f;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const float outer =
                items[i].mainSize + mainChrome(items[i].box, row) +
                mainMargins(items[i].box, row);
            if (wrap && !line.items.empty() && used + mainGap + outer > mainAvailable + 0.5f) {
                lines.push_back(line);
                line = FlexLine{};
                used = 0.0f;
            }
            if (!line.items.empty()) {
                used += mainGap;
            }
            used += outer;
            line.items.push_back(i);
        }
        lines.push_back(line);

        // 3. Resolve each line's flexible lengths: grow or shrink into the free
        //    space, then clamp by min/max.
        for (FlexLine& l : lines) {
            const float gaps = mainGap * static_cast<float>(l.items.size() - 1);
            float outer = gaps;
            float grow = 0.0f;
            float shrinkBase = 0.0f;
            for (const std::size_t idx : l.items) {
                const FlexItem& item = items[idx];
                outer += item.mainSize + mainChrome(item.box, row) + mainMargins(item.box, row);
                grow += item.style.flex.grow;
                shrinkBase += item.mainSize * item.style.flex.shrink;
            }
            const float freeSpace = mainAvailable - outer;
            if (freeSpace > 0.5f && grow > 0.0f) {
                for (const std::size_t idx : l.items) {
                    items[idx].mainSize += freeSpace * (items[idx].style.flex.grow / grow);
                }
            } else if (freeSpace < -0.5f && shrinkBase > 0.0f) {
                // Shrinking is weighted by base size, so a wider item gives up more
                // for the same shrink factor.
                for (const std::size_t idx : l.items) {
                    const float share =
                        items[idx].mainSize * items[idx].style.flex.shrink / shrinkBase;
                    items[idx].mainSize =
                        std::max(0.0f, items[idx].mainSize + freeSpace * share);
                }
            }
            // min/max clamp after flexing, as CSS requires. An item the clamp
            // actually moved is frozen: it can no longer give or take room, so the
            // remaining items settle the difference.
            for (const std::size_t idx : l.items) {
                const float clamped =
                    clampMain(items[idx].mainSize, items[idx].box, row);
                items[idx].frozen = std::fabs(clamped - items[idx].mainSize) > 0.5f;
                items[idx].mainSize = clamped;
            }
            redistribute(items, l, row, mainGap, mainAvailable);
        }

        // 4. Size each line on the cross axis. An item's cross size is its own
        //    cross-axis length, or for an auto one the height/width its content
        //    turned out to need once laid out. A row measures the height at the
        //    item's resolved main size, so wrapping text contributes every line
        //    it will actually use; a column measures the max-content width.
        for (FlexItem& item : items) {
            const bool crossAuto = row ? item.style.height.isAuto()
                                       : item.style.width.isAuto();
            float natural = crossAuto ? 0.0f : (row ? item.box.height : item.box.width);
            if (crossAuto && row) {
                ComputedStyle probe = item.style;
                probe.width = Length(item.mainSize);
                probe.height = Length();
                probe.boxSizing = BoxSizing::ContentBox;
                probe.margin = {};
                const float chrome =
                    verticalEdges(item.box.padding, item.box.borderWidth);
                natural = measureAtWidth(
                              *item.el, probe,
                              item.mainSize +
                                  horizontalEdges(item.box.padding, item.box.borderWidth)) -
                          chrome;
                if (item.style.aspectRatio() > 0.0f) {
                    natural = item.mainSize / item.style.aspectRatio();
                }
            } else if (crossAuto) {
                natural = measureMaxContentWidth(*item.el, item.style);
            }
            item.crossSize = clampCross(natural, item.box, row);
        }
        for (FlexLine& l : lines) {
            l.crossSize = 0.0f;
            for (const std::size_t idx : l.items) {
                const FlexItem& item = items[idx];
                l.crossSize = std::max(l.crossSize,
                                       item.crossSize + crossChrome(item.box, row) +
                                           crossMargins(item.box, row));
            }
        }

        // A single-line container sizes its line to the container's inner cross
        // size when that is definite, which is what makes a row of cards fill its
        // panel. `nowrap` is always single-line; a wrapping container does the
        // same only under `align-content: stretch`, since `align-items` aligns
        // items inside a line rather than sizing the line itself.
        if (lines.size() == 1 && crossDefinite > 0.0f &&
            (s.flexWrap == ComputedStyle::NoWrap ||
             s.alignContent == ComputedStyle::AlignStretch)) {
            lines[0].crossSize = crossDefinite;
        }

        // 5. Stack the lines on the cross axis. An auto cross size is exactly
        //    the space the lines need; a definite one gives wrap-reverse a fixed
        //    far edge to stack against.
        float crossCursor = 0.0f;
        for (FlexLine& l : lines) {
            l.crossPos = crossCursor;
            crossCursor += l.crossSize + crossGap;
        }
        if (!lines.empty()) {
            crossCursor -= crossGap;
        }
        applyWrapReverse(s, lines, crossDefinite > 0.0f ? crossDefinite : crossCursor,
                         crossGap);

        // 6. Place each item on the main axis, honouring `justify-content`.
        for (FlexLine& l : lines) {
            const float gaps = mainGap * static_cast<float>(l.items.size() - 1);
            float outer = gaps;
            for (const std::size_t idx : l.items) {
                const FlexItem& item = items[idx];
                outer += item.mainSize + mainChrome(item.box, row) + mainMargins(item.box, row);
            }
            // Recomputed from the final sizes, because growing changed the total.
            float freeSpace = mainAvailable - outer;
            const float n = static_cast<float>(l.items.size());
            // Positive free space goes to the auto margins before
            // `justify-content` sees it: every auto edge takes an equal share,
            // and `justify-content` then has nothing left to distribute. With
            // no room the auto margins are zero and alignment works as usual.
            int mainAutos = 0;
            for (const std::size_t idx : l.items) {
                const FlexItem& item = items[idx];
                if (row ? item.style.margin.left.isAuto()
                        : item.style.margin.top.isAuto()) {
                    ++mainAutos;
                }
                if (row ? item.style.margin.right.isAuto()
                        : item.style.margin.bottom.isAuto()) {
                    ++mainAutos;
                }
            }
            if (mainAutos > 0 && freeSpace > 0.5f) {
                const float share = freeSpace / static_cast<float>(mainAutos);
                for (const std::size_t idx : l.items) {
                    FlexItem& item = items[idx];
                    if (row) {
                        if (item.style.margin.left.isAuto()) {
                            item.box.margin.left = share;
                        }
                        if (item.style.margin.right.isAuto()) {
                            item.box.margin.right = share;
                        }
                    } else {
                        if (item.style.margin.top.isAuto()) {
                            item.box.margin.top = share;
                        }
                        if (item.style.margin.bottom.isAuto()) {
                            item.box.margin.bottom = share;
                        }
                    }
                }
                freeSpace = 0.0f;
            }
            float before = 0.0f;
            float between = mainGap;
            switch (s.justifyContent) {
                case ComputedStyle::AlignEnd:
                    before = freeSpace;
                    break;
                case ComputedStyle::AlignCenter:
                    before = freeSpace * 0.5f;
                    break;
                case ComputedStyle::AlignSpaceBetween:
                    if (n > 1.0f) {
                        between = mainGap + std::max(0.0f, freeSpace) / (n - 1.0f);
                    }
                    break;
                case ComputedStyle::AlignSpaceAround:
                    // Each item gets an equal slice, half of it on either side.
                    before = std::max(0.0f, freeSpace) / n * 0.5f;
                    between = mainGap + std::max(0.0f, freeSpace) / n;
                    break;
                case ComputedStyle::AlignSpaceEvenly:
                    before = std::max(0.0f, freeSpace) / (n + 1.0f);
                    between = mainGap + std::max(0.0f, freeSpace) / (n + 1.0f);
                    break;
                default:
                    break;
            }

            // A reversed direction lays the same run out from the far end, so the
            // natural order is computed first and then mirrored.
            const bool reverse = s.flexDirection == ComputedStyle::RowReverse ||
                                 s.flexDirection == ComputedStyle::ColumnReverse;
            float cursor = before;
            std::vector<float> natural;
            natural.reserve(l.items.size());
            for (std::size_t k = 0; k < l.items.size(); ++k) {
                natural.push_back(cursor);
                const FlexItem& item = items[l.items[k]];
                cursor += item.mainSize + mainChrome(item.box, row) +
                          mainMargins(item.box, row);
                if (k + 1 < l.items.size()) {
                    cursor += between;
                }
            }
            for (std::size_t k = 0; k < l.items.size(); ++k) {
                FlexItem& item = items[l.items[k]];
                const float start = row ? item.box.margin.left : item.box.margin.top;
                const float end = row ? item.box.margin.right : item.box.margin.bottom;
                // `natural[k]` is the item's margin-box start, so mirroring has
                // to move the whole margin box, start margin included.
                const float outerW = start + item.mainSize + end +
                                     mainChrome(item.box, row);
                const float pos = reverse ? mainAvailable - (natural[k] + outerW)
                                          : natural[k];
                item.mainPos = pos + start;
            }
        }

        // 7. Place each item on the cross axis, honouring `align-self` over
        //    `align-items`, and lay it out at the size decided above.
        float maxMain = 0.0f;
        float maxCross = 0.0f;
        for (FlexLine& l : lines) {
            for (const std::size_t idx : l.items) {
                FlexItem& item = items[idx];
                const ComputedStyle::FlexAlign align =
                    item.style.alignSelf != ComputedStyle::AlignStretch
                        ? item.style.alignSelf
                        : s.alignItems;
                // `align-self: stretch` fills the line only when the item has no
                // definite size on the cross axis and neither cross margin is
                // auto: an auto margin takes the free space instead, which is
                // what makes the margin win over the alignment.
                float crossContent = item.crossSize;
                const bool crossSizeAuto = row ? item.style.height.isAuto()
                                               : item.style.width.isAuto();
                const bool crossStartAuto =
                    row ? item.style.margin.top.isAuto()
                        : item.style.margin.left.isAuto();
                const bool crossEndAuto =
                    row ? item.style.margin.bottom.isAuto()
                        : item.style.margin.right.isAuto();
                const bool crossAuto = crossStartAuto || crossEndAuto;
                float crossStart = row ? item.box.margin.top : item.box.margin.left;
                float crossEnd = row ? item.box.margin.bottom : item.box.margin.right;
                if (align == ComputedStyle::AlignStretch && crossSizeAuto &&
                    !crossAuto) {
                    crossContent = std::max(0.0f, l.crossSize - crossChrome(item.box, row) -
                                                     crossStart - crossEnd);
                }
                float freeSpace =
                    std::max(0.0f, l.crossSize - crossStart - crossChrome(item.box, row) -
                                       crossContent - crossEnd);
                if (crossAuto) {
                    if (crossStartAuto && crossEndAuto) {
                        crossStart = freeSpace * 0.5f;
                        crossEnd = freeSpace - crossStart;
                    } else if (crossStartAuto) {
                        crossStart = freeSpace;
                        crossEnd = 0.0f;
                    } else {
                        crossStart = 0.0f;
                        crossEnd = freeSpace;
                    }
                    if (row) {
                        item.box.margin.top = crossStart;
                        item.box.margin.bottom = crossEnd;
                    } else {
                        item.box.margin.left = crossStart;
                        item.box.margin.right = crossEnd;
                    }
                    item.crossPos = l.crossPos + crossStart;
                } else if (align == ComputedStyle::AlignEnd) {
                    item.crossPos = l.crossPos + freeSpace + crossStart;
                } else if (align == ComputedStyle::AlignCenter) {
                    item.crossPos = l.crossPos + freeSpace * 0.5f + crossStart;
                } else {
                    item.crossPos = l.crossPos + crossStart;
                }

                // Both sizes are content-box lengths, since `resolveBox` already
                // reduced whatever the author wrote, so they become the item's width
                // and height directly and `box-sizing` has to stop agreeing to reduce
                // them again. The margins are cleared because the algorithm already
                // spent them in `mainPos`/`crossPos`, and `run` would otherwise add
                // them a second time.
                ComputedStyle sized = item.style;
                sized.width = Length(std::max(0.0f, row ? item.mainSize : crossContent));
                sized.height = Length(std::max(0.0f, row ? crossContent : item.mainSize));
                sized.boxSizing = BoxSizing::ContentBox;
                sized.margin = {};

                // `mainPos` and `crossPos` already have the start margin added, so
                // the item is placed straight from them.
                const float x = contentX + (row ? item.mainPos : item.crossPos);
                const float y = contentY + (row ? item.crossPos : item.mainPos);
                runWith(*item.el, sized, std::max(0.0f, sized.width.resolve()), x, y);
                // How far the items reach along each axis. For a column the main
                // extent is a height and the cross one a width.
                // `mainPos`/`crossPos` are the border-box starts, so the
                // reach adds only the trailing margin; the leading one is
                // already inside the position.
                maxMain = std::max(maxMain, item.mainPos +
                                                 (row ? item.el->layoutWidth()
                                                      : item.el->layoutHeight()) +
                                                 (row ? item.box.margin.right
                                                      : item.box.margin.bottom));
                maxCross = std::max(maxCross, item.crossPos +
                                                  (row ? item.el->layoutHeight()
                                                       : item.el->layoutWidth()) +
                                                  (row ? item.box.margin.bottom
                                                       : item.box.margin.right));
            }
        }

        // `maxMain` is how far the items reach along the main axis and `maxCross`
        // along the cross one, so each maps back to a different physical axis. A
        // container with no authored size on that axis is as large as its content,
        // which is what makes an `inline-flex` box shrink to fit its items.
        outWidth = box.width > 0.0f ? box.width : (row ? maxMain : maxCross);
        return box.height > 0.0f ? box.height : (row ? maxCross : maxMain);
    }

    // `wrap-reverse` stacks the lines from the far end of the cross axis instead
    // of the near one. Only the line offsets change; items keep their place inside
    // their line.
    void applyWrapReverse(const ComputedStyle& s, std::vector<FlexLine>& lines,
                          float crossExtent, float crossGap) {
        if (s.flexWrap != ComputedStyle::WrapReverse) {
            return;
        }
        // The cross-start edge is the far edge, so the first line occupies the
        // bottom (or right) edge and later lines stack back towards the start.
        float cursor = std::max(0.0f, crossExtent);
        for (FlexLine& line : lines) {
            cursor -= line.crossSize;
            line.crossPos = cursor;
            cursor -= crossGap;
        }
    }

    // Grid. A container places its children row-major into a track list sized by
    // `grid-template-columns`; rows are content-sized unless `grid-template-rows`
    // gives them tracks. Track sizes are fixed lengths, `fr` shares of the leftover
    // space, or `auto` content widths -- enough for the common card and form grids.
    float layoutGridContainer(Element& el, const ComputedStyle& s, const UsedBox& box,
                              float availableWidth, float contentX, float contentY,
                              float& outWidth) {
        std::vector<Element*> children = flexChildren(el);
        const float contentWidth =
            box.width > 0.0f ? box.width
                             : std::max(0.0f, availableWidth - box.horizontalChrome());

        std::vector<ComputedStyle::GridTrack> columns = s.gridColumns;
        std::vector<std::vector<std::string>> columnLines;
        const bool columnsAutoFill = s.gridColumnsAutoFill && !columns.empty();
        if (columns.empty()) {
            if (s.gridTemplateAreasColumns > 0) {
                for (std::size_t c = 0; c < s.gridTemplateAreasColumns; ++c) {
                    ComputedStyle::GridTrack t;
                    t.isAuto = true;
                    columns.push_back(t);
                }
            } else {
                ComputedStyle::GridTrack only;
                only.isAuto = true;
                columns.push_back(only);
            }
            columnLines.assign(columns.size() + 1, {});
        } else {
            columnLines = s.gridColumnLines;
            if (columnLines.size() != columns.size() + 1) {
                columnLines.assign(columns.size() + 1, {});
            }
            // Named areas can define more columns than the explicit track list.
            // Keep the explicit tracks first and add content-sized ones.
            if (!columnsAutoFill) {
                while (columns.size() < s.gridTemplateAreasColumns) {
                    ComputedStyle::GridTrack track;
                    track.isAuto = true;
                    columns.push_back(track);
                    columnLines.emplace_back();
                }
            }
        }
        // `repeat(auto-fill/auto-fit, <body>)`: the stored list is one repetition of
        // the body, so repeat it to as many tracks as the container fits, each at
        // least as wide as the body's own minimum. `auto-fit` drops the tracks no
        // item lands in, which for this row-major placement is the same as capping
        // the count at the number of items.
        if (columnsAutoFill) {
            const float em = s.fontSize;
            const float rem = resolver_.rootFontSize();
            const float autoGap = std::max(0.0f, s.columnGap);
            float bodyMin = 0.0f;
            for (const ComputedStyle::GridTrack& track : columns) {
                float floorSize =
                    track.hasMin ? track.minSize.resolve(contentWidth, em, rem) : 0.0f;
                if (!track.isFr && !track.isAuto) {
                    floorSize = std::max(floorSize, track.size.resolve(contentWidth, em, rem));
                }
                bodyMin += std::max(0.0f, floorSize);
            }
            bodyMin += autoGap * static_cast<float>(columns.size() - 1);
            const float denominator = bodyMin + autoGap;
            std::size_t repeatCount =
                denominator > 0.0f
                    ? static_cast<std::size_t>(std::floor((contentWidth + autoGap) / denominator))
                    : 1;
            if (repeatCount < 1) {
                repeatCount = 1;
            }
            if (s.gridColumnsAutoFit && !children.empty()) {
                repeatCount = std::min(repeatCount, children.size());
            }
            const std::size_t bodyTracks = columns.size();
            const std::vector<std::vector<std::string>> bodyLines = columnLines;
            std::vector<ComputedStyle::GridTrack> expanded;
            expanded.reserve(repeatCount * bodyTracks);
            for (std::size_t n = 0; n < repeatCount; ++n) {
                for (const ComputedStyle::GridTrack& track : columns) {
                    expanded.push_back(track);
                }
            }
            columns = std::move(expanded);

            // Repeat the body's line names the same way as its tracks. The final
            // line of one repetition and the first line of the next share a line.
            columnLines.assign(1, {});
            for (std::size_t n = 0; n < repeatCount; ++n) {
                for (const std::string& name : bodyLines.front()) {
                    if (std::find(columnLines.back().begin(), columnLines.back().end(),
                                  name) == columnLines.back().end()) {
                        columnLines.back().push_back(name);
                    }
                }
                for (std::size_t line = 1; line < bodyLines.size(); ++line) {
                    columnLines.push_back(bodyLines[line]);
                }
            }
        }
        std::size_t columnCount = columns.size();
        if (columnLines.size() < columnCount + 1) {
            columnLines.resize(columnCount + 1);
        }

        std::vector<std::vector<std::string>> rowLines = s.gridRowLines;
        const std::size_t rowLineCount =
            std::max(s.gridRows.size(), s.gridTemplateAreasRows);
        if (rowLines.size() < rowLineCount + 1) {
            rowLines.resize(rowLineCount + 1);
        }
        // Named areas generate `<area>-start` / `<area>-end` line names on both
        // axes, so `grid-column: head-start / head-end` works alongside
        // `grid-area: head`.
        auto addLineName = [](std::vector<std::vector<std::string>>& lines,
                              std::size_t line, const std::string& name) {
            if (lines.size() <= line) {
                lines.resize(line + 1);
            }
            if (std::find(lines[line].begin(), lines[line].end(), name) ==
                lines[line].end()) {
                lines[line].push_back(name);
            }
        };
        for (const auto& [areaName, rect] : s.gridTemplateAreas) {
            if (rect.colStart > 0) {
                addLineName(columnLines,
                            static_cast<std::size_t>(rect.colStart - 1),
                            areaName + "-start");
            }
            if (rect.colEnd > 0) {
                addLineName(columnLines,
                            static_cast<std::size_t>(rect.colEnd - 1),
                            areaName + "-end");
            }
            if (rect.rowStart > 0) {
                addLineName(rowLines,
                            static_cast<std::size_t>(rect.rowStart - 1),
                            areaName + "-start");
            }
            if (rect.rowEnd > 0) {
                addLineName(rowLines,
                            static_cast<std::size_t>(rect.rowEnd - 1),
                            areaName + "-end");
            }
        }

        // Named grid lines. A line list has one entry per grid line; line 0 is
        // the leading edge. Occurrences are 1-based and a negative occurrence
        // counts from the end, matching `<name> <integer>`.
        auto resolveNamedLine = [](const std::vector<std::vector<std::string>>& lines,
                                   int line, const std::string& name,
                                   int occurrence) -> int {
            if (line > 0) {
                return line;
            }
            if (name.empty()) {
                return 0;
            }
            std::vector<int> matches;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (std::find(lines[i].begin(), lines[i].end(), name) != lines[i].end()) {
                    matches.push_back(static_cast<int>(i) + 1);
                }
            }
            if (matches.empty()) {
                return 0;
            }
            int index = occurrence > 0
                            ? occurrence - 1
                            : static_cast<int>(matches.size()) + occurrence;
            if (index < 0 || index >= static_cast<int>(matches.size())) {
                return 0;
            }
            return matches[static_cast<std::size_t>(index)];
        };
        auto resolveNextNamedLine = [](const std::vector<std::vector<std::string>>& lines,
                                       int startLine, const std::string& name) -> int {
            if (startLine <= 0 || name.empty()) {
                return 0;
            }
            for (std::size_t i = static_cast<std::size_t>(startLine);
                 i < lines.size(); ++i) {
                if (std::find(lines[i].begin(), lines[i].end(), name) != lines[i].end()) {
                    return static_cast<int>(i) + 1;
                }
            }
            return 0;
        };
        auto resolvePreviousNamedLine =
            [](const std::vector<std::vector<std::string>>& lines,
               int endLine, const std::string& name) -> int {
            if (endLine <= 1 || name.empty()) {
                return 0;
            }
            for (int i = endLine - 2; i >= 0; --i) {
                const auto& names = lines[static_cast<std::size_t>(i)];
                if (std::find(names.begin(), names.end(), name) != names.end()) {
                    return i + 1;
                }
            }
            return 0;
        };

        struct GridPlacement {
            int colStart = 0;
            int rowStart = 0;
            int colSpan = 1;
            int rowSpan = 1;
        };
        auto placementFor = [&](const ComputedStyle& cs) {
            GridPlacement placement;
            placement.colSpan = std::max(1, cs.gridColumnSpan);
            placement.rowSpan = std::max(1, cs.gridRowSpan);
            if (!cs.gridAreaName.empty()) {
                const auto it = s.gridTemplateAreas.find(cs.gridAreaName);
                if (it != s.gridTemplateAreas.end()) {
                    placement.colStart = it->second.colStart;
                    placement.rowStart = it->second.rowStart;
                    placement.colSpan =
                        std::max(1, it->second.colEnd - it->second.colStart);
                    placement.rowSpan =
                        std::max(1, it->second.rowEnd - it->second.rowStart);
                }
                return placement;
            }

            int colStart = resolveNamedLine(columnLines, cs.gridColumnStart,
                                            cs.gridColumnStartName,
                                            cs.gridColumnStartOccurrence);
            int colEnd = resolveNamedLine(columnLines, cs.gridColumnEnd,
                                          cs.gridColumnEndName,
                                          cs.gridColumnEndOccurrence);
            int rowStart = resolveNamedLine(rowLines, cs.gridRowStart,
                                            cs.gridRowStartName,
                                            cs.gridRowStartOccurrence);
            int rowEnd = resolveNamedLine(rowLines, cs.gridRowEnd,
                                          cs.gridRowEndName,
                                          cs.gridRowEndOccurrence);

            if (!cs.gridColumnSpanName.empty()) {
                if (colStart > 0 && colEnd == 0) {
                    const int next = resolveNextNamedLine(columnLines, colStart,
                                                          cs.gridColumnSpanName);
                    if (next > colStart) {
                        colEnd = next;
                    }
                } else if (colStart == 0 && colEnd > 0) {
                    const int previous = resolvePreviousNamedLine(
                        columnLines, colEnd, cs.gridColumnSpanName);
                    if (previous > 0) {
                        colStart = previous;
                    }
                } else if (colStart == 0 && colEnd == 0) {
                    const int first = resolveNamedLine(columnLines, 0,
                                                       cs.gridColumnSpanName, 1);
                    if (first > 0) {
                        colEnd = first;
                        colStart = std::max(1, first - placement.colSpan);
                    }
                }
            }
            if (!cs.gridRowSpanName.empty()) {
                if (rowStart > 0 && rowEnd == 0) {
                    const int next = resolveNextNamedLine(rowLines, rowStart,
                                                          cs.gridRowSpanName);
                    if (next > rowStart) {
                        rowEnd = next;
                    }
                } else if (rowStart == 0 && rowEnd > 0) {
                    const int previous = resolvePreviousNamedLine(
                        rowLines, rowEnd, cs.gridRowSpanName);
                    if (previous > 0) {
                        rowStart = previous;
                    }
                } else if (rowStart == 0 && rowEnd == 0) {
                    const int first = resolveNamedLine(rowLines, 0,
                                                       cs.gridRowSpanName, 1);
                    if (first > 0) {
                        rowEnd = first;
                        rowStart = std::max(1, first - placement.rowSpan);
                    }
                }
            }

            // An end line alone pins the item so it finishes on that line. Since
            // placement is start-based, derive the start from the span.
            if (colStart == 0 && colEnd > 0) {
                colStart = std::max(1, colEnd - std::max(1, placement.colSpan));
            }
            if (rowStart == 0 && rowEnd > 0) {
                rowStart = std::max(1, rowEnd - std::max(1, placement.rowSpan));
            }
            if (colStart > 0 && colEnd > colStart) {
                placement.colSpan = colEnd - colStart;
            }
            if (rowStart > 0 && rowEnd > rowStart) {
                placement.rowSpan = rowEnd - rowStart;
            }
            placement.colStart = colStart;
            placement.rowStart = rowStart;
            return placement;
        };

        // Placement. Items with explicit `grid-column` / `grid-row` start lines are
        // pinned first; the remaining items are auto-placed in row-major order,
        // honouring the occupied cells the pinned items claim. A 1-based CSS line
        // number n maps to 0-based column/row index n-1.
        std::vector<int> cSpan(children.size(), 1);
        std::vector<int> rSpan(children.size(), 1);
        std::vector<int> cStart(children.size(), 0);
        std::vector<int> rStart(children.size(), 0);
        std::vector<std::size_t> itemCol(children.size(), 0);
        std::vector<std::size_t> itemRow(children.size(), 0);
        std::vector<float> naturalWidth(children.size(), 0.0f);
        // hasExplicitCol[i] / hasExplicitRow[i]: true when the item was pinned.
        std::vector<bool> hasExplicitCol(children.size(), false);
        std::vector<bool> hasExplicitRow(children.size(), false);

        for (std::size_t i = 0; i < children.size(); ++i) {
            const ComputedStyle cs = resolver_.resolve(*children[i]);
            const GridPlacement placement = placementFor(cs);
            cSpan[i] = std::min<int>(static_cast<int>(columnCount),
                                     std::max(1, placement.colSpan));
            rSpan[i] = std::max(1, placement.rowSpan);
            cStart[i] = placement.colStart;
            rStart[i] = placement.rowStart;
            if (cStart[i] > 0) {
                // Keep the span inside the explicit grid even when a named end
                // line sits past the last track; the start itself is clamped by
                // pass 1 when it is outside the grid.
                const int startIndex = std::min<int>(
                    static_cast<int>(columnCount) - 1, cStart[i] - 1);
                cSpan[i] = std::min(cSpan[i],
                                    static_cast<int>(columnCount) - startIndex);
            }
            hasExplicitCol[i] = cStart[i] > 0;
            hasExplicitRow[i] = rStart[i] > 0;
            naturalWidth[i] = measureNatural(*children[i], cs, true);
        }

        std::vector<std::vector<bool>> occupied;
        // Ensure rows 0..row exist and every row is wide enough for `columns`.
        // Column-flow auto-placement can widen the grid after rows already exist,
        // so existing rows are grown to match rather than left short.
        auto claimRows = [&occupied](std::size_t row, std::size_t columns) {
            while (occupied.size() <= row) {
                occupied.push_back(std::vector<bool>(columns, false));
            }
            for (std::vector<bool>& r : occupied) {
                if (r.size() < columns) {
                    r.resize(columns, false);
                }
            }
        };

        // Pass 1 – pin items that have an explicit column start and/or row start.
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (!hasExplicitCol[i] && !hasExplicitRow[i]) {
                continue;
            }
            // 1-based CSS line → 0-based index, clamped to [0, columnCount-1].
            const std::size_t c =
                hasExplicitCol[i]
                    ? std::min(columnCount - 1,
                               static_cast<std::size_t>(std::max(1, cStart[i]) - 1))
                    : 0;  // will be resolved in pass 2 for column-auto / row-pinned
            const std::size_t r =
                hasExplicitRow[i]
                    ? static_cast<std::size_t>(std::max(1, rStart[i]) - 1)
                    : 0;  // row-auto: deferred to pass 2
            if (hasExplicitCol[i]) {
                itemCol[i] = c;
            }
            if (hasExplicitRow[i]) {
                itemRow[i] = r;
            }
            // If both axes are explicit, mark the cells occupied now.
            if (hasExplicitCol[i] && hasExplicitRow[i]) {
                const std::size_t csz = static_cast<std::size_t>(cSpan[i]);
                const std::size_t rsz = static_cast<std::size_t>(rSpan[i]);
                claimRows(r + rsz - 1, columnCount);
                for (std::size_t rr = r; rr < r + rsz; ++rr) {
                    for (std::size_t cc = c; cc < c + csz; ++cc) {
                        if (cc < columnCount) {
                            occupied[rr][cc] = true;
                        }
                    }
                }
            }
        }

        // Pass 1b – resolve column-auto / row-explicit items: find the first free
        // column in the pinned row, then mark those cells.
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (!hasExplicitRow[i] || hasExplicitCol[i]) {
                continue;
            }
            const std::size_t r = itemRow[i];
            const std::size_t csz = static_cast<std::size_t>(cSpan[i]);
            const std::size_t rsz = static_cast<std::size_t>(rSpan[i]);
            claimRows(r + rsz - 1, columnCount);
            std::size_t c = 0;
            while (c + csz <= columnCount) {
                bool free = true;
                for (std::size_t rr = r; rr < r + rsz && free; ++rr) {
                    for (std::size_t cc = c; cc < c + csz && free; ++cc) {
                        free = !occupied[rr][cc];
                    }
                }
                if (free) {
                    break;
                }
                ++c;
            }
            if (c + csz > columnCount) {
                c = 0;
            }
            itemCol[i] = c;
            hasExplicitCol[i] = true;
            for (std::size_t rr = r; rr < r + rsz; ++rr) {
                for (std::size_t cc = c; cc < c + csz; ++cc) {
                    if (cc < columnCount) {
                        occupied[rr][cc] = true;
                    }
                }
            }
        }

        // Pass 2 – auto-place remaining items (no explicit placement on either axis,
        // or explicit column only, deferred row resolution).
        const bool isColumnFlow = (s.gridAutoFlow == ComputedStyle::AutoFlowColumn ||
                                   s.gridAutoFlow == ComputedStyle::AutoFlowColumnDense);
        const bool isDense = (s.gridAutoFlow == ComputedStyle::AutoFlowRowDense ||
                              s.gridAutoFlow == ComputedStyle::AutoFlowColumnDense);
        const std::size_t maxRows = std::max<std::size_t>(
            1, std::max(s.gridRows.size(), s.gridTemplateAreasRows));

        std::size_t nextRow = 0;
        std::size_t nextCol = 0;
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (hasExplicitCol[i] && hasExplicitRow[i]) {
                continue;  // fully pinned in pass 1
            }
            const std::size_t csz = static_cast<std::size_t>(cSpan[i]);
            const std::size_t rsz = static_cast<std::size_t>(rSpan[i]);

            if (isColumnFlow) {
                std::size_t c = isDense ? 0 : nextCol;
                std::size_t r = hasExplicitRow[i] ? itemRow[i] : (isDense ? 0 : nextRow);
                while (true) {
                    if (r + rsz > maxRows || (hasExplicitRow[i] && r != itemRow[i])) {
                        r = hasExplicitRow[i] ? itemRow[i] : 0;
                        ++c;
                        continue;
                    }
                    while (columns.size() < c + csz) {
                        columns.push_back(s.hasGridAutoColumns ? s.gridAutoColumns
                                                              : ComputedStyle::GridTrack{Length{}, 0.0f, false, true, false, Length{}});
                    }
                    claimRows(r + rsz - 1, columns.size());
                    bool free = true;
                    for (std::size_t rr = r; rr < r + rsz && free; ++rr) {
                        for (std::size_t cc = c; cc < c + csz && free; ++cc) {
                            free = !occupied[rr][cc];
                        }
                    }
                    if (free) {
                        break;
                    }
                    if (hasExplicitRow[i]) {
                        ++c;
                    } else {
                        ++r;
                    }
                }
                claimRows(r + rsz - 1, columns.size());
                for (std::size_t rr = r; rr < r + rsz; ++rr) {
                    for (std::size_t cc = c; cc < c + csz; ++cc) {
                        occupied[rr][cc] = true;
                    }
                }
                itemCol[i] = c;
                itemRow[i] = r;
                if (!hasExplicitRow[i] && !isDense) {
                    nextRow = r + rsz;
                    nextCol = c;
                    if (nextRow >= maxRows) {
                        nextRow = 0;
                        nextCol = c + 1;
                    }
                }
            } else {
                std::size_t r = isDense ? 0 : nextRow;
                std::size_t c = hasExplicitCol[i] ? itemCol[i] : (isDense ? 0 : nextCol);
                while (true) {
                    if (c + csz > columns.size() || (hasExplicitCol[i] && c != itemCol[i])) {
                        c = hasExplicitCol[i] ? itemCol[i] : 0;
                        ++r;
                        continue;
                    }
                    claimRows(r + rsz - 1, columns.size());
                    bool free = true;
                    for (std::size_t rr = r; rr < r + rsz && free; ++rr) {
                        for (std::size_t cc = c; cc < c + csz && free; ++cc) {
                            free = !occupied[rr][cc];
                        }
                    }
                    if (free) {
                        break;
                    }
                    if (hasExplicitCol[i]) {
                        ++r;
                    } else {
                        ++c;
                    }
                }
                claimRows(r + rsz - 1, columns.size());
                for (std::size_t rr = r; rr < r + rsz; ++rr) {
                    for (std::size_t cc = c; cc < c + csz; ++cc) {
                        if (cc < columns.size()) {
                            occupied[rr][cc] = true;
                        }
                    }
                }
                itemCol[i] = c;
                itemRow[i] = r;
                if (!hasExplicitCol[i] && !isDense) {
                    nextCol = c + csz;
                    nextRow = r;
                    if (nextCol >= columns.size()) {
                        nextCol = 0;
                        nextRow = r + 1;
                    }
                }
            }
        }
        columnCount = columns.size();
        std::size_t rowCount = 0;
        for (std::size_t i = 0; i < children.size(); ++i) {
            rowCount = std::max(rowCount, itemRow[i] + static_cast<std::size_t>(rSpan[i]));
        }

        // Resolve the fixed and auto parts of every column. An `auto` track takes
        // the widest natural size among the items that land in it, divided across
        // any columns they span.
        std::vector<float> widths(columnCount, 0.0f);
        float fixedTotal = 0.0f;
        float frTotal = 0.0f;
        for (std::size_t c = 0; c < columnCount; ++c) {
            const ComputedStyle::GridTrack& track = columns[c];
            // A `minmax()` floor is a base size every kind of track respects.
            const float floor = track.hasMin && !track.minSize.isAuto()
                                    ? std::max(0.0f, track.minSize.resolve(contentWidth, s.fontSize,
                                                                           resolver_.rootFontSize()))
                                    : 0.0f;
            if (track.isFr) {
                frTotal += std::max(0.0f, track.fr);
                widths[c] = floor;
                fixedTotal += floor;
                continue;
            }
            if (track.isAuto) {
                float natural = 0.0f;
                for (std::size_t i = 0; i < children.size(); ++i) {
                    if (!(itemCol[i] <= c && c < itemCol[i] + static_cast<std::size_t>(cSpan[i]))) {
                        continue;
                    }
                    natural = std::max(natural, naturalWidth[i] / static_cast<float>(cSpan[i]));
                }
                widths[c] = std::max(floor, natural);
                fixedTotal += widths[c];
                continue;
            }
            widths[c] = std::max(floor, track.size.resolve(contentWidth, s.fontSize,
                                                           resolver_.rootFontSize()));
            fixedTotal += widths[c];
        }
        // The gap between tracks; the number of interior gaps is one per boundary.
        const float columnGap = std::max(0.0f, s.columnGap);
        const float rowGap = std::max(0.0f, s.rowGap);
        const float gapTotal = columnGap * static_cast<float>(columnCount - 1);
        if (frTotal > 0.0f) {
            const float leftover = std::max(0.0f, contentWidth - fixedTotal - gapTotal);
            for (std::size_t c = 0; c < columnCount; ++c) {
                if (columns[c].isFr) {
                    widths[c] += leftover * (std::max(0.0f, columns[c].fr) / frTotal);
                }
            }
        }

        // Free space left after the tracks and gaps. `justify-content` /
        // `align-content` decide whether it sits before, after, around or between
        // the tracks; the default start leaves it all at the end.
        auto distribute = [](float extra, std::size_t count, ComputedStyle::FlexAlign align,
                             float& lead, float& gap) {
            lead = 0.0f;
            gap = 0.0f;
            if (extra <= 0.0f || count == 0) {
                return;
            }
            switch (align) {
                case ComputedStyle::AlignEnd: lead = extra; break;
                case ComputedStyle::AlignCenter: lead = extra * 0.5f; break;
                case ComputedStyle::AlignSpaceBetween:
                    gap = count > 1 ? extra / static_cast<float>(count - 1) : 0.0f;
                    break;
                case ComputedStyle::AlignSpaceAround:
                    gap = extra / static_cast<float>(count);
                    lead = gap * 0.5f;
                    break;
                case ComputedStyle::AlignSpaceEvenly:
                    gap = extra / static_cast<float>(count + 1);
                    lead = gap;
                    break;
                default: break;
            }
        };

        float trackSum = 0.0f;
        for (float trackWidth : widths) {
            trackSum += trackWidth;
        }
        float columnLead = 0.0f;
        float columnExtra = 0.0f;
        distribute(std::max(0.0f, contentWidth - trackSum - gapTotal), columnCount,
                   s.justifyContent, columnLead, columnExtra);

        std::vector<float> columnX(columnCount, 0.0f);
        float x = columnLead;
        for (std::size_t c = 0; c < columnCount; ++c) {
            columnX[c] = x;
            x += widths[c] + columnGap + columnExtra;
        }

        // Lay each item out at the width `justify-self` gives it, at a temporary
        // vertical spot, so its height can decide the row. The final Y is written
        // once the rows are known, which avoids laying anything out twice.
        std::vector<float> rowHeights(rowCount, 0.0f);
        std::vector<float> itemX(children.size(), 0.0f);
        float maxRight = 0.0f;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const std::size_t col = itemCol[i];
            const std::size_t row = itemRow[i];
            ComputedStyle sized = resolver_.resolve(*children[i]);
            const ComputedStyle::FlexAlign justify =
                sized.justifySelf != ComputedStyle::AlignStretch ? sized.justifySelf
                                                                 : s.justifyItems;
            // The width an item may claim is its own cell plus every cell it spans,
            // with the gaps between them.
            float spanWidth = 0.0f;
            for (std::size_t k = 0; k < static_cast<std::size_t>(cSpan[i]); ++k) {
                spanWidth += widths[col + k];
            }
            spanWidth += columnGap * static_cast<float>(cSpan[i] - 1);
            float w = spanWidth;
            float off = 0.0f;
            if (justify != ComputedStyle::AlignStretch) {
                w = std::min(naturalWidth[i], spanWidth);
                if (justify == ComputedStyle::AlignCenter) {
                    off = (spanWidth - w) * 0.5f;
                } else if (justify == ComputedStyle::AlignEnd) {
                    off = spanWidth - w;
                }
            }
            sized.width = Length(w);
            sized.boxSizing = BoxSizing::ContentBox;
            sized.margin = {};
            // `align-self: stretch` fills a row whose height is fixed by
            // `grid-template-rows`; a content-sized row already matches its tallest
            // item, so there is nothing to stretch to. A row-spanning item would have
            // to measure every row it covers, so it is left alone.
            const ComputedStyle::FlexAlign vAlign =
                sized.alignSelf != ComputedStyle::AlignStretch ? sized.alignSelf
                                                               : s.alignItems;
            if (vAlign == ComputedStyle::AlignStretch && rSpan[i] == 1) {
                const ComputedStyle::GridTrack* rowTrack = nullptr;
                if (row < s.gridRows.size()) {
                    rowTrack = &s.gridRows[row];
                } else if (s.hasGridAutoRows) {
                    rowTrack = &s.gridAutoRows;
                }
                if (rowTrack && !rowTrack->isAuto && !rowTrack->isFr) {
                    const float trackHeight =
                        std::max(0.0f, rowTrack->size.resolve(0.0f, s.fontSize,
                                                              resolver_.rootFontSize()));
                    const float chrome = usedBox(sized, w).verticalChrome();
                    sized.height = Length(std::max(0.0f, trackHeight - chrome));
                }
            }
            runWith(*children[i], sized, w, contentX + columnX[col] + off, contentY);
            itemX[i] = columnX[col] + off;
            // A row-spanning item's height is shared by the rows it covers; a
            // single-row item simply raises its own row, which is the common case.
            const float perRow = std::max(
                0.0f, (children[i]->layoutHeight() -
                       rowGap * static_cast<float>(rSpan[i] - 1)) /
                          static_cast<float>(rSpan[i]));
            for (std::size_t r = row; r < row + static_cast<std::size_t>(rSpan[i]); ++r) {
                rowHeights[r] = std::max(rowHeights[r], perRow);
            }
            maxRight = std::max(maxRight, itemX[i] + children[i]->layoutWidth());
        }

        // `grid-template-rows` and `grid-auto-rows` override content rows with tracks;
        // absent entries leave rows at their tallest items.
        for (std::size_t r = 0; r < rowCount; ++r) {
            const ComputedStyle::GridTrack* track = nullptr;
            if (r < s.gridRows.size()) {
                track = &s.gridRows[r];
            } else if (s.hasGridAutoRows) {
                track = &s.gridAutoRows;
            }
            if (track && !track->isAuto && !track->isFr) {
                rowHeights[r] = std::max(0.0f, track->size.resolve(0.0f, s.fontSize,
                                                                   resolver_.rootFontSize()));
            }
        }
        float rowSum = 0.0f;
        for (float trackHeight : rowHeights) {
            rowSum += trackHeight;
        }
        const float rowGapTotal = rowGap * static_cast<float>(rowCount > 0 ? rowCount - 1 : 0);
        float rowLead = 0.0f;
        float rowExtra = 0.0f;
        distribute(std::max(0.0f, (box.height > 0.0f ? box.height : 0.0f) - rowSum - rowGapTotal),
                   rowCount, s.alignContent, rowLead, rowExtra);

        std::vector<float> rowY(rowCount, 0.0f);
        float y = rowLead;
        for (std::size_t r = 0; r < rowCount; ++r) {
            rowY[r] = y;
            y += rowHeights[r] + rowGap + rowExtra;
        }
        // Second pass: stretch multi-row items to their full span height now that
        // rowY and rowHeights are both finalised. Single-row items were already
        // sized in the first pass (when the row track was known); only multi-row
        // items need this deferred re-layout.
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (rSpan[i] <= 1) {
                continue;
            }
            ComputedStyle cs2 = resolver_.resolve(*children[i]);
            const ComputedStyle::FlexAlign align2 =
                cs2.alignSelf != ComputedStyle::AlignStretch ? cs2.alignSelf : s.alignItems;
            if (align2 != ComputedStyle::AlignStretch) {
                continue;
            }
            const std::size_t row2 = itemRow[i];
            const std::size_t last2 = row2 + static_cast<std::size_t>(rSpan[i]) - 1;
            const float spanH = rowY[last2] + rowHeights[last2] - rowY[row2];
            if (spanH <= 0.0f) {
                continue;
            }
            const UsedBox b2 = usedBox(cs2, children[i]->layoutWidth());
            const float chrome2 = verticalEdges(b2.padding, b2.borderWidth);
            cs2.height = Length(std::max(0.0f, spanH - chrome2));
            cs2.margin = {};
            runWith(*children[i], cs2,
                    children[i]->layoutWidth() - horizontalEdges(b2.padding, b2.borderWidth),
                    contentX + itemX[i], contentY);
        }
        for (std::size_t i = 0; i < children.size(); ++i) {
            const std::size_t row = itemRow[i];
            const ComputedStyle cs = resolver_.resolve(*children[i]);
            const ComputedStyle::FlexAlign align =
                cs.alignSelf != ComputedStyle::AlignStretch ? cs.alignSelf : s.alignItems;
            // The block an item may align inside is every row it spans, gaps
            // included; for a single row that is just its own track.
            const std::size_t last = row + static_cast<std::size_t>(rSpan[i]) - 1;
            const float spanTop = rowY[row];
            const float spanHeight = rowY[last] + rowHeights[last] - spanTop;
            float off = 0.0f;
            if (align == ComputedStyle::AlignCenter) {
                off = (spanHeight - children[i]->layoutHeight()) * 0.5f;
            } else if (align == ComputedStyle::AlignEnd) {
                off = spanHeight - children[i]->layoutHeight();
            }
            children[i]->setLayoutY(contentY + spanTop + off);
        }

        outWidth = box.width > 0.0f ? box.width : maxRight;
        if (box.height > 0.0f) {
            return box.height;
        }
        return rowCount == 0 ? 0.0f : std::max(0.0f, y - rowGap - rowExtra);
    }

    // CSS table layout. Rows (direct, or reached through a row group) are gathered
    // into a grid; a column is as wide as its widest cell and a row as tall as its
    // tallest. `border-spacing` separates the cells unless the borders collapse.
    // Cells honour `colspan` / `rowspan` from their HTML attributes.
    float layoutTable(Element& el, const ComputedStyle& s, const UsedBox& box,
                      float availableWidth, float contentX, float contentY,
                      float& outWidth) {
        const bool separate = s.borderCollapse != ComputedStyle::CollapseCollapse;
        const float spacingX = separate ? s.borderSpacingH : 0.0f;
        const float spacingY = separate ? s.borderSpacingV : 0.0f;

        // One resolved edge of the collapsed border model. The winning edge is
        // painted by one of the adjacent cells, so a shared grid line appears
        // once instead of twice.
        struct BorderEdge {
            float width = 0.0f;
            SDL_Color color = {0, 0, 0, 0};
            ComputedStyle::BorderStyle style = ComputedStyle::BorderStyleNone;
            int priority = -1;
            bool valid = false;
        };
        struct Cell {
            Element* el = nullptr;
            ComputedStyle style;
            ComputedStyle layoutStyle;
            UsedBox box;
            int row = 0;
            int col = 0;
            int colspan = 1;
            int rowspan = 1;
            float width = 0.0f;
            float minWidth = 0.0f;
            float maxWidth = 0.0f;
            BorderEdge top;
            BorderEdge right;
            BorderEdge bottom;
            BorderEdge left;
        };
        struct Row {
            Element* el = nullptr;
            Element* group = nullptr;
            ComputedStyle style;
            ComputedStyle groupStyle;
            float y = 0.0f;
            float height = 0.0f;
            std::vector<Cell> cells;
        };

        std::vector<Row> rows;
        std::vector<Element*> captions;

        auto collectRow = [&](Element* rowEl, Element* groupEl) {
            Row row;
            row.el = rowEl;
            row.group = groupEl;
            row.style = resolver_.resolve(*rowEl);
            if (groupEl) {
                row.groupStyle = resolver_.resolve(*groupEl);
            }
            for (const auto& child : rowEl->children()) {
                if (!child || child->isTextNode() || !laidOut(*child)) {
                    continue;
                }
                const ComputedStyle cs = resolver_.resolve(*child);
                if (!cs.isTableCell()) {
                    continue;
                }
                Cell cell;
                cell.el = child.get();
                cell.style = cs;
                cell.layoutStyle = cs;
                cell.box = usedBox(cs, kUnbounded);
                cell.colspan = spanAttribute(*child, "colspan");
                cell.rowspan = spanAttribute(*child, "rowspan");
                row.cells.push_back(std::move(cell));
            }
            rows.push_back(std::move(row));
        };

        for (const auto& child : el.children()) {
            if (!child || child->isTextNode()) {
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::None || !cs.visible) {
                continue;
            }
            if (cs.isTableRow()) {
                collectRow(child.get(), nullptr);
            } else if (cs.isTableRowGroup()) {
                // Row groups are non-visual nodes, so they are traversed explicitly
                // rather than laid out as boxes.
                for (const auto& gc : child->children()) {
                    if (!gc || gc->isTextNode()) {
                        continue;
                    }
                    if (resolver_.resolve(*gc).isTableRow()) {
                        collectRow(gc.get(), child.get());
                    }
                }
            } else if (cs.display == ComputedStyle::TableCaption) {
                captions.push_back(child.get());
            }
        }

        // Give every cell a column, skipping slots a `rowspan` from an earlier row
        // has already claimed.
        int columns = 0;
        std::vector<int> occupied;  // per column: first row not already claimed
        for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
            int col = 0;
            for (Cell& cell : rows[r].cells) {
                while (col < static_cast<int>(occupied.size()) && occupied[col] > r) {
                    ++col;
                }
                cell.row = r;
                cell.col = col;
                const int end = col + cell.colspan;
                if (end > static_cast<int>(occupied.size())) {
                    occupied.resize(end, 0);
                }
                for (int c = col; c < end; ++c) {
                    occupied[c] = std::max(occupied[c], r + cell.rowspan);
                }
                col = end;
            }
            columns = std::max(columns, col);
        }

        // `<col>` and `<colgroup>` author column widths before any cell is
        // measured. A col inherits its group's width unless it declares one;
        // `span` repeats the definition. These columns also count when the
        // table's cells do not reach them, so a trailing `<col span>` can make
        // an empty column.
        const float availableContent =
            availableWidth > 0.0f && availableWidth < kUnbounded
                ? std::max(0.0f, availableWidth - box.horizontalChrome())
                : kUnbounded;
        const float basis = box.width > 0.0f
                                ? box.width
                                : (availableContent < kUnbounded ? availableContent : 0.0f);
        std::vector<float> hintWidth;
        auto addHint = [&](float width, int span) {
            for (int i = 0; i < span; ++i) {
                hintWidth.push_back(width);
            }
        };
        auto addCol = [&](Element& colEl, float inherited) {
            const ComputedStyle cs = resolver_.resolve(colEl);
            if (cs.display == ComputedStyle::None) {
                return;
            }
            const float width = cs.width.isAuto()
                                    ? inherited
                                    : std::max(0.0f, usedBox(cs, basis).width);
            addHint(width, spanAttribute(colEl, "span"));
        };
        for (const auto& child : el.children()) {
            if (!child || child->isTextNode()) {
                continue;
            }
            if (child->tag() == "colgroup") {
                const ComputedStyle groupStyle = resolver_.resolve(*child);
                if (groupStyle.display == ComputedStyle::None) {
                    continue;
                }
                const float groupWidth =
                    groupStyle.width.isAuto()
                        ? -1.0f
                        : std::max(0.0f, usedBox(groupStyle, basis).width);
                bool sawCol = false;
                for (const auto& gc : child->children()) {
                    if (gc && !gc->isTextNode() && gc->tag() == "col") {
                        sawCol = true;
                        addCol(*gc, groupWidth);
                    }
                }
                if (!sawCol) {
                    addHint(groupWidth, spanAttribute(*child, "span"));
                }
            } else if (child->tag() == "col") {
                addCol(*child, -1.0f);
            }
        }
        columns = std::max(columns, static_cast<int>(hintWidth.size()));
        columns = std::max(1, columns);

        // Collapsed borders: resolve one edge for every cell side. A cell owns its
        // right and bottom edges; left and top are owned by the neighbour, so the
        // shared line is painted once. Outer cell edges are suppressed when the
        // table itself paints a border, and row/row-group borders compete with
        // their cells before the winning edge is handed to the cell that paints
        // it. Losers are zeroed so no second stroke is drawn on top.
        auto styleEdge = [](const ComputedStyle& st, int side) {
            BorderEdge edge;
            const ComputedStyle::BorderStyle style =
                side == 0   ? st.borderStyles.top
                : side == 1 ? st.borderStyles.right
                : side == 2 ? st.borderStyles.bottom
                            : st.borderStyles.left;
            const Length& length = side == 0   ? st.borderWidth.top
                                   : side == 1 ? st.borderWidth.right
                                   : side == 2 ? st.borderWidth.bottom
                                               : st.borderWidth.left;
            edge.style = style;
            edge.width = length.resolve();
            edge.color = st.borderColor;
            edge.valid = style != ComputedStyle::BorderStyleNone && edge.width > 0.0f;
            return edge;
        };
        auto styleRank = [](ComputedStyle::BorderStyle style) {
            switch (style) {
                case ComputedStyle::BorderDouble: return 8;
                case ComputedStyle::BorderSolid: return 7;
                case ComputedStyle::BorderDashed: return 6;
                case ComputedStyle::BorderDotted: return 5;
                case ComputedStyle::BorderRidge: return 4;
                case ComputedStyle::BorderOutset: return 3;
                case ComputedStyle::BorderGroove: return 2;
                case ComputedStyle::BorderInset: return 1;
                default: return 0;
            }
        };
        auto considerEdge = [&](BorderEdge& best, const ComputedStyle& st, int side,
                                int priority) {
            BorderEdge candidate = styleEdge(st, side);
            if (!candidate.valid) {
                return;
            }
            bool wins = !best.valid;
            if (!wins) {
                if (candidate.width > best.width + 0.001f) {
                    wins = true;
                } else if (std::fabs(candidate.width - best.width) <= 0.001f) {
                    const int rank = styleRank(candidate.style);
                    const int bestRank = styleRank(best.style);
                    wins = rank != bestRank ? rank > bestRank
                                            : priority > best.priority;
                }
            }
            if (wins) {
                candidate.priority = priority;
                best = candidate;
            }
        };
        const auto tableHasBorder = [&](int side) {
            return styleEdge(s, side).valid;
        };

        for (Row& row : rows) {
            row.el->clearLayoutBorder();
            if (row.group) {
                row.group->clearLayoutBorder();
            }
            for (Cell& cell : row.cells) {
                cell.el->clearLayoutBorder();
            }
        }

        if (!separate) {
            // Rows and row groups never paint their own border in the collapsed
            // model; their edges have already been folded into a cell.
            const Element::LayoutBorder noBorder;
            for (Row& row : rows) {
                row.el->setLayoutBorder(noBorder);
                if (row.group) {
                    row.group->setLayoutBorder(noBorder);
                }
            }

            std::vector<std::vector<Cell*>> grid(
                rows.size(), std::vector<Cell*>(columns, nullptr));
            for (Row& row : rows) {
                for (Cell& cell : row.cells) {
                    const int endCol = std::min(columns, cell.col + cell.colspan);
                    const int endRow =
                        std::min(static_cast<int>(rows.size()), cell.row + cell.rowspan);
                    for (int r = cell.row; r < endRow; ++r) {
                        for (int c = cell.col; c < endCol; ++c) {
                            grid[r][c] = &cell;
                        }
                    }
                }
            }

            for (Row& row : rows) {
                for (Cell& cell : row.cells) {
                    const int endCol = std::min(columns, cell.col + cell.colspan);
                    const int endRow =
                        std::min(static_cast<int>(rows.size()), cell.row + cell.rowspan);

                    if (cell.col == 0 && !tableHasBorder(3)) {
                        considerEdge(cell.left, cell.style, 3, 3);
                        for (int r = cell.row; r < endRow; ++r) {
                            considerEdge(cell.left, rows[r].style, 3, 2);
                            if (rows[r].group) {
                                considerEdge(cell.left, rows[r].groupStyle, 3, 1);
                            }
                        }
                    }
                    if (endCol >= columns && !tableHasBorder(1)) {
                        considerEdge(cell.right, cell.style, 1, 3);
                        for (int r = cell.row; r < endRow; ++r) {
                            considerEdge(cell.right, rows[r].style, 1, 2);
                            if (rows[r].group) {
                                considerEdge(cell.right, rows[r].groupStyle, 1, 1);
                            }
                        }
                    }
                    if (cell.row == 0 && !tableHasBorder(0)) {
                        considerEdge(cell.top, cell.style, 0, 3);
                        considerEdge(cell.top, rows[0].style, 0, 2);
                        if (rows[0].group) {
                            considerEdge(cell.top, rows[0].groupStyle, 0, 1);
                        }
                    }
                    if (endRow >= static_cast<int>(rows.size()) && !tableHasBorder(2)) {
                        const Row& last = rows[endRow - 1];
                        considerEdge(cell.bottom, cell.style, 2, 3);
                        considerEdge(cell.bottom, last.style, 2, 2);
                        if (last.group) {
                            considerEdge(cell.bottom, last.groupStyle, 2, 1);
                        }
                    }

                    if (endCol < columns) {
                        considerEdge(cell.right, cell.style, 1, 3);
                        for (int r = cell.row; r < endRow; ++r) {
                            if (Cell* neighbour = grid[r][endCol]) {
                                considerEdge(cell.right, neighbour->style, 3, 3);
                            }
                        }
                    }
                    if (endRow < static_cast<int>(rows.size())) {
                        considerEdge(cell.bottom, cell.style, 2, 3);
                        for (int c = cell.col; c < endCol; ++c) {
                            if (Cell* neighbour = grid[endRow][c]) {
                                considerEdge(cell.bottom, neighbour->style, 0, 3);
                            }
                        }
                        const Row& above = rows[endRow - 1];
                        const Row& below = rows[endRow];
                        considerEdge(cell.bottom, above.style, 2, 2);
                        considerEdge(cell.bottom, below.style, 0, 2);
                        if (above.group && above.group != below.group) {
                            considerEdge(cell.bottom, above.groupStyle, 2, 1);
                        }
                        if (below.group && below.group != above.group) {
                            considerEdge(cell.bottom, below.groupStyle, 0, 1);
                        }
                    }
                }
            }

            for (Row& row : rows) {
                for (Cell& cell : row.cells) {
                    cell.layoutStyle = cell.style;
                    const auto apply = [&](int side, const BorderEdge& edge) {
                        Length& width = side == 0   ? cell.layoutStyle.borderWidth.top
                                        : side == 1 ? cell.layoutStyle.borderWidth.right
                                        : side == 2 ? cell.layoutStyle.borderWidth.bottom
                                                    : cell.layoutStyle.borderWidth.left;
                        ComputedStyle::BorderStyle& style =
                            side == 0   ? cell.layoutStyle.borderStyles.top
                            : side == 1 ? cell.layoutStyle.borderStyles.right
                            : side == 2 ? cell.layoutStyle.borderStyles.bottom
                                        : cell.layoutStyle.borderStyles.left;
                        width = Length(edge.valid ? edge.width : 0.0f);
                        style = edge.valid ? edge.style
                                           : ComputedStyle::BorderStyleNone;
                    };
                    apply(0, cell.top);
                    apply(1, cell.right);
                    apply(2, cell.bottom);
                    apply(3, cell.left);
                    cell.box = usedBox(cell.layoutStyle, kUnbounded);

                    Element::LayoutBorder painted;
                    const auto toPainted = [](const BorderEdge& edge) {
                        Element::LayoutBorderSide side;
                        if (edge.valid) {
                            side.width = edge.width;
                            side.color = edge.color;
                            side.style = edge.style;
                        }
                        return side;
                    };
                    painted.top = toPainted(cell.top);
                    painted.right = toPainted(cell.right);
                    painted.bottom = toPainted(cell.bottom);
                    painted.left = toPainted(cell.left);
                    cell.el->setLayoutBorder(painted);
                }
            }
        }

        // Column widths: an authored column width is a floor; authored cell
        // widths and intrinsic minimums establish the lower bound, while the
        // max-content width establishes the upper bound. The table's used width
        // chooses a point between the two, so a column shrinks toward its
        // minimum before it starts squeezing another column below its own.
        std::vector<float> colMin(columns, 0.0f);
        std::vector<float> colMax(columns, 0.0f);
        for (int c = 0; c < static_cast<int>(hintWidth.size()) && c < columns; ++c) {
            if (hintWidth[c] >= 0.0f) {
                colMin[c] = std::max(colMin[c], hintWidth[c]);
                colMax[c] = std::max(colMax[c], hintWidth[c]);
            }
        }

        // Each cell contributes a min-content and max-content width. An authored
        // width and min-width are floors for both, while max-width caps the
        // max-content contribution without pushing it below the minimum.
        for (Row& row : rows) {
            for (Cell& cell : row.cells) {
                const float chrome = cell.box.horizontalChrome();
                const float contentMin = measureMinContent(*cell.el, cell.layoutStyle);
                // A cell's authored width is a floor, not its preferred width:
                // the max-content probe must still see the whole content.
                ComputedStyle maxProbe = cell.layoutStyle;
                maxProbe.width = decltype(maxProbe.width){};
                const float contentMax = measureNatural(*cell.el, maxProbe, true);
                float minW = contentMin + chrome;
                float maxW = contentMax + chrome;
                if (cell.box.width > 0.0f) {
                    minW = std::max(minW, cell.box.width + chrome);
                    maxW = std::max(maxW, cell.box.width + chrome);
                }
                if (cell.box.minWidth > 0.0f) {
                    minW = std::max(minW, cell.box.minWidth + chrome);
                }
                if (cell.box.maxWidth > 0.0f) {
                    maxW = std::min(maxW, cell.box.maxWidth + chrome);
                }
                cell.minWidth = std::max(0.0f, minW);
                cell.maxWidth = std::max(cell.minWidth, maxW);
            }
        }

        for (Row& row : rows) {
            for (Cell& cell : row.cells) {
                if (cell.colspan == 1) {
                    colMin[cell.col] = std::max(colMin[cell.col], cell.minWidth);
                    colMax[cell.col] = std::max(colMax[cell.col], cell.maxWidth);
                }
            }
        }

        // A spanning cell imposes the same constraint on the sum of the columns
        // it covers. Narrow spans are resolved first so a wider span sees their
        // settled contributions; only the deficit is shared, never the excess.
        std::vector<Cell*> spanning;
        for (Row& row : rows) {
            for (Cell& cell : row.cells) {
                if (cell.colspan > 1) {
                    spanning.push_back(&cell);
                }
            }
        }
        std::stable_sort(spanning.begin(), spanning.end(),
                         [](const Cell* a, const Cell* b) {
                             return a->colspan < b->colspan;
                         });
        const auto spanWidth = [&](const std::vector<float>& widths, const Cell& cell) {
            const int end = std::min(columns, cell.col + cell.colspan);
            float width = spacingX * static_cast<float>(std::max(0, end - cell.col - 1));
            for (int c = cell.col; c < end; ++c) {
                width += widths[c];
            }
            return width;
        };
        for (Cell* cell : spanning) {
            const int end = std::min(columns, cell->col + cell->colspan);
            if (end <= cell->col) {
                continue;
            }
            const float have = spanWidth(colMin, *cell);
            if (cell->minWidth > have) {
                const float share =
                    (cell->minWidth - have) / static_cast<float>(end - cell->col);
                for (int c = cell->col; c < end; ++c) {
                    colMin[c] += share;
                }
            }
        }
        for (int c = 0; c < columns; ++c) {
            colMax[c] = std::max(colMax[c], colMin[c]);
        }
        for (Cell* cell : spanning) {
            const int end = std::min(columns, cell->col + cell->colspan);
            if (end <= cell->col) {
                continue;
            }
            const float have = spanWidth(colMax, *cell);
            if (cell->maxWidth > have) {
                const float share =
                    (cell->maxWidth - have) / static_cast<float>(end - cell->col);
                for (int c = cell->col; c < end; ++c) {
                    colMax[c] += share;
                }
            }
        }
        for (int c = 0; c < columns; ++c) {
            colMax[c] = std::max(colMax[c], colMin[c]);
        }

        std::vector<float> colWidth(columns, 0.0f);
        auto columnSpanWidth = [&](const Cell& cell) {
            const int end = std::min(columns, cell.col + cell.colspan);
            float w = spacingX * static_cast<float>(std::max(0, end - cell.col - 1));
            for (int c = cell.col; c < end; ++c) {
                w += colWidth[c];
            }
            return w;
        };
        // The left edge of a column, spacing included: a cell is positioned by the
        // column it occupies, not by where the previous cell in its row ended.
        auto columnLeft = [&](int col) {
            float x = contentX + spacingX;
            for (int c = 0; c < col && c < columns; ++c) {
                x += colWidth[c] + spacingX;
            }
            return x;
        };
        // A cell is as wide as its column: the authored width only informs the
        // column sizing, so the used width is dropped before laying the cell out.
        auto cellSized = [&](const Cell& cell) {
            ComputedStyle sized = cell.layoutStyle;
            sized.width = decltype(sized.width){};
            return sized;
        };

        const float spacingTotal = spacingX * static_cast<float>(columns + 1);
        float sumMin = 0.0f;
        float sumMax = 0.0f;
        for (int c = 0; c < columns; ++c) {
            sumMin += colMin[c];
            sumMax += colMax[c];
        }
        const float minTable = spacingTotal + sumMin;
        const float maxTable = std::max(minTable, spacingTotal + sumMax);

        // Auto tables take the authored width when there is one, otherwise shrink
        // to fit the containing block between the min-content and max-content
        // widths. Fixed tables use the available width as their base and let the
        // first row divide it below.
        float contentWidth = 0.0f;
        if (s.tableLayout == ComputedStyle::TableLayoutFixed) {
            contentWidth = box.width > 0.0f
                               ? std::max(box.width, minTable)
                               : (availableContent < kUnbounded
                                      ? std::max(maxTable, availableContent)
                                      : maxTable);
        } else if (box.width > 0.0f) {
            contentWidth = std::max(box.width, minTable);
        } else if (availableContent < kUnbounded) {
            contentWidth = std::max(minTable, std::min(maxTable, availableContent));
        } else {
            contentWidth = maxTable;
        }

        const float free = std::max(0.0f, contentWidth - spacingTotal);
        if (free >= sumMax) {
            for (int c = 0; c < columns; ++c) {
                colWidth[c] = colMax[c];
            }
            const float extra = free - sumMax;
            if (extra > 0.0f && columns > 0) {
                const float each = extra / static_cast<float>(columns);
                for (float& width : colWidth) {
                    width += each;
                }
            }
        } else if (free > sumMin && sumMax > sumMin) {
            const float ratio = (free - sumMin) / (sumMax - sumMin);
            for (int c = 0; c < columns; ++c) {
                colWidth[c] = colMin[c] + (colMax[c] - colMin[c]) * ratio;
            }
        } else {
            for (int c = 0; c < columns; ++c) {
                colWidth[c] = colMin[c];
            }
        }

        // `table-layout: fixed` sizes columns from the first row: a declared width
        // wins and the rest share what is left.
        if (s.tableLayout == ComputedStyle::TableLayoutFixed && columns > 0) {
            std::vector<float> fixed(columns, -1.0f);
            bool anyFixed = false;
            // A `<col>`/`<colgroup>` width wins over the first row's cell width;
            // cells only fill columns the column definitions left automatic.
            for (int c = 0; c < static_cast<int>(hintWidth.size()) && c < columns; ++c) {
                if (hintWidth[c] >= 0.0f) {
                    fixed[c] = hintWidth[c];
                    anyFixed = true;
                }
            }
            if (!rows.empty()) {
                for (Cell& cell : rows[0].cells) {
                    if (cell.colspan == 1 && cell.box.width > 0.0f &&
                        fixed[cell.col] < 0.0f) {
                        fixed[cell.col] = cell.box.width + cell.box.horizontalChrome();
                        anyFixed = true;
                    }
                }
            }
            const float free = std::max(0.0f, contentWidth - spacingTotal);
            float declared = 0.0f;
            int open = 0;
            for (float w : fixed) {
                if (w >= 0.0f) {
                    declared += w;
                } else {
                    ++open;
                }
            }
            for (int c = 0; c < columns; ++c) {
                if (fixed[c] >= 0.0f) {
                    colWidth[c] = fixed[c];
                } else if (anyFixed && open > 0) {
                    colWidth[c] = std::max(0.0f, free - declared) / static_cast<float>(open);
                } else {
                    colWidth[c] = free / static_cast<float>(columns);
                }
            }
        }

        // Row heights from the single-row cells.
        for (Row& row : rows) {
            float height = std::max(0.0f, row.style.height.resolve(0.0f, row.style.fontSize, 16.0f));
            for (Cell& cell : row.cells) {
                if (cell.rowspan != 1) {
                    continue;
                }
                cell.width = columnSpanWidth(cell);
                const ComputedStyle sized = cellSized(cell);
                const UsedBox sbox = usedBox(sized, cell.width);
                float cw = 0.0f;
                const float content =
                    cell.box.height > 0.0f
                        ? cell.box.height
                        : layoutContents(*cell.el, sized, sbox, cell.width, contentX,
                                         contentY, cw);
                height = std::max(height, content + sbox.verticalChrome());
            }
            row.height = height;
        }
        // A cell spanning several rows can still be taller than they are; the
        // excess is added to the last row it covers.
        for (Row& row : rows) {
            for (Cell& cell : row.cells) {
                if (cell.rowspan <= 1) {
                    continue;
                }
                cell.width = columnSpanWidth(cell);
                const ComputedStyle sized = cellSized(cell);
                const UsedBox sbox = usedBox(sized, cell.width);
                float cw = 0.0f;
                const float content =
                    cell.box.height > 0.0f
                        ? cell.box.height
                        : layoutContents(*cell.el, sized, sbox, cell.width, contentX,
                                         contentY, cw);
                const float wanted = content + sbox.verticalChrome();
                const int last =
                    std::min<int>(static_cast<int>(rows.size()) - 1, cell.row + cell.rowspan - 1);
                float spanned = spacingY * static_cast<float>(cell.rowspan - 1);
                for (int r = cell.row; r <= last; ++r) {
                    spanned += rows[r].height;
                }
                if (wanted > spanned) {
                    rows[last].height += wanted - spanned;
                }
            }
        }

        // A caption spans the table's width and sits above or below the rows
        // depending on `caption-side`.
        auto layCaption = [&](Element* caption, float atY) {
            const ComputedStyle cs = resolver_.resolve(*caption);
            runWith(*caption, cs, contentWidth, contentX, atY);
            const UsedBox captionBox = usedBox(cs, contentWidth);
            return captionBox.margin.top + caption->layoutHeight() + captionBox.margin.bottom;
        };
        float cursor = contentY;
        for (Element* caption : captions) {
            if (s.captionSide != ComputedStyle::CaptionBottom) {
                cursor += layCaption(caption, cursor);
            }
        }

        float y = cursor + spacingY;
        for (Row& row : rows) {
            row.y = y;
            for (Cell& cell : row.cells) {
                cell.width = columnSpanWidth(cell);
                const int last =
                    std::min<int>(static_cast<int>(rows.size()) - 1, cell.row + cell.rowspan - 1);
                float height = spacingY * static_cast<float>(last - cell.row);
                for (int r = cell.row; r <= last; ++r) {
                    height += rows[r].height;
                }
                runWith(*cell.el, cellSized(cell), cell.width, columnLeft(cell.col), y);
                // The cell stretches to the height of the rows it covers.
                cell.el->setLayoutHeight(std::max(cell.el->layoutHeight(), height));
            }
            if (row.el) {
                row.el->clearTextRuns();
                row.el->setLayoutX(contentX);
                row.el->setLayoutY(row.y);
                row.el->setLayoutWidth(contentWidth);
                row.el->setLayoutHeight(row.height);
            }
            y += row.height + spacingY;
        }

        for (Element* caption : captions) {
            if (s.captionSide == ComputedStyle::CaptionBottom) {
                y += layCaption(caption, y);
            }
        }

        outWidth = contentWidth;
        return box.height > 0.0f ? box.height : std::max(0.0f, y - contentY);
    }

    // Every inline descendant of `el` flows into one line-breaking context, so
    // wrapping stays correct across element boundaries: "a <span>b c d</span>"
    // can break inside the span.
    float layoutInlineContainer(Element& el, const ComputedStyle& s, const UsedBox& box,
                                float availableWidth, float contentX, float contentY,
                                float& outWidth) {
        const bool probe = availableWidth >= kUnbounded;
        // A probe measures max-content, so it must not wrap at all; otherwise
        // every word lands on its own line and the result is min-content.
        const float inner =
            box.width > 0.0f ? box.width
                             : probe ? kUnbounded
                                     : std::max(0.0f, availableWidth - box.horizontalChrome());

        // `text-wrap: balance|pretty` re-breaks the paragraph before the real
        // pass: probes run the same breaker at candidate widths and roll their
        // throwaway runs back, then one pass lays out the chosen width. That
        // needs wrapping to be possible at all, and a clamped paragraph keeps
        // its committed lines.
        const bool rebreak = !probe && s.lineClamp <= 0 && inner > 0.0f &&
                             s.textWrapStyle != ComputedStyle::TextWrapStyle::Auto &&
                             s.whiteSpace != ComputedStyle::WrapNone &&
                             s.whiteSpace != ComputedStyle::WrapPre &&
                             s.textWrapMode != ComputedStyle::TextWrapMode::Nowrap;
        const std::size_t runStart = el.textRuns().size();
        const float wrapWidth = rebreak
                                    ? rebreakWidth(el, s, inner, contentX, contentY, runStart)
                                    : inner;

        Flow flow(*this, el, s, metricsFor(font_, s), wrapWidth, contentX, contentY);
        flow.inlineBox(el, s);
        flow.finish();

        if (box.width > 0.0f) {
            outWidth = box.width;
        } else if (probe) {
            outWidth = flow.widest();
        } else {
            outWidth = std::max(inner, flow.widest());
        }
        return flow.cursorY();
    }

    static constexpr float kUnbounded = 1.0e6f;
    static constexpr float kUnboundedProbe = kUnbounded;
    // `text-wrap` re-breaking stops paying off past a handful of lines, and
    // every candidate width costs a full breaker pass.
    static constexpr int kMaxRebreakLines = 6;

    // Greedy line breaking for one inline formatting context.
    class Flow {
    public:
        Flow(Layout& owner, Element& owner_el, const ComputedStyle& s, Metrics m,
             float maxWidth, float originX, float originY)
            : owner_(owner),
              ownerEl_(owner_el),
              style_(s),
              metrics_(m),
              lineHeight_(m.lineHeight),
              maxWidth_(maxWidth),
              originX_(originX),
              originY_(originY),
              spacing_(spacingFor(s.whiteSpace)),
              lineClamp_(maxWidth >= kUnbounded / 2 ? 0 : s.lineClamp) {
            lineRunStartIndex_ = owner_el.textRuns().size();
            // Form controls draw their value, not authored inline content, so
            // the first-letter pseudo never applies to them.
            firstLetterPending_ =
                owner_.hasFirstLetterRules() && owner_el.tag() != "input" &&
                owner_el.tag() != "textarea" && owner_el.tag() != "select";
            if (owner_.hasFirstLineRules()) {
                firstLineFont_ = owner_.firstLineFont(owner_el, s);
                firstLineHasFont_ =
                    firstLineFont_.sizeSet || firstLineFont_.familySet ||
                    firstLineFont_.weightSet || firstLineFont_.styleSet ||
                    firstLineFont_.lineHeightSet;
            }
            if (s.textIndent.unit != sui::Length::Auto) {
                penX_ = std::max(0.0f, s.textIndent.resolve(s.fontSize, s.fontSize, 16.0f));
            }
            syncLine();
        }

        // Applies a winning `::first-line` font/line-height slot to `s`, but
        // only where the run still inherits the block's own value. An inline
        // child that sets its own font keeps it, as the pseudo-element's
        // declarations would lose to a descendant's.
        bool applyFirstLineFont(ComputedStyle& s) const {
            if (!firstLineHasFont_) {
                return false;
            }
            bool changed = false;
            if (firstLineFont_.sizeSet && s.fontSize == style_.fontSize) {
                s.fontSize = firstLineFont_.size;
                changed = true;
            }
            if (firstLineFont_.familySet && s.fontFamily == style_.fontFamily) {
                s.fontFamily = firstLineFont_.familyList.empty()
                                   ? firstLineFont_.family
                                   : firstLineFont_.familyList;
                changed = true;
            }
            if (firstLineFont_.weightSet && s.fontWeight == style_.fontWeight) {
                s.fontWeight = firstLineFont_.weight;
                s.fontBold = firstLineFont_.weight >= 600;
                changed = true;
            }
            if (firstLineFont_.styleSet && s.fontStyle == style_.fontStyle) {
                s.fontStyle = firstLineFont_.style;
                changed = true;
            }
            if (firstLineFont_.lineHeightSet && s.lineHeight == style_.lineHeight) {
                s.lineHeight = firstLineFont_.lineHeight;
                changed = true;
            }
            return changed;
        }

        // Sets the current line's left inset and available width from any floats
        // overlapping the line box. Without floats this leaves the line exactly
        // as wide as the container.
        void syncLine() {
            float left = originX_;
            float right = originX_ + maxWidth_;
            if (owner_.floats_) {
                owner_.floats_->available(originY_ + cursorY_,
                                          originY_ + cursorY_ + lineHeight_, left,
                                          right);
            }
            lineIndent_ = std::max(0.0f, left - originX_);
            lineAvail_ = std::max(0.0f, right - originX_ - lineIndent_);
            if (lineIndent_ > 0.0f || lineAvail_ < maxWidth_) {
                sawFloat_ = true;
            }
        }

        void text(std::string_view text, const ComputedStyle& s) {
            const Metrics m = metricsFor(owner_.font_, s);
            // A smaller font inside a line raises nothing; the line box keeps
            // the container's height.
            const float ascentDelta = std::min(0.0f, m.ascent - metrics_.ascent);
            if (!spacing_.keepsNewlines) {
                words(text, s, m, ascentDelta);
                return;
            }
            if (spacing_.breakSpaces) {
                breakSpaced(text, s, m, ascentDelta);
                return;
            }
            // A newline in the text ends the line instead of collapsing to a space.
            std::size_t start = 0;
            while (start <= text.size()) {
                const std::size_t nl = text.find('\n', start);
                const std::size_t end = nl == std::string_view::npos ? text.size() : nl;
                if (end > start) {
                    if (spacing_.keepsWhitespace) {
                        // Tabs open a gap to the next tab stop; the pieces
                        // around them keep their spaces literally, so no
                        // implicit inter-word space is added between calls.
                        std::size_t from = start;
                        while (from < end) {
                            const std::size_t tab = text.find('\t', from);
                            const std::size_t stop =
                                tab == std::string_view::npos || tab > end ? end : tab;
                            if (stop > from) {
                                word(text.substr(from, stop - from), s, m, ascentDelta, true);
                            }
                            if (tab == std::string_view::npos || tab >= end) {
                                break;
                            }
                            tabGap(s, m, ascentDelta);
                            from = tab + 1;
                        }
                    } else {
                        words(text.substr(start, end - start), s, m, ascentDelta);
                    }
                }
                if (nl == std::string_view::npos) {
                    break;
                }
                newLine();
                start = nl + 1;
            }
        }

        void lineBreak() {
            firstLetterPending_ = false;
            if (lineHasContent_) {
                newLine();
            }
        }

        // Flows `el`'s children into the current line context. Inline boxes are
        // transparent here: their contents continue the same line. Anything
        // atomic (inline-block, replaced media) is placed as a unit.
        void inlineBox(Element& el, const ComputedStyle& container) {
            // A gauge paints its bar from attributes, never its children:
            // fallback text stays hidden, like a browser with support.
            if (el.tag() == "meter" || el.tag() == "progress") {
                return;
            }
            // An inside marker flows as the first inline content, taking space
            // on the line like a browser's. Outside markers paint separately.
            if (el.tag() == "li") {
                const ComputedStyle own = owner_.resolveStyle(el);
                if (own.listStylePosition == ComputedStyle::LSPInside &&
                    own.listStyleType != ComputedStyle::LSTNone) {
                    std::string marker;
                    if (isOrderedMarker(own.listStyleType)) {
                        int ordinal = 1;
                        if (const Element* parent = el.parent()) {
                            for (const auto& sib : parent->children()) {
                                if (!sib || sib.get() == &el) {
                                    break;
                                }
                                if (!sib->isTextNode() && sib->tag() == "li") {
                                    ++ordinal;
                                }
                            }
                        }
                        marker = listMarkerText(own.listStyleType, ordinal) + ".";
                    } else {
                        marker = listMarkerText(own.listStyleType, 0);
                    }
                    // A non-breaking gap survives collapsing where a plain
                    // space would not. The marker is not the item's first
                    // letter; re-arm the split for the content after it.
                    const bool pendingLetter = firstLetterPending_;
                    firstLetterPending_ = false;
                    text(marker + "\xC2\xA0", own);
                    if (pendingLetter && linesCommitted_ == 0) {
                        firstLetterPending_ = true;
                        firstLetterAfterMarker_ = true;
                    }
                }
            }
            // A control draws its current value where a normal element would
            // draw its children, so its text takes part in the same line context.
            // Password fields mask the value with bullet characters. A file
            // control shows only the file name, never the path. A textarea
            // uses its own `white-space`, which defaults to pre-wrap, so its
            // newlines survive into the line grid.
            if (el.tag() == "input" || el.tag() == "textarea") {
                std::string value = el.value();
                if (el.tag() == "input" && el.attribute("type") == "file") {
                    value = el.fileName();
                }
                if (!value.empty()) {
                    if (el.attribute("type") == "password") {
                        std::size_t chars = 0;
                        for (unsigned char c : value) {
                            if ((c & 0xC0) != 0x80) {
                                ++chars;
                            }
                        }
                        std::string masked;
                        masked.reserve(chars * 3);
                        for (std::size_t k = 0; k < chars; ++k) {
                            masked += "\xE2\x80\xA2";  // Unicode bullet U+2022 (•)
                        }
                        text(masked, container);
                    } else {
                        text(value, container);
                    }
                } else {
                    // An empty field shows its placeholder hint, if the rebuild
                    // made one, in the box's own style (never password-masked).
                    for (const auto& child : el.children()) {
                        if (child && child->tag() == "::placeholder") {
                            const ComputedStyle hint = owner_.resolveStyle(*child);
                            text(child->textContent(), hint);
                            break;
                        }
                    }
                }
                return;
            }
            for (const auto& child : el.children()) {
                if (!child) {
                    continue;
                }
                if (child->isTextNode()) {
                    // Bare text directly under a closed <details> is hidden with
                    // everything but the summary.
                    if (el.tag() == "details" && !el.hasAttribute("open")) {
                        continue;
                    }
                    if (!child->text().empty()) {
                        text(child->text(), container);
                    }
                    continue;
                }
                if (child->tag() == "br") {
                    // Forced break regardless of its computed display.
                    lineBreak();
                    continue;
                }
                if (child->tag() == "wbr") {
                    // A soft break opportunity: the next word starts a fresh
                    // line when anything is already on this one. Unlike `br`
                    // it adds no line when nothing precedes it.
                    breakAllowed_ = true;
                    continue;
                }
                if (!owner_.isLaidOut(*child)) {
                    continue;
                }
                if (clamped_) {
                    owner_.clearSubtree(*child);
                    clampSuppressed_ = true;
                    continue;
                }
                const ComputedStyle cs = owner_.resolveStyle(*child);
                // A floated child leaves the line and reserves space beside it
                // for every following line.
                if (isFloated(cs)) {
                    if (owner_.floats_) {
                        const float floatY = lineHasContent_
                                                 ? originY_ + cursorY_ + lineHeight_
                                                 : originY_ + cursorY_;
                        owner_.placeFloat(*child, cs, maxWidth_, originX_, floatY);
                        syncLine();
                    }
                    continue;
                }
                if (cs.display == ComputedStyle::Inline) {
                    inlineBox(*child, cs);
                    continue;
                }
                atomic(*child, cs);
            }
        }

        void atomic(Element& child, const ComputedStyle& cs) {
            firstLetterPending_ = false;
            firstLetterAfterSpace_ = false;
            firstLetterAfterMarker_ = false;
            const UsedBox box = owner_.usedBox(cs, maxWidth_);
            // Measure before deciding to wrap: an auto-width atomic box is as
            // wide as its contents want, so the break test needs that number. The
            // probe must not wrap the atomic's contents around the container's
            // floats, so they are hidden for the measurement.
            float boxWidth = box.width;
            if (boxWidth <= 0.0f) {
                FloatContext* const savedFloats = owner_.floats_;
                owner_.floats_ = nullptr;
                owner_.run(child, kUnboundedProbe, 0.0f, 0.0f);
                owner_.floats_ = savedFloats;
                boxWidth = child.layoutWidth();
            }
            const float outerW = boxWidth + box.margin.left + box.margin.right +
                                 box.borderWidth.left + box.borderWidth.right +
                                 box.padding.left + box.padding.right;
            if (breakAllowed_) {
                breakAllowed_ = false;
                if (lineHasContent_ && !spacing_.wraps &&
                    lineIndent_ + penX_ + outerW > lineAvail_) {
                    newLine();
                }
            }
            if (lineHasContent_ && cs.textWrapMode == ComputedStyle::TextWrapMode::Wrap &&
                lineIndent_ + penX_ + outerW > lineAvail_) {
                newLine();
            }
            if (clamped_) {
                // The break above spent the cap: the box lays out nowhere.
                owner_.clearSubtree(child);
                clampSuppressed_ = true;
                return;
            }
            const float lineTop = originY_ + cursorY_;
            const float top = lineTop + box.margin.top + verticalAlignShift(cs);
            // The atomic box is placed as a unit; the container's floats do not
            // wrap its own contents.
            FloatContext* const savedFloats = owner_.floats_;
            owner_.floats_ = nullptr;
            owner_.run(child, maxWidth_, originX_ + lineIndent_ + penX_ + box.margin.left, top);
            owner_.floats_ = savedFloats;
            // `top` and `bottom` align the box against the line box rather than the
            // baseline. The line box is the container's own line height, so its
            // extent is known here: `bottom` drops the box so its outer bottom sits
            // on it, `top` seats its outer top on it.
            float boxBottom = cursorY_ + box.margin.top + child.layoutHeight() +
                              box.margin.bottom;
            // `text-top`/`text-bottom` measure against the parent's content area
            // rather than the line box; here the line box is the container's own
            // line height, which is what the content area spans, so they land
            // together with `top`/`bottom`.
            if (cs.verticalAlign == ComputedStyle::VAlignBottom ||
                cs.verticalAlign == ComputedStyle::VAlignTextBottom) {
                child.setLayoutY(lineTop + lineHeight_ - child.layoutHeight() -
                                 box.margin.bottom);
                boxBottom = cursorY_ + lineHeight_;
            } else if (cs.verticalAlign == ComputedStyle::VAlignTop ||
                       cs.verticalAlign == ComputedStyle::VAlignTextTop) {
                child.setLayoutY(lineTop + box.margin.top);
            }
            penX_ += box.margin.left + child.layoutWidth() + box.margin.right;
            lineHasContent_ = true;
            widest_ = std::max(widest_, penX_);
            bottom_ = std::max(bottom_, boxBottom);
        }

        // Reorders one finished line's runs into visual order (positions only;
        // the vector stays logical so caret mapping keeps working). Pure LTR
        // lines return untouched. Levels come from FriBiDi over the joined
        // line text, and fribidi_reorder_line maps visual positions back to
        // logical runs; runs then lay out from the line start (or end for an
        // RTL base) with uniform gaps. Mixed-script runs keep printed order
        // inside, and caret/selection stay logical approximations.
        void reorderBidiLine() {
            auto& runs = ownerEl_.textRuns();
            if (lineRunStartIndex_ >= runs.size()) {
                return;
            }
            // Logical chars with their run slot; gap spaces keep neighbors honest.
            std::vector<FriBidiChar> ustr;
            std::vector<int> charRun;
            for (std::size_t k = lineRunStartIndex_; k < runs.size(); ++k) {
                if (k > lineRunStartIndex_) {
                    ustr.push_back(' ');
                    charRun.push_back(-1);
                }
                const std::string& text = runs[k].text;
                for (std::size_t i = 0; i < text.size();) {
                    unsigned char c = static_cast<unsigned char>(text[i]);
                    FriBidiChar cp = 0xFFFDu;
                    std::size_t extra = 0;
                    if (c < 0x80) {
                        cp = c;
                    } else if ((c & 0xE0) == 0xC0) {
                        extra = 1;
                        cp = c & 0x1Fu;
                    } else if ((c & 0xF0) == 0xE0) {
                        extra = 2;
                        cp = c & 0x0Fu;
                    } else if ((c & 0xF8) == 0xF0) {
                        extra = 3;
                        cp = c & 0x07u;
                    }
                    bool ok = extra > 0 || c < 0x80;
                    for (std::size_t j = 1; ok && j <= extra; ++j) {
                        if (i + j >= text.size()) {
                            ok = false;
                            break;
                        }
                        const unsigned char cc =
                            static_cast<unsigned char>(text[i + j]);
                        if ((cc & 0xC0) != 0x80) {
                            ok = false;
                            break;
                        }
                        cp = (cp << 6) | (cc & 0x3Fu);
                    }
                    if (!ok) {
                        cp = 0xFFFDu;
                        extra = 0;
                    }
                    i += extra + 1;
                    ustr.push_back(cp);
                    charRun.push_back(static_cast<int>(k - lineRunStartIndex_));
                }
            }
            if (ustr.empty()) {
                return;
            }
            const FriBidiStrIndex len = static_cast<FriBidiStrIndex>(ustr.size());
            std::vector<FriBidiCharType> types(static_cast<std::size_t>(len));
            fribidi_get_bidi_types(ustr.data(), len, types.data());
            bool rtl = false;
            for (FriBidiCharType type : types) {
                if (type == FRIBIDI_TYPE_RTL || type == FRIBIDI_TYPE_AL) {
                    rtl = true;
                    break;
                }
            }
            if (!rtl) {
                return;
            }
            // The base direction is the block's own: `rtl` flips mixed-script
            // lines even when Latin leads, and `ltr` (the default) never moves
            // runs -- first-strong guessing would flip Latin-led RTL text that
            // CSS lays out straight.
            FriBidiParType base = style_.direction == ComputedStyle::Direction::RTL
                                      ? FRIBIDI_PAR_RTL
                                      : FRIBIDI_PAR_LTR;
            std::vector<FriBidiBracketType> brackets(static_cast<std::size_t>(len));
            fribidi_get_bracket_types(ustr.data(), len, types.data(), brackets.data());
            std::vector<FriBidiLevel> levels(static_cast<std::size_t>(len));
            if (fribidi_get_par_embedding_levels_ex(types.data(), brackets.data(), len,
                                                     &base, levels.data()) == 0) {
                return;
            }
            std::vector<FriBidiStrIndex> map(static_cast<std::size_t>(len));
            for (FriBidiStrIndex i = 0; i < len; ++i) {
                map[static_cast<std::size_t>(i)] = i;
            }
            const FriBidiLevel reordered = fribidi_reorder_line(
                FRIBIDI_FLAGS_DEFAULT, types.data(), len, 0, base, levels.data(),
                ustr.data(), map.data());
            if (reordered == 0) {
                return;
            }
            // Visual run order: runs by first visual appearance. Positions
            // only; the vector stays logical.
            const std::size_t count = runs.size() - lineRunStartIndex_;
            std::vector<std::size_t> order;
            order.reserve(count);
            std::vector<bool> seen(count, false);
            for (FriBidiStrIndex visual = 0; visual < len; ++visual) {
                const int slot = charRun[static_cast<std::size_t>(map[static_cast<std::size_t>(visual)])];
                if (slot >= 0 && !seen[static_cast<std::size_t>(slot)]) {
                    seen[static_cast<std::size_t>(slot)] = true;
                    order.push_back(static_cast<std::size_t>(slot));
                }
            }
            for (std::size_t k = 0; k < count; ++k) {
                if (!seen[k]) {
                    order.push_back(k);
                }
            }
            // Visual order always lays left to right from the line start;
            // the base direction already shaped the order itself. (Lines stay
            // left-anchored; RTL right alignment is not modeled.)
            float cursor = runs[lineRunStartIndex_].x;
            const float gap = metrics_.spaceWidth;
            for (std::size_t slot : order) {
                auto& run = runs[lineRunStartIndex_ + slot];
                run.x = cursor;
                cursor += run.width + gap;
            }
        }

        // Spreads one finished line across its width: center and right shift
        // every run, justify spreads the leftover across the gaps in visual
        // order, and anything else stays put. The last line justifies as
        // start, and unbounded (max-content) probes never distribute at all.
        // Only run positions move; pen widths stay, so shrink-to-fit and caret
        // mapping measure content, not placement.
        void distributeLine(bool last) {
            auto& runs = ownerEl_.textRuns();
            if (lineRunStartIndex_ >= runs.size() || lineAvail_ >= kUnbounded / 2) {
                return;
            }
            const bool rtl = style_.direction == ComputedStyle::Direction::RTL;
            // The last line answers `text-align-last`: `auto` repeats the block
            // alignment, except a justified block starts its last line over.
            ComputedStyle::TextAlign align = style_.textAlign;
            if (last) {
                const auto lastAlign = style_.textAlignLast;
                using Last = ComputedStyle::TextAlignLast;
                if (lastAlign == Last::Auto) {
                    if (align == ComputedStyle::Justify) {
                        align = ComputedStyle::Start;
                    }
                } else if (lastAlign == Last::Left) {
                    align = ComputedStyle::Left;
                } else if (lastAlign == Last::Center) {
                    align = ComputedStyle::Center;
                } else if (lastAlign == Last::Right) {
                    align = ComputedStyle::Right;
                } else if (lastAlign == Last::Justify) {
                    align = ComputedStyle::Justify;
                } else if (lastAlign == Last::Start) {
                    align = ComputedStyle::Start;
                } else {
                    align = ComputedStyle::End;
                }
            }
            if (align == ComputedStyle::Start) {
                align = rtl ? ComputedStyle::Right : ComputedStyle::Left;
            } else if (align == ComputedStyle::End) {
                align = rtl ? ComputedStyle::Left : ComputedStyle::Right;
            }
            const float leftover = lineAvail_ - penX_;
            if (!(leftover > 0.0f)) {
                return;
            }
            if (align == ComputedStyle::Justify) {
                const std::size_t count = runs.size() - lineRunStartIndex_;
                if (count < 2) {
                    return;
                }
                std::vector<std::size_t> order;
                order.reserve(count);
                for (std::size_t k = lineRunStartIndex_; k < runs.size(); ++k) {
                    order.push_back(k);
                }
                std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    return runs[a].x < runs[b].x;
                });
                const float each = leftover / static_cast<float>(count - 1);
                for (std::size_t k = 1; k < count; ++k) {
                    runs[order[k]].x += each * static_cast<float>(k);
                }
                return;
            }
            float shift = 0.0f;
            if (align == ComputedStyle::Center) {
                shift = leftover * 0.5f;
            } else if (align == ComputedStyle::Right) {
                shift = leftover;
            } else {
                return;
            }
            for (std::size_t k = lineRunStartIndex_; k < runs.size(); ++k) {
                runs[k].x += shift;
            }
        }

        void newLine() {
            if (clamped_) {
                // Past the cap there are no more lines: swallow the break. A
                // break with content on the would-be line means something real
                // was cut, which keeps the speculative ellipsis.
                if (lineHasContent_) {
                    clampSuppressed_ = true;
                }
                lineHasContent_ = false;
                return;
            }
            reorderBidiLine();
            distributeLine(false);
            ++linesCommitted_;
            if (lineClamp_ > 0 && linesCommitted_ >= lineClamp_) {
                // The line just finished is the last allowed: end it with an
                // ellipsis and swallow everything after. finish() takes the
                // ellipsis back when nothing was actually cut.
                appendClampEllipsis();
                clamped_ = true;
                lineHasContent_ = false;
                return;
            }
            breakAllowed_ = false;
            cursorY_ += lineHeight_;
            lineHeight_ = metrics_.lineHeight;
            penX_ = 0.0f;
            lineHasContent_ = false;
            ellipsisAdded_ = false;
            lineRunStartIndex_ = ownerEl_.textRuns().size();
            syncLine();
        }

        void finish() {
            if (clamped_ && !clampSuppressed_) {
                retractClampEllipsis();
            }
            reorderBidiLine();
            distributeLine(true);
        }

        // Ends the last allowed line with an ellipsis, dropping whole tail
        // runs until it fits. Speculative: finish() retracts it when nothing
        // past the cap was actually cut.
        void appendClampEllipsis() {
            auto& runs = ownerEl_.textRuns();
            const std::string kEllipsis = "\xE2\x80\xA6";
            const float ellWidth = textWidth(owner_.font_, style_, kEllipsis);
            const float lineEnd = originX_ + lineIndent_ + lineAvail_;
            float endX = originX_ + lineIndent_;
            for (std::size_t k = lineRunStartIndex_; k < runs.size(); ++k) {
                endX = std::max(endX, runs[k].x + runs[k].width);
            }
            while (runs.size() > lineRunStartIndex_ && endX + ellWidth > lineEnd) {
                droppedClampRuns_.push_back(runs.back());
                runs.pop_back();
                endX = originX_ + lineIndent_;
                for (std::size_t k = lineRunStartIndex_; k < runs.size(); ++k) {
                    endX = std::max(endX, runs[k].x + runs[k].width);
                }
            }
            ownerEl_.addTextRun(
                {kEllipsis, endX,
                 originY_ + cursorY_ + verticalAlignShift(style_, &metrics_, &metrics_), ellWidth,
                 lineHeight_, metrics_.ascent, style_.fontSize, style_.color.r,
                 style_.color.g, style_.color.b, style_.color.a, style_.fontBold,
                 style_.fontStyle != ComputedStyle::FontNormal, style_.fontFamily});
            penX_ = endX - originX_ - lineIndent_ + ellWidth;
        }

        // Takes the speculative ellipsis back out: the content fit the cap, so
        // the dropped tail runs go home in order.
        void retractClampEllipsis() {
            auto& runs = ownerEl_.textRuns();
            if (!runs.empty() && runs.back().text == "\xE2\x80\xA6") {
                runs.pop_back();
            }
            for (auto it = droppedClampRuns_.rbegin(); it != droppedClampRuns_.rend(); ++it) {
                runs.push_back(*it);
            }
            droppedClampRuns_.clear();
        }

        float cursorY() const {
            // The trailing line is never committed, so count it here.
            const float tail = lineHasContent_ ? lineHeight_ : 0.0f;
            return std::max(cursorY_ + tail, bottom_);
        }
        float penX() const { return penX_; }
        float widest() const { return widest_; }
        // What the pass produced, for `text-wrap` re-break probing: the number
        // of lines (the trailing partial one counted), the words on the last
        // line, and whether floats shaped any line.
        int lineCount() const { return linesCommitted_ + (lineHasContent_ ? 1 : 0); }
        int lastLineWords() const {
            int words = 0;
            const std::vector<Element::TextRun>& runs = ownerEl_.textRuns();
            for (std::size_t k = lineRunStartIndex_; k < runs.size(); ++k) {
                if (!runs[k].text.empty() && runs[k].text != " ") {
                    ++words;
                }
            }
            return words;
        }
        bool sawFloat() const { return sawFloat_; }

    private:
        // What `white-space` changes. `nowrap` collapses runs of spaces but never
        // breaks a line. `pre` and `pre-wrap` treat a newline in the text as a line
        // break rather than whitespace; `pre` also keeps runs of spaces, so a line
        // is taken whole and never wraps.
        struct Spacing {
            bool wraps = true;
            bool keepsWhitespace = false;
            bool keepsNewlines = false;
            // break-spaces: every space is its own run (wrapping after any of
            // them), so inter-word gaps come from runs, never from word().
            bool breakSpaces = false;
        };

        static Spacing spacingFor(ComputedStyle::WhiteSpace value) {
            switch (value) {
                case ComputedStyle::WrapNone:
                    return {false, false, false};
                case ComputedStyle::WrapPre:
                    return {false, true, true};
                case ComputedStyle::WrapPreWrap:
                    return {true, false, true};
                case ComputedStyle::WrapBreakSpaces:
                    return {true, false, true, true};
                case ComputedStyle::WrapText:
                    break;
            }
            return {};
        }

        // Splits at whitespace, collapsing runs of spaces into the single space
        // word() inserts between runs.
        void words(std::string_view text, const ComputedStyle& s, const Metrics& m, float ascentDelta) {
            std::size_t i = 0;
            while (i < text.size()) {
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
                    ++i;
                }
                const std::size_t start = i;
                while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
                    ++i;
                }
                if (i == start) {
                    break;
                }
                word(text.substr(start, i - start), s, m, ascentDelta);
            }
        }

        // Tokenizes like words(), but every space becomes its own run taking
        // room, so lines wrap after any space and trailing spaces hang onto the
        // next line instead of vanishing.
        void breakSpaced(std::string_view text, const ComputedStyle& s, const Metrics& m,
                         float ascentDelta) {
            std::size_t i = 0;
            while (i < text.size()) {
                if (text[i] == '\n') {
                    newLine();
                    ++i;
                    continue;
                }
                if (std::isspace(static_cast<unsigned char>(text[i]))) {
                    if (text[i] == '\t') {
                        tabGap(s, m, ascentDelta);
                    } else {
                        preservedSpace(s, m, ascentDelta);
                    }
                    ++i;
                    continue;
                }
                const std::size_t start = i;
                while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
                    ++i;
                }
                word(text.substr(start, i - start), s, m, ascentDelta);
            }
        }

        // One tab stop's width: a `tab-size` number counts space widths, a
        // length is absolute with font-relative units against the run's own
        // font (`rem` against the document root's).
        float tabStop(const ComputedStyle& s, const Metrics& m) const {
            if (s.tabSize.unit == Length::Number) {
                return s.tabSize.value > 0.0f ? s.tabSize.value * m.spaceWidth : 0.0f;
            }
            const float len =
                s.tabSize.resolve(0.0f, s.fontSize, owner_.resolver_.rootFontSize());
            return len > 0.0f ? len : 0.0f;
        }

        // A tab in preserved whitespace advances the pen to the next tab stop
        // measured from the line's left edge. The gap is a blank run, so it
        // takes room without painting a glyph and selection can cover it. A
        // `tab-size` of 0 makes the tab advance nothing, like a browser's.
        void tabGap(const ComputedStyle& s, const Metrics& m, float ascentDelta) {
            if (ellipsisAdded_) {
                return;
            }
            if (clamped_) {
                breakAllowed_ = false;
                clampSuppressed_ = true;
                return;
            }
            const float stop = tabStop(s, m);
            auto advance = [&]() {
                if (stop <= 0.0f) {
                    return 0.0f;
                }
                const float used = lineIndent_ + penX_;
                // A pen exactly on a stop advances a whole stop, never zero.
                return (std::floor(used / stop) + 1.0f) * stop - used;
            };
            float gap = advance();
            // Only break-spaces wraps preserved whitespace; a tab that would
            // run past the line starts the next one, like a preserved space.
            if (spacing_.wraps && s.textWrapMode == ComputedStyle::TextWrapMode::Wrap &&
                lineHasContent_ && lineIndent_ + penX_ + gap > lineAvail_) {
                newLine();
                if (clamped_) {
                    clampSuppressed_ = true;
                    return;
                }
                gap = advance();
            }
            ownerEl_.addTextRun({" ", originX_ + lineIndent_ + penX_,
                                 originY_ + cursorY_ + verticalAlignShift(s, &metrics_, &m),
                                 gap, lineHeight_, metrics_.ascent + ascentDelta,
                                 s.fontSize, s.color.r, s.color.g, s.color.b, s.color.a,
                                 s.fontBold, s.fontStyle != ComputedStyle::FontNormal,
                                 s.fontFamily});
            penX_ += gap;
            lineHasContent_ = true;
            if (firstLetterPending_ && linesCommitted_ == 0) {
                firstLetterAfterSpace_ = true;
            }
            widest_ = std::max(widest_, penX_);
        }

        // One blank run a space wide. Wraps like a word when it overflows, so a
        // trailing space starts the next line instead of hanging past the edge.
        void preservedSpace(const ComputedStyle& s, const Metrics& m, float ascentDelta) {
            if (ellipsisAdded_) {
                return;
            }
            if (clamped_) {
                clampSuppressed_ = true;
                return;
            }
            if (spacing_.wraps && s.textWrapMode == ComputedStyle::TextWrapMode::Wrap &&
                lineHasContent_ && lineIndent_ + penX_ + metrics_.spaceWidth > lineAvail_) {
                newLine();
                if (clamped_) {
                    clampSuppressed_ = true;
                    return;
                }
            }
            ownerEl_.addTextRun({" ", originX_ + lineIndent_ + penX_,
                                 originY_ + cursorY_ + verticalAlignShift(s, &metrics_, &m),
                                 metrics_.spaceWidth, lineHeight_,
                                 metrics_.ascent + ascentDelta, s.fontSize, s.color.r, s.color.g,
                                 s.color.b, s.color.a, s.fontBold,
                                 s.fontStyle != ComputedStyle::FontNormal, s.fontFamily});
            penX_ += metrics_.spaceWidth;
            lineHasContent_ = true;
            if (firstLetterPending_ && linesCommitted_ == 0) {
                firstLetterAfterSpace_ = true;
            }
            widest_ = std::max(widest_, penX_);
        }

        bool isEllipsisActive() const {
            return style_.textOverflow == ComputedStyle::TextOverflowEllipsis &&
                   (style_.overflowX != ComputedStyle::Visible || style_.overflowY != ComputedStyle::Visible) &&
                   maxWidth_ < kUnbounded;
        }

        // Breaks one overlong word across lines at codepoint boundaries, one run
        // per piece. The first piece keeps the inter-word space; continuations
        // start flush. A piece that fits nowhere on a fresh line still takes a
        // single codepoint, so the loop always progresses.
        void breakAcrossLines(std::string_view w, const ComputedStyle& s, const Metrics& m,
                              float ascentDelta) {
            const std::string wt = transformText(s, w);
            // Codepoint boundaries up front, so the longest fitting prefix is a
            // binary search rather than a reshape per character.
            std::vector<std::size_t> bounds{0};
            for (std::size_t i = 0; i < wt.size();) {
                ++i;
                while (i < wt.size() &&
                       (static_cast<unsigned char>(wt[i]) & 0xC0) == 0x80) {
                    ++i;
                }
                bounds.push_back(i);
            }
            std::size_t from = 0;
            bool first = true;
            while (from < bounds.size() - 1) {
                float space = 0.0f;
                if (first && lineHasContent_) {
                    space = metrics_.spaceWidth;
                    if (s.wordSpacing.unit != sui::Length::Auto) {
                        space += s.wordSpacing.resolve(s.fontSize, s.fontSize, 16.0f);
                    }
                }
                float avail = lineAvail_ - lineIndent_ - penX_ - space;
                if (avail <= 0.0f && lineHasContent_) {
                    newLine();
                    continue;
                }
                // Longest piece fitting avail; at least one codepoint on a fresh
                // line so a zero-width slot still progresses.
                std::size_t lo = from + 1, hi = bounds.size() - 1, best = from;
                while (lo <= hi) {
                    const std::size_t mid = lo + (hi - lo) / 2;
                    if (textWidth(owner_.font_, s, std::string_view(wt).substr(from, bounds[mid] - from)) <=
                        avail) {
                        best = mid;
                        lo = mid + 1;
                    } else if (mid == 0) {
                        break;
                    } else {
                        hi = mid - 1;
                    }
                }
                if (best == from) {
                    if (lineHasContent_) {
                        newLine();
                        continue;
                    }
                    best = from + 1;
                }
                if (clamped_) {
                    clampSuppressed_ = true;
                    return;
                }
                const std::string piece = wt.substr(bounds[from], bounds[best] - bounds[from]);
                const float pieceWidth = textWidth(owner_.font_, s, piece);
                ownerEl_.addTextRun(
                    {piece, originX_ + lineIndent_ + penX_ + space,
                     originY_ + cursorY_ + verticalAlignShift(s, &metrics_, &m), pieceWidth,
                     lineHeight_, metrics_.ascent + ascentDelta, s.fontSize, s.color.r,
                     s.color.g, s.color.b, s.color.a, s.fontBold,
                     s.fontStyle != ComputedStyle::FontNormal, s.fontFamily});
                penX_ += space + pieceWidth;
                lineHasContent_ = true;
                widest_ = std::max(widest_, penX_);
                from = best;
                first = false;
                if (from < bounds.size() - 1) {
                    newLine();
                }
            }
        }

        // The soft-hyphen breaks inside a word: `first` is the first one, a
        // byte offset just before a U+00AD, and `fitting` the last whose piece
        // plus the `-` a taken break paints fits `avail`. Both are 0 when there
        // is no break to take.
        std::pair<std::size_t, std::size_t> softBreaks(std::string_view word,
                                                       const ComputedStyle& s,
                                                       float avail) const {
            const float hyphenWidth = textWidth(owner_.font_, s, "-");
            std::size_t first = 0;
            std::size_t fitting = 0;
            for (std::size_t i = 1; i + 1 < word.size(); ++i) {
                if (!isSoftHyphenAt(word, i)) {
                    continue;
                }
                if (i + kSoftHyphen.size() >= word.size()) {
                    break; // Nothing follows: a break here has no rest to hyphenate.
                }
                if (first == 0) {
                    first = i;
                }
                if (textWidth(owner_.font_, s, word.substr(0, i)) + hyphenWidth <= avail) {
                    fitting = i;
                }
            }
            return {first, fitting};
        }

        // `hyphens`: a word that does not fit its line breaks at its soft
        // hyphens, and every taken break paints as `-`. Pieces go down lines
        // until the rest of the word fits; a break is only taken on a line that
        // then has something on it. `none` is handled by the caller not calling
        // this at all. Returns what is left of the word, which the caller lays
        // out through its ordinary path — gap and wrap check included.
        std::string_view breakAtSoftHyphens(std::string_view word, const ComputedStyle& s,
                                           const Metrics& m, float ascentDelta,
                                           bool suppressGap) {
            while (true) {
                const bool gapApplies = lineHasContent_ && !spacing_.breakSpaces && !suppressGap;
                const float checkGap = gapApplies ? metrics_.spaceWidth : 0.0f;
                const float avail = lineAvail_ - lineIndent_ - penX_ - checkGap;
                if (textWidth(owner_.font_, s, word) <= avail) {
                    return word;
                }
                const auto [first, fitting] = softBreaks(word, s, avail);
                if (first == 0) {
                    return word;
                }
                std::size_t cut = fitting;
                if (cut == 0) {
                    // Nothing fits the line as it stands. With something already
                    // on it the word moves down whole; on a bare line the first
                    // break is taken even though its piece overflows, which is
                    // how a break opportunity at a soft hyphen behaves.
                    if (lineHasContent_) {
                        newLine();
                        if (clamped_) {
                            clampSuppressed_ = true;
                            return {};
                        }
                        continue;
                    }
                    cut = first;
                }
                float space = checkGap;
                if (gapApplies && s.wordSpacing.unit != sui::Length::Auto) {
                    space += s.wordSpacing.resolve(s.fontSize, s.fontSize, 16.0f);
                }
                const std::string piece = transformText(s, word.substr(0, cut)) + "-";
                const float pieceWidth = textWidth(owner_.font_, s, piece);
                ownerEl_.addTextRun(
                    {piece, originX_ + lineIndent_ + penX_ + space,
                     originY_ + cursorY_ + verticalAlignShift(s, &metrics_, &m), pieceWidth,
                     lineHeight_, metrics_.ascent + ascentDelta, s.fontSize, s.color.r,
                     s.color.g, s.color.b, s.color.a, s.fontBold,
                     s.fontStyle != ComputedStyle::FontNormal, s.fontFamily});
                penX_ += space + pieceWidth;
                lineHasContent_ = true;
                widest_ = std::max(widest_, penX_);
                word.remove_prefix(cut + kSoftHyphen.size());
                newLine();
                if (clamped_) {
                    clampSuppressed_ = true;
                    return {};
                }
                if (word.empty()) {
                    return {};
                }
            }
        }

        void word(std::string_view w, const ComputedStyle& s, const Metrics& m,
                  float ascentDelta, bool suppressGap = false) {
            if (ellipsisAdded_) {
                return;
            }
            if (clamped_) {
                // Past the cap words are cut content, which keeps the ellipsis.
                breakAllowed_ = false;
                clampSuppressed_ = true;
                return;
            }
            // The first formatted line may win a different font and line
            // height. Recomputed after a break, when the pseudo no longer
            // applies to the following line.
            ComputedStyle firstLineStyle;
            const ComputedStyle* activeStyle = &s;
            Metrics activeMetrics = m;
            float activeAscentDelta = ascentDelta;
            const auto refreshActive = [&]() {
                activeStyle = &s;
                activeMetrics = m;
                activeAscentDelta = ascentDelta;
                if (linesCommitted_ != 0 || !firstLineHasFont_) {
                    return;
                }
                firstLineStyle = s;
                if (!applyFirstLineFont(firstLineStyle)) {
                    return;
                }
                activeStyle = &firstLineStyle;
                activeMetrics = metricsFor(owner_.font_, firstLineStyle);
                activeAscentDelta =
                    std::min(0.0f, activeMetrics.ascent - metrics_.ascent);
                if (activeMetrics.lineHeight > lineHeight_) {
                    lineHeight_ = activeMetrics.lineHeight;
                    syncLine();
                }
            };
            const auto baselineForActive = [&]() {
                // A first-line font brings its own ascent; an inline child
                // with a larger font keeps the engine's established baseline.
                return activeStyle != &s ? activeMetrics.ascent
                                         : metrics_.ascent + activeAscentDelta;
            };
            refreshActive();
            // A preserved-space run may hand the whole leading whitespace
            // along with the word. It belongs before the letter and never
            // consumes the first-letter opportunity.
            if (firstLetterPending_ && !w.empty()) {
                std::size_t lead = 0;
                while (lead < w.size() &&
                       std::isspace(static_cast<unsigned char>(w[lead]))) {
                    ++lead;
                }
                if (lead > 0) {
                    for (std::size_t k = 0; k < lead; ++k) {
                        preservedSpace(*activeStyle, activeMetrics,
                                       activeAscentDelta);
                    }
                    w.remove_prefix(lead);
                    if (w.empty()) {
                        return;
                    }
                }
            }
            // `::first-letter` splits the first grapheme of the first formatted
            // line into its own run, so the pseudo font can change its metrics
            // and the first line's height. Leading preserved spaces and an
            // inside marker may already sit on the line; they do not consume
            // the letter.
            if (firstLetterPending_ && !w.empty() && linesCommitted_ == 0 &&
                (!lineHasContent_ || firstLetterAfterSpace_ ||
                 firstLetterAfterMarker_)) {
                firstLetterPending_ = false;
                firstLetterAfterSpace_ = false;
                firstLetterAfterMarker_ = false;
                const PseudoFont letterFont =
                    owner_.firstLetterFont(ownerEl_, *activeStyle);
                if (letterFont.sizeSet || letterFont.familySet ||
                    letterFont.weightSet || letterFont.styleSet ||
                    letterFont.lineHeightSet || letterFont.floatSet) {
                    // One codepoint plus its UTF-8 continuations. A grapheme
                    // that joins several codepoints (a ZWJ emoji) is wider than
                    // this split, matching the engine's per-codepoint runs.
                    std::size_t bytes = 1;
                    while (bytes < w.size() &&
                           (static_cast<unsigned char>(w[bytes]) & 0xC0) == 0x80) {
                        ++bytes;
                    }
                    ComputedStyle letterStyle = *activeStyle;
                    if (letterFont.sizeSet) {
                        letterStyle.fontSize = letterFont.size;
                    }
                    if (letterFont.familySet) {
                        letterStyle.fontFamily = letterFont.familyList.empty()
                                                     ? letterFont.family
                                                     : letterFont.familyList;
                    }
                    if (letterFont.weightSet) {
                        letterStyle.fontWeight = letterFont.weight;
                        letterStyle.fontBold = letterFont.weight >= 600;
                    }
                    if (letterFont.styleSet) {
                        letterStyle.fontStyle = letterFont.style;
                    }
                    if (letterFont.lineHeightSet) {
                        letterStyle.lineHeight = letterFont.lineHeight;
                    }
                    const Metrics letterMetrics = metricsFor(owner_.font_, letterStyle);
                    const std::string_view letter = w.substr(0, bytes);
                    const float letterWidth =
                        textWidth(owner_.font_, letterStyle, letter);
                    // A floated first letter becomes a drop cap: its glyph box
                    // joins the float context and the paragraph wraps beside
                    // it. Only a letter starting an empty line can use the
                    // full-height form; leading spaces or an inside marker
                    // keep the inline split.
                    const bool dropCap =
                        letterFont.floatSet &&
                        letterFont.floatType != ComputedStyle::FloatNone &&
                        !lineHasContent_ && penX_ == 0.0f && owner_.floats_;
                    if (dropCap) {
                        const bool left =
                            letterFont.floatType == ComputedStyle::FloatLeft;
                        const float x =
                            left ? originX_ + lineIndent_
                                 : originX_ + lineIndent_ + lineAvail_ - letterWidth;
                        const float y = originY_ + cursorY_;
                        ownerEl_.addTextRun(
                            {transformText(letterStyle, letter), x, y, letterWidth,
                             letterMetrics.lineHeight, letterMetrics.ascent,
                             letterStyle.fontSize, letterStyle.color.r,
                             letterStyle.color.g, letterStyle.color.b,
                             letterStyle.color.a, letterStyle.fontBold,
                             letterStyle.fontStyle != ComputedStyle::FontNormal,
                             letterStyle.fontFamily});
                        owner_.floats_->add({y, y + letterMetrics.lineHeight, x,
                                             x + letterWidth, left});
                        bottom_ = std::max(bottom_, cursorY_ + letterMetrics.lineHeight);
                        lineHasContent_ = true;
                        widest_ = std::max(widest_, letterWidth);
                        // The line inset now covers the drop cap, so the rest
                        // of the word starts at the pen without a gap.
                        syncLine();
                    } else {
                        lineHeight_ = std::max(lineHeight_, letterMetrics.lineHeight);
                        syncLine();
                        ownerEl_.addTextRun(
                            {transformText(letterStyle, letter),
                             originX_ + lineIndent_ + penX_,
                             originY_ + cursorY_ +
                                 verticalAlignShift(letterStyle, &metrics_,
                                                    &letterMetrics),
                             letterWidth, letterMetrics.lineHeight,
                             letterMetrics.ascent, letterStyle.fontSize,
                             letterStyle.color.r, letterStyle.color.g,
                             letterStyle.color.b, letterStyle.color.a,
                             letterStyle.fontBold,
                             letterStyle.fontStyle != ComputedStyle::FontNormal,
                             letterStyle.fontFamily});
                        penX_ += letterWidth;
                        lineHasContent_ = true;
                        widest_ = std::max(widest_, penX_);
                    }
                    w.remove_prefix(bytes);
                    // The rest is the same word: it must continue without an
                    // inter-word gap, even though the line now has content.
                    if (!w.empty()) {
                        word(w, s, m, ascentDelta, true);
                    }
                    return;
                }
            }
            // Wrapping has to be on for this run: `white-space` sets the box
            // wide mode, and `text-wrap: nowrap` (inherited or on an inner
            // span) turns the run's own breaks off without touching its
            // preserved whitespace.
            const bool wraps = spacing_.wraps &&
                               activeStyle->textWrapMode == ComputedStyle::TextWrapMode::Wrap;
            // `break-all` fills lines character by character; `break-word` only
            // breaks a word that overflows its own line. Either way the pieces
            // become their own runs. An ellipsis truncates instead, and a
            // textarea keeps one run per word so caret mapping keeps working.
            const bool breakAll =
                activeStyle->wordBreak == ComputedStyle::WordBreak::BreakAll;
            const bool breakWord =
                !breakAll &&
                activeStyle->overflowWrap == ComputedStyle::OverflowWrap::BreakWord;
            // `hyphens`: a soft hyphen offers a break where the word would
            // overflow. `auto` has no dictionary to add opportunities of its
            // own, so it breaks exactly like `manual`; `none` never breaks at
            // one (the character stays invisible either way, since measurement
            // and painting both drop it). `break-all` needs no opportunity of
            // its own, and a textarea keeps its runs word-sized.
            const bool softHyphens =
                wraps && !breakAll && !isEllipsisActive() && ownerEl_.tag() != "textarea" &&
                (activeStyle->hyphens == ComputedStyle::Hyphens::Manual ||
                 activeStyle->hyphens == ComputedStyle::Hyphens::Auto) &&
                w.find(kSoftHyphen) != std::string_view::npos;
            if (softHyphens) {
                w = breakAtSoftHyphens(w, *activeStyle, activeMetrics,
                                       activeAscentDelta, suppressGap);
                if (w.empty()) {
                    return;  // Every piece was laid, or the line cap was reached.
                }
            }
            float wWidth = textWidth(owner_.font_, *activeStyle, w);
            if (wraps && !isEllipsisActive() && ownerEl_.tag() != "textarea" &&
                (breakAll || (breakWord && wWidth > lineAvail_ - lineIndent_))) {
                breakAcrossLines(w, *activeStyle, activeMetrics, activeAscentDelta);
                return;
            }
            float wrapGap = suppressGap ? 0.0f : activeMetrics.spaceWidth;
            if (wraps && lineHasContent_ &&
                lineIndent_ + penX_ + wrapGap + wWidth > lineAvail_) {
                newLine();
                if (clamped_) {
                    clampSuppressed_ = true;
                    return;
                }
                // The new line is no longer the first: remeasure the word in
                // the element's own font before placing it.
                refreshActive();
                wWidth = textWidth(owner_.font_, *activeStyle, w);
                wrapGap = suppressGap ? 0.0f : activeMetrics.spaceWidth;
            }
            // Gaps come from space runs in break-spaces mode, never from word().
            float space = 0.0f;
            if (lineHasContent_ && !spacing_.breakSpaces && !suppressGap) {
                space = activeMetrics.spaceWidth;
                if (activeStyle->wordSpacing.unit != sui::Length::Auto) {
                    space += activeStyle->wordSpacing.resolve(
                        activeStyle->fontSize, activeStyle->fontSize, 16.0f);
                }
            }
            // A pending soft break matters only where ordinary wrapping is
            // off: under normal wrapping the overflow check below reaches the
            // same break on its own. Either way a fitting word stays put.
            if (breakAllowed_) {
                breakAllowed_ = false;
                if (lineHasContent_ && !wraps &&
                    lineIndent_ + penX_ + space + wWidth > lineAvail_) {
                    newLine();
                    if (clamped_) {
                        clampSuppressed_ = true;
                        return;
                    }
                    refreshActive();
                    wWidth = textWidth(owner_.font_, *activeStyle, w);
                    space = activeMetrics.spaceWidth;
                    if (activeStyle->wordSpacing.unit != sui::Length::Auto) {
                        space += activeStyle->wordSpacing.resolve(
                            activeStyle->fontSize, activeStyle->fontSize, 16.0f);
                    }
                }
            }

            if (isEllipsisActive() && lineIndent_ + penX_ + space + wWidth > lineAvail_) {
                // The current word overflows the container line box and text-overflow: ellipsis is active.
                const std::string kEllipsis = "\xE2\x80\xA6";
                const float ellWidth = textWidth(owner_.font_, *activeStyle, kEllipsis);
                const float usedX = lineIndent_ + penX_ + space;
                const float availForWord = lineAvail_ - usedX - ellWidth;

                if (availForWord > 0.0f) {
                    std::string prefix;
                    std::size_t idx = 0;
                    while (idx < w.size()) {
                        std::size_t next = idx + 1;
                        while (next < w.size() && (static_cast<unsigned char>(w[next]) & 0xC0) == 0x80) {
                            ++next;
                        }
                        std::string cand = prefix + std::string(w.substr(idx, next - idx));
                        if (textWidth(owner_.font_, *activeStyle, cand) > availForWord) {
                            break;
                        }
                        prefix = std::move(cand);
                        idx = next;
                    }
                    if (!prefix.empty()) {
                        std::string result = transformText(*activeStyle, prefix) + kEllipsis;
                        const float rWidth =
                            textWidth(owner_.font_, *activeStyle, result);
                        ownerEl_.addTextRun({result, originX_ + lineIndent_ + penX_ + space,
                                             originY_ + cursorY_ +
                                                 verticalAlignShift(*activeStyle, &metrics_,
                                                                    &activeMetrics),
                                             rWidth, lineHeight_,
                                             baselineForActive(),
                                             activeStyle->fontSize, activeStyle->color.r,
                                             activeStyle->color.g, activeStyle->color.b,
                                             activeStyle->color.a, activeStyle->fontBold,
                                             activeStyle->fontStyle !=
                                                 ComputedStyle::FontNormal,
                                             activeStyle->fontFamily});
                        penX_ += space + rWidth;
                        lineHasContent_ = true;
                        widest_ = std::max(widest_, penX_);
                        ellipsisAdded_ = true;
                        return;
                    }
                }

                if (lineAvail_ - (lineIndent_ + penX_ + space) >= ellWidth) {
                    ownerEl_.addTextRun(
                        {kEllipsis, originX_ + lineIndent_ + penX_ + space,
                         originY_ + cursorY_ +
                             verticalAlignShift(*activeStyle, &metrics_, &activeMetrics),
                         ellWidth, lineHeight_, baselineForActive(),
                         activeStyle->fontSize,
                         activeStyle->color.r, activeStyle->color.g,
                         activeStyle->color.b, activeStyle->color.a,
                         activeStyle->fontBold,
                         activeStyle->fontStyle != ComputedStyle::FontNormal,
                         activeStyle->fontFamily});
                    penX_ += space + ellWidth;
                    lineHasContent_ = true;
                    widest_ = std::max(widest_, penX_);
                    ellipsisAdded_ = true;
                    return;
                }

                auto& runs = ownerEl_.textRuns();
                while (runs.size() > lineRunStartIndex_) {
                    const float avail = (originX_ + lineIndent_ + lineAvail_) - runs.back().x;
                    if (avail >= ellWidth) {
                        std::string& rText = runs.back().text;
                        while (!rText.empty() &&
                               textWidth(owner_.font_, *activeStyle, rText) >
                                   avail - ellWidth) {
                            std::size_t last = rText.size() - 1;
                            while (last > 0 && (static_cast<unsigned char>(rText[last]) & 0xC0) == 0x80) {
                                --last;
                            }
                            rText.erase(last);
                        }
                        rText += kEllipsis;
                        runs.back().width =
                            textWidth(owner_.font_, *activeStyle, rText);
                        penX_ = (runs.back().x - originX_ - lineIndent_) + runs.back().width;
                        lineHasContent_ = true;
                        widest_ = std::max(widest_, penX_);
                        ellipsisAdded_ = true;
                        return;
                    }
                    runs.pop_back();
                }

                ownerEl_.addTextRun(
                    {kEllipsis, originX_ + lineIndent_,
                     originY_ + cursorY_ +
                         verticalAlignShift(*activeStyle, &metrics_, &activeMetrics),
                     ellWidth, lineHeight_, baselineForActive(),
                     activeStyle->fontSize, activeStyle->color.r,
                     activeStyle->color.g, activeStyle->color.b,
                     activeStyle->color.a, activeStyle->fontBold,
                     activeStyle->fontStyle != ComputedStyle::FontNormal,
                     activeStyle->fontFamily});
                penX_ = ellWidth;
                lineHasContent_ = true;
                widest_ = std::max(widest_, penX_);
                ellipsisAdded_ = true;
                return;
            }

            std::string wt = transformText(*activeStyle, w);
            ownerEl_.addTextRun(
                {wt, originX_ + lineIndent_ + penX_ + space,
                 originY_ + cursorY_ +
                     verticalAlignShift(*activeStyle, &metrics_, &activeMetrics),
                 wWidth, lineHeight_, baselineForActive(),
                 activeStyle->fontSize, activeStyle->color.r, activeStyle->color.g,
                 activeStyle->color.b, activeStyle->color.a, activeStyle->fontBold,
                 activeStyle->fontStyle != ComputedStyle::FontNormal,
                 activeStyle->fontFamily});
            penX_ += space + wWidth;
            lineHasContent_ = true;
            widest_ = std::max(widest_, penX_);
        }

        Layout& owner_;
        Element& ownerEl_;
        const ComputedStyle& style_;
        Metrics metrics_;
        // The current line's height. A larger `::first-letter` grows the first
        // line; newLine() restores the container's height for later lines.
        float lineHeight_ = 0.0f;
        float maxWidth_ = 0.0f;
        float originX_ = 0.0f;
        float originY_ = 0.0f;
        float penX_ = 0.0f;
        float cursorY_ = 0.0f;
        float bottom_ = 0.0f;
        float widest_ = 0.0f;
        // Current line geometry with floats taken into account: `lineIndent_` is
        // the left inset and `lineAvail_` is the width left after it.
        float lineIndent_ = 0.0f;
        float lineAvail_ = 0.0f;
        bool lineHasContent_ = false;
        // The first formatted line still owes its first-grapheme split. Leading
        // preserved spaces or an inside marker may already have content on the
        // line, so those paths remember that the letter still follows.
        bool firstLetterPending_ = false;
        bool firstLetterAfterSpace_ = false;
        bool firstLetterAfterMarker_ = false;
        // `::first-line` applies until the first committed break. The pseudo
        // font is resolved once against the block's own style.
        PseudoFont firstLineFont_;
        bool firstLineHasFont_ = false;
        // Set when floats narrow a line, which rules `text-wrap` re-breaking out:
        // the inset changes per line, so one target width is meaningless.
        bool sawFloat_ = false;
        // Set by a `<wbr/>`: the next word starts a fresh line when anything
        // is already on this one. Cleared by every word, taken or not.
        bool breakAllowed_ = false;
        bool ellipsisAdded_ = false;
        std::size_t lineRunStartIndex_ = 0;
        Spacing spacing_ = spacingFor(ComputedStyle::WrapText);
        // `line-clamp`: lines committed so far, the cap (0 while probing, so
        // max-content measurement never clamps), and the clamp state. The
        // ellipsis is speculative until content past the cap proves it needed.
        const int lineClamp_ = 0;
        int linesCommitted_ = 0;
        bool clamped_ = false;
        bool clampSuppressed_ = false;
        std::vector<Element::TextRun> droppedClampRuns_;
    };

    // One throwaway breaker pass for `text-wrap` probing: runs it, reads what
    // it produced, and rolls the runs back to where the caller started.
    struct WrapProbe {
        int lines = 0;
        int lastWords = 0;
        bool sawFloat = false;
    };

    WrapProbe probeWrap(Element& el, const ComputedStyle& s, float width, float contentX,
                        float contentY, std::size_t runStart) {
        // The pass places the container's float children again, and the float
        // context is shared: the probe works on a copy so its entries are
        // thrown away instead of shoving the next real placement lower.
        FloatContext* const savedFloats = floats_;
        FloatContext scratch;
        if (savedFloats) {
            scratch = *savedFloats;
            floats_ = &scratch;
        }
        Flow probe(*this, el, s, metricsFor(font_, s), width, contentX, contentY);
        probe.inlineBox(el, s);
        probe.finish();
        const WrapProbe out{probe.lineCount(), probe.lastLineWords(), probe.sawFloat()};
        floats_ = savedFloats;
        el.textRuns().resize(runStart);
        return out;
    }

    // The width `text-wrap: balance|pretty` lays the paragraph out at, or
    // `inner` when the greedy break already reads well.
    float rebreakWidth(Element& el, const ComputedStyle& s, float inner, float contentX,
                       float contentY, std::size_t runStart) {
        const WrapProbe greedy = probeWrap(el, s, inner, contentX, contentY, runStart);
        if (greedy.lines < 2 || greedy.lines > kMaxRebreakLines || greedy.sawFloat) {
            return inner;
        }
        if (s.textWrapStyle == ComputedStyle::TextWrapStyle::Balance) {
            // Bisect for the smallest width that still fits the same number of
            // lines: the widest line stops being the greedy one.
            float lo = 0.0f;
            float hi = inner;
            for (int step = 0; step < 6; ++step) {
                const float mid = (lo + hi) * 0.5f;
                if (mid <= 1.0f) {
                    break;
                }
                if (probeWrap(el, s, mid, contentX, contentY, runStart).lines ==
                    greedy.lines) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            return hi;
        }
        // `pretty` fights one-word last lines: step the width down until a word
        // is pulled down, and stop once narrowing starts a line of its own.
        if (greedy.lastWords >= 2) {
            return inner;
        }
        for (int step = 1; step <= 6; ++step) {
            const float width = inner * (1.0f - 0.05f * static_cast<float>(step));
            const WrapProbe probe = probeWrap(el, s, width, contentX, contentY, runStart);
            if (probe.lines == greedy.lines && probe.lastWords >= 2) {
                return width;
            }
            if (probe.lines > greedy.lines) {
                break;
            }
        }
        return inner;
    }

    const StyleResolver& resolver_;
    float viewportWidth_ = 0.0f;
    float viewportHeight_ = 0.0f;
    const TextFont* font_ = nullptr;

    // Margin collapsing asks the same questions repeatedly while a pass walks
    // nested blocks. Cache the structural answers for this layout pass; width
    // is part of the key because percentage margins/padding resolve against it.
    using MarginKey = std::pair<const Element*, float>;
    mutable std::map<MarginKey, MarginStrut> topCache_;
    mutable std::map<MarginKey, MarginStrut> bottomCache_;
    mutable std::map<MarginKey, bool> throughCache_;
    mutable std::map<const Element*, bool> pureCache_;
    mutable std::map<const Element*, bool> floatChildCache_;
    mutable std::map<const Element*, bool> laidOutCache_;
};

struct ScrollExtent {
    float right = 0.0f;
    float bottom = 0.0f;
};

// After both passes, a box that clips has to know where its content actually
// ends, and every laid-out element should report honest client/scroll extents to
// script. One post-order walk folds the furthest descendant edge per element,
// records them, and clamps the offsets of the boxes whose axis clips. Text runs
// count as content, so a fixed-height box with too much text scrolls too.
ScrollExtent measureScrollExtents(Element& el, const StyleResolver& resolver) {
    float right = el.layoutX() + el.layoutWidth();
    float bottom = el.layoutY() + el.layoutHeight();
    for (const Element::TextRun& run : el.textRuns()) {
        right = std::max(right, run.x + run.width);
        bottom = std::max(bottom, run.y + run.height);
    }
    for (const auto& child : el.children()) {
        if (child && !child->isTextNode()) {
            const ScrollExtent childExtent = measureScrollExtents(*child, resolver);
            right = std::max(right, childExtent.right);
            bottom = std::max(bottom, childExtent.bottom);
        }
    }
    if (el.isVisual()) {
        const ComputedStyle s = resolver.resolve(el);
        if (s.display != ComputedStyle::None) {
            el.setScrollExtents(
                el.paddingWidth(), el.paddingHeight(),
                std::max(el.paddingWidth(), right - el.contentX()),
                std::max(el.paddingHeight(), bottom - el.contentY()),
                s.overflowX != ComputedStyle::Visible, s.overflowY != ComputedStyle::Visible);
        }
    }
    return {right, bottom};
}

// Every laid-out width and height in one number, so a multipass layout can
// tell a settled tree apart from one whose container queries are still moving
// boxes. Widths settle top-down; heights settle after a pass, which is why
// height queries need one extra confirmation pass. The walk is deterministic,
// so equal trees sum equal.
double layoutGeometryChecksum(const Element& el) {
    double sum = static_cast<double>(el.layoutWidth() + el.layoutHeight());
    for (const auto& child : el.children()) {
        if (child && !child->isTextNode()) {
            sum += layoutGeometryChecksum(*child);
        }
    }
    return sum;
}

// Elements skipped as `display:none` keep whatever boxes an earlier pass (or
// frame) gave them. With container queries flipping display mid-layout that
// stale geometry would disagree with paint — which skips them — so a
// multipass layout zeroes hidden boxes and drops their runs once the passes
// settle. Anything else keeps the behavior it always had.
void clearHiddenBoxes(Element& el, const StyleResolver& resolver) {
    // Non-visual wrappers still contain laid-out descendants, so only the
    // zeroing (not the walk) skips them.
    if (el.isVisual() && resolver.resolve(el).display == ComputedStyle::None) {
        el.setLayoutX(0.0f);
        el.setLayoutY(0.0f);
        el.setLayoutWidth(0.0f);
        el.setLayoutHeight(0.0f);
        el.setPaddingX(0.0f);
        el.setPaddingY(0.0f);
        el.setPaddingWidth(0.0f);
        el.setPaddingHeight(0.0f);
        el.setContentX(0.0f);
        el.setContentY(0.0f);
        el.setContentWidth(0.0f);
        el.setContentHeight(0.0f);
        el.clearTextRuns();
    }
    for (const auto& child : el.children()) {
        if (child && !child->isTextNode()) {
            clearHiddenBoxes(*child, resolver);
        }
    }
}

}  // namespace

namespace {

void shiftTree(Element& el, float dx, float dy) {
    if (dx == 0.0f && dy == 0.0f) {
        return;
    }
    el.setLayoutX(el.layoutX() + dx);
    el.setLayoutY(el.layoutY() + dy);
    el.setPaddingX(el.paddingX() + dx);
    el.setPaddingY(el.paddingY() + dy);
    el.setContentX(el.contentX() + dx);
    el.setContentY(el.contentY() + dy);
    for (Element::TextRun& run : el.textRuns()) {
        run.x += dx;
        run.y += dy;
    }
    for (const auto& child : el.children()) {
        if (child && !child->isTextNode()) {
            shiftTree(*child, dx, dy);
        }
    }
}

}  // namespace

void BlockLayout::centerInViewport(Element& el, float viewportWidth,
                                   float viewportHeight) {
    // A modal dialog centers itself every frame from a fresh layout, so the
    // move never accumulates and hit-testing and painting follow the boxes.
    shiftTree(el, (viewportWidth - el.layoutWidth()) * 0.5f - el.layoutX(),
              (viewportHeight - el.layoutHeight()) * 0.5f - el.layoutY());
}

void BlockLayout::translateSubtree(Element& el, float dx, float dy) {
    // A nested document lays out at its own origin; this lands it on the box
    // that hosts it. Callers re-layout first, so the shift never accumulates.
    shiftTree(el, dx, dy);
}

void BlockLayout::layout(Element* root, const LayoutInput& input) {
    if (!root || !input.resolver) {
        return;
    }
    // Container queries read laid-out boxes, which only exist after a pass:
    // with container rules in play, repeat until geometry settles (at most four
    // passes; definite widths settle on the second, heights on the third).
    // Anything else lays out exactly once, as before.
    double settled = -1.0;
    for (int pass = 0; pass < 4; ++pass) {
        Layout layout(*input.resolver, input.font, input.viewportWidth,
                      input.viewportHeight);
        layout.run(*root, input.viewportWidth, 0.0f, 0.0f);
        layout.placeOutOfFlow();
        if (!input.resolver->anyContainerRules()) {
            break;
        }
        const double geometry = layoutGeometryChecksum(*root);
        if (geometry == settled) {
            break;
        }
        settled = geometry;
    }
    if (input.resolver->anyContainerRules()) {
        clearHiddenBoxes(*root, *input.resolver);
    }
    measureScrollExtents(*root, *input.resolver);
}

}  // namespace sui

#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

// A CSS length. Percentages and font-relative units cannot become a number until
// layout knows the containing block, so the unit survives style resolution and
// is only resolved at use. Constructing from a float means a plain px value,
// which keeps `margin = {8, 0, 8, 0}` valid.
struct Length {
    enum Unit : std::uint8_t { Auto, Px, Percent, Em, Rem };

    Unit unit = Auto;
    float value = 0.0f;

    constexpr Length() noexcept = default;
    constexpr Length(float px) noexcept : unit(Px), value(px) {}
    constexpr Length(Unit u, float v) noexcept : unit(u), value(v) {}

    // `percentBase` is whatever the percentage is relative to: the containing
    // block's width or height, or the parent font size for font-size itself.
    // `emBase` is the element's own font size, `remBase` the root's.
    float resolve(float percentBase = 0.0f, float emBase = 0.0f,
                  float remBase = 0.0f) const noexcept {
        switch (unit) {
            case Auto:
                return 0.0f;
            case Px:
                return value;
            case Percent:
                return value * 0.01f * percentBase;
            case Em:
                return value * emBase;
            case Rem:
                return value * remBase;
        }
        return 0.0f;
    }

    bool isAuto() const noexcept { return unit == Auto; }
};

// Whether `width`/`height` describe the content box or the border box.
enum class BoxSizing : std::uint8_t { ContentBox, BorderBox };

// Plain numbers, so layout can add them without re-resolving units.
struct Edges {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
};

struct BoxEdge {
    Length top;
    Length right;
    Length bottom;
    Length left;
};

struct ComputedStyle {
    enum Display : std::uint8_t { Block, Inline, InlineBlock, None };

    Display display = Block;

    // Dimensions. Lengths keep their unit until layout resolves them against the
    // containing block; see UsedBox.
    Length width;   // Auto = shrink or fill, per display
    Length height;  // Auto = content-derived
    Length minWidth;
    Length minHeight;
    Length maxWidth;  // Auto = none
    Length maxHeight; // Auto = none

    BoxSizing boxSizing = BoxSizing::ContentBox;

    BoxEdge margin;
    BoxEdge padding;
    BoxEdge borderWidth;

    // Colors
    SDL_Color backgroundColor = {0, 0, 0, 0};  // transparent
    SDL_Color color = {255, 255, 255, 255};    // default text
    SDL_Color borderColor = {63, 63, 70, 255};

    // Typography
    float fontSize = 16.0f;  // px
    std::string fontFamily;   // empty = default
    bool fontBold = false;
    float lineHeight = 1.2f;  // multiplier

    // Text alignment
    enum TextAlign : std::uint8_t { Left, Center, Right, Justify };
    TextAlign textAlign = Left;

    // Positioning (minimal for now)
    enum Position : std::uint8_t { Static, Relative, Absolute, Fixed };
    Position position = Static;
    Length left;
    Length top;
    Length right;
    Length bottom;

    // Visibility
    bool visible = true;

    // Which inheritable properties the user-agent style set on its own. Anything
    // not flagged here flows down from the parent. Author rules and inline
    // styles apply after inheritance, so they always win.
    enum Specified : std::uint16_t {
        SpecifiedColor = 1u << 0,
        SpecifiedFontSize = 1u << 1,
        SpecifiedFontFamily = 1u << 2,
        SpecifiedFontBold = 1u << 3,
        SpecifiedLineHeight = 1u << 4,
        SpecifiedTextAlign = 1u << 5,
    };
    std::uint16_t specified = 0;
};

// The box-model lengths of a style after percentages and font-relative units have
// been resolved against a containing block. Layout measures with these instead of
// re-deriving them per property, and `width`/`height` are already in content-box
// terms even when the author wrote border-box.
struct UsedBox {
    float width = 0.0f;  // 0 = auto
    float height = 0.0f; // 0 = auto
    float minWidth = 0.0f;
    float maxWidth = 0.0f;  // 0 = none
    float minHeight = 0.0f;
    float maxHeight = 0.0f; // 0 = none
    Edges margin;
    Edges padding;
    Edges borderWidth;

    float horizontalChrome() const noexcept {
        return margin.left + margin.right + borderWidth.left + borderWidth.right +
               padding.left + padding.right;
    }
    float verticalChrome() const noexcept {
        return margin.top + margin.bottom + borderWidth.top + borderWidth.bottom +
               padding.top + padding.bottom;
    }
};

// Resolves a style's box lengths. Percentages resolve against the containing
// block; `em` uses the element's own font size and `rem` the root's, matching CSS.
inline UsedBox resolveBox(const ComputedStyle& style, float containingWidth,
                          float containingHeight, float rootFontSize) noexcept {
    const float blockBase = containingHeight;
    const float em = style.fontSize;

    auto px = [&](const Length& l, float base) { return l.resolve(base, em, rootFontSize); };
    // A percentage margin or padding is always relative to the containing block's
    // *inline* size, whichever edge it sits on; only width and height use the
    // matching axis.
    auto inlineEdges = [&](const BoxEdge& e) {
        return Edges{px(e.top, containingWidth), px(e.right, containingWidth),
                     px(e.bottom, containingWidth), px(e.left, containingWidth)};
    };

    UsedBox box;
    box.margin = inlineEdges(style.margin);
    box.padding = inlineEdges(style.padding);
    // border-width takes no percentages or font-relative units in CSS.
    box.borderWidth = Edges{px(style.borderWidth.top, 0.0f), px(style.borderWidth.right, 0.0f),
                            px(style.borderWidth.bottom, 0.0f), px(style.borderWidth.left, 0.0f)};

    box.minWidth = px(style.minWidth, containingWidth);
    box.maxWidth = px(style.maxWidth, containingWidth);
    box.minHeight = px(style.minHeight, blockBase);
    box.maxHeight = px(style.maxHeight, blockBase);

    const float chromeW = box.borderWidth.left + box.borderWidth.right + box.padding.left +
                          box.padding.right;
    const float chromeH = box.borderWidth.top + box.borderWidth.bottom + box.padding.top +
                          box.padding.bottom;

    box.width = style.width.isAuto() ? 0.0f : px(style.width, containingWidth);
    box.height = style.height.isAuto() ? 0.0f : px(style.height, blockBase);
    if (style.boxSizing == BoxSizing::BorderBox) {
        // The authored width covers padding and border, so the content box is
        // what is left over.
        box.width = box.width > 0.0f ? std::max(0.0f, box.width - chromeW) : 0.0f;
        box.height = box.height > 0.0f ? std::max(0.0f, box.height - chromeH) : 0.0f;
    }
    return box;
}

// Fills in the inheritable properties the user-agent style left unspecified.
inline void inheritInto(ComputedStyle& style, const ComputedStyle& parent) noexcept {
    if (!(style.specified & ComputedStyle::SpecifiedColor)) {
        style.color = parent.color;
    }
    if (!(style.specified & ComputedStyle::SpecifiedFontSize)) {
        style.fontSize = parent.fontSize;
    }
    if (!(style.specified & ComputedStyle::SpecifiedFontFamily)) {
        style.fontFamily = parent.fontFamily;
    }
    if (!(style.specified & ComputedStyle::SpecifiedFontBold)) {
        style.fontBold = parent.fontBold;
    }
    if (!(style.specified & ComputedStyle::SpecifiedLineHeight)) {
        style.lineHeight = parent.lineHeight;
    }
    if (!(style.specified & ComputedStyle::SpecifiedTextAlign)) {
        style.textAlign = parent.textAlign;
    }
}

}  // namespace sui
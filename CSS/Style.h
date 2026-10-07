#pragma once

#include <SDL3/SDL.h>

#include "CSS/Easing.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sui {

struct Length;

// A calc()/min()/max()/clamp() expression tree. A Value node holds one length
// leaf; Add/Sub/Mul/Div and the range nodes hold their operands as lengths that
// may themselves be further expressions. `dimensioned` records whether any leaf
// carries a unit, which is what tells a legal `100% / 2` apart from an illegal
// `100% / 2px` while the tree is being built.
struct MathExpr {
    enum class Op : std::uint8_t { Value, Add, Sub, Mul, Div, Min, Max, Clamp };

    Op op = Op::Value;
    std::vector<Length> args;
    bool dimensioned = false;

    float eval(float percentBase, float emBase, float remBase) const;
};

// A CSS length. Percentages and font-relative units cannot become a number until
// layout knows the containing block, so the unit survives style resolution and
// is only resolved at use. Constructing from a float means a plain px value,
// which keeps `margin = {8, 0, 8, 0}` valid. A Calc value carries a shared
// expression tree instead of a number; Number marks the unitless operand that
// only ever appears inside multiplication and division.
struct Length {
    enum Unit : std::uint8_t { Auto, Px, Percent, Em, Rem, Number, Calc };

    Unit unit = Auto;
    float value = 0.0f;
    std::shared_ptr<const MathExpr> expr;

    constexpr Length() noexcept = default;
    constexpr Length(float px) noexcept : unit(Px), value(px) {}
    constexpr Length(Unit u, float v) noexcept : unit(u), value(v) {}
    explicit Length(std::shared_ptr<const MathExpr> e) noexcept
        : unit(Calc), expr(std::move(e)) {}

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
            case Number:
                return value;
            case Calc:
                return expr ? expr->eval(percentBase, emBase, remBase) : 0.0f;
        }
        return 0.0f;
    }

    bool isAuto() const noexcept { return unit == Auto; }
};

inline float MathExpr::eval(float percentBase, float emBase, float remBase) const {
    const auto at = [&](std::size_t i) {
        return args[i].resolve(percentBase, emBase, remBase);
    };
    switch (op) {
        case Op::Value:
            return at(0);
        case Op::Add:
            return at(0) + at(1);
        case Op::Sub:
            return at(0) - at(1);
        case Op::Mul:
            return at(0) * at(1);
        case Op::Div: {
            const float divisor = at(1);
            return divisor != 0.0f ? at(0) / divisor : 0.0f;
        }
        case Op::Min: {
            float best = at(0);
            for (std::size_t i = 1; i < args.size(); ++i) {
                best = std::min(best, at(i));
            }
            return best;
        }
        case Op::Max: {
            float best = at(0);
            for (std::size_t i = 1; i < args.size(); ++i) {
                best = std::max(best, at(i));
            }
            return best;
        }
        case Op::Clamp: {
            const float lo = at(0);
            const float middle = at(1);
            const float hi = at(2);
            return std::max(lo, std::min(middle, hi));
        }
    }
    return 0.0f;
}

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

// Padding plus border on both edges of one axis, as a single number. Flex sizing
// needs this because it decides what an item may grow or shrink by.
inline float horizontalEdges(const Edges& padding, const Edges& border) noexcept {
    return padding.left + padding.right + border.left + border.right;
}

inline float verticalEdges(const Edges& padding, const Edges& border) noexcept {
    return padding.top + padding.bottom + border.top + border.bottom;
}

struct ComputedStyle {
    enum Display : std::uint8_t {
        Block,
        Inline,
        InlineBlock,
        Flex,
        InlineFlex,
        Grid,
        InlineGrid,
        None
    };

    Display display = Block;

    bool isFlexContainer() const noexcept {
        return display == Display::Flex || display == Display::InlineFlex;
    }
    bool isGridContainer() const noexcept {
        return display == Display::Grid || display == Display::InlineGrid;
    }

    // Dimensions. Lengths keep their unit until layout resolves them against the
    // containing block; see UsedBox.
    Length width;   // Auto = shrink or fill, per display
    Length height;  // Auto = content-derived
    Length minWidth;
    Length minHeight;
    Length maxWidth;  // Auto = none
    Length maxHeight; // Auto = none

    BoxSizing boxSizing = BoxSizing::ContentBox;

    // Flexbox. A flex container lays its children out along one axis; the free
    // space left over is distributed by justify-content on the main axis and the
    // children are aligned on the cross axis by align-items.
    // Justify and Align share their non-stretch values, so they share one enum and
    // an align field can be used wherever a justify field is expected. The values
    // are prefixed because `Center` and `Justify` already exist in TextAlign.
    enum FlexAlign : std::uint8_t {
        AlignStretch,
        AlignStart,
        AlignEnd,
        AlignCenter,
        AlignBaseline,
        AlignSpaceBetween,
        AlignSpaceAround,
        AlignSpaceEvenly
    };
    enum FlexDirection : std::uint8_t { Row, RowReverse, Column, ColumnReverse };
    enum FlexWrap : std::uint8_t { NoWrap, Wrap, WrapReverse };

    FlexDirection flexDirection = Row;
    FlexAlign justifyContent = AlignStart;
    FlexAlign alignItems = AlignStretch;
    FlexAlign alignSelf = AlignStretch;
    FlexAlign alignContent = AlignStretch;
    // Grid item alignment within its own area, on the inline and block axes.
    FlexAlign justifyItems = AlignStretch;
    FlexAlign justifySelf = AlignStretch;
    FlexWrap flexWrap = NoWrap;
    // `gap` sets both; `row-gap` and `column-gap` override one each. Auto means
    // "not authored", which is what makes the override detectable.
    float gap = 0.0f;
    float rowGap = -1.0f;
    float columnGap = -1.0f;

    // One value per child, as authored. `Flex` means the browser's `flex: 0 1 auto`,
    // which is not the same as "unset".
    struct FlexFactors {
        float grow = 0.0f;
        float shrink = 1.0f;
        Length basis;  // Auto = use the item's own width/height
    };
    FlexFactors flex;

    BoxEdge margin;
    BoxEdge padding;
    BoxEdge borderWidth;

    // Colors
    SDL_Color backgroundColor = {0, 0, 0, 0};  // transparent
    SDL_Color color = {255, 255, 255, 255};    // default text
    SDL_Color borderColor = {63, 63, 70, 255};

    // How a border edge is drawn. Prefixed because `Solid`, `Dashed` and friends
    // read too loosely on their own next to the other unscoped enumerators here.
    // The values CSS treats as 3D are drawn as plain strokes; see Difference.md.
    enum BorderStyle : std::uint8_t {
        BorderStyleNone,
        BorderSolid,
        BorderDashed,
        BorderDotted,
        BorderDouble,
        BorderGroove,
        BorderRidge,
        BorderInset,
        BorderOutset
    };
    struct PerSide {
        BorderStyle top = BorderSolid;
        BorderStyle right = BorderSolid;
        BorderStyle bottom = BorderSolid;
        BorderStyle left = BorderSolid;
    };
    PerSide borderStyles;

    // Corner rounding, kept as lengths because a percentage resolves against the
    // box's own width and height, which layout is the first to know.
    struct RadiusPair {
        Length x;
        Length y;
    };
    struct Radii {
        RadiusPair topLeft;
        RadiusPair topRight;
        RadiusPair bottomRight;
        RadiusPair bottomLeft;
    };
    Radii borderRadii;

    // An outline is drawn outside the border box and takes no space in layout.
    struct Outline {
        Length width;
        Length offset;
        BorderStyle style = BorderStyleNone;
        SDL_Color color = {59, 130, 246, 255};
    };
    Outline outline;

    // Group opacity. Multiplied into the alpha of everything the element paints,
    // including its descendants, which is what makes it different from a colour's
    // own alpha.
    float opacity = 1.0f;

    // Shadows are painted in list order, so a later one sits over an earlier one.
    struct BoxShadow {
        bool inset = false;
        Length offsetX;
        Length offsetY;
        Length blurRadius;
        Length spread;
        SDL_Color color = {0, 0, 0, 96};
    };
    std::vector<BoxShadow> boxShadows;

    struct TextShadow {
        Length offsetX;
        Length offsetY;
        Length blurRadius;
        SDL_Color color = {0, 0, 0, 128};
    };
    std::vector<TextShadow> textShadows;

    // Background layers. CSS paints the *first* image layer on top of the ones after
    // it, and `background-color` sits below all of them, so index 0 here is the
    // topmost. The companion properties are parallel lists rather than one struct per
    // layer because CSS accumulates them independently and then matches them up by
    // position, repeating the shorter list — see `backgroundListAt`.
    struct GradientStop {
        // An `Auto` offset means "not authored". A gradient gives an unauthored stop
        // the midpoint of its neighbours, and gives a single one a flat fill, so an
        // explicit `0%` is not the same thing and has to stay distinguishable.
        Length offset;
        SDL_Color color = {0, 0, 0, 255};
    };

    struct LinearGradient {
        // Degrees clockwise from "to top", which is how CSS defines the angle. 0deg
        // is up, 90deg right, 180deg down.
        float angle = 180.0f;
        std::vector<GradientStop> stops;
    };

    struct RadialGradient {
        enum Shape : std::uint8_t { Circle, Ellipse };
        enum Extent : std::uint8_t { FarthestCorner, ClosestSide, FarthestSide, ClosestCorner };
        Shape shape = Circle;
        Extent extent = FarthestCorner;
        // The centre, defaulting to the middle of the box. The default is a
        // percentage rather than a pixel count, so it follows the box it is sized
        // against instead of landing 50px from its left edge.
        Length centreX{Length::Percent, 50.0f};
        Length centreY{Length::Percent, 50.0f};
        // Only read when `hasRadii`; a circle takes the first, an ellipse both.
        Length radiusX;
        Length radiusY;
        bool hasRadii = false;
        // The focus, which defaults to the centre. `hasFocus` distinguishes "at the
        // centre" from "not authored", which matters only for the centre in the spec.
        Length focalX;
        Length focalY;
        bool hasFocus = false;
        std::vector<GradientStop> stops;
    };

    struct BackgroundImage {
        enum Kind : std::uint8_t { None, Url, LinearGradientImage, RadialGradientImage };
        Kind kind = None;
        std::string href;
        LinearGradient linear;
        RadialGradient radial;
    };
    std::vector<BackgroundImage> backgroundImages;

    struct BackgroundSize {
        enum Mode : std::uint8_t { Explicit, AutoSize, Cover, Contain };
        Mode mode = AutoSize;
        // `Auto` on an axis means the image's own size there, except where the other
        // axis is explicit, in which case it means "whatever keeps the aspect ratio".
        Length width;
        Length height;
    };
    std::vector<BackgroundSize> backgroundSizes;

    // A keyword pair plus a length offset, which is what every CSS `background-position`
    // form reduces to. `right 10px` is `End` with a negative offset, because a length
    // after `right` measures *back* from the edge.
    struct BackgroundPosition {
        enum Keyword : std::uint8_t { Start, Center, End };
        Keyword horizontal = Start;
        Keyword vertical = Start;
        Length offsetX;
        Length offsetY;
    };
    std::vector<BackgroundPosition> backgroundPositions;

    struct RepeatPair {
        bool x = true;
        bool y = true;
        // `space` and `round` both need the tile count and the leftover, which only
        // the paint stage knows, so the choice is carried rather than resolved.
        enum Kind : std::uint8_t { Repeat, NoRepeat, Space, Round };
        Kind kind = Repeat;
    };
    std::vector<RepeatPair> backgroundRepeats;

    // Which of the three boxes a layer paints inside. Prefixed because `BorderBox`
    // reads ambiguously next to the border members above.
    enum BackgroundBox : std::uint8_t { BackgroundBorderBox, BackgroundPaddingBox,
                                        BackgroundContentBox };
    std::vector<BackgroundBox> backgroundClips;
    std::vector<BackgroundBox> backgroundOrigins;
    // Only `Fixed` differs from `Scroll` once there is a scrolling viewport, so this
    // is parsed and carried but currently paints the same either way.
    enum BackgroundAttachment : std::uint8_t { BackgroundScroll, BackgroundFixed,
                                                BackgroundLocal };
    std::vector<BackgroundAttachment> backgroundAttachments;

    // Typography
    float fontSize = 16.0f;  // px
    std::string fontFamily;   // empty = default
    bool fontBold = false;
    float lineHeight = 1.2f;  // multiplier

    // Text properties (basic)
    enum FontStyle : std::uint8_t { FontNormal, FontItalic, FontOblique };
    FontStyle fontStyle = FontNormal;
    enum TextDecorationLine : std::uint8_t { DecNone, DecUnderline, DecOverline, DecLineThrough };
    TextDecorationLine textDecorationLine = DecNone;
    Length letterSpacing;
    Length wordSpacing;
    Length textIndent;
    enum TextTransform : std::uint8_t { TransformNone, TransformUppercase, TransformLowercase, TransformCapitalize };
    TextTransform textTransform = TransformNone;
    enum VerticalAlign : std::uint8_t { VAlignBaseline, VAlignTop, VAlignBottom, VAlignMiddle, VAlignSub, VAlignSuper };
    VerticalAlign verticalAlign = VAlignBaseline;
    std::string transform;
    std::string filter;

    // Grid track sizing from `grid-template-columns` / `grid-template-rows`. A
    // track is a fixed length, an `fr` share of the leftover space, or `auto`
    // (content-sized). Kept as a list so several tracks can be described; a
    // missing row list means every row is content-sized.
    struct GridTrack {
        Length size;      // used when !isFr && !isAuto
        float fr = 0.0f;  // used when isFr
        bool isFr = false;
        bool isAuto = false;
        // `minmax(min, max)` floor. `hasMin` says to take `max(size, minSize)`;
        // an `auto` or unitless minimum resolves to zero.
        bool hasMin = false;
        Length minSize;
    };
    std::vector<GridTrack> gridColumns;
    std::vector<GridTrack> gridRows;
    // Set when the track list is a single `repeat(auto-fill, ...)` / `repeat(auto-fit,
    // ...)`. The body is stored in gridColumns/gridRows and layout repeats it to as
    // many tracks as fit the container; `autoFit` collapses the leftovers.
    bool gridColumnsAutoFill = false;
    bool gridColumnsAutoFit = false;
    bool gridRowsAutoFill = false;
    bool gridRowsAutoFit = false;

    // Custom properties (`--name`). They are values, not styles: nothing types or
    // validates them, and `var(--name)` resolves against them once this element's
    // whole cascade has settled. They inherit like every other property — this
    // engine has no per-property inheritance table, so they copy like the rest.
    std::map<std::string, std::string, std::less<>> custom;

    // Transition and animation declarations, kept as the parallel lists CSS pairs
    // positionally, always repeating a shorter list's last entry (see listAt in
    // CSS/Animation.h). They are computed values, not inherited: an animation on
    // a parent does not reach its children. The empty-initialised vectors are the
    // "not authored" state, so `transition-property` alone (duration defaults to
    // 0) and a shorthand both read naturally off the same shape.
    std::vector<std::string> transitionProperties;
    std::vector<float> transitionDurationsMs;
    std::vector<Easing> transitionEasings;
    std::vector<float> transitionDelaysMs;

    std::vector<std::string> animationNames;
    std::vector<float> animationDurationsMs;
    std::vector<Easing> animationEasings;
    std::vector<float> animationDelaysMs;
    std::vector<float> animationIterations;  // < 0 = infinite
    std::vector<std::string> animationDirections;
    std::vector<std::string> animationFills;
    // 0 = running, 1 = paused. A bool vector would make listAt() bind its proxy
    // temporaries to a `const bool&`, so the flags are bytes.
    std::vector<std::uint8_t> animationPaused;

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

    // Clipping. `Visible` means children may spill past the padding box; the other
    // three crop them to it. `Auto` is kept distinct from `Scroll` so the computed
    // value reads back honestly even though the engine does not draw scrollbars.
    enum Overflow : std::uint8_t { Visible, Hidden, Scroll, Auto };
    Overflow overflowX = Visible;
    Overflow overflowY = Visible;

    // Paint order within a stacking context. Higher wins; equal values keep
    // document order. Only positioned elements and flex/grid items take part.
    int zIndex = 0;
    bool positioned = false;

    // `white-space: nowrap` and `pre` stop the formatter breaking at spaces. `pre`
    // additionally keeps newlines and runs of spaces. Prefixed because `NoWrap`
    // already belongs to FlexWrap.
    enum WhiteSpace : std::uint8_t { WrapText, WrapNone, WrapPre, WrapPreWrap };
    WhiteSpace whiteSpace = WrapText;

    // The text a generated `::before` or `::after` puts in the box. Empty means the
    // pseudo-element is not generated at all.
    std::string content;

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
        SpecifiedFontStyle = 1u << 6,
        SpecifiedTextDecorationLine = 1u << 7,
        SpecifiedLetterSpacing = 1u << 8,
        SpecifiedWordSpacing = 1u << 9,
        SpecifiedTextIndent = 1u << 10,
        SpecifiedTextTransform = 1u << 11,
        SpecifiedVerticalAlign = 1u << 12,
        SpecifiedTransform = 1u << 13,
        SpecifiedFilter = 1u << 14,
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
    // `border-style: none` is what removes a border, not a width of zero, so an
    // authored `border-width: 5px` with the style set to none still takes no space.
    if (style.borderStyles.top == ComputedStyle::BorderStyleNone) box.borderWidth.top = 0.0f;
    if (style.borderStyles.right == ComputedStyle::BorderStyleNone) box.borderWidth.right = 0.0f;
    if (style.borderStyles.bottom == ComputedStyle::BorderStyleNone) box.borderWidth.bottom = 0.0f;
    if (style.borderStyles.left == ComputedStyle::BorderStyleNone) box.borderWidth.left = 0.0f;

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
    if (!(style.specified & ComputedStyle::SpecifiedFontStyle)) {
        style.fontStyle = parent.fontStyle;
    }
    if (!(style.specified & ComputedStyle::SpecifiedTextDecorationLine)) {
        style.textDecorationLine = parent.textDecorationLine;
    }
    if (!(style.specified & ComputedStyle::SpecifiedLetterSpacing)) {
        style.letterSpacing = parent.letterSpacing;
    }
    if (!(style.specified & ComputedStyle::SpecifiedWordSpacing)) {
        style.wordSpacing = parent.wordSpacing;
    }
    if (!(style.specified & ComputedStyle::SpecifiedTextIndent)) {
        style.textIndent = parent.textIndent;
    }
    if (!(style.specified & ComputedStyle::SpecifiedTextTransform)) {
        style.textTransform = parent.textTransform;
    }
    if (!(style.specified & ComputedStyle::SpecifiedVerticalAlign)) {
        style.verticalAlign = parent.verticalAlign;
    }
    if (!(style.specified & ComputedStyle::SpecifiedTransform)) {
        style.transform = parent.transform;
    }
    if (!(style.specified & ComputedStyle::SpecifiedFilter)) {
        style.filter = parent.filter;
    }
    // Custom properties always inherit; the resolved element's own cascade then
    // overwrites the entries it declares locally.
    style.custom = parent.custom;
}

}  // namespace sui
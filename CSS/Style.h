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
        Table,
        InlineTable,
        TableRow,
        TableCell,
        TableRowGroup,
        TableHeaderGroup,
        TableFooterGroup,
        TableColumn,
        TableColumnGroup,
        TableCaption,
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
    // An explicit `accent-color` for native controls (checkbox, radio, range,
    // progress); `auto` leaves `hasAccentColor` clear and the controls paint
    // their defaults. Inherited like `color`.
    SDL_Color accentColor = {0, 0, 0, 255};
    bool hasAccentColor = false;

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

    // A `clip-path` shape, kept as lengths for the same reason as the radii:
    // percentages resolve against the reference box at paint time. Anything
    // the parser cannot express (`path()`, `url()`) stays `None`.
    struct ClipPath {
        enum class Shape : std::uint8_t { None, Inset, Circle, Ellipse, Polygon };
        // Sizes relative to the box: the border box, unless `box` says else.
        // `fill-box`, `stroke-box` and `view-box` behave as the border box on
        // HTML elements.
        enum class Box : std::uint8_t { Margin, Border, Padding, Content };
        // Circle/ellipse radii given as keywords instead of lengths.
        enum class Radial : std::uint8_t {
            Explicit,
            ClosestSide,
            FarthestSide,
            ClosestCorner,
            FarthestCorner
        };
        struct Point {
            Length x;
            Length y;
        };
        Shape shape = Shape::None;
        Box box = Box::Border;
        Length inset[4] = {};  // top, right, bottom, left
        Length insetRadius;    // `round`, one radius for every corner
        Radial radius = Radial::Explicit;  // circle
        Length radiusLength;
        Radial radiusX = Radial::Explicit;  // ellipse
        Radial radiusY = Radial::Explicit;
        Length radiusXLength;
        Length radiusYLength;
        // `at` position; defaulted to 50% 50% when the shape omits it.
        Length centerX = Length(Length::Percent, 50.0f);
        Length centerY = Length(Length::Percent, 50.0f);
        std::vector<Point> points;  // polygon, three or more
        bool evenOdd = false;
    };
    ClipPath clipPath;

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

    struct ConicalGradient {
        // Degrees clockwise from 12 o'clock, where the sweep starts.
        float angle = 0.0f;
        // The centre, defaulting to the middle of the box for the same reason
        // as the radial default above.
        Length centreX{Length::Percent, 50.0f};
        Length centreY{Length::Percent, 50.0f};
        std::vector<GradientStop> stops;
    };

    struct BackgroundImage {
        enum Kind : std::uint8_t { None, Url, LinearGradientImage, RadialGradientImage, ConicalGradientImage };
        Kind kind = None;
        std::string href;
        LinearGradient linear;
        RadialGradient radial;
        ConicalGradient conical;
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

    // A single `mask-image` layer. Gradients and `url()` sources paint into a
    // per-layer destination sized by `mask-size`, placed by `mask-position`
    // within its `mask-origin` box, and tiled by `mask-repeat`; the layer is
    // clipped to its `mask-clip` box. Layers combine through `mask-composite`
    // (see below).
    struct MaskImage {
        enum class Kind : std::uint8_t { None, LinearGradient, RadialGradient, ConicalGradient, Url };
        Kind kind = Kind::None;
        LinearGradient linear;
        RadialGradient radial;
        ConicalGradient conical;
        std::string href;
    };
    // One entry per mask layer; shorter lists repeat their last entry. The
    // geometry lists reuse the background types (declared below): an unauthored
    // size is `auto`, position starts top-left, repeat tiles, and both boxes
    // are the border box.
    enum class MaskMode : std::uint8_t { MatchSource, Alpha, Luminance };
    enum class MaskComposite : std::uint8_t { Add, Subtract, Intersect, Exclude };
    std::vector<MaskImage> maskImages;
    std::vector<MaskMode> maskModes;
    std::vector<MaskComposite> maskComposites;

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

    // Per-layer mask geometry, reusing the background types above. An
    // unauthored size is `auto` (intrinsic size, whole box for gradients),
    // position starts top-left, repeat tiles both axes, and both boxes are the
    // border box. Shorter lists repeat their last entry, like the background.
    std::vector<BackgroundSize> maskSizes;
    std::vector<BackgroundPosition> maskPositions;
    std::vector<RepeatPair> maskRepeats;
    std::vector<BackgroundBox> maskOrigins;
    std::vector<BackgroundBox> maskClips;

    // Which component of a `<mask>` element's content masks: brightness or the
    // channel. Stored and serialized; nothing reads it yet, because SVG mask
    // elements do not render.
    enum class MaskType : std::uint8_t { Luminance, Alpha };
    MaskType maskType = MaskType::Luminance;

    // A 9-patch border mask: the source is sliced into corners, edges, and a
    // middle, and each zone paints its own destination band. Slice insets are
    // numbers (source px) or percentages of the source; widths are lengths,
    // numbers (multiples of the border thickness), or auto (the slice's own
    // size); outsets are lengths or border multiples, never negative. Repeat
    // is per axis; mode reads the sample as brightness or channel.
    struct MaskBorder {
        MaskImage source;
        struct Slice {
            Length top;
            Length right;
            Length bottom;
            Length left;
            bool fill = false;
        };
        Slice slice;
        struct Width {
            Length top;
            Length right;
            Length bottom;
            Length left;
        };
        Width width;
        struct Outset {
            Length top;
            Length right;
            Length bottom;
            Length left;
        };
        Outset outset;
        enum class Repeat : std::uint8_t { Stretch, Repeat, Round, Space };
        Repeat repeatX = Repeat::Stretch;
        Repeat repeatY = Repeat::Stretch;
        enum class Mode : std::uint8_t { Luminance, Alpha };
        Mode mode = Mode::Luminance;
    };
    MaskBorder maskBorder;

    // A 9-patch border image: like the mask border but painted instead of
    // masking. Slice insets are numbers (source px) or percentages; widths
    // are lengths, border multiples, or auto (the slice's own size); outsets
    // are lengths or multiples, never negative. Repeat is per axis.
    struct BorderImage {
        BackgroundImage source;
        struct Slice {
            Length top;
            Length right;
            Length bottom;
            Length left;
            bool fill = false;
        };
        Slice slice;
        struct Width {
            Length top;
            Length right;
            Length bottom;
            Length left;
        };
        Width width;
        struct Outset {
            Length top;
            Length right;
            Length bottom;
            Length left;
        };
        Outset outset;
        enum class Repeat : std::uint8_t { Stretch, Repeat, Round, Space };
        Repeat repeatX = Repeat::Stretch;
        Repeat repeatY = Repeat::Stretch;
    };
    BorderImage borderImage;

    // Typography
    float fontSize = 16.0f;  // px
    std::string fontFamily;   // empty = default
    bool fontBold = false;
    float lineHeight = 1.2f;  // multiplier
    // Pair kerning in shaped runs. Inherited; `none` keeps ligatures and
    // marks but sets pairs unkerned.
    bool fontKerning = true;
    // Horizontal scale of glyphs and advances: 1.0 is normal, below 1 condenses,
    // above 1 expands. Inherited.
    float fontStretch = 1.0f;

    // Text properties (basic)
    enum FontStyle : std::uint8_t { FontNormal, FontItalic, FontOblique };
    FontStyle fontStyle = FontNormal;
    enum TextDecorationLine : std::uint8_t { DecNone, DecUnderline, DecOverline, DecLineThrough };
    TextDecorationLine textDecorationLine = DecNone;
    SDL_Color textDecorationColor{0, 0, 0, 0};
    bool hasTextDecorationColor = false;
    Length letterSpacing;
    Length wordSpacing;
    Length textIndent;
    enum TextTransform : std::uint8_t { TransformNone, TransformUppercase, TransformLowercase, TransformCapitalize };
    TextTransform textTransform = TransformNone;
    enum VerticalAlign : std::uint8_t {
        VAlignBaseline,
        VAlignTop,
        VAlignBottom,
        VAlignMiddle,
        VAlignSub,
        VAlignSuper,
        VAlignTextTop,
        VAlignTextBottom
    };
    VerticalAlign verticalAlign = VAlignBaseline;
    std::string transform;
    std::string filter;
    // What is already painted behind the border box, filtered before the box
    // paints over it. Same chain grammar as `filter:`; `opacity()` and
    // `drop-shadow()` in the chain stay unapplied.
    std::string backdropFilter;

    // Where scale/rotate turn about: two box-relative offsets, in the same
    // length/percentage terms CSS allows for `transform-origin` (a keyword edge
    // or a percentage, resolved against the border box when the transform is
    // applied), plus a z length for 3D (percentages never resolve there).
    // Defaults to the box centre, CSS's initial `50% 50%`.
    struct TransformOrigin {
        Length x{Length::Percent, 50.0f};
        Length y{Length::Percent, 50.0f};
        Length z;
    };
    TransformOrigin transformOrigin;
    // A `perspective` length funnels children's 3D through a viewer this far
    // in front of their plane; 0 is `none`. `transform-style` asks children to
    // share 3D space, which the flat compositor cannot do, so it only parses.
    // `backface-visibility: hidden` culls groups whose plane faces away.
    float perspective = 0.0f;
    enum class TransformStyle : std::uint8_t { Flat, Preserve3d };
    TransformStyle transformStyle = TransformStyle::Flat;
    bool backfaceHidden = false;

    // Scroll snapping: which axes snap and how strictly, plus each child's
    // alignment within the snapport. `none` disables everywhere.
    enum class ScrollSnapAxis : std::uint8_t { None, X, Y, Both };
    ScrollSnapAxis snapAxis = ScrollSnapAxis::None;
    enum class ScrollSnapStrictness : std::uint8_t { Proximity, Mandatory };
    ScrollSnapStrictness snapStrictness = ScrollSnapStrictness::Proximity;
    enum class ScrollSnapAlign : std::uint8_t { None, Start, End, Center };
    ScrollSnapAlign snapAlignX = ScrollSnapAlign::None;
    ScrollSnapAlign snapAlignY = ScrollSnapAlign::None;
    // How a scroll of this box moves: `Auto` jumps to the offset, `Smooth`
    // eases toward it over several frames. Not inherited.
    enum class ScrollBehavior : std::uint8_t { Auto, Smooth };
    ScrollBehavior scrollBehavior = ScrollBehavior::Auto;
    // The snapport inset from the padding box, per side. Start snap positions
    // shift by the leading inset, end positions by the trailing one.
    struct ScrollPadding {
        Length top;
        Length right;
        Length bottom;
        Length left;
    };
    ScrollPadding scrollPadding;

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
    // `grid-column` / `grid-row` explicit line placement and span.
    // `gridColumnStart` / `gridRowStart` are 1-based CSS line numbers; 0 means auto
    // (the item is placed by the auto-placement algorithm). `gridColumnEnd` /
    // `gridRowEnd` are similarly 1-based end lines; 0 means auto. When both start and
    // end are given the span is `end - start`; when only start is given the existing
    // `gridColumnSpan` / `gridRowSpan` counts extend from it.
    int gridColumnStart = 0;
    int gridColumnEnd   = 0;
    int gridRowStart    = 0;
    int gridRowEnd      = 0;
    int gridColumnSpan = 1;
    int gridRowSpan = 1;

    // Grid auto track sizing and auto flow
    GridTrack gridAutoRows;
    GridTrack gridAutoColumns;
    bool hasGridAutoRows = false;
    bool hasGridAutoColumns = false;

    enum GridAutoFlow : std::uint8_t {
        AutoFlowRow,
        AutoFlowColumn,
        AutoFlowRowDense,
        AutoFlowColumnDense
    };
    GridAutoFlow gridAutoFlow = AutoFlowRow;

    // Named grid areas from `grid-template-areas`. Each area is a 1-based line rectangle.
    struct GridAreaRect {
        int rowStart = 0;
        int rowEnd   = 0;
        int colStart = 0;
        int colEnd   = 0;
    };
    std::map<std::string, GridAreaRect> gridTemplateAreas;
    std::string gridTemplateAreasRaw;
    std::size_t gridTemplateAreasColumns = 0;
    std::size_t gridTemplateAreasRows = 0;
    std::string gridAreaName;

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
    enum Position : std::uint8_t { Static, Relative, Absolute, Fixed, Sticky };
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

    // Containment for `@container` size queries. `InlineSize` (or `Size`)
    // marks a query container, optionally named; `Normal` (default) does not
    // establish one. Only widths are queried, and layout containment itself is
    // not applied: sizes are read as laid out.
    enum ContainerType : std::uint8_t { ContainerNormal, ContainerInlineSize, ContainerSize };
    ContainerType containerType = ContainerNormal;
    std::vector<std::string> containerNames;

    // Paint order within a stacking context. Higher wins; equal values keep
    // document order. Only positioned elements and flex/grid items take part.
    int zIndex = 0;
    bool positioned = false;

    // `white-space: nowrap` and `pre` stop the formatter breaking at spaces. `pre`
    // additionally keeps newlines and runs of spaces. Prefixed because `NoWrap`
    // already belongs to FlexWrap.
    enum WhiteSpace : std::uint8_t { WrapText, WrapNone, WrapPre, WrapPreWrap };
    WhiteSpace whiteSpace = WrapText;

    // How overflow text is rendered when clipped: `Clip` (default) or `Ellipsis`.
    enum TextOverflow : std::uint8_t { TextOverflowClip, TextOverflowEllipsis };
    TextOverflow textOverflow = TextOverflowClip;

    // Preferred aspect ratio (aspectRatioW / aspectRatioH). 0 means `auto` (not set).
    float aspectRatioW = 0.0f;
    float aspectRatioH = 0.0f;
    float aspectRatio() const noexcept {
        return aspectRatioH > 0.0f ? aspectRatioW / aspectRatioH : 0.0f;
    }

    // The text a generated `::before` or `::after` puts in the box. Empty means the
    // pseudo-element is not generated at all, unless hasContent is true.
    std::string content;
    bool hasContent = false;

    // How a replaced element (img, video) fits its content box. `Contain` is the
    // historical default (aspect-fit down to fit); `Cover` aspect-fills, cropping
    // to cover the whole box; `Fill` stretches without keeping the aspect ratio;
    // `None` keeps the image at its intrinsic size; `ScaleDown` is `min(none, contain)`.
    enum ObjectFit : std::uint8_t { ObjContain, ObjCover, ObjFill, ObjNone, ObjScaleDown };
    ObjectFit objectFit = ObjContain;

    // List bullet / counter style. `Disc` (•) for unordered, `Decimal` for ordered.
    enum ListStyleType : std::uint8_t { LSTDisc, LSTCircle, LSTSquare, LSTDecimal, LSTNone };
    ListStyleType listStyleType = LSTDisc;
    // An image marker (`url()`), painted at intrinsic size instead of the
    // procedural glyph; empty means none. A URL that fails to load falls back
    // to the type glyph.
    std::string listStyleImage;

    // Visibility
    bool visible = true;
    // Float and clear (block layout)
    enum FloatType : std::uint8_t { FloatNone, FloatLeft, FloatRight };
    FloatType floatType = FloatNone;
    enum ClearType : std::uint8_t { ClearNone, ClearLeft, ClearRight, ClearBoth };
    ClearType clearType = ClearNone;

    // Table layout. `border-collapse` picks the separated edge model (a gap
    // between every cell, sized by `border-spacing`) or the collapsed one (no
    // gap). `table-layout` decides how the columns are sized.
    enum BorderCollapse : std::uint8_t { CollapseSeparate, CollapseCollapse };
    BorderCollapse borderCollapse = CollapseSeparate;
    float borderSpacingH = 0.0f;
    float borderSpacingV = 0.0f;
    enum TableLayoutKind : std::uint8_t { TableLayoutAuto, TableLayoutFixed };
    TableLayoutKind tableLayout = TableLayoutAuto;
    enum CaptionSide : std::uint8_t { CaptionTop, CaptionBottom };
    CaptionSide captionSide = CaptionTop;

    bool isTableContainer() const noexcept {
        return display == Display::Table || display == Display::InlineTable;
    }
    bool isTableRow() const noexcept { return display == Display::TableRow; }
    bool isTableRowGroup() const noexcept {
        return display == Display::TableRowGroup ||
               display == Display::TableHeaderGroup ||
               display == Display::TableFooterGroup;
    }
    bool isTableCell() const noexcept { return display == Display::TableCell; }

    // CSS counters, keyed by name. `counterReset` sets the counter to `value` on
    // the element that declares it; `counterIncrement` adds `value` to it.
    struct CounterOp {
        std::string name;
        int value = 0;
    };
    std::vector<CounterOp> counterReset;
    std::vector<CounterOp> counterIncrement;

    // List marker position: whether the bullet/number sits outside (default) or
    // inside the element's content box.
    enum ListStylePosition : std::uint8_t { LSPOutside, LSPInside };
    ListStylePosition listStylePosition = LSPOutside;

    // Pointer events: when true (from `pointer-events: none`), the element does not
    // intercept pointer events during hit testing.
    bool pointerEventsNone = false;

    // The pointer shape asked for while this element is hovered (`cursor`). The full
    // CSS keyword set is parsed; the platform layer maps each value onto the nearest
    // SDL system cursor. `CursorAuto` means no preference, so the platform default
    // stays in place. Cursor inherits, so an unstyled descendant over a link keeps
    // the link's pointer shape.
    enum Cursor : std::uint8_t {
        CursorAuto,
        CursorDefault,
        CursorNone,
        CursorContextMenu,
        CursorHelp,
        CursorPointer,
        CursorProgress,
        CursorWait,
        CursorCell,
        CursorCrosshair,
        CursorText,
        CursorVerticalText,
        CursorAlias,
        CursorCopy,
        CursorMove,
        CursorNoDrop,
        CursorNotAllowed,
        CursorGrab,
        CursorGrabbing,
        CursorAllScroll,
        CursorColResize,
        CursorRowResize,
        CursorEResize,
        CursorNResize,
        CursorNeResize,
        CursorNwResize,
        CursorSResize,
        CursorSeResize,
        CursorSwResize,
        CursorWResize,
        CursorEwResize,
        CursorNsResize,
        CursorNeswResize,
        CursorNwseResize,
        CursorZoomIn,
        CursorZoomOut
    };
    Cursor cursor = CursorAuto;

    // Which inheritable properties the user-agent style set on its own. Anything
    // not flagged here flows down from the parent. Author rules and inline
    // styles apply after inheritance, so they always win.
    enum Specified : std::uint32_t {
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
        SpecifiedTransformOrigin = 1u << 15,
        SpecifiedListStyleType = 1u << 16,
        SpecifiedTextDecorationColor = 1u << 17,
        SpecifiedPointerEvents = 1u << 18,
        SpecifiedCursor = 1u << 19,
        SpecifiedListStylePosition = 1u << 20,
        SpecifiedAccentColor = 1u << 21,
        SpecifiedFontKerning = 1u << 22,
        SpecifiedListStyleImage = 1u << 23,
        SpecifiedFontStretch = 1u << 24,
        SpecifiedScrollBehavior = 1u << 25,
    };
    std::uint32_t specified = 0;
};

// The CSS cursor keyword set, as one name/value table so the parser and the
// serializer cannot disagree about a spelling. Lives here rather than in the
// resolver because the application layer needs the same enum-to-keyword mapping
// when it hands the shape to the platform.
struct CursorKeyword {
    std::string_view name;
    ComputedStyle::Cursor value;
};
inline constexpr CursorKeyword kCursorKeywords[] = {
    {"auto", ComputedStyle::CursorAuto},
    {"default", ComputedStyle::CursorDefault},
    {"none", ComputedStyle::CursorNone},
    {"context-menu", ComputedStyle::CursorContextMenu},
    {"help", ComputedStyle::CursorHelp},
    {"pointer", ComputedStyle::CursorPointer},
    {"progress", ComputedStyle::CursorProgress},
    {"wait", ComputedStyle::CursorWait},
    {"cell", ComputedStyle::CursorCell},
    {"crosshair", ComputedStyle::CursorCrosshair},
    {"text", ComputedStyle::CursorText},
    {"vertical-text", ComputedStyle::CursorVerticalText},
    {"alias", ComputedStyle::CursorAlias},
    {"copy", ComputedStyle::CursorCopy},
    {"move", ComputedStyle::CursorMove},
    {"no-drop", ComputedStyle::CursorNoDrop},
    {"not-allowed", ComputedStyle::CursorNotAllowed},
    {"grab", ComputedStyle::CursorGrab},
    {"grabbing", ComputedStyle::CursorGrabbing},
    {"all-scroll", ComputedStyle::CursorAllScroll},
    {"col-resize", ComputedStyle::CursorColResize},
    {"row-resize", ComputedStyle::CursorRowResize},
    {"e-resize", ComputedStyle::CursorEResize},
    {"n-resize", ComputedStyle::CursorNResize},
    {"ne-resize", ComputedStyle::CursorNeResize},
    {"nw-resize", ComputedStyle::CursorNwResize},
    {"s-resize", ComputedStyle::CursorSResize},
    {"se-resize", ComputedStyle::CursorSeResize},
    {"sw-resize", ComputedStyle::CursorSwResize},
    {"w-resize", ComputedStyle::CursorWResize},
    {"ew-resize", ComputedStyle::CursorEwResize},
    {"ns-resize", ComputedStyle::CursorNsResize},
    {"nesw-resize", ComputedStyle::CursorNeswResize},
    {"nwse-resize", ComputedStyle::CursorNwseResize},
    {"zoom-in", ComputedStyle::CursorZoomIn},
    {"zoom-out", ComputedStyle::CursorZoomOut},
};

inline bool cursorKeyword(std::string_view word, ComputedStyle::Cursor& out) {
    for (const auto& entry : kCursorKeywords) {
        if (word == entry.name) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

inline std::string_view cursorName(ComputedStyle::Cursor value) {
    for (const auto& entry : kCursorKeywords) {
        if (value == entry.value) {
            return entry.name;
        }
    }
    return "auto";
}

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
    const float ar = style.aspectRatio();
    if (ar > 0.0f) {
        if (box.width > 0.0f && style.height.isAuto()) {
            box.height = box.width / ar;
        } else if (box.height > 0.0f && style.width.isAuto()) {
            box.width = box.height * ar;
        }
    }
    return box;
}

// Fills in the inheritable properties the user-agent style left unspecified.
inline void inheritInto(ComputedStyle& style, const ComputedStyle& parent) noexcept {
    if (!(style.specified & ComputedStyle::SpecifiedColor)) {
        style.color = parent.color;
    }
    if (!(style.specified & ComputedStyle::SpecifiedAccentColor)) {
        style.accentColor = parent.accentColor;
        style.hasAccentColor = parent.hasAccentColor;
    }
    if (!(style.specified & ComputedStyle::SpecifiedFontKerning)) {
        style.fontKerning = parent.fontKerning;
    }
    if (!(style.specified & ComputedStyle::SpecifiedFontStretch)) {
        style.fontStretch = parent.fontStretch;
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
    if (!(style.specified & ComputedStyle::SpecifiedListStyleType)) {
        style.listStyleType = parent.listStyleType;
    }
    if (!(style.specified & ComputedStyle::SpecifiedListStylePosition)) {
        style.listStylePosition = parent.listStylePosition;
    }
    if (!(style.specified & ComputedStyle::SpecifiedListStyleImage)) {
        style.listStyleImage = parent.listStyleImage;
    }
    if (!(style.specified & ComputedStyle::SpecifiedTextDecorationColor)) {
        style.textDecorationColor = parent.textDecorationColor;
        style.hasTextDecorationColor = parent.hasTextDecorationColor;
    }
    if (!(style.specified & ComputedStyle::SpecifiedPointerEvents)) {
        style.pointerEventsNone = parent.pointerEventsNone;
    }
    if (!(style.specified & ComputedStyle::SpecifiedCursor)) {
        style.cursor = parent.cursor;
    }
    if (!(style.specified & ComputedStyle::SpecifiedListStylePosition)) {
        style.listStylePosition = parent.listStylePosition;
    }
    // `transform` and `filter` are not inherited in CSS: they describe the box
    // they are declared on. The paint walk already carries a translated or
    // filtered box's contents with it, so copying the string down would apply the
    // effect a second time per descendant (and make every descendant claim a
    // stacking context of its own).
    // Custom properties always inherit; the resolved element's own cascade then
    // overwrites the entries it declares locally.
    style.custom = parent.custom;
}

}  // namespace sui
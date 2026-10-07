#include "Layout/BlockLayout.h"

#include "CSS/StyleResolver.h"
#include "Rendering/Text.h"
#include "SUI/DOM/Element.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace sui {
namespace {

bool isBlockLevel(const ComputedStyle& s) {
    // CSS puts `display: flex` in the block formatting context; only
    // `inline-flex` behaves like an inline-level box.
    return s.display == ComputedStyle::Block || s.display == ComputedStyle::Flex;
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
    if (font && font->hasFont() && s.fontSize > 0.0f) {
        const TextFont::VerticalMetrics vm = font->verticalMetrics(s.fontSize);
        m.ascent = vm.ascent;
        m.lineHeight = std::max(s.fontSize * s.lineHeight, vm.height);
        m.spaceWidth = font->measureText(" ", s.fontSize);
    } else {
        m.ascent = s.fontSize * 0.8f;
        m.lineHeight = s.fontSize * s.lineHeight;
        m.spaceWidth = s.fontSize * 0.25f;
    }
    return m;
}

std::string transformText(const ComputedStyle& s, std::string_view t) {
    if (s.textTransform == ComputedStyle::TransformNone) {
        return std::string(t);
    }
    std::string out;
    out.resize(t.size());
    std::size_t j = 0;
    for (char c : t) {
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
float verticalAlignShift(const ComputedStyle& s) {
    switch (s.verticalAlign) {
        case ComputedStyle::VAlignSub: return s.fontSize * 0.16f;
        case ComputedStyle::VAlignSuper: return -s.fontSize * 0.33f;
        case ComputedStyle::VAlignMiddle: return -s.fontSize * 0.25f;
        default: return 0.0f;
    }
}

float textWidth(const TextFont* font, const ComputedStyle& s, std::string_view t) {
    if (t.empty()) {
        return 0.0f;
    }
    std::string tt = transformText(s, t);
    float w = 0.0f;
    if (font && font->hasFont() && s.fontSize > 0.0f) {
        w = font->measureText(tt, s.fontSize);
    } else {
        w = static_cast<float>(tt.size()) * s.fontSize * 0.5f;
    }
    if (s.letterSpacing.unit != sui::Length::Auto && !tt.empty()) {
        const float ls = s.letterSpacing.resolve(s.fontSize, s.fontSize, 16.0f);
        w += ls * static_cast<float>(tt.size() - 1);
    }
    // A synthetic italic leans the glyph tops to the right, so the run claims the
    // extra width rather than letting the last letter overlap what follows.
    if (s.fontStyle != ComputedStyle::FontNormal && !tt.empty()) {
        w += s.fontSize * 0.21f;
    }
    return w;
}

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

    // Exposed for Flow, which needs the same answers run() uses.
    bool isLaidOut(Element& el) const { return laidOut(el); }
    ComputedStyle resolveStyle(Element& el) const { return resolver_.resolve(el); }

    // A percentage height has to resolve against a containing block that is
    // usually itself content-sized, which is not knowable while laying out. The
    // viewport is used instead: that is the only definite height in a typical
    // desktop page, and it is what an author writing `height: 50%` expects.
    UsedBox usedBox(const ComputedStyle& s, float availableWidth) const {
        return resolveBox(s, availableWidth, viewportHeight_, resolver_.rootFontSize());
    }

    // Lays out `el` with the given available width. Returns the outer height
    // consumed, margins included.
    float run(Element& el, float availableWidth, float originX, float originY) {
        const ComputedStyle s = resolver_.resolve(el);
        const UsedBox box = usedBox(s, availableWidth);
        el.clearTextRuns();
        if (s.display == ComputedStyle::None || !s.visible) {
            el.setLayoutX(originX);
            el.setLayoutY(originY);
            el.setLayoutWidth(0.0f);
            el.setLayoutHeight(0.0f);
            return 0.0f;
        }

        const float contentX = originX + box.margin.left + box.borderWidth.left +
                               box.padding.left;
        const float contentY = originY + box.margin.top + box.borderWidth.top +
                               box.padding.top;

        // An inline-level box with auto width shrinks to fit its contents, the
        // way a browser sizes a button or an image. The probe below measures
        // the unwrapped width; run() then repeats the layout at the real width.
        if (box.width <= 0.0f && s.display != ComputedStyle::Block) {
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
        float contentHeight = 0.0f;
        contentHeight = layoutContents(el, s, box, availableWidth, contentX, contentY,
                                       contentWidth);
        cbOwner_ = savedOwner;

        if (box.height > 0.0f) {
            contentHeight = box.height;
        }
        contentWidth = applyMinMax(contentWidth, box);

        const float borderW = contentWidth + box.borderWidth.left + box.borderWidth.right +
                              box.padding.left + box.padding.right;
        const float borderH = contentHeight + box.borderWidth.top + box.borderWidth.bottom +
                              box.padding.top + box.padding.bottom;

        el.setLayoutX(originX + box.margin.left);
        el.setLayoutY(originY + box.margin.top);
        el.setLayoutWidth(borderW);
        el.setLayoutHeight(borderH);
        recordBoxes(el, contentX, contentY, contentWidth, contentHeight, box);

        if (s.position != ComputedStyle::Static) {
            if (s.position == ComputedStyle::Absolute ||
                s.position == ComputedStyle::Fixed) {
                // An out-of-flow box takes no space in its parent and is placed
                // once the flow has fixed its containing block.
                pending_.push_back({&el, cbOwner_, s.position == ComputedStyle::Fixed});
                return 0.0f;
            }
            relative_.push_back({&el, s});
        }
        return borderH + box.margin.top + box.margin.bottom;
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

    // The padding box an out-of-flow box resolves against. Set while laying out a
    // positioned element, so its own out-of-flow descendants anchor to it.
    Element* cbOwner_ = nullptr;

    bool laidOut(Element& el) const {
        if (!el.isVisual()) {
            return false;
        }
        const ComputedStyle cs = resolver_.resolve(el);
        return cs.display != ComputedStyle::None && cs.visible;
    }

    bool hasBlockChild(Element& el) const {
        for (const auto& child : el.children()) {
            if (child && !child->isTextNode() && laidOut(*child) &&
                isBlockLevel(resolver_.resolve(*child))) {
                return true;
            }
        }
        return false;
    }

    float layoutBlockContainer(Element& el, const UsedBox& box, float availableWidth,
                               float contentX, float contentY, float& outHeight,
                               float& outWidth) {
        float contentWidth = applyMinMax(
            box.width > 0.0f ? box.width
                             : std::max(0.0f, availableWidth - box.horizontalChrome()),
            box);
        if (box.width <= 0.0f) {
            // Auto width still has to fit fixed-width children.
            for (const auto& child : el.children()) {
                if (!child || child->isTextNode() || !laidOut(*child)) {
                    continue;
                }
                const UsedBox childBox = usedBox(resolver_.resolve(*child), contentWidth);
                if (childBox.width > 0.0f) {
                    contentWidth = applyMinMax(
                        std::max(contentWidth, childBox.width + childBox.horizontalChrome()),
                        box);
                }
            }
        }

        float cursorY = 0.0f;
        for (const auto& child : el.children()) {
            if (!child || child->isTextNode() || !laidOut(*child)) {
                continue;
            }
            cursorY += run(*child, contentWidth, contentX, contentY + cursorY);
        }
        outHeight = cursorY;
        outWidth = contentWidth;
        return cursorY;
    }

    // Widest the element's own contents want when nothing wraps. The layout
    // this writes is discarded: run() repeats it at the real width.
    float probeWidth(Element& el, const UsedBox& box) {
        float width = 0.0f;
        layoutContents(el, resolver_.resolve(el), box, kUnbounded, 0.0f, 0.0f, width);
        return width;
    }

    // The natural content size on one axis, used as a flex item's automatic basis.
    float measureNatural(Element& el, const ComputedStyle& s, bool row) {
        float width = 0.0f;
        const float height = layoutContents(el, s, usedBox(s, kUnbounded), kUnbounded, 0.0f,
                                            0.0f, width);
        return row ? width : height;
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
                         float& outWidth) {
        if (s.isFlexContainer()) {
            return layoutFlexContainer(el, s, box, availableWidth, contentX, contentY,
                                       outWidth);
        }
        if (s.isGridContainer()) {
            return layoutGridContainer(el, s, box, availableWidth, contentX, contentY,
                                       outWidth);
        }
        if (hasBlockChild(el)) {
            float height = 0.0f;
            layoutBlockContainer(el, box, availableWidth, contentX, contentY, height, outWidth);
            return height;
        }
        return layoutInlineContainer(el, s, box, availableWidth, contentX, contentY, outWidth);
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

    // The items a flex container positions: its visual children, in order. Text
    // nodes between them are only whitespace and are dropped.
    std::vector<Element*> flexChildren(Element& el) const {
        std::vector<Element*> out;
        for (const auto& child : el.children()) {
            if (child && !child->isTextNode() && laidOut(*child)) {
                out.push_back(child.get());
            }
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
        const float crossAvailable =
            crossDefinite > 0.0f
                ? crossDefinite
                : (row ? std::max(0.0f, viewportHeight_ -
                                             verticalEdges(box.padding, box.borderWidth))
                       : std::max(0.0f, availableWidth - box.horizontalChrome()));
        const float gap = std::max(0.0f, s.gap);

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
            if (wrap && !line.items.empty() && used + gap + outer > mainAvailable + 0.5f) {
                lines.push_back(line);
                line = FlexLine{};
                used = 0.0f;
            }
            if (!line.items.empty()) {
                used += gap;
            }
            used += outer;
            line.items.push_back(i);
        }
        lines.push_back(line);

        // 3. Resolve each line's flexible lengths: grow or shrink into the free
        //    space, then clamp by min/max.
        for (FlexLine& l : lines) {
            const float gaps = gap * static_cast<float>(l.items.size() - 1);
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
            redistribute(items, l, row, gap, mainAvailable);
        }

        // 4. Size each line on the cross axis. An item's cross size is its own
        //    cross-axis length, or for an auto one the height/width its content
        //    turned out to need once laid out.
        for (FlexLine& l : lines) {
            l.crossSize = 0.0f;
            for (const std::size_t idx : l.items) {
                FlexItem& item = items[idx];
                const float natural = row ? item.box.height : item.box.width;
                item.crossSize = natural;
                l.crossSize = std::max(l.crossSize,
                                       natural + crossChrome(item.box, row) +
                                           crossMargins(item.box, row));
            }
        }

        // A container with a definite cross size stretches its single line to fill
        // it, which is what makes a row of cards fill its panel. Without a definite
        // cross size the line is only as tall as its tallest item.
        if (lines.size() == 1 && crossDefinite > 0.0f &&
            (row ? s.alignContent : s.alignItems) == ComputedStyle::AlignStretch) {
            lines[0].crossSize = crossAvailable;
        }

        // 5. Stack the lines on the cross axis.
        float crossCursor = 0.0f;
        for (FlexLine& l : lines) {
            l.crossPos = crossCursor;
            crossCursor += l.crossSize + gap;
        }
        if (!lines.empty()) {
            crossCursor -= gap;
        }
        applyWrapReverse(s, lines, crossAvailable);

        // 6. Place each item on the main axis, honouring `justify-content`.
        for (FlexLine& l : lines) {
            const float gaps = gap * static_cast<float>(l.items.size() - 1);
            float outer = gaps;
            for (const std::size_t idx : l.items) {
                const FlexItem& item = items[idx];
                outer += item.mainSize + mainChrome(item.box, row) + mainMargins(item.box, row);
            }
            // Recomputed from the final sizes, because growing changed the total.
            const float freeSpace = mainAvailable - outer;
            const float n = static_cast<float>(l.items.size());
            float before = 0.0f;
            float between = gap;
            switch (s.justifyContent) {
                case ComputedStyle::AlignEnd:
                    before = freeSpace;
                    break;
                case ComputedStyle::AlignCenter:
                    before = freeSpace * 0.5f;
                    break;
                case ComputedStyle::AlignSpaceBetween:
                    if (n > 1.0f) {
                        between = gap + std::max(0.0f, freeSpace) / (n - 1.0f);
                    }
                    break;
                case ComputedStyle::AlignSpaceAround:
                    // Each item gets an equal slice, half of it on either side.
                    before = std::max(0.0f, freeSpace) / n * 0.5f;
                    between = gap + std::max(0.0f, freeSpace) / n;
                    break;
                case ComputedStyle::AlignSpaceEvenly:
                    before = std::max(0.0f, freeSpace) / (n + 1.0f);
                    between = gap + std::max(0.0f, freeSpace) / (n + 1.0f);
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
                const float pos = reverse ? mainAvailable -
                                                (natural[k] + item.mainSize + end) -
                                                mainChrome(item.box, row)
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
                // Stretch fills the line only when the item has no definite size on
                // the cross axis: its height in a row, its width in a column.
                // `crossSize` is a content-box length, so stretching to the line's
                // cross extent has to give the chrome back before it can be used as
                // one, and the two are kept apart because the extent is what the
                // alignment arithmetic needs while the content box is what the item
                // is actually laid out at.
                float crossContent = item.crossSize;
                const float authored = row ? item.box.height : item.box.width;
                if (align == ComputedStyle::AlignStretch && authored <= 0.0f) {
                    crossContent =
                        std::max(0.0f, l.crossSize - crossChrome(item.box, row));
                }
                const float cross = crossMargins(item.box, row) +
                                    crossChrome(item.box, row) + crossContent;
                const float freeSpace = std::max(0.0f, l.crossSize - cross);
                if (align == ComputedStyle::AlignEnd) {
                    item.crossPos = l.crossPos + freeSpace;
                } else if (align == ComputedStyle::AlignCenter) {
                    item.crossPos = l.crossPos + freeSpace * 0.5f;
                } else {
                    item.crossPos = l.crossPos;
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
                maxMain = std::max(maxMain, item.mainPos +
                                                 (row ? item.el->layoutWidth()
                                                      : item.el->layoutHeight()) +
                                                 mainMargins(item.box, row));
                maxCross = std::max(maxCross, item.crossPos + item.el->layoutHeight() +
                                                 crossMargins(item.box, row));
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
                          float crossAvailable) {
        if (s.flexWrap != ComputedStyle::WrapReverse) {
            return;
        }
        float total = 0.0f;
        for (const FlexLine& line : lines) {
            total += line.crossSize;
        }
        const float gaps = std::max(0.0f, s.gap) * static_cast<float>(lines.size() - 1);
        float cursor = std::max(0.0f, crossAvailable - total - gaps);
        for (FlexLine& line : lines) {
            line.crossPos = cursor;
            cursor += line.crossSize + std::max(0.0f, s.gap);
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
        if (columns.empty()) {
            ComputedStyle::GridTrack only;
            only.isAuto = true;
            columns.push_back(only);
        }
        // `repeat(auto-fill/auto-fit, <body>)`: the stored list is one repetition of
        // the body, so repeat it to as many tracks as the container fits, each at
        // least as wide as the body's own minimum. `auto-fit` drops the tracks no
        // item lands in, which for this row-major placement is the same as capping
        // the count at the number of items.
        if (s.gridColumnsAutoFill && !columns.empty()) {
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
            std::vector<ComputedStyle::GridTrack> expanded;
            expanded.reserve(repeatCount * columns.size());
            for (std::size_t n = 0; n < repeatCount; ++n) {
                for (const ComputedStyle::GridTrack& track : columns) {
                    expanded.push_back(track);
                }
            }
            columns = std::move(expanded);
        }
        const std::size_t columnCount = columns.size();

        // Auto-placement. Items fill the rows in order, each claiming a
        // cSpan x rSpan block of cells; the cursor never moves backwards, so a
        // spanning item that does not fit in the remaining columns drops to the
        // next row. With every span one this reproduces plain row-major order.
        std::vector<int> cSpan(children.size(), 1);
        std::vector<int> rSpan(children.size(), 1);
        std::vector<std::size_t> itemCol(children.size(), 0);
        std::vector<std::size_t> itemRow(children.size(), 0);
        std::vector<float> naturalWidth(children.size(), 0.0f);
        for (std::size_t i = 0; i < children.size(); ++i) {
            const ComputedStyle cs = resolver_.resolve(*children[i]);
            cSpan[i] = std::min<int>(columnCount, std::max(1, cs.gridColumnSpan));
            rSpan[i] = std::max(1, cs.gridRowSpan);
            naturalWidth[i] = measureNatural(*children[i], cs, true);
        }
        std::vector<std::vector<bool>> occupied;
        auto claimRows = [&occupied](std::size_t row, std::size_t columns) {
            while (occupied.size() <= row) {
                occupied.push_back(std::vector<bool>(columns, false));
            }
        };
        std::size_t nextRow = 0;
        std::size_t nextCol = 0;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const std::size_t csz = static_cast<std::size_t>(cSpan[i]);
            const std::size_t rsz = static_cast<std::size_t>(rSpan[i]);
            std::size_t r = nextRow;
            std::size_t c = nextCol;
            while (true) {
                if (c + csz > columnCount) {
                    c = 0;
                    ++r;
                    continue;
                }
                claimRows(r + rsz - 1, columnCount);
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
            claimRows(r + rsz - 1, columnCount);
            for (std::size_t rr = r; rr < r + rsz; ++rr) {
                for (std::size_t cc = c; cc < c + csz; ++cc) {
                    occupied[rr][cc] = true;
                }
            }
            itemCol[i] = c;
            itemRow[i] = r;
            nextCol = c + csz;
            nextRow = r;
            if (nextCol >= columnCount) {
                nextCol = 0;
                nextRow = r + 1;
            }
        }
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
            if (vAlign == ComputedStyle::AlignStretch && rSpan[i] == 1 &&
                row < s.gridRows.size()) {
                const ComputedStyle::GridTrack& rowTrack = s.gridRows[row];
                if (!rowTrack.isAuto && !rowTrack.isFr) {
                    const float trackHeight =
                        std::max(0.0f, rowTrack.size.resolve(0.0f, s.fontSize,
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

        // `grid-template-rows` overrides a content row with its own track; an
        // absent list leaves every row at its tallest item.
        if (!s.gridRows.empty()) {
            for (std::size_t r = 0; r < rowCount && r < s.gridRows.size(); ++r) {
                const ComputedStyle::GridTrack& track = s.gridRows[r];
                if (!track.isAuto && !track.isFr) {
                    rowHeights[r] = std::max(0.0f, track.size.resolve(0.0f, s.fontSize,
                                                                     resolver_.rootFontSize()));
                }
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

        Flow flow(*this, el, s, metricsFor(font_, s), inner, contentX, contentY);
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

    // Greedy line breaking for one inline formatting context.
    class Flow {
    public:
        Flow(Layout& owner, Element& owner_el, const ComputedStyle& s, Metrics m,
             float maxWidth, float originX, float originY)
            : owner_(owner),
              ownerEl_(owner_el),
              style_(s),
              metrics_(m),
              maxWidth_(maxWidth),
              originX_(originX),
              originY_(originY),
              spacing_(spacingFor(s.whiteSpace)) {
            if (s.textIndent.unit != sui::Length::Auto) {
                penX_ = std::max(0.0f, s.textIndent.resolve(s.fontSize, s.fontSize, 16.0f));
            }
        }

        void text(std::string_view text, const ComputedStyle& s) {
            const Metrics m = metricsFor(owner_.font_, s);
            // A smaller font inside a line raises nothing; the line box keeps
            // the container's height.
            const float ascentDelta = std::min(0.0f, m.ascent - metrics_.ascent);
            if (!spacing_.keepsNewlines) {
                words(text, s, ascentDelta);
                return;
            }
            // A newline in the text ends the line instead of collapsing to a space.
            std::size_t start = 0;
            while (start <= text.size()) {
                const std::size_t nl = text.find('\n', start);
                const std::size_t end = nl == std::string_view::npos ? text.size() : nl;
                if (end > start) {
                    if (spacing_.keepsWhitespace) {
                        word(text.substr(start, end - start), s, ascentDelta);
                    } else {
                        words(text.substr(start, end - start), s, ascentDelta);
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
            if (lineHasContent_) {
                newLine();
            }
        }

        // Flows `el`'s children into the current line context. Inline boxes are
        // transparent here: their contents continue the same line. Anything
        // atomic (inline-block, replaced media) is placed as a unit.
        void inlineBox(Element& el, const ComputedStyle& container) {
            // A text field draws its current value where a normal element would draw
            // its children, so its text takes part in the same line context.
            if (el.tag() == "input") {
                const std::string value = el.value();
                if (!value.empty()) {
                    text(value, container);
                }
                return;
            }
            for (const auto& child : el.children()) {
                if (!child) {
                    continue;
                }
                if (child->isTextNode()) {
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
                if (!owner_.isLaidOut(*child)) {
                    continue;
                }
                const ComputedStyle cs = owner_.resolveStyle(*child);
                if (cs.display == ComputedStyle::Inline) {
                    inlineBox(*child, cs);
                    continue;
                }
                atomic(*child, cs);
            }
        }

        void atomic(Element& child, const ComputedStyle& cs) {
            const UsedBox box = owner_.usedBox(cs, maxWidth_);
            // Measure before deciding to wrap: an auto-width atomic box is as
            // wide as its contents want, so the break test needs that number.
            float boxWidth = box.width;
            if (boxWidth <= 0.0f) {
                owner_.run(child, kUnboundedProbe, 0.0f, 0.0f);
                boxWidth = child.layoutWidth();
            }
            const float outerW = boxWidth + box.margin.left + box.margin.right +
                                 box.borderWidth.left + box.borderWidth.right +
                                 box.padding.left + box.padding.right;
            if (lineHasContent_ && penX_ + outerW > maxWidth_) {
                newLine();
            }
            const float lineTop = originY_ + cursorY_;
            const float top = lineTop + box.margin.top + verticalAlignShift(cs);
            owner_.run(child, maxWidth_, originX_ + penX_ + box.margin.left, top);
            // `top` and `bottom` align the box against the line box rather than the
            // baseline. The line box is the container's own line height, so its
            // extent is known here: `bottom` drops the box so its outer bottom sits
            // on it, `top` seats its outer top on it.
            float boxBottom = cursorY_ + box.margin.top + child.layoutHeight() +
                              box.margin.bottom;
            if (cs.verticalAlign == ComputedStyle::VAlignBottom) {
                child.setLayoutY(lineTop + metrics_.lineHeight - child.layoutHeight() -
                                 box.margin.bottom);
                boxBottom = cursorY_ + metrics_.lineHeight;
            } else if (cs.verticalAlign == ComputedStyle::VAlignTop) {
                child.setLayoutY(lineTop + box.margin.top);
            }
            penX_ += box.margin.left + child.layoutWidth() + box.margin.right;
            lineHasContent_ = true;
            widest_ = std::max(widest_, penX_);
            bottom_ = std::max(bottom_, boxBottom);
        }

        void newLine() {
            cursorY_ += metrics_.lineHeight;
            penX_ = 0.0f;
            lineHasContent_ = false;
        }

        void finish() {}

        float cursorY() const {
            // The trailing line is never committed, so count it here.
            const float tail = lineHasContent_ ? metrics_.lineHeight : 0.0f;
            return std::max(cursorY_ + tail, bottom_);
        }
        float penX() const { return penX_; }
        float widest() const { return widest_; }

    private:
        // What `white-space` changes. `nowrap` collapses runs of spaces but never
        // breaks a line. `pre` and `pre-wrap` treat a newline in the text as a line
        // break rather than whitespace; `pre` also keeps runs of spaces, so a line
        // is taken whole and never wraps.
        struct Spacing {
            bool wraps = true;
            bool keepsWhitespace = false;
            bool keepsNewlines = false;
        };

        static Spacing spacingFor(ComputedStyle::WhiteSpace value) {
            switch (value) {
                case ComputedStyle::WrapNone:
                    return {false, false, false};
                case ComputedStyle::WrapPre:
                    return {false, true, true};
                case ComputedStyle::WrapPreWrap:
                    return {true, false, true};
                case ComputedStyle::WrapText:
                    break;
            }
            return {};
        }

        // Splits at whitespace, collapsing runs of spaces into the single space
        // word() inserts between runs.
        void words(std::string_view text, const ComputedStyle& s, float ascentDelta) {
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
                word(text.substr(start, i - start), s, ascentDelta);
            }
        }

        void word(std::string_view w, const ComputedStyle& s, float ascentDelta) {
            const float wWidth = textWidth(owner_.font_, s, w);
            if (spacing_.wraps && lineHasContent_ &&
                penX_ + metrics_.spaceWidth + wWidth > maxWidth_) {
                newLine();
            }
            float space = lineHasContent_ ? metrics_.spaceWidth : 0.0f;
            if (lineHasContent_ && s.wordSpacing.unit != sui::Length::Auto) {
                space += s.wordSpacing.resolve(s.fontSize, s.fontSize, 16.0f);
            }
            std::string wt = transformText(s, w);
            ownerEl_.addTextRun({wt, originX_ + penX_ + space,
                                 originY_ + cursorY_ + verticalAlignShift(s), wWidth,
                                 metrics_.lineHeight, metrics_.ascent + ascentDelta,
                                 s.fontSize, s.color.r, s.color.g, s.color.b, s.color.a,
                                 s.fontBold, s.fontStyle != ComputedStyle::FontNormal});
            penX_ += space + wWidth;
            lineHasContent_ = true;
            widest_ = std::max(widest_, penX_);
        }

        Layout& owner_;
        Element& ownerEl_;
        const ComputedStyle& style_;
        Metrics metrics_;
        float maxWidth_ = 0.0f;
        float originX_ = 0.0f;
        float originY_ = 0.0f;
        float penX_ = 0.0f;
        float cursorY_ = 0.0f;
        float bottom_ = 0.0f;
        float widest_ = 0.0f;
        bool lineHasContent_ = false;
        Spacing spacing_ = spacingFor(ComputedStyle::WrapText);
    };

    const StyleResolver& resolver_;
    float viewportWidth_ = 0.0f;
    float viewportHeight_ = 0.0f;
    const TextFont* font_ = nullptr;
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

}  // namespace

void BlockLayout::layout(Element* root, const LayoutInput& input) {
    if (!root || !input.resolver) {
        return;
    }
    Layout layout(*input.resolver, input.font, input.viewportWidth, input.viewportHeight);
    layout.run(*root, input.viewportWidth, 0.0f, 0.0f);
    layout.placeOutOfFlow();
    measureScrollExtents(*root, *input.resolver);
}

}  // namespace sui

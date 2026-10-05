#include "Layout/BlockLayout.h"

#include "CSS/StyleResolver.h"
#include "Rendering/Text.h"
#include "SUI/DOM/Element.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace sui {
namespace {

bool isBlockLevel(const ComputedStyle& s) {
    return s.display == ComputedStyle::Block;
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

float textWidth(const TextFont* font, const ComputedStyle& s, std::string_view t) {
    if (t.empty()) {
        return 0.0f;
    }
    if (font && font->hasFont() && s.fontSize > 0.0f) {
        return font->measureText(t, s.fontSize);
    }
    return static_cast<float>(t.size()) * s.fontSize * 0.5f;
}

class Layout {
public:
    Layout(const StyleResolver& resolver, const TextFont* font, float viewportHeight)
        : resolver_(resolver), font_(font), viewportHeight_(viewportHeight) {}

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

        float contentWidth = 0.0f;
        float contentHeight = 0.0f;
        contentHeight = layoutContents(el, s, box, availableWidth, contentX, contentY,
                                       contentWidth);

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
        return borderH + box.margin.top + box.margin.bottom;
    }

private:
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

    float layoutContents(Element& el, const ComputedStyle& s, const UsedBox& box,
                         float availableWidth, float contentX, float contentY,
                         float& outWidth) {
        if (hasBlockChild(el)) {
            float height = 0.0f;
            layoutBlockContainer(el, box, availableWidth, contentX, contentY, height, outWidth);
            return height;
        }
        return layoutInlineContainer(el, s, box, availableWidth, contentX, contentY, outWidth);
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
              originY_(originY) {}

        void text(std::string_view text, const ComputedStyle& s) {
            const Metrics m = metricsFor(owner_.font_, s);
            // A smaller font inside a line raises nothing; the line box keeps
            // the container's height.
            const float ascentDelta = std::min(0.0f, m.ascent - metrics_.ascent);
            std::size_t i = 0;
            while (i < text.size()) {
                // A newline in markup is whitespace, not a forced break.
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

        void lineBreak() {
            if (lineHasContent_) {
                newLine();
            }
        }

        // Flows `el`'s children into the current line context. Inline boxes are
        // transparent here: their contents continue the same line. Anything
        // atomic (inline-block, replaced media) is placed as a unit.
        void inlineBox(Element& el, const ComputedStyle& container) {
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
            const float top = originY_ + cursorY_ + box.margin.top;
            owner_.run(child, maxWidth_, originX_ + penX_ + box.margin.left, top);
            penX_ += box.margin.left + child.layoutWidth() + box.margin.right;
            lineHasContent_ = true;
            widest_ = std::max(widest_, penX_);
            bottom_ = std::max(bottom_, cursorY_ + box.margin.top + child.layoutHeight() +
                                                box.margin.bottom);
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
        void word(std::string_view w, const ComputedStyle& s, float ascentDelta) {
            const float wWidth = textWidth(owner_.font_, s, w);
            if (lineHasContent_ &&
                penX_ + metrics_.spaceWidth + wWidth > maxWidth_) {
                newLine();
            }
            const float space = lineHasContent_ ? metrics_.spaceWidth : 0.0f;
            ownerEl_.addTextRun({std::string(w), originX_ + penX_ + space,
                                 originY_ + cursorY_, wWidth, metrics_.lineHeight,
                                 metrics_.ascent + ascentDelta, s.fontSize, s.color.r,
                                 s.color.g, s.color.b, s.color.a});
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
    };

    const StyleResolver& resolver_;
    const TextFont* font_ = nullptr;
    float viewportHeight_ = 0.0f;
};

}  // namespace

void BlockLayout::layout(Element* root, const LayoutInput& input) {
    if (!root || !input.resolver) {
        return;
    }
    Layout layout(*input.resolver, input.font, input.viewportHeight);
    layout.run(*root, input.viewportWidth, 0.0f, 0.0f);
}

}  // namespace sui

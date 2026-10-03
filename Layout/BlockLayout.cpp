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

float horizontalChrome(const ComputedStyle& s) {
    return s.margin.left + s.margin.right + s.borderWidth.left + s.borderWidth.right +
           s.padding.left + s.padding.right;
}

float applyMinMax(float value, const ComputedStyle& s) {
    if (s.minWidth > 0.0f) {
        value = std::max(value, s.minWidth);
    }
    if (s.maxWidth > 0.0f) {
        value = std::min(value, s.maxWidth);
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
    Layout(const StyleResolver& resolver, const TextFont* font)
        : resolver_(resolver), font_(font) {}

    // Exposed for Flow, which needs the same answers run() uses.
    bool isLaidOut(Element& el) const { return laidOut(el); }
    ComputedStyle resolveStyle(Element& el) const { return resolver_.resolve(el); }

    // Lays out `el` with the given available width. Returns the outer height
    // consumed, margins included.
    float run(Element& el, float availableWidth, float originX, float originY) {
        const ComputedStyle s = resolver_.resolve(el);
        el.clearTextRuns();
        if (s.display == ComputedStyle::None || !s.visible) {
            el.setLayoutX(originX);
            el.setLayoutY(originY);
            el.setLayoutWidth(0.0f);
            el.setLayoutHeight(0.0f);
            return 0.0f;
        }

        const float contentX =
            originX + s.margin.left + s.borderWidth.left + s.padding.left;
        const float contentY =
            originY + s.margin.top + s.borderWidth.top + s.padding.top;

        // An inline-level box with auto width shrinks to fit its contents, the
        // way a browser sizes a button or an image. The probe below measures
        // the unwrapped width; run() then repeats the layout at the real width.
        if (s.width <= 0.0f && s.display != ComputedStyle::Block) {
            availableWidth =
                std::min(availableWidth, probeWidth(el, s) + horizontalChrome(s));
            el.clearTextRuns();  // the probe's runs are throwaway measurements
        }

        float contentWidth = 0.0f;
        float contentHeight = 0.0f;
        contentHeight =
            layoutContents(el, s, availableWidth, contentX, contentY, contentWidth);

        if (s.height > 0.0f) {
            contentHeight = s.height;
        }
        contentWidth = applyMinMax(contentWidth, s);

        const float borderW = contentWidth + s.borderWidth.left + s.borderWidth.right +
                              s.padding.left + s.padding.right;
        const float borderH = contentHeight + s.borderWidth.top + s.borderWidth.bottom +
                              s.padding.top + s.padding.bottom;

        el.setLayoutX(originX + s.margin.left);
        el.setLayoutY(originY + s.margin.top);
        el.setLayoutWidth(borderW);
        el.setLayoutHeight(borderH);
        return borderH + s.margin.top + s.margin.bottom;
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

    float layoutBlockContainer(Element& el, const ComputedStyle& s, float availableWidth,
                               float contentX, float contentY, float& outHeight,
                               float& outWidth) {
        float contentWidth = applyMinMax(
            s.width > 0.0f ? s.width
                           : std::max(0.0f, availableWidth - horizontalChrome(s)),
            s);
        if (s.width <= 0.0f) {
            // Auto width still has to fit fixed-width children.
            for (const auto& child : el.children()) {
                if (!child || child->isTextNode() || !laidOut(*child)) {
                    continue;
                }
                const ComputedStyle cs = resolver_.resolve(*child);
                if (cs.width > 0.0f) {
                    contentWidth = applyMinMax(
                        std::max(contentWidth, cs.width + horizontalChrome(cs)), s);
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
    float probeWidth(Element& el, const ComputedStyle& s) {
        float width = 0.0f;
        layoutContents(el, s, kUnbounded, 0.0f, 0.0f, width);
        return width;
    }

    float layoutContents(Element& el, const ComputedStyle& s, float availableWidth,
                         float contentX, float contentY, float& outWidth) {
        if (hasBlockChild(el)) {
            float height = 0.0f;
            layoutBlockContainer(el, s, availableWidth, contentX, contentY, height,
                                 outWidth);
            return height;
        }
        return layoutInlineContainer(el, s, availableWidth, contentX, contentY, outWidth);
    }

    // Every inline descendant of `el` flows into one line-breaking context, so
    // wrapping stays correct across element boundaries: "a <span>b c d</span>"
    // can break inside the span.
    float layoutInlineContainer(Element& el, const ComputedStyle& s, float availableWidth,
                               float contentX, float contentY, float& outWidth) {
        const bool probe = availableWidth >= kUnbounded;
        // A probe measures max-content, so it must not wrap at all; otherwise
        // every word lands on its own line and the result is min-content.
        const float inner =
            s.width > 0.0f ? s.width
                           : probe ? kUnbounded
                                   : std::max(0.0f, availableWidth - horizontalChrome(s));

        Flow flow(*this, el, s, metricsFor(font_, s), inner, contentX, contentY);
        flow.inlineBox(el, s);
        flow.finish();

        if (s.width > 0.0f) {
            outWidth = s.width;
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
            // Measure before deciding to wrap: an auto-width atomic box is as
            // wide as its contents want, so the break test needs that number.
            float boxWidth = cs.width;
            if (boxWidth <= 0.0f) {
                owner_.run(child, kUnboundedProbe, 0.0f, 0.0f);
                boxWidth = child.layoutWidth();
            }
            const float outerW =
                boxWidth + cs.margin.left + cs.margin.right + cs.borderWidth.left +
                cs.borderWidth.right + cs.padding.left + cs.padding.right;
            if (lineHasContent_ && penX_ + outerW > maxWidth_) {
                newLine();
            }
            const float top = originY_ + cursorY_ + cs.margin.top;
            owner_.run(child, maxWidth_, originX_ + penX_ + cs.margin.left, top);
            penX_ += cs.margin.left + child.layoutWidth() + cs.margin.right;
            lineHasContent_ = true;
            widest_ = std::max(widest_, penX_);
            bottom_ = std::max(bottom_, cursorY_ + cs.margin.top +
                                                child.layoutHeight() + cs.margin.bottom);
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
};

}  // namespace

void BlockLayout::layout(Element* root, const LayoutInput& input) {
    if (!root || !input.resolver) {
        return;
    }
    Layout layout(*input.resolver, input.font);
    layout.run(*root, input.viewportWidth, 0.0f, 0.0f);
}

}  // namespace sui

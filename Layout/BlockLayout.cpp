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

        float contentWidth = 0.0f;
        float contentHeight = 0.0f;
        if (hasBlockChild(el)) {
            contentWidth = layoutBlockContainer(el, s, availableWidth, contentX, contentY,
                                                contentHeight);
        } else {
            layoutInlineContainer(el, s, availableWidth, contentX, contentY, contentWidth,
                                  contentHeight);
        }

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
                               float contentX, float contentY, float& outHeight) {
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
        return contentWidth;
    }

    // All children are inline-level (or text): wrap into lines, flowing
    // inline-block boxes and <br> along with the text.
    void layoutInlineContainer(Element& el, const ComputedStyle& s, float availableWidth,
                               float contentX, float contentY, float& outWidth,
                               float& outHeight) {
        const Metrics m = metricsFor(font_, s);
        const float inner =
            s.width > 0.0f ? s.width : std::max(0.0f, availableWidth - horizontalChrome(s));

        Flow flow(*this, el, s, m, inner, contentX, contentY);
        for (const auto& child : el.children()) {
            if (!child) {
                continue;
            }
            if (child->isTextNode()) {
                if (!child->text().empty()) {
                    flow.text(child->text(), s);
                }
                continue;
            }
            if (!laidOut(*child)) {
                continue;
            }
            if (child->tag() == "br") {
                // Forced break regardless of its computed display.
                flow.lineBreak();
                continue;
            }
            const ComputedStyle cs = resolver_.resolve(*child);
            if (cs.display == ComputedStyle::Inline) {
                // Plain inline boxes contribute their own contents in place.
                layoutInlineContainer(*child, cs, inner - flow.penX(), contentX + flow.penX(),
                                      contentY + flow.cursorY(), outWidth, outHeight);
                continue;
            }
            flow.atomic(*child, cs);
        }
        flow.finish();

        outHeight = flow.cursorY();
        outWidth = s.width > 0.0f ? s.width : std::max(inner, flow.widest());
    }

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

        void atomic(Element& child, const ComputedStyle& cs) {
            const float outerW = cs.margin.left + cs.margin.right;
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

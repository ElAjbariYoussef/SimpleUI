#include "Layout/Layout.h"

#include "CSS/DefaultStyle.h"
#include "SUI/DOM/Element.h"

#include <algorithm>
#include <charconv>

namespace sui {
namespace {

float resolveWidth(const ComputedStyle& style, float containerWidth) {
    if (style.width > 0.0f) {
        return style.width;
    }
    // auto: for block, fill container; for inline, content-based (0 for now)
    if (style.display == ComputedStyle::Block) {
        return containerWidth;
    }
    return 0.0f;
}

float resolveHeight(const ComputedStyle& style, float /*containerHeight*/) {
    if (style.height > 0.0f) {
        return style.height;
    }
    return 0.0f;
}

ComputedStyle mergedStyle(Element& el) {
    ComputedStyle style = defaultStyleFor(el.tag());
    // Inline style attribute (very minimal: only width/height/background-color for now)
    if (el.hasAttribute("style")) {
        (void)el.attribute("style");
        // TODO: parse a tiny subset of inline CSS when needed
    }
    // Attribute hints
    if (el.hasAttribute("width")) {
        const auto& raw = el.attribute("width");
        float v = 0.0f;
        std::from_chars(raw.data(), raw.data() + raw.size(), v);
        if (v > 0.0f) {
            style.width = v;
        }
    }
    if (el.hasAttribute("height")) {
        const auto& raw = el.attribute("height");
        float v = 0.0f;
        std::from_chars(raw.data(), raw.data() + raw.size(), v);
        if (v > 0.0f) {
            style.height = v;
        }
    }
    return style;
}

}  // namespace

void LayoutEngine::layout(Element* root, const LayoutContext& context) {
    if (!root) {
        return;
    }

    root->visit([&](Element& el) {
        ComputedStyle style = mergedStyle(el);
        if (style.display == ComputedStyle::None || !style.visible) {
            return;
        }

        BoxModel box;
        box.margin = style.margin;
        box.border = style.borderWidth;
        box.padding = style.padding;

        const float cw = resolveWidth(style, context.containerWidth);
        const float ch = resolveHeight(style, 0.0f);
        box.setContentSize(cw, ch);

        // For now, set bounds at origin (block stacking handled later)
        box.bounds = {0.0f, 0.0f, box.outerWidth(), box.outerHeight()};
        // TODO: store box/style on element
    });
}

}  // namespace sui
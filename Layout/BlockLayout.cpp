#include "Layout/BlockLayout.h"

#include "CSS/StyleResolver.h"
#include "Layout/LayoutNode.h"
#include "SUI/DOM/Element.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sui {
namespace {

float contentWidthFor(const ComputedStyle& s, float containerW) {
    if (s.width > 0.0f) {
        return s.width;
    }
    if (s.display == ComputedStyle::Block) {
        return std::max(0.0f, containerW - (s.margin.left + s.borderWidth.left +
                                             s.padding.left + s.padding.right +
                                             s.borderWidth.right + s.margin.right));
    }
    return 0.0f;
}

float contentHeightFor(const ComputedStyle& s, float contentH) {
    if (s.height > 0.0f) {
        return s.height;
    }
    return std::max(0.0f, contentH);
}

}  // namespace

void BlockLayout::layout(Element* root, const LayoutContext& ctx,
                         const StyleResolver& resolver) {
    if (!root) {
        return;
    }

    float y = 0.0f;
    root->visit([&](Element& el) {
        ComputedStyle s = resolver.resolve(el);
        if (s.display == ComputedStyle::None || !s.visible) {
            return;
        }
        BoxModel box;
        box.margin = s.margin;
        box.border = s.borderWidth;
        box.padding = s.padding;
        const float cw = contentWidthFor(s, ctx.containerWidth);
        box.setContentSize(cw, 0.0f);  // height auto for now
        // Outer width/height approximate
        const float outerW = box.outerWidth();
        const float outerH = box.outerHeight();
        // Position: block stacking (very rough)
        const float px = box.margin.left;
        const float py = y + box.margin.top;
        const float pw = std::max(0.0f, outerW - box.margin.left - box.margin.right);
        const float ph = std::max(0.0f, outerH - box.margin.top - box.margin.bottom);
        box.bounds = {px, py, pw, ph};
        el.setLayoutX(px);
        el.setLayoutY(py);
        el.setLayoutWidth(pw);
        el.setLayoutHeight(ph);
        y += outerH;
    });
}

}  // namespace sui
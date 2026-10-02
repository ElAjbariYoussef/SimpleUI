#pragma once

#include "CSS/Style.h"

namespace sui {

class Element;

struct LayoutBox {
    BoxModel box;
    ComputedStyle style;
    // Relative position within parent content area
    float x = 0.0f;
    float y = 0.0f;
};

class LayoutNode {
public:
    explicit LayoutNode(Element* element) : element_(element) {}

    Element* element() const noexcept { return element_; }
    LayoutBox& layoutBox() noexcept { return layoutBox_; }
    const LayoutBox& layoutBox() const noexcept { return layoutBox_; }

private:
    Element* element_ = nullptr;
    LayoutBox layoutBox_;
};

}  // namespace sui
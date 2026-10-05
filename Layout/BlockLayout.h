#pragma once

#include "CSS/Style.h"

namespace sui {

class Element;
class StyleResolver;
class TextFont;

struct LayoutInput {
    const StyleResolver* resolver = nullptr;
    const TextFont* font = nullptr;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
};

// Block flow: block-level children stack vertically inside the parent's content
// box; inline children are collected into shared line boxes. Heights are
// content-derived unless the style pins one.
class BlockLayout {
public:
    void layout(Element* root, const LayoutInput& input);
};

}  // namespace sui

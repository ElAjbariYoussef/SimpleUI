#pragma once

#include "CSS/Style.h"

#include <vector>

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
    // Centers an already-laid-out subtree in the viewport: every box and run
    // moves, so hit-testing and painting follow without further offsets. The
    // frame recomputes it from a fresh layout, so nothing accumulates.
    static void centerInViewport(Element& el, float viewportWidth, float viewportHeight);

    // Moves a laid-out subtree by (dx, dy): boxes, padding/content boxes, and
    // text runs together. A nested document lays out at its own origin and is
    // translated onto the box that hosts it.
    static void translateSubtree(Element& el, float dx, float dy);
};

// The centre x and vertical span of each painted column rule, one per gap
// between the used columns recorded by the latest layout pass. `gap` is the
// used `column-gap`. Empty for anything with fewer than two columns.
struct ColumnRuleSpan {
    float x = 0.0f;
    float top = 0.0f;
    float height = 0.0f;
};
std::vector<ColumnRuleSpan> columnRuleSpans(const Element& el, float gap);

}  // namespace sui

// The list a frame is painted from, in CSS paint order.
//
// In CSS the root stacking context paints in this order: its own background,
// then negative `z-index`, then every in-flow box, then positioned boxes with
// `z-index: auto` or `0`, then positive `z-index`. Document order breaks ties
// inside a layer.
//
// Elements that open a stacking context of their own -- a positioned `z-index`,
// an `opacity` under 1, a `transform`, a `filter` -- keep their whole subtree
// to themselves: the group sorts into the context around it by the group root's
// own layer and `z-index`, and everything below it is ordered inside. A `z-index`
// in the group therefore cannot climb above a sibling of the group, which is
// what the flat list used to allow. A `position` with `z-index: auto` opens a
// group for its in-flow content only: CSS sends its positioned descendants back
// out to the context around it, and so does this walk.
#pragma once

#include <SDL3/SDL_rect.h>

#include <cstddef>
#include <vector>

#include "CSS/Style.h"

namespace sui {

class Element;
class StyleResolver;

struct PaintItem {
    Element* el;
    int layer = 0;
    int zIndex = 0;
    std::size_t order = 0;
    // The tightest rect every ancestor's `overflow` imposes, in window
    // coordinates. A width or height of zero or less means no clip at all.
    SDL_FRect clip{0.0f, 0.0f, 0.0f, 0.0f};
    // Scroll offsets accumulated from clipping ancestors. Content moves up and
    // left by them, so this element's box is drawn at layout minus the offset;
    // its own scroll only shifts what is inside it.
    float dx = 0.0f;
    float dy = 0.0f;
    // Every ancestor's `opacity` multiplied together. Carried per item rather than
    // composited through an offscreen buffer, which differs only where two elements
    // inside the same group overlap.
    float opacity = 1.0f;
};

// True when `overflow` on this style crops anything, so its boxes are clipped to
// the padding box and its children are shifted by its scroll offsets.
bool clipsAnything(const ComputedStyle& s);

// True when an element opens a stacking context of its own, so its box and its
// in-flow descendants are ordered as one unit inside the context around it. A
// positioned box with `z-index: auto` counts too: CSS then lets its positioned
// descendants rejoin the context above, which this predicate cannot see, but its
// in-flow content still belongs with its box.
bool createsPaintGroup(const ComputedStyle& s);

// The root's paint list in CSS order: back to front, groups emitted right after
// the box that opens them. Elements that do not paint (hidden, non-visual
// wrappers) are absent; the walk needs no device or renderer.
std::vector<PaintItem> computePaintOrder(Element& root, const StyleResolver& resolver);

}  // namespace sui

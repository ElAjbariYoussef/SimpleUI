// The operations a frame is painted from, in CSS paint order.
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
//
// When a group root carries an `opacity` under 1 -- or a `filter: opacity()` --
// the walk brackets its boxes with BeginGroup/EndGroup ops: the executor
// renders them into an offscreen surface and blends the surface back at the
// group's alpha, so overlapping members mix with each other first, the way CSS
// composites a stacking context. `bounds` is the smallest window rect that
// holds the whole group -- including shadows and outlines that paint outside
// its boxes -- clipped to what its ancestors allow.
#pragma once

#include <SDL3/SDL_rect.h>

#include <cstddef>
#include <vector>

#include "CSS/Style.h"

namespace sui {

class Element;
class StyleResolver;

struct PaintItem {
    Element* el = nullptr;
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
    // The alpha to draw the box with. A composited group's alpha is *not* in
    // here: its members paint at full strength into an offscreen surface and
    // the group's alpha is applied when that surface is blended back, so normal
    // documents see this at 1. It stays a multiplier for the alphas that were
    // never offscreen (a `display:none` ancestor, or a group that could not
    // allocate a surface).
    float opacity = 1.0f;
};

// One step of the paint sequence. `Box` draws an element; `BeginGroup` and
// `EndGroup` bracket the boxes of one stacking context the executor must
// composite offscreen. For BeginGroup, `bounds` is in window coordinates and
// `alpha` is the group's opacity to blend the surface back with. When the
// group root carries a scale() or rotate(), `transformed` is set and the
// surface is mapped to the screen by the affine `m` -- x' = m[0]x + m[2]y + m[4],
// y' = m[1]x + m[3]y + m[5] -- about `origin`, the resolved `transform-origin`
// point (a window-coordinate position), instead of being blitted axis-aligned.
// `blur` is a `filter: blur()` radius in pixels; when a BeginGroup carries one,
// the executor reads the finished surface back, blurs it, and blits the result.
// `shift` is a `filter:` colour-shift chain (brightness/contrast/saturate/
// grayscale/sepia/invert/hue-rotate): the executor applies the composed affine
// `filter[12]` to the same readback. An affine colour map commutes with the box
// blur, so the two filters share one readback whatever the chain's order.
struct PaintOp {
    enum class Kind { Box, BeginGroup, EndGroup };
    Kind kind = Kind::Box;
    PaintItem item;
    SDL_FRect bounds{0.0f, 0.0f, 0.0f, 0.0f};
    float alpha = 1.0f;
    bool transformed = false;
    float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    SDL_FPoint origin{0.0f, 0.0f};
    float blur = 0.0f;
    bool shift = false;
    float filter[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
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

// The operations a frame is painted from, back to front. Elements that do not
// paint (hidden, non-visual wrappers) are absent. The walk needs no device or
// renderer, so tests can assert on the sequence, the group bounds, and the
// alphas directly.
std::vector<PaintOp> computePaintOps(Element& root, const StyleResolver& resolver);

}  // namespace sui
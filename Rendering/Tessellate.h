// Scanline tessellation for SVG fills: closed loops become a triangle soup
// under a fill rule, so even-odd holes and nonzero windings paint correctly
// where a fan from one vertex only covers convex shapes.
#pragma once

#include "SUI/DOM/Element.h"

#include <SDL3/SDL_rect.h>

#include <vector>

namespace sui {

// Whether a closed loop is convex: every turn goes the same way, ignoring
// straight runs. A convex loop fills exactly as a fan from any vertex.
bool isConvexLoop(const std::vector<SDL_FPoint>& loop);

// Tessellates closed loops into triangles, three points per triangle. Bands
// run between consecutive distinct vertex heights; each band fills the spans
// its edge crossings name (pairs for even-odd, nonzero winding counts
// otherwise) as quads split into two triangles. Degenerate loops and empty
// spans contribute nothing.
std::vector<SDL_FPoint> tessellateFill(
    const std::vector<std::vector<SDL_FPoint>>& loops, bool evenOdd);

// The `fill-rule` a shape paints with: its own `fill-rule` attribute wins,
// then the nearest ancestor's, so `evenodd` and `nonzero` inherit through
// `g`. Anything else leaves the property unset and the search continues;
// nothing authored means nonzero.
bool svgFillEvenOdd(const Element& el);

}  // namespace sui

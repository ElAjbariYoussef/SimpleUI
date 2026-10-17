// SVG text chunks: a <text> element's content split at styling and
// positioning boundaries, so tspans paint with their own fill, size, and
// pen. One chunk per maximal run sharing a style node; multi-value x/y/dx/dy
// lists and rotate stay out of scope (the first value positions the chunk).
#pragma once

#include "CSS/StyleResolver.h"
#include "SUI/DOM/Element.h"

#include "Rendering/Renderer.h"

#include <functional>
#include <string>
#include <vector>

namespace sui {

// Fill, size, and family a text chunk paints with: the nearest authored
// `fill` (with `none` stopping the search and garbage continuing it,
// defaulting to black), `font-size` (nearest authored wins), and `font-family`
// (nearest authored wins), each falling back to the CSS value, on the way up
// through tspan, text, g, and svg.
struct SvgTextStyle {
    bool hasFill = false;
    Color fill{0, 0, 0, 255};
    float pxSize = 0.0f;
    std::string family;
};

SvgTextStyle svgTextStyle(const Element& node, float cssFontSize,
                          std::string_view cssFamily, float scaleX);

// One paintable run: collapsed text, the node styling it, and an optional
// absolute start plus relative shift, all in pixels. A chunk inside a
// <textPath> carries that ancestor and lays its glyphs along the path
// instead of the straight pen.
struct SvgTextChunk {
    std::string text;
    const Element* styleNode = nullptr;
    const Element* pathNode = nullptr;
    bool positioned = false;
    float x = 0.0f;
    float y = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    SvgTextStyle style;
};

// Splits a <text> element's descendants into chunks: text nodes and nested
// tspans in document order (any other element contributes its text plainly),
// breaking at tspan boundaries and at authored x/y/dx/dy. Whitespace
// collapses exactly as if the whole content were one string, so markup
// indentation between tspans behaves like it does without them.
std::vector<SvgTextChunk> collectSvgTextChunks(const Element& text, float cssFontSize,
                                               std::string_view cssFamily, float scaleX,
                                               float scaleY, float baseW, float baseH);

// Arc-length parameterization of flattened loops for text on a path: total
// length plus the point and tangent at a distance, looping closed shapes
// through their seam. Concatenated loops run end to end.
struct SvgPathPoint {
    float x = 0.0f;
    float y = 0.0f;
    float angleDeg = 0.0f;
};

float svgLoopsLength(const std::vector<std::vector<SDL_FPoint>>& loops);
bool svgPointAtDistance(const std::vector<std::vector<SDL_FPoint>>& loops, float distance,
                        SvgPathPoint& out);

}  // namespace sui

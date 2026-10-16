// SVG markers: arrowheads and friends painted at shape vertices. A
// marker-start/mid/end attribute names a <marker> by url(#id); the marker's
// viewBox meets into its markerWidth/markerHeight viewport (scaled by the
// referencing stroke width, or raw for userSpaceOnUse), its refX/refY lands
// on the vertex, and `orient` rotates the result (auto follows the path
// tangent, a number is degrees).
#pragma once

#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"

#include "Rendering/Renderer.h"

#include <string_view>

namespace sui {

// A resolved marker placement: marker user point m paints at
// vertex + R(angle) * (scale * m + offset), all in pixels. The offset folds
// the meet centering together with the refX/refY shift.
struct SvgMarkerFrame {
    float vertexX = 0.0f;
    float vertexY = 0.0f;
    float scale = 1.0f;
    float cosA = 1.0f;
    float sinA = 0.0f;
    float offX = 0.0f;
    float offY = 0.0f;
};

SDL_FPoint svgMarkerPlace(const SvgMarkerFrame& frame, float mx, float my);

// The <marker> an attribute names, or null for `none`, garbage, a missing
// id, or anything that is not a marker element.
const Element* resolveMarkerRef(Document& document, std::string_view attr);

// Resolves a marker element's placement at a vertex: viewport mapping, ref
// shift, and orient. `tangentDeg` answers `orient="auto"` (reversed for a
// start marker under `auto-start-reverse`); `strokeWidth` is the referencing
// shape's width in user units for markerUnits scaling.
SvgMarkerFrame svgMarkerFrame(const Element& marker, float vertexX, float vertexY,
                               float tangentDeg, float strokeWidth, bool reverse);

// The path direction at a vertex, in degrees: outgoing for a start or middle
// vertex, incoming for an end vertex. A closed loop wraps around the seam, so
// the last vertex looks back at the first. Zero-length segments read as 0.
float svgVertexAngle(const std::vector<SDL_FPoint>& points, std::size_t at, bool end,
                     bool closed);

}  // namespace sui

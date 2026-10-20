// SVG paint servers: linear and radial gradients a `fill` (or `stroke`) can
// name with `url(#id)`. The parser reads the gradient element's presentation
// attributes and stop children into engine coordinates, and the shader builder
// maps objectBoundingBox or userSpaceOnUse units onto a shape's bounding box.
// Strokes and text take solid fallbacks; only shape fills reach the full
// gradient shader.
#pragma once

#include "Rendering/Renderer.h"
#include "SUI/DOM/Element.h"

#include <string_view>
#include <vector>

namespace sui {

struct SvgGradientStop {
    float offset = 0.0f;
    Color color{0, 0, 0, 255};
};

struct SvgGradient {
    bool radial = false;
    // gradientUnits: objectBoundingBox (default) or userSpaceOnUse.
    bool objectBoundingBox = true;
    // Linear coordinates, in bbox fractions or user units depending on the
    // units above. Defaults are the spec's 0%,0% to 100%,0%.
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 1.0f;
    float y2 = 0.0f;
    // Radial centre and radius. Defaults are 50%,50% r=50%.
    float cx = 0.5f;
    float cy = 0.5f;
    float r = 0.5f;
    std::vector<SvgGradientStop> stops;
};

// The gradient a `paint` value names, or null when it is not `url(#id)` or the
// id does not name a linearGradient/radialGradient. `radial` reports which of
// the two tags matched.
const Element* resolveSvgGradientRef(const Element& shape, std::string_view paint,
                                     bool& radial);

// The fallback colour after `url(#id)` (`fill="url(#g) red"`), false when the
// value has no trailing colour or the colour does not parse.
bool svgPaintFallbackColor(std::string_view paint, Color& out);

// Whether a paint value is a `url(...)` reference at all, valid or not. An
// unresolvable reference with no fallback paints nothing.
bool svgPaintIsUrl(std::string_view paint);

// Parses a gradient element into its stop list and geometry. False when no
// usable stop was found.
bool parseSvgGradient(const Element& gradient, bool radial, SvgGradient& out);

// Builds the renderer shader for a shape whose device-space bounding box is
// `bbox`. `view` maps user units for userSpaceOnUse gradients; `alpha` folds
// the frame's opacity into every stop.
Shader svgGradientShader(const SvgGradient& gradient, const SDL_FRect& bbox,
                         const SvgView& view, float alpha);

}  // namespace sui

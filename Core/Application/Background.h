// Painting an element's background: the flat colour, then the image layers.
//
// The CSS background properties are a family of parallel comma-separated lists rather
// than one record per layer, and the engine keeps them that way — matching the lists up
// is a paint-time concern that also has to know which of the three boxes each layer is
// relative to. Everything here works in window coordinates.
#pragma once

#include <SDL3/SDL.h>

#include "CSS/Style.h"
#include "Rendering/ImageCache.h"
#include "Rendering/Renderer.h"

namespace sui {

class TextFont;

// The border, padding and content boxes of an element, in window coordinates. The
// three are computed together because every background layer picks one of them and the
// differences are the border widths and the padding, which cannot be derived twice
// without risking a half-pixel of disagreement.
struct BackgroundBoxes {
    SDL_FRect border;
    SDL_FRect padding;
    SDL_FRect content;
};

// `element` is only used for its own box; its children are painted by the caller and
// carry their own backgrounds.
void paintBackground(const ComputedStyle& style, const BackgroundBoxes& boxes, float fontSize,
                     float rootFontSize, float opacity, Renderer& renderer, ImageCache& images,
                     const Radii& borderRadii);

// Multiplies surface alpha by a `mask-image` gradient stretched over
// `destination` (already in surface coordinates). Pure pixels and math: no
// device, so tests drive it directly. Anything but a gradient is a no-op, as
// is an empty stop list.
void applyMaskImage(std::uint8_t* pixels, int width, int height,
                    const ComputedStyle::MaskImage& mask, const SDL_FRect& destination,
                    float fontSize, float rootFontSize);

}  // namespace sui

// Painting an element's background: the flat colour, then the image layers.
//
// The CSS background properties are a family of parallel comma-separated lists rather
// than one record per layer, and the engine keeps them that way — matching the lists up
// is a paint-time concern that also has to know which of the three boxes each layer is
// relative to. Everything here works in window coordinates.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

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

// Multiplies surface alpha by `mask-image` layers painted into `boxes`
// (already in surface coordinates). Pure pixels and math: no device, so tests
// drive it directly. Each layer is sized by `mask-size`, placed by
// `mask-position` within its `mask-origin` box, tiled by `mask-repeat`, and
// clipped to its `mask-clip` box; outside a layer's painted area the mask is
// transparent. Layers combine through `mask-composite` (`add` unions,
// `subtract` cuts, `intersect` multiplies, `exclude` xors), each read through
// its `mask-mode` (`alpha`, `luminance`, or `match-source` which is alpha for
// gradients and luminance for urls). Shorter mode, composite, and geometry
// lists repeat their last entry; an empty geometry list is the initial value
// (`auto` size, top-left, tiling repeat, border boxes). A missing fetcher
// leaves url() layers transparent.
using FetchMaskBytes =
    std::function<const DecodedImage*(std::string_view href)>;
void applyMaskLayers(std::uint8_t* pixels, int width, int height,
                     const std::vector<ComputedStyle::MaskImage>& layers,
                     const std::vector<ComputedStyle::MaskMode>& modes,
                     const std::vector<ComputedStyle::MaskComposite>& composites,
                     const std::vector<ComputedStyle::BackgroundSize>& sizes,
                     const std::vector<ComputedStyle::BackgroundPosition>& positions,
                     const std::vector<ComputedStyle::RepeatPair>& repeats,
                     const std::vector<ComputedStyle::BackgroundBox>& origins,
                     const std::vector<ComputedStyle::BackgroundBox>& clips,
                     const BackgroundBoxes& boxes, float fontSize, float rootFontSize,
                     FetchMaskBytes fetch);

// Single-layer form, kept for the gradient tests: url() layers need a fetcher
// and stay transparent without one.
void applyMaskImage(std::uint8_t* pixels, int width, int height,
                    const ComputedStyle::MaskImage& mask, const SDL_FRect& destination,
                    float fontSize, float rootFontSize);
// Multiplies surface alpha by a `mask-border` 9-patch painted into `boxes`
// (already in surface coordinates). Pure pixels and math: no device, so tests
// drive it directly. The source is sliced into corners, edges, and a middle;
// corners stretch, edges stretch across and tile along per `mask-border-repeat`
// (`stretch`/`repeat`/`round`/`space`), and the middle tiles only with `fill`
// (otherwise it is transparent). Widths size the bands, outsets grow the area;
// outside the outset area pixels keep their alpha. A missing url source hides
// everything, like a missing `mask-image` url.
void applyMaskBorder(std::uint8_t* pixels, int width, int height,
                     const ComputedStyle::MaskBorder& border,
                     const BackgroundBoxes& boxes, float fontSize, float rootFontSize,
                     FetchMaskBytes fetch);

// One blit of a 9-patch border image: a source-px rect onto a paint-coords
// rect. Pure geometry, so tests pin the tiling without a device.
struct BorderTile {
    SDL_FRect src;
    SDL_FRect dst;
};
std::vector<BorderTile> borderImageTiles(const ComputedStyle::BorderImage& image,
                                         float srcW, float srcH,
                                         const BackgroundBoxes& boxes, float fontSize,
                                         float rootFontSize);

// Paints a `border-image` 9-patch over `boxes` (window coordinates): corners
// stretch, edges tile per the repeat modes, and the middle paints only with
// `fill`. Gradients render once over the outset area and slice from there;
// repeat modes behave as stretch for gradient sources. A missing url source
// paints nothing.
void paintBorderImage(const ComputedStyle::BorderImage& image,
                      const BackgroundBoxes& boxes, float fontSize, float rootFontSize,
                      float opacity, Renderer& renderer, ImageCache& images);

}  // namespace sui

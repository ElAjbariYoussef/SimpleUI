#include "Core/Application/Background.h"
#include "Rendering/ColorMatrix.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace sui {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// CSS matches the parallel background lists by position and repeats the shorter one, so
// `background-size: 10px` applies to every layer however many images there are.
template <typename T>
const T& listAt(const std::vector<T>& list, std::size_t index) {
    static const T fallback{};
    if (list.empty()) {
        return fallback;
    }
    return list[std::min(index, list.size() - 1)];
}

SDL_FRect boxOf(const BackgroundBoxes& boxes, ComputedStyle::BackgroundBox which) {
    switch (which) {
        case ComputedStyle::BackgroundPaddingBox:
            return boxes.padding;
        case ComputedStyle::BackgroundContentBox:
            return boxes.content;
        case ComputedStyle::BackgroundBorderBox:
            break;
    }
    return boxes.border;
}

// A layer clipped to the padding or content box has its corners pulled in by whatever
// sits between the two boxes, because the radii describe the border box.
Radii radiiForBox(const Radii& borderRadii, const SDL_FRect& borderBox,
                  const SDL_FRect& paintBox) {
    if (!borderRadii.any()) {
        return borderRadii;
    }
    const float insetLeft = paintBox.x - borderBox.x;
    const float insetTop = paintBox.y - borderBox.y;
    const float insetRight = borderBox.x + borderBox.w - (paintBox.x + paintBox.w);
    const float insetBottom = borderBox.y + borderBox.h - (paintBox.y + paintBox.h);

    Radii radii;
    radii.topLeft = {std::max(0.0f, borderRadii.topLeft.x - insetLeft),
                     std::max(0.0f, borderRadii.topLeft.y - insetTop)};
    radii.topRight = {std::max(0.0f, borderRadii.topRight.x - insetRight),
                      std::max(0.0f, borderRadii.topRight.y - insetTop)};
    radii.bottomRight = {std::max(0.0f, borderRadii.bottomRight.x - insetRight),
                         std::max(0.0f, borderRadii.bottomRight.y - insetBottom)};
    radii.bottomLeft = {std::max(0.0f, borderRadii.bottomLeft.x - insetLeft),
                        std::max(0.0f, borderRadii.bottomLeft.y - insetBottom)};
    return radii.clamped(paintBox.w, paintBox.h);
}

// A radius only survives on a corner the smaller box still reaches, which is what makes
// a background image smaller than its box come out square-cornered where it is cut.
Radii radiiClippedTo(const Radii& radii, const SDL_FRect& from, const SDL_FRect& to) {
    const float epsilon = 0.01f;
    const auto keep = [&](float outer, float inner) { return inner >= outer - epsilon; };
    Radii out;
    out.topLeft = {keep(from.x, to.x) ? radii.topLeft.x : 0.0f,
                   keep(from.y, to.y) ? radii.topLeft.y : 0.0f};
    out.topRight = {keep(from.x + from.w, to.x + to.w) ? radii.topRight.x : 0.0f,
                    keep(from.y, to.y) ? radii.topRight.y : 0.0f};
    out.bottomRight = {keep(from.x + from.w, to.x + to.w) ? radii.bottomRight.x : 0.0f,
                       keep(from.y + from.h, to.y + to.h) ? radii.bottomRight.y : 0.0f};
    out.bottomLeft = {keep(from.x, to.x) ? radii.bottomLeft.x : 0.0f,
                      keep(from.y + from.h, to.y + to.h) ? radii.bottomLeft.y : 0.0f};
    return out.clamped(to.w, to.h);
}

// The two axes of `background-size`, given the layer's intrinsic size. `auto` means the
// image's own size on an axis, except where the other axis is explicit, in which case
// it means "whatever keeps the aspect ratio".
SDL_FRect resolveSize(const ComputedStyle::BackgroundSize& size, const SDL_FRect& box,
                      float intrinsicWidth, float intrinsicHeight, bool isGradient) {
    if (isGradient) {
        // A gradient has no intrinsic size, so `auto` is the whole box and only an
        // explicit value or `cover`/`contain` changes it.
        const float scale = [&] {
            if (size.mode == ComputedStyle::BackgroundSize::Cover) {
                return std::max(box.w / std::max(1e-6f, intrinsicWidth),
                                box.h / std::max(1e-6f, intrinsicHeight));
            }
            if (size.mode == ComputedStyle::BackgroundSize::Contain) {
                return std::min(box.w / std::max(1e-6f, intrinsicWidth),
                                box.h / std::max(1e-6f, intrinsicHeight));
            }
            return 0.0f;
        }();
        SDL_FRect out = box;
        if (size.mode == ComputedStyle::BackgroundSize::Explicit) {
            if (size.width.unit != Length::Auto) {
                out.w = size.width.resolve(box.w);
            }
            if (size.height.unit != Length::Auto) {
                out.h = size.height.resolve(box.h);
            }
        } else if (scale != 0.0f) {
            out.w = intrinsicWidth * scale;
            out.h = intrinsicHeight * scale;
        }
        return out;
    }

    switch (size.mode) {
        case ComputedStyle::BackgroundSize::Cover:
        case ComputedStyle::BackgroundSize::Contain: {
            const float sx = box.w / std::max(1e-6f, intrinsicWidth);
            const float sy = box.h / std::max(1e-6f, intrinsicHeight);
            const float scale = size.mode == ComputedStyle::BackgroundSize::Cover
                                    ? std::max(sx, sy)
                                    : std::min(sx, sy);
            return SDL_FRect{0.0f, 0.0f, intrinsicWidth * scale, intrinsicHeight * scale};
        }
        case ComputedStyle::BackgroundSize::AutoSize:
            return SDL_FRect{0.0f, 0.0f, intrinsicWidth, intrinsicHeight};
        case ComputedStyle::BackgroundSize::Explicit:
            break;
    }

    const bool autoWidth = size.width.unit == Length::Auto;
    const bool autoHeight = size.height.unit == Length::Auto;
    const float ratio = intrinsicHeight / std::max(1e-6f, intrinsicWidth);
    SDL_FRect out{0.0f, 0.0f, intrinsicWidth, intrinsicHeight};
    if (autoWidth && autoHeight) {
        // `background-size: auto auto` is the image's own size, which is also what the
        // unstated default asks for.
        return out;
    }
    if (!autoWidth && !autoHeight) {
        out.w = size.width.resolve(box.w);
        out.h = size.height.resolve(box.h);
    } else if (!autoWidth) {
        out.w = size.width.resolve(box.w);
        out.h = out.w * ratio;
    } else {
        out.h = size.height.resolve(box.h);
        out.w = out.h / std::max(1e-6f, ratio);
    }
    return out;
}

// Where a layer of the given size sits within its positioning area. A keyword picks the
// edge or the middle, and the authored length then moves it away from whichever edge that
// was, which is why an offset after `right` is stored negative. The area is
// `background-origin`'s box and not the clip box, so a layer clipped to the border box can
// still be positioned from the padding box.
SDL_FRect placeLayer(const ComputedStyle::BackgroundSize& size,
                     const ComputedStyle::BackgroundPosition& position,
                     const SDL_FRect& originBox, float intrinsicWidth, float intrinsicHeight,
                     bool isGradient, float fontSize, float rootFontSize) {
    const SDL_FRect scaled =
        resolveSize(size, originBox, intrinsicWidth, intrinsicHeight, isGradient);
    const auto offset = [](ComputedStyle::BackgroundPosition::Keyword keyword, float available,
                           float extent) {
        switch (keyword) {
            case ComputedStyle::BackgroundPosition::Center:
                return (available - extent) * 0.5f;
            case ComputedStyle::BackgroundPosition::End:
                return available - extent;
            case ComputedStyle::BackgroundPosition::Start:
                break;
        }
        return 0.0f;
    };
    const float dx =
        offset(position.horizontal, originBox.w, scaled.w) +
        position.offsetX.resolve(originBox.w, fontSize, rootFontSize);
    const float dy =
        offset(position.vertical, originBox.h, scaled.h) +
        position.offsetY.resolve(originBox.h, fontSize, rootFontSize);
    return SDL_FRect{originBox.x + dx, originBox.y + dy, scaled.w, scaled.h};
}

// An unauthored stop sits at the midpoint of its neighbours; the first and last clamp
// to the ends. Doing it in two sweeps handles a run of consecutive unauthored stops,
// which a single left-to-right pass would resolve to zero.
std::vector<float> resolveStopOffsets(const std::vector<ComputedStyle::GradientStop>& stops,
                                      float lineLength, float fontSize, float rootFontSize) {
    std::vector<float> out;
    out.reserve(stops.size());
    for (const ComputedStyle::GradientStop& stop : stops) {
        if (stop.offset.unit == Length::Auto) {
            out.push_back(-1.0f);
            continue;
        }
        // Both forms end up as a fraction of the line, but they get there differently: a
        // percentage *is* its own fraction, whereas a length has to be measured. Clamping
        // before the division would cap a 20px stop at 1 instead of spreading it over the
        // whole ramp.
        const float fraction =
            stop.offset.unit == Length::Percent
                ? stop.offset.value * 0.01f
                : stop.offset.resolve(0.0f, fontSize, rootFontSize) /
                      std::max(1e-6f, lineLength);
        out.push_back(std::clamp(fraction, 0.0f, 1.0f));
    }
    if (out.empty()) {
        return out;
    }
    if (out.front() < 0.0f) {
        out.front() = 0.0f;
    }
    if (out.back() < 0.0f) {
        out.back() = 1.0f;
    }
    std::size_t i = 1;
    while (i + 1 < out.size()) {
        if (out[i] >= 0.0f) {
            ++i;
            continue;
        }
        std::size_t next = i + 1;
        while (next < out.size() && out[next] < 0.0f) {
            ++next;
        }
        const float from = out[i - 1];
        const float to = next < out.size() ? out[next] : 1.0f;
        const float span = (to - from) / static_cast<float>(next - i + 1);
        for (std::size_t k = i; k < next; ++k) {
            out[k] = from + span * static_cast<float>(k - i + 1);
        }
        i = next;
    }
    // CSS clamps a stop that would sit behind its predecessor rather than letting the
    // colour jump backwards along the ramp.
    for (std::size_t k = 1; k < out.size(); ++k) {
        out[k] = std::max(out[k], out[k - 1]);
    }
    return out;
}

std::vector<GradientStop> toGradientStops(const std::vector<ComputedStyle::GradientStop>& stops,
                                          float lineLength, float fontSize, float rootFontSize,
                                          float opacity) {
    const std::vector<float> offsets = resolveStopOffsets(stops, lineLength, fontSize,
                                                           rootFontSize);
    std::vector<GradientStop> out;
    out.reserve(stops.size());
    for (std::size_t i = 0; i < stops.size(); ++i) {
        out.push_back(GradientStop{offsets[i],
                                   multiplyAlpha(toColor(stops[i].color), opacity)});
    }
    return out;
}

// The gradient line for a box and an angle. CSS measures the angle clockwise from "to
// top", so the direction is `(sin, -cos)`, and the line is long enough to reach both
// corners on the side it points at.
float gradientLine(float angleDegrees, const SDL_FRect& rect, LinearGradient& out) {
    const float radians = angleDegrees * kPi / 180.0f;
    const float dx = std::sin(radians);
    const float dy = -std::cos(radians);
    const float length = std::abs(rect.w * dx) + std::abs(rect.h * dy);
    const float cx = rect.x + rect.w * 0.5f;
    const float cy = rect.y + rect.h * 0.5f;
    out.x1 = cx - dx * length * 0.5f;
    out.y1 = cy - dy * length * 0.5f;
    out.x2 = cx + dx * length * 0.5f;
    out.y2 = cy + dy * length * 0.5f;
    return length;
}

// A radial gradient's radii, per CSS Images 3: start from what was authored, then grow
// or shrink them until the chosen extent is satisfied. A circle comes out as an ellipse
// with equal radii, so the two go through the same arithmetic.
void radialRadii(const ComputedStyle::RadialGradient& source, const SDL_FRect& rect,
                 float fontSize, float rootFontSize, RadialGradient& out) {
    const float cx = source.centreX.resolve(rect.w, fontSize, rootFontSize) + rect.x;
    const float cy = source.centreY.resolve(rect.h, fontSize, rootFontSize) + rect.y;

    float rx = 0.0f;
    float ry = 0.0f;
    if (source.hasRadii) {
        rx = std::abs(source.radiusX.resolve(rect.w, fontSize, rootFontSize));
        ry = source.shape == ComputedStyle::RadialGradient::Circle
                 ? rx
                 : std::abs(source.radiusY.resolve(rect.h, fontSize, rootFontSize));
    } else {
        rx = rect.w * 0.5f;
        ry = rect.h * 0.5f;
    }

    const float toLeftEdge = std::abs(cx - rect.x);
    const float toRightEdge = std::abs(rect.x + rect.w - cx);
    const float toTopEdge = std::abs(cy - rect.y);
    const float toBottomEdge = std::abs(rect.y + rect.h - cy);

    switch (source.extent) {
        case ComputedStyle::RadialGradient::ClosestSide:
            rx = std::min(rx, std::min(toLeftEdge, toRightEdge));
            ry = std::min(ry, std::min(toTopEdge, toBottomEdge));
            break;
        case ComputedStyle::RadialGradient::FarthestSide:
            rx = std::max(rx, std::max(toLeftEdge, toRightEdge));
            ry = std::max(ry, std::max(toTopEdge, toBottomEdge));
            break;
        case ComputedStyle::RadialGradient::ClosestCorner: {
            // Scale up until the nearer side just fits, which for an ellipse means
            // whichever of the two ratios is larger.
            const float fx = rx > 1e-6f ? std::min(toLeftEdge, toRightEdge) / rx : 1.0f;
            const float fy = ry > 1e-6f ? std::min(toTopEdge, toBottomEdge) / ry : 1.0f;
            const float factor = std::max(fx, fy);
            rx *= factor;
            ry *= factor;
            break;
        }
        case ComputedStyle::RadialGradient::FarthestCorner: {
            const float fx = rx > 1e-6f ? std::max(toLeftEdge, toRightEdge) / rx : 1.0f;
            const float fy = ry > 1e-6f ? std::max(toTopEdge, toBottomEdge) / ry : 1.0f;
            const float factor = std::min(fx, fy);
            rx *= factor;
            ry *= factor;
            break;
        }
    }

    out.cx = cx;
    out.cy = cy;
    out.radiusX = rx;
    out.radiusY = ry;
    out.focalX = source.hasFocus ? source.focalX.resolve(rect.w, fontSize, rootFontSize) + rect.x
                                 : cx;
    out.focalY = source.hasFocus ? source.focalY.resolve(rect.h, fontSize, rootFontSize) + rect.y
                                 : cy;
}

// The area a layer actually covers. A tiling axis reaches the whole clip box, and a
// non-tiling one reaches only as far as the image does, because past that the edge
// texels would be clamped and smeared out to the border. The two axes are settled
// independently, so `repeat-x` still covers the full width.
bool coveredArea(const SDL_FRect& clipBox, const SDL_FRect& destination, bool tileX,
                 bool tileY, SDL_FRect& out) {
    const float left = tileX ? clipBox.x : std::max(clipBox.x, destination.x);
    const float top = tileY ? clipBox.y : std::max(clipBox.y, destination.y);
    const float right =
        tileX ? clipBox.x + clipBox.w : std::min(clipBox.x + clipBox.w, destination.x + destination.w);
    const float bottom =
        tileY ? clipBox.y + clipBox.h : std::min(clipBox.y + clipBox.h, destination.y + destination.h);
    if (right - left <= 0.0f || bottom - top <= 0.0f) {
        return false;
    }
    out = SDL_FRect{left, top, right - left, bottom - top};
    return true;
}

Repeat repeatOf(const ComputedStyle::RepeatPair& pair) {
    if (pair.x && pair.y) {
        return Repeat::Tile;
    }
    if (pair.x) {
        return Repeat::TileX;
    }
    if (pair.y) {
        return Repeat::TileY;
    }
    return Repeat::No;
}

// Maps p into [lo, lo + extent) for a tiling mask axis, so a repeated layer
// samples its tile wherever the point lands.
float wrapAxis(float p, float lo, float extent) {
    float rel = std::fmod(p - lo, extent);
    if (rel < 0.0f) {
        rel += extent;
    }
    return lo + rel;
}

// Maps p within [dstLo, dstLo + dstLen) to [0, srcLen) per a border repeat
// mode, given one tile's dst width. Stretch covers the span in one tile;
// repeat clips the last tile; round rescales to whole tiles; space leaves
// even gaps (false) around whole tiles.
bool mapBorderAxis(float p, float dstLo, float dstLen, float srcLen, float tileDst,
                   ComputedStyle::MaskBorder::Repeat mode, float& q) {
    using Repeat = ComputedStyle::MaskBorder::Repeat;
    if (dstLen <= 0.0f || srcLen <= 0.0f) {
        return false;
    }
    const float rel = p - dstLo;
    if (mode == Repeat::Stretch) {
        if (rel < 0.0f || rel >= dstLen) {
            return false;
        }
        q = rel / dstLen * srcLen;
        return true;
    }
    if (tileDst <= 0.0f) {
        return false;
    }
    if (mode == Repeat::Round) {
        const int n = std::max(1, static_cast<int>(std::lround(dstLen / tileDst)));
        const float t = dstLen / static_cast<float>(n);
        float r = std::fmod(rel, t);
        if (r < 0.0f) {
            r += t;
        }
        q = r / t * srcLen;
        return true;
    }
    if (mode == Repeat::Space) {
        const int n = static_cast<int>(dstLen / tileDst);
        if (n >= 1) {
            const float slot = dstLen / static_cast<float>(n);
            const float off = (slot - tileDst) * 0.5f;
            float r = std::fmod(rel, slot);
            if (r < 0.0f) {
                r += slot;
            }
            if (r < off || r >= off + tileDst) {
                return false;
            }
            q = (r - off) / tileDst * srcLen;
            return true;
        }
        // Smaller than one tile: a single clipped tile, like repeat.
    }
    float r = std::fmod(rel, tileDst);
    if (r < 0.0f) {
        r += tileDst;
    }
    q = r / tileDst * srcLen;
    return true;
}

// Whether a surface point lands inside a rect, right/bottom-exclusive like the
// hit test, so a layer edge never bleeds into the next pixel.
bool insideBox(float px, float py, const SDL_FRect& box) {
    return px >= box.x && px < box.x + box.w && py >= box.y && py < box.y + box.h;
}

}  // namespace

// The colour is the bottom layer, painted across whichever box the first
// `background-clip` names (defaulting to the border box). The colour has no
// layer of its own, so it takes the first layer's clip.
void paintBackgroundColor(const ComputedStyle& style, const BackgroundBoxes& boxes,
                          float /*fontSize*/, float /*rootFontSize*/, float opacity,
                          Renderer& renderer, const Radii& borderRadii) {
    const ComputedStyle::BackgroundBox colourClip =
        style.backgroundClips.empty() ? ComputedStyle::BackgroundBorderBox
                                     : style.backgroundClips.front();
    const SDL_FRect colourBox = boxOf(boxes, colourClip);
    const Color colour = multiplyAlpha(toColor(style.backgroundColor), opacity);
    if (colour.a > 0 && colourBox.w > 0.0f && colourBox.h > 0.0f) {
        renderer.fillRect(colourBox, Shader::solid(colour),
                          radiiForBox(borderRadii, boxes.border, colourBox));
    }
}

void paintIsolatedBackground(const ComputedStyle& style, const BackgroundBoxes& boxes,
                             float fontSize, float rootFontSize, float opacity,
                             Renderer& renderer, ImageCache& images,
                             const Radii& borderRadii);

// One image layer by index, or nothing for `none` and empty clips. Paints to
// the current render target, so the isolated path can redirect it per layer.
void paintBackgroundLayer(const ComputedStyle& style, const BackgroundBoxes& boxes,
                          std::size_t index, float fontSize, float rootFontSize,
                          float opacity, Renderer& renderer, ImageCache& images,
                          const Radii& borderRadii) {
    if (index < style.backgroundImages.size()) {
        const ComputedStyle::BackgroundImage& layer = style.backgroundImages[index];
        if (layer.kind == ComputedStyle::BackgroundImage::None) {
            return;
        }
        const SDL_FRect clipBox = boxOf(boxes, listAt(style.backgroundClips, index));
        if (clipBox.w <= 0.0f || clipBox.h <= 0.0f) {
            return;
        }
        const Radii radii = radiiForBox(borderRadii, boxes.border, clipBox);
        const ComputedStyle::BackgroundPosition position = listAt(style.backgroundPositions, index);
        const ComputedStyle::BackgroundSize size = listAt(style.backgroundSizes, index);
        const ComputedStyle::RepeatPair repeatPair = listAt(style.backgroundRepeats, index);

        // The positioning area starts as the box the origin is measured from, which is
        // what `background-origin` names and defaults to the padding box.
        const SDL_FRect originBox =
            style.backgroundOrigins.empty()
                ? boxOf(boxes, ComputedStyle::BackgroundPaddingBox)
                : boxOf(boxes, listAt(style.backgroundOrigins, index));

        Shader shader;
        SDL_FRect paintBox = clipBox;
        switch (layer.kind) {
            case ComputedStyle::BackgroundImage::Url: {
                SDL_Texture* texture =
                    images.texture(renderer.sdl(), layer.href);
                if (!texture) {
                    return;
                }
                float intrinsicWidth = 0.0f;
                float intrinsicHeight = 0.0f;
                SDL_GetTextureSize(texture, &intrinsicWidth, &intrinsicHeight);
                const SDL_FRect destination =
                    placeLayer(size, position, originBox, intrinsicWidth, intrinsicHeight, false,
                               fontSize, rootFontSize);
                shader = Shader::image(texture, SDL_FRect{0.0f, 0.0f, intrinsicWidth,
                                                          intrinsicHeight},
                                       destination, repeatOf(repeatPair),
                                       Color{255, 255, 255,
                                             static_cast<Uint8>(std::lround(opacity * 255.0f))});
                SDL_FRect visible;
                if (!coveredArea(clipBox, destination, repeatPair.x, repeatPair.y, visible)) {
                    return;
                }
                paintBox = visible;
                break;
            }
            case ComputedStyle::BackgroundImage::LinearGradientImage: {
                const SDL_FRect destination = placeLayer(size, position, originBox, clipBox.w,
                                                          clipBox.h, true, fontSize, rootFontSize);
                LinearGradient gradient;
                const float lineLength = gradientLine(layer.linear.angle, destination, gradient);
                gradient.stops = toGradientStops(layer.linear.stops, lineLength, fontSize,
                                                 rootFontSize, opacity);
                if (gradient.stops.empty()) {
                    return;
                }
                shader = Shader::linear(gradient);
                SDL_FRect visible;
                if (!coveredArea(clipBox, destination, repeatPair.x, repeatPair.y, visible)) {
                    return;
                }
                paintBox = visible;
                break;
            }
            case ComputedStyle::BackgroundImage::RadialGradientImage: {
                const SDL_FRect destination = placeLayer(size, position, originBox, clipBox.w,
                                                          clipBox.h, true, fontSize, rootFontSize);
                RadialGradient gradient;
                radialRadii(layer.radial, destination, fontSize, rootFontSize, gradient);
                gradient.stops = toGradientStops(
                    layer.radial.stops, std::max(gradient.radiusX, gradient.radiusY), fontSize,
                    rootFontSize, opacity);
                if (gradient.stops.empty()) {
                    return;
                }
                shader = Shader::radial(gradient);
                SDL_FRect visible;
                if (!coveredArea(clipBox, destination, repeatPair.x, repeatPair.y, visible)) {
                    return;
                }
                paintBox = visible;
                break;
            }
            case ComputedStyle::BackgroundImage::ConicalGradientImage: {
                const SDL_FRect destination = placeLayer(size, position, originBox, clipBox.w,
                                                          clipBox.h, true, fontSize, rootFontSize);
                ConicalGradient gradient;
                gradient.cx = layer.conical.centreX.resolve(destination.w, fontSize,
                                                            rootFontSize) +
                              destination.x;
                gradient.cy = layer.conical.centreY.resolve(destination.h, fontSize,
                                                            rootFontSize) +
                              destination.y;
                gradient.fromAngle = layer.conical.angle;
                gradient.radius = std::max(destination.w, destination.h) * 0.5f;
                // Stop positions are fractions of the turn, so percentages need
                // no measuring length.
                gradient.stops = toGradientStops(layer.conical.stops, 1.0f, fontSize,
                                                 rootFontSize, opacity);
                if (gradient.stops.empty()) {
                    return;
                }
                shader = Shader::conical(gradient);
                SDL_FRect visible;
                if (!coveredArea(clipBox, destination, repeatPair.x, repeatPair.y, visible)) {
                    return;
                }
                paintBox = visible;
                break;
            }
            case ComputedStyle::BackgroundImage::None:
                return;
        }

        const Radii paintRadii = paintBox.w == clipBox.w && paintBox.h == clipBox.h
                                     ? radii
                                     : radiiClippedTo(radii, clipBox, paintBox);
        renderer.fillRect(paintBox, shader, paintRadii);
    }
}

// Whether any background layer blends: an empty list (or all normal) paints
// straight onto the page, anything else isolates the stack from the backdrop.
bool usesBackgroundBlending(const ComputedStyle& style) {
    for (ComputedStyle::MixBlendMode mode : style.backgroundBlendModes) {
        if (mode != ComputedStyle::MixBlendMode::Normal) {
            return true;
        }
    }
    return false;
}

void paintBackground(const ComputedStyle& style, const BackgroundBoxes& boxes, float fontSize,
                     float rootFontSize, float opacity, Renderer& renderer, ImageCache& images,
                     const Radii& borderRadii) {
    // CSS paints the first image layer on top, so the list is walked backwards.
    if (usesBackgroundBlending(style)) {
        paintIsolatedBackground(style, boxes, fontSize, rootFontSize, opacity, renderer,
                                images, borderRadii);
        return;
    }
    paintBackgroundColor(style, boxes, fontSize, rootFontSize, opacity, renderer,
                         borderRadii);
    for (std::size_t index = style.backgroundImages.size(); index-- > 0;) {
        paintBackgroundLayer(style, boxes, index, fontSize, rootFontSize, opacity, renderer,
                             images, borderRadii);
    }
}

// Blends the background stack in isolation: the colour goes down over
// transparency, then each image layer blends with what the lower layers left,
// and the finished stack composites over the page. The `background-blend-mode`
// list pairs with the image list and cycles short lists, the way the other
// background lists do. Each layer renders into a scratch target the size of
// the border box (shifted to the origin, so window-coordinate layers land in
// place), reads back, and blends on the CPU with the same compositor the
// mix-blend-mode groups use.
void paintIsolatedBackground(const ComputedStyle& style, const BackgroundBoxes& boxes,
                             float fontSize, float rootFontSize, float opacity,
                             Renderer& renderer, ImageCache& images,
                             const Radii& borderRadii) {
    const int width = static_cast<int>(std::ceil(boxes.border.w));
    const int height = static_cast<int>(std::ceil(boxes.border.h));
    if (width <= 0 || height <= 0) {
        return;
    }
    SDL_Renderer* sdl = renderer.sdl();
    SDL_Texture* previous = SDL_GetRenderTarget(sdl);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                          SDL_TEXTUREACCESS_TARGET);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
    SDL_Texture* scratch = SDL_CreateTextureWithProperties(sdl, props);
    SDL_DestroyProperties(props);
    if (!scratch) {
        SDL_SetRenderTarget(sdl, previous);
        paintBackgroundColor(style, boxes, fontSize, rootFontSize, opacity, renderer,
                             borderRadii);
        for (std::size_t index = style.backgroundImages.size(); index-- > 0;) {
            paintBackgroundLayer(style, boxes, index, fontSize, rootFontSize, opacity,
                                 renderer, images, borderRadii);
        }
        return;
    }
    // Layers paint in border-box coordinates, so every box shifts up-left by
    // the border origin; sizes stay identical, which is what the radii math
    // and the placement code resolve against.
    const BackgroundBoxes shifted{
        SDL_FRect{0.0f, 0.0f, boxes.border.w, boxes.border.h},
        SDL_FRect{boxes.padding.x - boxes.border.x, boxes.padding.y - boxes.border.y,
                  boxes.padding.w, boxes.padding.h},
        SDL_FRect{boxes.content.x - boxes.border.x, boxes.content.y - boxes.border.y,
                  boxes.content.w, boxes.content.h}};
    std::vector<std::uint8_t> accum(static_cast<std::size_t>(width) *
                                    static_cast<std::size_t>(height) * 4u,
                                    0);
    const auto blendOne = [&](int index, ComputedStyle::MixBlendMode mode) {
        SDL_SetRenderTarget(sdl, scratch);
        SDL_SetRenderDrawColor(sdl, 0, 0, 0, 0);
        SDL_RenderClear(sdl);
        if (index < 0) {
            paintBackgroundColor(style, shifted, fontSize, rootFontSize, opacity, renderer,
                                 borderRadii);
        } else {
            paintBackgroundLayer(style, shifted, static_cast<std::size_t>(index), fontSize,
                                 rootFontSize, opacity, renderer, images, borderRadii);
        }
        SDL_Surface* read = SDL_RenderReadPixels(sdl, nullptr);
        if (!read) {
            return;
        }
        SDL_Surface* conv = SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
        SDL_DestroySurface(read);
        if (!conv || conv->w != width || conv->h != height) {
            SDL_DestroySurface(conv);
            return;
        }
        blendImage(static_cast<std::uint8_t*>(conv->pixels), width, height, accum.data(),
                   mode);
        std::memcpy(accum.data(), conv->pixels, accum.size());
        SDL_DestroySurface(conv);
    };
    using Blend = ComputedStyle::MixBlendMode;
    blendOne(-1, Blend::Normal);
    for (std::size_t index = style.backgroundImages.size(); index-- > 0;) {
        if (index < style.backgroundImages.size() &&
            style.backgroundImages[index].kind !=
                ComputedStyle::BackgroundImage::None) {
            blendOne(static_cast<int>(index),
                     listAt(style.backgroundBlendModes, index));
        }
    }
    SDL_SetRenderTarget(sdl, previous);
    SDL_DestroyTexture(scratch);
    SDL_Texture* finished =
        SDL_CreateTexture(sdl, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width,
                          height);
    if (!finished) {
        return;
    }
    SDL_UpdateTexture(finished, nullptr, accum.data(), width * 4);
    SDL_SetTextureBlendMode(finished, SDL_BLENDMODE_BLEND);
    const SDL_FRect dst{boxes.border.x, boxes.border.y, static_cast<float>(width),
                        static_cast<float>(height)};
    SDL_RenderTexture(sdl, finished, nullptr, &dst);
    SDL_DestroyTexture(finished);
}

void applyMaskImage(std::uint8_t* pixels, int width, int height,
                    const ComputedStyle::MaskImage& mask, const SDL_FRect& destination,
                    float fontSize, float rootFontSize) {
    const BackgroundBoxes boxes{destination, destination, destination};
    applyMaskLayers(pixels, width, height, std::vector<ComputedStyle::MaskImage>{mask}, {},
                    {}, {}, {}, {}, {}, {}, boxes, fontSize, rootFontSize,
                    FetchMaskBytes{});
}

// Bilinear sample of decoded RGBA bytes at uv in [0,1]; edges clamp, so a
// stretched mask never reads out of bounds.
Color sampleBilinear(const DecodedImage& image, float u, float v) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
        return Color{0, 0, 0, 0};
    }
    const float fx = std::min(std::max(u, 0.0f), 1.0f) * (image.width - 1);
    const float fy = std::min(std::max(v, 0.0f), 1.0f) * (image.height - 1);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const float tx = fx - x0;
    const float ty = fy - y0;
    const auto& bytes = image.rgba;
    const int stride = image.width * 4;
    std::uint8_t out[4];
    for (int c = 0; c < 4; ++c) {
        const float a = bytes[y0 * stride + x0 * 4 + c];
        const float b = bytes[y0 * stride + x1 * 4 + c];
        const float cc = bytes[y1 * stride + x0 * 4 + c];
        const float d = bytes[y1 * stride + x1 * 4 + c];
        out[c] = static_cast<std::uint8_t>(
            std::lround(a + (b - a) * tx + (cc - a) * ty + (a - b - cc + d) * tx * ty));
    }
    return Color{out[0], out[1], out[2], out[3]};
}

// One mask layer's value at a surface point, 0..1: the painted colour read
// through its mode. `match-source` is alpha for gradients and luminance for
// urls, which is what makes `linear-gradient(black, transparent)` fade while
// photos mask by brightness.
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
                     FetchMaskBytes fetch) {
    using Mask = ComputedStyle::MaskImage;
    using Mode = ComputedStyle::MaskMode;
    using Composite = ComputedStyle::MaskComposite;
    if (!pixels || width <= 0 || height <= 0 || boxes.border.w <= 0.0f ||
        boxes.border.h <= 0.0f || layers.empty()) {
        return;
    }
    // Resolve each layer once: its sized/placed destination, clip box, tiling
    // axes, gradient shader, and decoded url bytes.
    struct ResolvedLayer {
        Shader shader;
        bool hasShader = false;
        const DecodedImage* image = nullptr;
        SDL_FRect dest{0.0f, 0.0f, 0.0f, 0.0f};
        SDL_FRect clip{0.0f, 0.0f, 0.0f, 0.0f};
        bool tileX = true;
        bool tileY = true;
    };
    std::vector<ResolvedLayer> resolved;
    resolved.reserve(layers.size());
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const auto& layer = layers[i];
        ResolvedLayer entry;
        const SDL_FRect originBox = boxOf(boxes, listAt(origins, i));
        entry.clip = boxOf(boxes, listAt(clips, i));
        const ComputedStyle::RepeatPair repeat = listAt(repeats, i);
        entry.tileX = repeat.x;
        entry.tileY = repeat.y;
        if (layer.kind == Mask::Kind::LinearGradient) {
            entry.dest =
                placeLayer(listAt(sizes, i), listAt(positions, i), originBox,
                           entry.clip.w, entry.clip.h, true, fontSize, rootFontSize);
            LinearGradient gradient;
            const float lineLength = gradientLine(layer.linear.angle, entry.dest, gradient);
            gradient.stops = toGradientStops(layer.linear.stops, lineLength, fontSize,
                                             rootFontSize, 1.0f);
            if (!gradient.stops.empty()) {
                entry.shader = Shader::linear(gradient);
                entry.hasShader = true;
            }
        } else if (layer.kind == Mask::Kind::RadialGradient) {
            entry.dest =
                placeLayer(listAt(sizes, i), listAt(positions, i), originBox,
                           entry.clip.w, entry.clip.h, true, fontSize, rootFontSize);
            RadialGradient gradient;
            radialRadii(layer.radial, entry.dest, fontSize, rootFontSize, gradient);
            gradient.stops = toGradientStops(
                layer.radial.stops, std::max(gradient.radiusX, gradient.radiusY), fontSize,
                rootFontSize, 1.0f);
            if (!gradient.stops.empty()) {
                entry.shader = Shader::radial(gradient);
                entry.hasShader = true;
            }
        } else if (layer.kind == Mask::Kind::ConicalGradient) {
            entry.dest =
                placeLayer(listAt(sizes, i), listAt(positions, i), originBox,
                           entry.clip.w, entry.clip.h, true, fontSize, rootFontSize);
            ConicalGradient gradient;
            gradient.cx =
                layer.conical.centreX.resolve(entry.dest.w, fontSize, rootFontSize) +
                entry.dest.x;
            gradient.cy =
                layer.conical.centreY.resolve(entry.dest.h, fontSize, rootFontSize) +
                entry.dest.y;
            gradient.fromAngle = layer.conical.angle;
            gradient.radius = std::max(entry.dest.w, entry.dest.h) * 0.5f;
            gradient.stops = toGradientStops(layer.conical.stops, 1.0f, fontSize,
                                             rootFontSize, 1.0f);
            if (!gradient.stops.empty()) {
                entry.shader = Shader::conical(gradient);
                entry.hasShader = true;
            }
        } else if (layer.kind == Mask::Kind::Url && fetch) {
            entry.image = fetch(layer.href);
            if (entry.image && entry.image->width > 0 && entry.image->height > 0) {
                entry.dest = placeLayer(
                    listAt(sizes, i), listAt(positions, i), originBox,
                    static_cast<float>(entry.image->width),
                    static_cast<float>(entry.image->height), false, fontSize, rootFontSize);
            }
        }
        resolved.push_back(std::move(entry));
    }
    const auto modeAt = [&](std::size_t i) {
        if (modes.empty()) {
            return Mode::MatchSource;
        }
        return modes[std::min(i, modes.size() - 1)];
    };
    const auto compositeAt = [&](std::size_t i) {
        if (composites.empty()) {
            return Composite::Add;
        }
        return composites[std::min(i, composites.size() - 1)];
    };
    // A layer sample point: wraps tiling axes into the destination and reports
    // whether a non-tiling axis lands outside it. Points outside the clip box
    // or a degenerate destination never reach here; the caller maps those to
    // transparent.
    const auto locate = [&](const ResolvedLayer& entry, float px, float py, float& qx,
                             float& qy) {
        qx = px;
        qy = py;
        if (!entry.tileX) {
            if (px < entry.dest.x || px >= entry.dest.x + entry.dest.w) {
                return false;
            }
        } else {
            qx = wrapAxis(px, entry.dest.x, entry.dest.w);
        }
        if (!entry.tileY) {
            if (py < entry.dest.y || py >= entry.dest.y + entry.dest.h) {
                return false;
            }
        } else {
            qy = wrapAxis(py, entry.dest.y, entry.dest.h);
        }
        return true;
    };
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const float px = static_cast<float>(col) + 0.5f;
            const float py = static_cast<float>(row) + 0.5f;
            float acc = 0.0f;
            for (std::size_t i = 0; i < layers.size(); ++i) {
                const auto& layer = layers[i];
                const auto& entry = resolved[i];
                // `none` is opaque (no masking); a gradient that failed to
                // resolve (empty stops) is also opaque so it stays quiet;
                // a url that failed to load is transparent, per masking rules.
                // Anything outside the clip box or the placed destination is
                // transparent too: an unsized layer simply does not reach it.
                float value = 1.0f;
                if (layer.kind == Mask::Kind::None) {
                    value = 1.0f;
                } else if (entry.hasShader) {
                    if (!insideBox(px, py, entry.clip) || entry.dest.w <= 0.0f ||
                        entry.dest.h <= 0.0f) {
                        value = 0.0f;
                    } else {
                        float qx = px, qy = py;
                        if (!locate(entry, px, py, qx, qy)) {
                            value = 0.0f;
                        } else {
                            const Color color = entry.shader.colorAt(qx, qy);
                            const Mode mode = modeAt(i);
                            if (mode == Mode::Alpha || mode == Mode::MatchSource) {
                                value = static_cast<float>(color.a) / 255.0f;
                            } else {
                                value = (0.2126f * color.r + 0.7152f * color.g +
                                         0.0722f * color.b) /
                                        255.0f;
                            }
                        }
                    }
                } else if (layer.kind == Mask::Kind::Url) {
                    if (!entry.image || entry.image->width <= 0 ||
                        entry.image->height <= 0 || entry.image->rgba.empty() ||
                        !insideBox(px, py, entry.clip) || entry.dest.w <= 0.0f ||
                        entry.dest.h <= 0.0f) {
                        value = 0.0f;
                    } else {
                        float qx = px, qy = py;
                        if (!locate(entry, px, py, qx, qy)) {
                            value = 0.0f;
                        } else {
                            const float u = (qx - entry.dest.x) / entry.dest.w;
                            const float v = (qy - entry.dest.y) / entry.dest.h;
                            const Color color = sampleBilinear(*entry.image, u, v);
                            const Mode mode = modeAt(i);
                            if (mode == Mode::Alpha) {
                                value = static_cast<float>(color.a) / 255.0f;
                            } else {
                                // `match-source` is luminance for urls, so photos
                                // mask by brightness.
                                value = (0.2126f * color.r + 0.7152f * color.g +
                                         0.0722f * color.b) /
                                        255.0f;
                            }
                        }
                    }
                } else {
                    // Unresolvable gradient: opaque so a bad layer stays quiet.
                    value = 1.0f;
                }
                if (i == 0) {
                    acc = value;
                    continue;
                }
                switch (compositeAt(i)) {
                    case Composite::Add:
                        acc = acc + value - acc * value;
                        break;
                    case Composite::Subtract:
                        acc = std::max(0.0f, acc - value);
                        break;
                    case Composite::Intersect:
                        acc = acc * value;
                        break;
                    case Composite::Exclude:
                        acc = acc + value - 2.0f * acc * value;
                        break;
                }
            }
            std::uint8_t* alpha = pixels + 4 * (row * width + col) + 3;
            *alpha = static_cast<std::uint8_t>(static_cast<float>(*alpha) * acc + 0.5f);
        }
    }
}

void applyMaskBorder(std::uint8_t* pixels, int width, int height,
                     const ComputedStyle::MaskBorder& border,
                     const BackgroundBoxes& boxes, float fontSize, float rootFontSize,
                     FetchMaskBytes fetch) {
    using Mask = ComputedStyle::MaskImage;
    using BorderRepeat = ComputedStyle::MaskBorder::Repeat;
    using BorderMode = ComputedStyle::MaskBorder::Mode;
    if (!pixels || width <= 0 || height <= 0) {
        return;
    }
    if (border.source.kind == Mask::Kind::None) {
        return;
    }
    // Outsets grow the border box into the area the border paints.
    const auto outsetPx = [&](const Length& l, float thickness) {
        if (l.unit == Length::Number) {
            return std::max(0.0f, l.value * thickness);
        }
        return std::max(0.0f, l.resolve(0.0f, fontSize, rootFontSize));
    };
    const float thickTop = std::max(0.0f, boxes.padding.y - boxes.border.y);
    const float thickLeft = std::max(0.0f, boxes.padding.x - boxes.border.x);
    const float thickBottom = std::max(
        0.0f, boxes.border.y + boxes.border.h - (boxes.padding.y + boxes.padding.h));
    const float thickRight = std::max(
        0.0f, boxes.border.x + boxes.border.w - (boxes.padding.x + boxes.padding.w));
    const SDL_FRect area{
        boxes.border.x - outsetPx(border.outset.left, thickLeft),
        boxes.border.y - outsetPx(border.outset.top, thickTop),
        boxes.border.w + outsetPx(border.outset.left, thickLeft) +
            outsetPx(border.outset.right, thickRight),
        boxes.border.h + outsetPx(border.outset.top, thickTop) +
            outsetPx(border.outset.bottom, thickBottom)};
    if (area.w <= 0.0f || area.h <= 0.0f) {
        return;
    }

    // Source raster: decoded bytes, or a gradient rendered over the area.
    DecodedImage raster;
    const DecodedImage* src = nullptr;
    if (border.source.kind == Mask::Kind::Url) {
        if (fetch) {
            src = fetch(border.source.href);
        }
        if (!src || src->width <= 0 || src->height <= 0 || src->rgba.empty()) {
            // A missing source hides everything, like a missing mask-image.
            for (int i = 0; i < width * height; ++i) {
                pixels[4 * i + 3] = 0;
            }
            return;
        }
    } else {
        const int rasterW = std::max(1, static_cast<int>(std::lround(area.w)));
        const int rasterH = std::max(1, static_cast<int>(std::lround(area.h)));
        const SDL_FRect rasterBox{0.0f, 0.0f, static_cast<float>(rasterW),
                                  static_cast<float>(rasterH)};
        Shader shader = Shader::solid(Color{0, 0, 0, 0});
        bool ready = false;
        if (border.source.kind == Mask::Kind::LinearGradient) {
            LinearGradient gradient;
            const float lineLength =
                gradientLine(border.source.linear.angle, rasterBox, gradient);
            gradient.stops = toGradientStops(border.source.linear.stops, lineLength,
                                             fontSize, rootFontSize, 1.0f);
            if (!gradient.stops.empty()) {
                shader = Shader::linear(gradient);
                ready = true;
            }
        } else if (border.source.kind == Mask::Kind::RadialGradient) {
            RadialGradient gradient;
            radialRadii(border.source.radial, rasterBox, fontSize, rootFontSize, gradient);
            gradient.stops = toGradientStops(
                border.source.radial.stops,
                std::max(gradient.radiusX, gradient.radiusY), fontSize, rootFontSize, 1.0f);
            if (!gradient.stops.empty()) {
                shader = Shader::radial(gradient);
                ready = true;
            }
        } else if (border.source.kind == Mask::Kind::ConicalGradient) {
            ConicalGradient gradient;
            gradient.cx = border.source.conical.centreX.resolve(
                              rasterBox.w, fontSize, rootFontSize);
            gradient.cy = border.source.conical.centreY.resolve(
                              rasterBox.h, fontSize, rootFontSize);
            gradient.fromAngle = border.source.conical.angle;
            gradient.radius = std::max(rasterBox.w, rasterBox.h) * 0.5f;
            gradient.stops = toGradientStops(border.source.conical.stops, 1.0f,
                                             fontSize, rootFontSize, 1.0f);
            if (!gradient.stops.empty()) {
                shader = Shader::conical(gradient);
                ready = true;
            }
        }
        if (!ready) {
            // An unresolvable gradient stays quiet.
            return;
        }
        raster.width = rasterW;
        raster.height = rasterH;
        raster.rgba.resize(static_cast<std::size_t>(rasterW) *
                           static_cast<std::size_t>(rasterH) * 4u);
        for (int y = 0; y < rasterH; ++y) {
            for (int x = 0; x < rasterW; ++x) {
                const Color color =
                    shader.colorAt(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                std::uint8_t* texel =
                    raster.rgba.data() + 4 * (y * rasterW + x);
                texel[0] = color.r;
                texel[1] = color.g;
                texel[2] = color.b;
                texel[3] = color.a;
            }
        }
        src = &raster;
    }
    const float srcW = static_cast<float>(src->width);
    const float srcH = static_cast<float>(src->height);

    // Slice insets in source px, clamped and overlap-scaled like a border image.
    const auto slicePx = [&](const Length& l, float dim) {
        float v = 0.0f;
        if (l.unit == Length::Percent) {
            v = l.value * 0.01f * dim;
        } else if (l.unit == Length::Number || l.unit == Length::Px) {
            v = l.value;
        }
        return std::min(std::max(v, 0.0f), dim);
    };
    float sliceTop = slicePx(border.slice.top, srcH);
    float sliceRight = slicePx(border.slice.right, srcW);
    float sliceBottom = slicePx(border.slice.bottom, srcH);
    float sliceLeft = slicePx(border.slice.left, srcW);
    if (sliceLeft + sliceRight > srcW && sliceLeft + sliceRight > 0.0f) {
        const float scale = srcW / (sliceLeft + sliceRight);
        sliceLeft *= scale;
        sliceRight *= scale;
    }
    if (sliceTop + sliceBottom > srcH && sliceTop + sliceBottom > 0.0f) {
        const float scale = srcH / (sliceTop + sliceBottom);
        sliceTop *= scale;
        sliceBottom *= scale;
    }

    // Band thicknesses: auto is the slice's own size, numbers multiply the
    // border thickness, lengths resolve.
    const auto widthPx = [&](const Length& l, float slice, float thickness) {
        if (l.isAuto()) {
            return slice;
        }
        if (l.unit == Length::Number) {
            return std::max(0.0f, l.value * thickness);
        }
        return std::max(0.0f, l.resolve(0.0f, fontSize, rootFontSize));
    };
    const float bandTop = widthPx(border.width.top, sliceTop, thickTop);
    const float bandRight = widthPx(border.width.right, sliceRight, thickRight);
    const float bandBottom = widthPx(border.width.bottom, sliceBottom, thickBottom);
    const float bandLeft = widthPx(border.width.left, sliceLeft, thickLeft);

    const float x0 = area.x;
    const float x1 = area.x + bandLeft;
    const float x2 = area.x + area.w - bandRight;
    const float x3 = area.x + area.w;
    const float y0 = area.y;
    const float y1 = area.y + bandTop;
    const float y2 = area.y + area.h - bandBottom;
    const float y3 = area.y + area.h;
    // Source regions in source px: corners, edges, middle.
    const float sx0 = 0.0f;
    const float sx1 = sliceLeft;
    const float sx2 = srcW - sliceRight;
    const float sx3 = srcW;
    const float sy0 = 0.0f;
    const float sy1 = sliceTop;
    const float sy2 = srcH - sliceBottom;
    const float sy3 = srcH;

    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const float px = static_cast<float>(col) + 0.5f;
            const float py = static_cast<float>(row) + 0.5f;
            if (!insideBox(px, py, area)) {
                continue;
            }
            const int zoneX = px < x1 ? 0 : (px < x2 ? 1 : 2);
            const int zoneY = py < y1 ? 0 : (py < y2 ? 1 : 2);
            if (zoneX == 1 && zoneY == 1 && !border.slice.fill) {
                // An unfilled middle is transparent.
                pixels[4 * (row * width + col) + 3] = 0;
                continue;
            }
            // Destination rect, source region, and per-axis tiling for the zone.
            SDL_FRect dst{0.0f, 0.0f, 0.0f, 0.0f};
            SDL_FRect region{0.0f, 0.0f, 0.0f, 0.0f};
            BorderRepeat modeX = BorderRepeat::Stretch;
            BorderRepeat modeY = BorderRepeat::Stretch;
            float tileX = 0.0f;
            float tileY = 0.0f;
            if (zoneX == 0) {
                dst.x = x0;
                dst.w = x1 - x0;
                region.x = sx0;
                region.w = sx1 - sx0;
            } else if (zoneX == 1) {
                dst.x = x1;
                dst.w = x2 - x1;
                region.x = sx1;
                region.w = sx2 - sx1;
                modeX = border.repeatX;
            } else {
                dst.x = x2;
                dst.w = x3 - x2;
                region.x = sx2;
                region.w = sx3 - sx2;
            }
            if (zoneY == 0) {
                dst.y = y0;
                dst.h = y1 - y0;
                region.y = sy0;
                region.h = sy1 - sy0;
            } else if (zoneY == 1) {
                dst.y = y1;
                dst.h = y2 - y1;
                region.y = sy1;
                region.h = sy2 - sy1;
                modeY = border.repeatY;
            } else {
                dst.y = y2;
                dst.h = y3 - y2;
                region.y = sy2;
                region.h = sy3 - sy2;
            }
            // Edge tiles scale so their cross axis matches the band, then tile
            // along it; the middle tiles at source scale.
            if (zoneX == 1 && zoneY != 1) {
                const float bandH = dst.h;
                const float srcHRegion = region.h;
                tileX = srcHRegion > 0.0f ? region.w * (bandH / srcHRegion) : 0.0f;
            } else if (zoneY == 1 && zoneX != 1) {
                const float bandW = dst.w;
                const float srcWRegion = region.w;
                tileY = srcWRegion > 0.0f ? region.h * (bandW / srcWRegion) : 0.0f;
            } else if (zoneX == 1 && zoneY == 1) {
                tileX = region.w;
                tileY = region.h;
            }
            float qx = 0.0f, qy = 0.0f;
            if (!mapBorderAxis(px, dst.x, dst.w, region.w, tileX, modeX, qx) ||
                !mapBorderAxis(py, dst.y, dst.h, region.h, tileY, modeY, qy)) {
                // A `space` gap is transparent.
                pixels[4 * (row * width + col) + 3] = 0;
                continue;
            }
            const float u = (region.x + qx) / srcW;
            const float v = (region.y + qy) / srcH;
            const Color color = sampleBilinear(*src, u, v);
            float value = 0.0f;
            if (border.mode == BorderMode::Alpha) {
                value = static_cast<float>(color.a) / 255.0f;
            } else {
                value = (0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b) /
                        255.0f;
            }
            std::uint8_t* alpha = pixels + 4 * (row * width + col) + 3;
            *alpha = static_cast<std::uint8_t>(static_cast<float>(*alpha) * value + 0.5f);
        }
    }
}

// The outset area a border image paints: the border box grown per side. Shared
// by the tile builder and the gradient raster so both agree on the extent.
SDL_FRect borderImageArea(const ComputedStyle::BorderImage& image,
                          const BackgroundBoxes& boxes, float fontSize, float rootFontSize) {
    const auto outsetPx = [&](const Length& l, float thickness) {
        if (l.unit == Length::Number) {
            return std::max(0.0f, l.value * thickness);
        }
        return std::max(0.0f, l.resolve(0.0f, fontSize, rootFontSize));
    };
    const float thickTop = std::max(0.0f, boxes.padding.y - boxes.border.y);
    const float thickLeft = std::max(0.0f, boxes.padding.x - boxes.border.x);
    const float thickBottom = std::max(
        0.0f, boxes.border.y + boxes.border.h - (boxes.padding.y + boxes.padding.h));
    const float thickRight = std::max(
        0.0f, boxes.border.x + boxes.border.w - (boxes.padding.x + boxes.padding.w));
    const float top = outsetPx(image.outset.top, thickTop);
    const float right = outsetPx(image.outset.right, thickRight);
    const float bottom = outsetPx(image.outset.bottom, thickBottom);
    const float left = outsetPx(image.outset.left, thickLeft);
    return SDL_FRect{boxes.border.x - left, boxes.border.y - top,
                     boxes.border.w + left + right, boxes.border.h + top + bottom};
}

// One axis of tile spans: (dst start, dst length, src start, src length).
struct TileSpan {
    float dst = 0.0f;
    float dstLen = 0.0f;
    float src = 0.0f;
    float srcLen = 0.0f;
};

std::vector<TileSpan> tileSpans(float dstLo, float dstLen, float srcLo, float srcLen,
                                float tileDst, ComputedStyle::BorderImage::Repeat mode) {
    using Repeat = ComputedStyle::BorderImage::Repeat;
    std::vector<TileSpan> spans;
    if (dstLen <= 0.0f || srcLen <= 0.0f) {
        return spans;
    }
    if (mode == Repeat::Stretch || tileDst <= 0.0f) {
        spans.push_back({dstLo, dstLen, srcLo, srcLen});
        return spans;
    }
    float step = tileDst;
    if (mode == Repeat::Round) {
        const int n = std::max(1, static_cast<int>(std::lround(dstLen / tileDst)));
        step = dstLen / static_cast<float>(n);
    }
    if (mode == Repeat::Space) {
        const int n = static_cast<int>(dstLen / tileDst);
        if (n >= 1) {
            // Whole tiles with even gaps; the gaps simply emit no tile.
            const float slot = dstLen / static_cast<float>(n);
            const float off = (slot - tileDst) * 0.5f;
            for (int i = 0; i < n; ++i) {
                spans.push_back({dstLo + i * slot + off, tileDst, srcLo, srcLen});
            }
            return spans;
        }
        // Smaller than one tile: a single clipped tile, like repeat.
    }
    const int n = std::max(1, static_cast<int>(std::ceil(dstLen / step)));
    for (int i = 0; i < n; ++i) {
        const float start = i * step;
        const float kept = std::min(step, dstLen - start);
        if (kept <= 0.0f) {
            break;
        }
        spans.push_back({dstLo + start, kept, srcLo + start / step * srcLen,
                         kept / step * srcLen});
    }
    return spans;
}

std::vector<BorderTile> borderImageTiles(const ComputedStyle::BorderImage& image,
                                         float srcW, float srcH,
                                         const BackgroundBoxes& boxes, float fontSize,
                                         float rootFontSize) {
    using Repeat = ComputedStyle::BorderImage::Repeat;
    std::vector<BorderTile> tiles;
    if (image.source.kind == ComputedStyle::BackgroundImage::None || srcW <= 0.0f ||
        srcH <= 0.0f) {
        return tiles;
    }
    // Slice insets in source px, clamped and overlap-scaled.
    const auto slicePx = [&](const Length& l, float dim) {
        float v = 0.0f;
        if (l.unit == Length::Percent) {
            v = l.value * 0.01f * dim;
        } else if (l.unit == Length::Number || l.unit == Length::Px) {
            v = l.value;
        }
        return std::min(std::max(v, 0.0f), dim);
    };
    float sliceTop = slicePx(image.slice.top, srcH);
    float sliceRight = slicePx(image.slice.right, srcW);
    float sliceBottom = slicePx(image.slice.bottom, srcH);
    float sliceLeft = slicePx(image.slice.left, srcW);
    if (sliceLeft + sliceRight > srcW && sliceLeft + sliceRight > 0.0f) {
        const float scale = srcW / (sliceLeft + sliceRight);
        sliceLeft *= scale;
        sliceRight *= scale;
    }
    if (sliceTop + sliceBottom > srcH && sliceTop + sliceBottom > 0.0f) {
        const float scale = srcH / (sliceTop + sliceBottom);
        sliceTop *= scale;
        sliceBottom *= scale;
    }
    // Band thicknesses: auto is the slice's own size, numbers multiply the
    // border thickness, lengths resolve.
    const float thickTop = std::max(0.0f, boxes.padding.y - boxes.border.y);
    const float thickLeft = std::max(0.0f, boxes.padding.x - boxes.border.x);
    const float thickBottom = std::max(
        0.0f, boxes.border.y + boxes.border.h - (boxes.padding.y + boxes.padding.h));
    const float thickRight = std::max(
        0.0f, boxes.border.x + boxes.border.w - (boxes.padding.x + boxes.padding.w));
    const auto widthPx = [&](const Length& l, float slice, float thickness) {
        if (l.isAuto()) {
            return slice;
        }
        if (l.unit == Length::Number) {
            return std::max(0.0f, l.value * thickness);
        }
        return std::max(0.0f, l.resolve(0.0f, fontSize, rootFontSize));
    };
    const float bandTop = widthPx(image.width.top, sliceTop, thickTop);
    const float bandRight = widthPx(image.width.right, sliceRight, thickRight);
    const float bandBottom = widthPx(image.width.bottom, sliceBottom, thickBottom);
    const float bandLeft = widthPx(image.width.left, sliceLeft, thickLeft);
    const SDL_FRect area = borderImageArea(image, boxes, fontSize, rootFontSize);
    if (area.w <= 0.0f || area.h <= 0.0f) {
        return tiles;
    }
    const float x0 = area.x;
    const float x1 = area.x + bandLeft;
    const float x2 = area.x + area.w - bandRight;
    const float x3 = area.x + area.w;
    const float y0 = area.y;
    const float y1 = area.y + bandTop;
    const float y2 = area.y + area.h - bandBottom;
    const float y3 = area.y + area.h;
    const float sx0 = 0.0f;
    const float sx1 = sliceLeft;
    const float sx2 = srcW - sliceRight;
    const float sx3 = srcW;
    const float sy0 = 0.0f;
    const float sy1 = sliceTop;
    const float sy2 = srcH - sliceBottom;
    const float sy3 = srcH;
    // One zone: a dst rect, a source region, per-axis tiling, and each tile's
    // dst length in source scale.
    const auto zone = [&](float dx, float dw, float sx, float sw, float dy, float dh,
                          float sy, float sh, Repeat modeX, Repeat modeY, float tileX,
                          float tileY) {
        for (const TileSpan& spanX : tileSpans(dx, dw, sx, sw, tileX, modeX)) {
            for (const TileSpan& spanY : tileSpans(dy, dh, sy, sh, tileY, modeY)) {
                tiles.push_back(BorderTile{SDL_FRect{spanX.src, spanY.src, spanX.srcLen,
                                                     spanY.srcLen},
                                           SDL_FRect{spanX.dst, spanY.dst, spanX.dstLen,
                                                     spanY.dstLen}});
            }
        }
    };
    const float midX = x2 - x1, midY = y2 - y1;
    const float midSX = sx2 - sx1, midSY = sy2 - sy1;
    // Corners stretch.
    zone(x0, x1 - x0, sx0, sx1 - sx0, y0, y1 - y0, sy0, sy1 - sy0, Repeat::Stretch,
         Repeat::Stretch, 0.0f, 0.0f);
    zone(x2, x3 - x2, sx2, sx3 - sx2, y0, y1 - y0, sy0, sy1 - sy0, Repeat::Stretch,
         Repeat::Stretch, 0.0f, 0.0f);
    zone(x0, x1 - x0, sx0, sx1 - sx0, y2, y3 - y2, sy2, sy3 - sy2, Repeat::Stretch,
         Repeat::Stretch, 0.0f, 0.0f);
    zone(x2, x3 - x2, sx2, sx3 - sx2, y2, y3 - y2, sy2, sy3 - sy2, Repeat::Stretch,
         Repeat::Stretch, 0.0f, 0.0f);
    // Edges stretch across and tile along, scaled so the cross axis matches.
    const float topTile = sy1 - sy0 > 0.0f ? midSX * ((y1 - y0) / (sy1 - sy0)) : 0.0f;
    const float bottomTile =
        sy3 - sy2 > 0.0f ? midSX * ((y3 - y2) / (sy3 - sy2)) : 0.0f;
    const float leftTile = sx1 - sx0 > 0.0f ? midSY * ((x1 - x0) / (sx1 - sx0)) : 0.0f;
    const float rightTile =
        sx3 - sx2 > 0.0f ? midSY * ((x3 - x2) / (sx3 - sx2)) : 0.0f;
    zone(x1, midX, sx1, midSX, y0, y1 - y0, sy0, sy1 - sy0, image.repeatX,
         Repeat::Stretch, topTile, 0.0f);
    zone(x1, midX, sx1, midSX, y2, y3 - y2, sy2, sy3 - sy2, image.repeatX,
         Repeat::Stretch, bottomTile, 0.0f);
    zone(x0, x1 - x0, sx0, sx1 - sx0, y1, midY, sy1, midSY, Repeat::Stretch,
         image.repeatY, 0.0f, leftTile);
    zone(x2, x3 - x2, sx2, sx3 - sx2, y1, midY, sy1, midSY, Repeat::Stretch,
         image.repeatY, 0.0f, rightTile);
    // The middle tiles both ways, but only with `fill`.
    if (image.slice.fill) {
        zone(x1, midX, sx1, midSX, y1, midY, sy1, midSY, image.repeatX, image.repeatY,
             midSX, midSY);
    }
    return tiles;
}

void paintBorderImage(const ComputedStyle::BorderImage& image,
                      const BackgroundBoxes& boxes, float fontSize, float rootFontSize,
                      float opacity, Renderer& renderer, ImageCache& images) {
    using Kind = ComputedStyle::BackgroundImage::Kind;
    if (image.source.kind == Kind::None) {
        return;
    }
    const Uint8 tint =
        static_cast<Uint8>(std::lround(std::min(std::max(opacity, 0.0f), 1.0f) * 255.0f));
    if (image.source.kind == Kind::Url) {
        SDL_Texture* texture = images.texture(renderer.sdl(), image.source.href);
        if (!texture) {
            return;
        }
        float srcW = 0.0f, srcH = 0.0f;
        SDL_GetTextureSize(texture, &srcW, &srcH);
        if (srcW <= 0.0f || srcH <= 0.0f) {
            return;
        }
        for (const BorderTile& tile :
             borderImageTiles(image, srcW, srcH, boxes, fontSize, rootFontSize)) {
            renderer.fillRect(tile.dst, Shader::image(texture, tile.src, tile.dst,
                                                      Repeat::No,
                                                      Color{255, 255, 255, tint}));
        }
        return;
    }
    // A gradient renders once over the outset area, then slices and tiles like
    // an image.
    const SDL_FRect area = borderImageArea(image, boxes, fontSize, rootFontSize);
    const int rasterW = std::max(1, static_cast<int>(std::lround(area.w)));
    const int rasterH = std::max(1, static_cast<int>(std::lround(area.h)));
    const SDL_FRect rasterBox{0.0f, 0.0f, static_cast<float>(rasterW),
                              static_cast<float>(rasterH)};
    Shader shader = Shader::solid(Color{0, 0, 0, 0});
    bool ready = false;
    if (image.source.kind == Kind::LinearGradientImage) {
        LinearGradient gradient;
        const float lineLength =
            gradientLine(image.source.linear.angle, rasterBox, gradient);
        gradient.stops = toGradientStops(image.source.linear.stops, lineLength,
                                         fontSize, rootFontSize, 1.0f);
        if (!gradient.stops.empty()) {
            shader = Shader::linear(gradient);
            ready = true;
        }
    } else if (image.source.kind == Kind::RadialGradientImage) {
        RadialGradient gradient;
        radialRadii(image.source.radial, rasterBox, fontSize, rootFontSize, gradient);
        gradient.stops = toGradientStops(
            image.source.radial.stops, std::max(gradient.radiusX, gradient.radiusY),
            fontSize, rootFontSize, 1.0f);
        if (!gradient.stops.empty()) {
            shader = Shader::radial(gradient);
            ready = true;
        }
    } else if (image.source.kind == Kind::ConicalGradientImage) {
        ConicalGradient gradient;
        gradient.cx = image.source.conical.centreX.resolve(rasterBox.w, fontSize,
                                                           rootFontSize);
        gradient.cy = image.source.conical.centreY.resolve(rasterBox.h, fontSize,
                                                           rootFontSize);
        gradient.fromAngle = image.source.conical.angle;
        gradient.radius = std::max(rasterBox.w, rasterBox.h) * 0.5f;
        gradient.stops = toGradientStops(image.source.conical.stops, 1.0f, fontSize,
                                         rootFontSize, 1.0f);
        if (!gradient.stops.empty()) {
            shader = Shader::conical(gradient);
            ready = true;
        }
    }
    if (!ready) {
        return;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(rasterW) *
                                    static_cast<std::size_t>(rasterH) * 4u);
    for (int y = 0; y < rasterH; ++y) {
        for (int x = 0; x < rasterW; ++x) {
            const Color color = shader.colorAt(static_cast<float>(x) + 0.5f,
                                               static_cast<float>(y) + 0.5f);
            std::uint8_t* texel = bytes.data() + 4 * (y * rasterW + x);
            texel[0] = color.r;
            texel[1] = color.g;
            texel[2] = color.b;
            texel[3] = color.a;
        }
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        rasterW, rasterH, SDL_PIXELFORMAT_RGBA32, bytes.data(), rasterW * 4);
    if (!surface) {
        return;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer.sdl(), surface);
    SDL_DestroySurface(surface);
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    // Tile sources address the raster from its own origin; destinations
    // address the window.
    for (const BorderTile& tile : borderImageTiles(
             image, static_cast<float>(rasterW), static_cast<float>(rasterH), boxes,
             fontSize, rootFontSize)) {
        renderer.fillRect(tile.dst, Shader::image(texture, tile.src, tile.dst, Repeat::No,
                                                  Color{255, 255, 255, tint}));
    }
    SDL_DestroyTexture(texture);
}

}  // namespace sui

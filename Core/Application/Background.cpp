#include "Core/Application/Background.h"

#include <algorithm>
#include <cmath>

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

}  // namespace

void paintBackground(const ComputedStyle& style, const BackgroundBoxes& boxes, float fontSize,
                     float rootFontSize, float opacity, Renderer& renderer, ImageCache& images,
                     const Radii& borderRadii) {
    // The colour is the bottom layer, so it goes down first across whichever box the
    // colour is clipped to. CSS puts `background-clip` on a layer; the colour has no
    // layer of its own, so it takes the first layer's clip, defaulting to the border box.
    const ComputedStyle::BackgroundBox colourClip =
        style.backgroundClips.empty() ? ComputedStyle::BackgroundBorderBox
                                     : style.backgroundClips.front();
    const SDL_FRect colourBox = boxOf(boxes, colourClip);
    const Color colour = multiplyAlpha(toColor(style.backgroundColor), opacity);
    if (colour.a > 0 && colourBox.w > 0.0f && colourBox.h > 0.0f) {
        renderer.fillRect(colourBox, Shader::solid(colour),
                          radiiForBox(borderRadii, boxes.border, colourBox));
    }

    // CSS paints the first image layer on top, so the list is walked backwards.
    for (std::size_t index = style.backgroundImages.size(); index-- > 0;) {
        const ComputedStyle::BackgroundImage& layer = style.backgroundImages[index];
        if (layer.kind == ComputedStyle::BackgroundImage::None) {
            continue;
        }
        const SDL_FRect clipBox = boxOf(boxes, listAt(style.backgroundClips, index));
        if (clipBox.w <= 0.0f || clipBox.h <= 0.0f) {
            continue;
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
                    continue;
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
                    continue;
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
                    continue;
                }
                shader = Shader::linear(gradient);
                SDL_FRect visible;
                if (!coveredArea(clipBox, destination, repeatPair.x, repeatPair.y, visible)) {
                    continue;
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
                    continue;
                }
                shader = Shader::radial(gradient);
                SDL_FRect visible;
                if (!coveredArea(clipBox, destination, repeatPair.x, repeatPair.y, visible)) {
                    continue;
                }
                paintBox = visible;
                break;
            }
            case ComputedStyle::BackgroundImage::None:
                continue;
        }

        const Radii paintRadii = paintBox.w == clipBox.w && paintBox.h == clipBox.h
                                     ? radii
                                     : radiiClippedTo(radii, clipBox, paintBox);
        renderer.fillRect(paintBox, shader, paintRadii);
    }
}

}  // namespace sui

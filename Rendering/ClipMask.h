// CPU clip-path masking for the frame executor. A clipped element composites
// through an offscreen surface (like a blur group); once its members have
// painted, the surface alpha is multiplied by the shape coverage and the
// existing filter pipeline runs on the masked result. Shapes resolve to
// surface pixels before rasterising, so this file does pure geometry: no
// styles, no elements, no device.
#pragma once

#include <SDL3/SDL_rect.h>

#include <cstdint>
#include <vector>

namespace sui {

// A `clip-path` shape resolved against its reference box, in surface pixels.
// Everything is already in px: percentages resolved against the box, font
// units against the font sizes.
struct ResolvedClip {
    enum class Kind : std::uint8_t { None, Rect, Circle, Ellipse, Polygon };

    Kind kind = Kind::None;
    // Rect: the box; `radius` rounds every corner.
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float radius = 0.0f;
    // Circle: `rx` equals `ry` equals `radius`. Ellipse: both radii live here.
    float cx = 0.0f;
    float cy = 0.0f;
    float rx = 0.0f;
    float ry = 0.0f;
    // Polygon: vertices in order, plus its fill rule.
    std::vector<SDL_FPoint> points;
    bool evenOdd = false;
};

// How much of the pixel centred at (x, y) the shape keeps, from 0 to 1. Edges
// cover half a pixel on each side, so diagonal and curved boundaries
// antialias instead of staircasing.
float clipCoverage(const ResolvedClip& shape, float x, float y);

// Whether the point reads as inside: coverage at least half.
inline bool clipContains(const ResolvedClip& shape, float x, float y) {
    return clipCoverage(shape, x, y) >= 0.5f;
}

// Shifts a resolved shape, for rebasing window coordinates into a surface.
inline void offsetResolvedClip(ResolvedClip& shape, float dx, float dy) {
    shape.x += dx;
    shape.y += dy;
    shape.cx += dx;
    shape.cy += dy;
    for (SDL_FPoint& point : shape.points) {
        point.x += dx;
        point.y += dy;
    }
}

// Multiplies the alpha of every ABGR8888 pixel by its coverage, leaving colour
// alone. A `None` shape is a no-op.
void applyClipMask(std::uint8_t* pixels, int width, int height,
                   const ResolvedClip& shape);

// Masks a group surface with a window-space shape: rebases the shape into the
// surface (whose origin sits at `boundsX`, `boundsY`) and applies it. This is
// the exact step the frame executor runs on a clipped group's readback.
inline void applyClipToSurface(std::uint8_t* pixels, int width, int height,
                               const ResolvedClip& shape, float boundsX, float boundsY) {
    ResolvedClip local = shape;
    offsetResolvedClip(local, -boundsX, -boundsY);
    applyClipMask(pixels, width, height, local);
}

}  // namespace sui

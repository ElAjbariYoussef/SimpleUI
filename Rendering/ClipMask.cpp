#include "Rendering/ClipMask.h"

#include <cmath>
#include <cstddef>

namespace sui {
namespace {

// Signed distance to an axis-aligned box centred at the origin with half-size
// `b` and corner radius `r`: negative inside, positive outside, zero on the
// edge. With `r` at zero this is the sharp box.
float sdRoundBox(float x, float y, float bx, float by, float r) {
    const float qx = std::fabs(x) - bx + r;
    const float qy = std::fabs(y) - by + r;
    const float ax = qx > 0.0f ? qx : 0.0f;
    const float ay = qy > 0.0f ? qy : 0.0f;
    const float outer = qx > qy ? qx : qy;
    return std::sqrt(ax * ax + ay * ay) + (outer < 0.0f ? outer : 0.0f) - r;
}

// Signed distance to a segment, for polygon edges.
float sdSegment(float px, float py, float ax, float ay, float bx, float by,
                float& nearest) {
    const float vx = bx - ax;
    const float vy = by - ay;
    const float wx = px - ax;
    const float wy = py - ay;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float dx = wx - t * vx;
    const float dy = wy - t * vy;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < nearest) {
        nearest = dist;
    }
    // Signed area side: which half-plane the point falls on.
    return vx * wy - vy * wx;
}

// Coverage from a signed distance: inside (negative) keeps everything within
// half a pixel of the edge, outside keeps nothing.
float coverageFromDistance(float dist) {
    float cover = 0.5f - dist;
    cover = cover < 0.0f ? 0.0f : (cover > 1.0f ? 1.0f : cover);
    return cover;
}

float rectCoverage(const ResolvedClip& shape, float x, float y) {
    const float hw = shape.w * 0.5f;
    const float hh = shape.h * 0.5f;
    float r = shape.radius;
    r = r < 0.0f ? 0.0f : (r > hw ? hw : (r > hh ? hh : r));
    return coverageFromDistance(
        sdRoundBox(x - (shape.x + hw), y - (shape.y + hh), hw, hh, r));
}

float circleCoverage(const ResolvedClip& shape, float x, float y) {
    const float dx = x - shape.cx;
    const float dy = y - shape.cy;
    return coverageFromDistance(std::sqrt(dx * dx + dy * dy) - shape.rx);
}

float ellipseCoverage(const ResolvedClip& shape, float x, float y) {
    if (shape.rx <= 0.0f || shape.ry <= 0.0f) {
        return 0.0f;
    }
    // Normalized distance and its gradient give a first-order signed distance.
    const float nx = (x - shape.cx) / shape.rx;
    const float ny = (y - shape.cy) / shape.ry;
    const float k0 = std::sqrt(nx * nx + ny * ny);
    if (k0 == 0.0f) {
        return 1.0f;
    }
    const float gx = nx / (shape.rx * k0);
    const float gy = ny / (shape.ry * k0);
    const float grad = std::sqrt(gx * gx + gy * gy);
    if (grad == 0.0f) {
        return k0 < 1.0f ? 1.0f : 0.0f;
    }
    return coverageFromDistance((k0 - 1.0f) / grad);
}

float polygonCoverage(const ResolvedClip& shape, float x, float y) {
    const std::size_t n = shape.points.size();
    if (n < 3) {
        return 0.0f;
    }
    // Even-odd counts crossings; nonzero sums winding. Either way the closest
    // edge distances the antialiased boundary.
    int winding = 0;
    float nearest = 1.0e30f;
    for (std::size_t i = 0; i < n; ++i) {
        const SDL_FPoint& a = shape.points[i];
        const SDL_FPoint& b = shape.points[(i + 1) % n];
        sdSegment(x, y, a.x, a.y, b.x, b.y, nearest);
        // A crossing counts when the edge straddles the horizontal ray right
        // of the point.
        if ((a.y > y) != (b.y > y)) {
            const float t = (y - a.y) / (b.y - a.y);
            if (a.x + t * (b.x - a.x) > x) {
                if (shape.evenOdd) {
                    winding ^= 1;
                } else {
                    winding += (b.y > a.y) ? 1 : -1;
                }
            }
        }
    }
    if (winding == 0) {
        return 0.0f;
    }
    return coverageFromDistance(-nearest);
}

}  // namespace

float clipCoverage(const ResolvedClip& shape, float x, float y) {
    switch (shape.kind) {
        case ResolvedClip::Kind::None:
            return 1.0f;
        case ResolvedClip::Kind::Rect:
            return rectCoverage(shape, x, y);
        case ResolvedClip::Kind::Circle:
            return circleCoverage(shape, x, y);
        case ResolvedClip::Kind::Ellipse:
            return ellipseCoverage(shape, x, y);
        case ResolvedClip::Kind::Polygon:
            return polygonCoverage(shape, x, y);
    }
    return 1.0f;
}

void applyClipMask(std::uint8_t* pixels, int width, int height,
                   const ResolvedClip& shape) {
    if (!pixels || width <= 0 || height <= 0 ||
        shape.kind == ResolvedClip::Kind::None) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const float cover =
                clipCoverage(shape, static_cast<float>(col) + 0.5f,
                             static_cast<float>(row) + 0.5f);
            std::uint8_t* alpha = pixels + 4 * (row * width + col) + 3;
            *alpha = static_cast<std::uint8_t>(static_cast<float>(*alpha) * cover + 0.5f);
        }
    }
}

}  // namespace sui

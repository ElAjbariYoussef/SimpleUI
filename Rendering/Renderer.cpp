#include "Rendering/Renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace sui {
namespace {

// One sample per 90 degrees of arc: smooth at text sizes without generating more
// vertices than the eye can pick out.
constexpr int kArcSamples = 8;

// A dash pattern of three times the stroke width, which is what CSS's own diagrams
// use and reads correctly at any scale.
constexpr float kDashMultiplier = 3.0f;

SDL_FColor toFColor(const Color& c) {
    return SDL_FColor{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                      static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}

// `SDL_RenderGeometry` takes normalized texture coordinates, and the image's own
// placement is already resolved into the destination, so a point maps to a
// fraction of the way across that destination. A solid or gradient fill has no
// texture, and the value is ignored for those.
SDL_FPoint uvOf(const Shader& shader, float x, float y) {
    if (!shader.isImage()) {
        return SDL_FPoint{0.0f, 0.0f};
    }
    const SDL_FRect& dst = shader.destination();
    const float w = dst.w > 0.0f ? dst.w : 1.0f;
    const float h = dst.h > 0.0f ? dst.h : 1.0f;
    return SDL_FPoint{(x - dst.x) / w, (y - dst.y) / h};
}

Color lerp(Color a, Color b, float t) {
    const auto channel = [t](std::uint8_t from, std::uint8_t to) {
        return static_cast<std::uint8_t>(std::lround(from + (to - from) * t));
    };
    return Color{channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
}

// The colour a gradient shows at `t`, holding the end colours past the ends. CSS
// says stops are sorted, so the list is searched rather than assumed ordered.
Color sampleStops(const std::vector<GradientStop>& stops, float t) {
    if (stops.empty()) {
        return Color{0, 0, 0, 0};
    }
    if (stops.size() == 1 || t <= stops.front().offset) {
        return stops.front().color;
    }
    if (t >= stops.back().offset) {
        return stops.back().color;
    }
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t > stops[i].offset) {
            continue;
        }
        const GradientStop& lo = stops[i - 1];
        const GradientStop& hi = stops[i];
        const float span = hi.offset - lo.offset;
        // Two stops at the same offset are a hard edge; the later one wins.
        return span <= 0.0f ? hi.color : lerp(lo.color, hi.color, (t - lo.offset) / span);
    }
    return stops.back().color;
}

// Where a point sits along a gradient line, normalised so 0 is the start and 1 the
// end. A degenerate line collapses to its first colour.
float projectLinear(const LinearGradient& g, float x, float y) {
    const float dx = g.x2 - g.x1;
    const float dy = g.y2 - g.y1;
    const float lenSq = dx * dx + dy * dy;
    if (lenSq <= 0.0f) {
        return 0.0f;
    }
    return ((x - g.x1) * dx + (y - g.y1) * dy) / lenSq;
}

float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

// Where a point sits on a conic sweep, 0 at `fromAngle` and 1 after a full turn.
// CSS counts clockwise from twelve o'clock on a y-down canvas, so the angle is
// `atan2(dx, -dy)`. The centre itself is every angle at once; it reads as the
// sweep start.
float conicalPosition(const ConicalGradient& g, float x, float y) {
    const float dx = x - g.cx;
    const float dy = y - g.cy;
    const float pi = static_cast<float>(M_PI);
    float degrees = std::atan2(dx, -dy) * 180.0f / pi;
    float turn = std::fmod(degrees - g.fromAngle, 360.0f);
    if (turn < 0.0f) {
        turn += 360.0f;
    }
    return turn / 360.0f;
}

// Where a point sits on a radial gradient, 0 at the focus and 1 past the ending
// shape. The gradient line runs from the focus through the centre and on to the
// far edge, so a point's position is where its own ray crosses that line: the
// distance from the focus divided by the ray's length. Scaling into a space where
// the ending shape is a unit circle makes the far edge sit at 1, and makes a
// concentric gradient exact rather than merely close.
float radialPosition(const RadialGradient& g, float x, float y) {
    const float rx = g.radiusX > 0.0f ? g.radiusX : 1.0f;
    const float ry = g.radiusY > 0.0f ? g.radiusY : 1.0f;
    // Point and focus, both relative to the centre and in the normalised space.
    const float px = (x - g.cx) / rx;
    const float py = (y - g.cy) / ry;
    const float fx = (g.focalX - g.cx) / rx;
    const float fy = (g.focalY - g.cy) / ry;
    const float mx = px - fx;
    const float my = py - fy;

    const float distance = std::hypot(fx, fy);
    if (distance <= 1e-6f) {
        // Concentric: the position is just how far out the point is.
        return clamp01(std::hypot(mx, my));
    }
    // Solve |P - Q| = lambda * |F - Q| for Q on the axis at F + t * u, which is the
    // definition CSS gives: every point shares one circle through the focus.
    const float ux = fx / distance;
    const float uy = fy / distance;
    const float along = mx * ux + my * uy;
    const float lambda = distance + 1.0f;
    const float a = 1.0f - lambda * lambda;
    const float b = along * along - a * (mx * mx + my * my);
    if (b <= 0.0f) {
        return clamp01(along / lambda);
    }
    const float root = std::sqrt(b);
    // `a` is negative for every lambda the spec allows, so the root that puts the
    // crossing in front of the focus is the one that subtracts it; the other lands
    // behind and would make the gradient run backwards.
    const float t = (along - root) / a;
    return clamp01(t / lambda);
}

// Which side of a directed edge is inside a convex polygon. The sign is worked out
// from the polygon's own winding so the clip does not care how the outline was built.
float insideSign(const std::vector<SDL_FPoint>& polygon) {
    double area = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const SDL_FPoint& a = polygon[i];
        const SDL_FPoint& b = polygon[(i + 1) % polygon.size()];
        area += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    return area >= 0.0 ? 1.0f : -1.0f;
}

bool isInside(const std::vector<SDL_FPoint>& polygon, const SDL_FPoint& p, float sign) {
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const SDL_FPoint& a = polygon[i];
        const SDL_FPoint& b = polygon[(i + 1) % polygon.size()];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (cross * sign < 0.0f) {
            return false;
        }
    }
    return true;
}

// Sutherland-Hodgman against one edge of the convex clip polygon.
void clipToEdge(std::vector<SDL_FPoint>& subject, std::vector<SDL_FPoint>& scratch,
                const SDL_FPoint& a, const SDL_FPoint& b, float sign) {
    if (subject.empty()) {
        return;
    }
    scratch.clear();
    const auto inside = [&](const SDL_FPoint& p) {
        return ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) * sign >= 0.0f;
    };
    for (std::size_t i = 0; i < subject.size(); ++i) {
        const SDL_FPoint& cur = subject[i];
        const SDL_FPoint& next = subject[(i + 1) % subject.size()];
        const bool curIn = inside(cur);
        const bool nextIn = inside(next);
        if (curIn != nextIn) {
            // The crossing point, found by the ratio of the two signed areas.
            const float dCur = (b.x - a.x) * (cur.y - a.y) - (b.y - a.y) * (cur.x - a.x);
            const float dNext = (b.x - a.x) * (next.y - a.y) - (b.y - a.y) * (next.x - a.x);
            const float t = dCur / (dCur - dNext);
            scratch.push_back(SDL_FPoint{cur.x + (next.x - cur.x) * t, cur.y + (next.y - cur.y) * t});
        }
        if (nextIn) {
            scratch.push_back(next);
        }
    }
    subject.swap(scratch);
}

// A point known to be inside a box, used to tell which way a stroke band extends.
SDL_FPoint centreOf(const SDL_FRect& rect) {
    return SDL_FPoint{rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f};
}

SDL_FPoint centroid(const std::vector<SDL_FPoint>& points) {
    double x = 0.0;
    double y = 0.0;
    for (const SDL_FPoint& p : points) {
        x += p.x;
        y += p.y;
    }
    const double n = static_cast<double>(points.size());
    return SDL_FPoint{static_cast<float>(x / n), static_cast<float>(y / n)};
}

// Points along an arc, inclusive of both ends.
void arc(std::vector<SDL_FPoint>& out, float cx, float cy, float rx, float ry, float from,
         float to, int samples) {
    for (int i = 0; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        const float a = from + (to - from) * t;
        out.push_back(SDL_FPoint{cx + std::cos(a) * rx, cy + std::sin(a) * ry});
    }
}

}  // namespace

Color toColor(SDL_Color color) {
    return Color{color.r, color.g, color.b, color.a};
}


Color withOpacity(Color color, float opacity) {
    color.a = static_cast<std::uint8_t>(std::lround(clamp01(opacity) * color.a));
    return color;
}

Color multiplyAlpha(Color color, float factor) {
    color.a = static_cast<std::uint8_t>(std::lround(clamp01(factor) * color.a));
    return color;
}

bool Radii::any() const {
    return topLeft.x > 0.0f || topLeft.y > 0.0f || topRight.x > 0.0f || topRight.y > 0.0f ||
           bottomRight.x > 0.0f || bottomRight.y > 0.0f || bottomLeft.x > 0.0f ||
           bottomLeft.y > 0.0f;
}

bool Radii::uniform() const {
    return topLeft.x == topRight.x && topLeft.x == bottomRight.x &&
           topLeft.x == bottomLeft.x && topLeft.y == topRight.y &&
           topLeft.y == bottomRight.y && topLeft.y == bottomLeft.y;
}

Radii Radii::all(float x, float y) {
    Radii r;
    r.topLeft = Corner{x, y};
    r.topRight = Corner{x, y};
    r.bottomRight = Corner{x, y};
    r.bottomLeft = Corner{x, y};
    return r;
}

Radii Radii::clamped(float width, float height) const {
    Radii r = *this;
    if (width <= 0.0f || height <= 0.0f) {
        return Radii{};
    }
    // Each pair of corners sharing an edge must fit within it; the scale factor that
    // achieves this is applied to every radius so the shape stays a scaled version
    // of what was asked for rather than an arbitrary truncation.
    const float horizontal = std::max(
        {r.topLeft.x + r.topRight.x, r.topRight.x + r.bottomRight.x,
         r.bottomRight.x + r.bottomLeft.x, r.bottomLeft.x + r.topLeft.x});
    if (horizontal > 0.0f) {
        const float f = std::min(1.0f, width / horizontal);
        for (Corner* c : {&r.topLeft, &r.topRight, &r.bottomRight, &r.bottomLeft}) {
            c->x *= f;
        }
    }
    const float vertical = std::max({r.topLeft.y + r.bottomLeft.y, r.topLeft.y + r.topRight.y,
                                     r.topRight.y + r.bottomRight.y,
                                     r.bottomRight.y + r.bottomLeft.y});
    if (vertical > 0.0f) {
        const float f = std::min(1.0f, height / vertical);
        for (Corner* c : {&r.topLeft, &r.topRight, &r.bottomRight, &r.bottomLeft}) {
            c->y *= f;
        }
    }
    return r;
}

Shader Shader::solid(Color color) {
    Shader shader;
    shader.kind_ = Kind::Solid;
    shader.solid_ = color;
    return shader;
}

Shader Shader::linear(const LinearGradient& gradient) {
    Shader shader;
    shader.kind_ = Kind::Linear;
    shader.linear_ = gradient;
    return shader;
}

Shader Shader::radial(const RadialGradient& gradient) {
    Shader shader;
    shader.kind_ = Kind::Radial;
    shader.radial_ = gradient;
    return shader;
}

Shader Shader::conical(const ConicalGradient& gradient) {
    Shader shader;
    shader.kind_ = Kind::Conical;
    shader.conical_ = gradient;
    return shader;
}

Shader Shader::image(SDL_Texture* texture, const SDL_FRect& source,
                     const SDL_FRect& destination, Repeat repeat, Color tint) {
    Shader shader;
    shader.kind_ = Kind::Image;
    shader.texture_ = texture;
    shader.source_ = source;
    shader.destination_ = destination;
    shader.repeat_ = repeat;
    shader.tint_ = tint;
    return shader;
}

bool Shader::needsMesh() const {
    if (kind_ == Kind::Radial || kind_ == Kind::Conical) {
        return true;
    }
    // A single ramp between the two ends is a linear function of position, so the
    // outline's corners determine the whole triangle exactly. A stop in between
    // makes a second ramp with a different slope, and the corner interpolation would
    // round that break off, so the edges have to be sampled as well.
    return kind_ == Kind::Linear && linear_.stops.size() > 2;
}

float Shader::meshStep() const {
    if (kind_ == Kind::Radial) {
        // The error across one cell grows as the square of the step over the radius,
        // and near the focus the field is a cone rather than a plane, so the step is
        // kept well under the radius.
        const float radius = std::max(radial_.radiusX, radial_.radiusY);
        return std::clamp(radius / 14.0f, 1.0f, 12.0f);
    }
    if (kind_ == Kind::Conical) {
        // The sweep bends uniformly in angle, so arc length — radius times angle
        // — decides the error, and the same relative step applies.
        return std::clamp(conical_.radius / 14.0f, 1.0f, 12.0f);
    }
    // A linear ramp only bends where a stop sits, and the corners already carry the
    // slope on either side, so a fine outline is enough.
    return 4.0f;
}

Color Shader::colorAt(float x, float y) const {
    switch (kind_) {
        case Kind::Solid:
            return solid_;
        case Kind::Linear:
            return sampleStops(linear_.stops, projectLinear(linear_, x, y));
        case Kind::Radial:
            return sampleStops(radial_.stops, radialPosition(radial_, x, y));
        case Kind::Conical:
            return sampleStops(conical_.stops, conicalPosition(conical_, x, y));
        case Kind::Image:
            return tint_;
    }
    return solid_;
}

std::vector<SDL_FPoint> Renderer::roundedOutline(const SDL_FRect& rect, const Radii& raw,
                                                 int samples) {
    if (!raw.any()) {
        return fourCorners(rect);
    }
    return sampledOutline(rect, raw, samples);
}

std::vector<SDL_FPoint> Renderer::fourCorners(const SDL_FRect& rect) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return {};
    }
    return {{rect.x, rect.y},
            {rect.x + rect.w, rect.y},
            {rect.x + rect.w, rect.y + rect.h},
            {rect.x, rect.y + rect.h}};
}

std::vector<SDL_FPoint> Renderer::sampledOutline(const SDL_FRect& rect, const Radii& raw,
                                                 int samples) {
    std::vector<SDL_FPoint> points;
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return points;
    }
    const Radii r = raw.clamped(rect.w, rect.h);
    points.reserve(static_cast<std::size_t>(samples) * 4 + 4);
    const float right = rect.x + rect.w;
    const float bottom = rect.y + rect.h;
    const float pi = static_cast<float>(M_PI);
    // Clockwise from the top-left arc, which is the winding a triangle fan needs.
    arc(points, rect.x + r.topLeft.x, rect.y + r.topLeft.y, r.topLeft.x, r.topLeft.y, pi,
        pi * 1.5f, samples);
    arc(points, right - r.topRight.x, rect.y + r.topRight.y, r.topRight.x, r.topRight.y,
        pi * 1.5f, pi * 2.0f, samples);
    arc(points, right - r.bottomRight.x, bottom - r.bottomRight.y, r.bottomRight.x,
        r.bottomRight.y, 0.0f, pi * 0.5f, samples);
    arc(points, rect.x + r.bottomLeft.x, bottom - r.bottomLeft.y, r.bottomLeft.x,
        r.bottomLeft.y, pi * 0.5f, pi, samples);
    return points;
}

std::vector<SDL_FPoint> Renderer::sideOutline(const SDL_FRect& rect, const Radii& raw,
                                              Side side, int samplesPerQuarter) {
    std::vector<SDL_FPoint> points;
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return points;
    }
    const Radii r = raw.clamped(rect.w, rect.h);
    const float right = rect.x + rect.w;
    const float bottom = rect.y + rect.h;
    const float pi = static_cast<float>(M_PI);
    const float eighth = pi * 0.25f;
    points.reserve(static_cast<std::size_t>(samplesPerQuarter) * 2 + 2);

    // Every corner arc is split in half, and each half belongs to the side it faces,
    // so the four sides together trace the outline exactly once.
    switch (side) {
        case Side::Top:
            arc(points, rect.x + r.topLeft.x, rect.y + r.topLeft.y, r.topLeft.x, r.topLeft.y,
                pi + eighth, pi * 1.5f, samplesPerQuarter);
            arc(points, right - r.topRight.x, rect.y + r.topRight.y, r.topRight.x, r.topRight.y,
                pi * 1.5f, pi * 1.75f, samplesPerQuarter);
            break;
        case Side::Right:
            arc(points, right - r.topRight.x, rect.y + r.topRight.y, r.topRight.x, r.topRight.y,
                pi * 1.75f, pi * 2.0f, samplesPerQuarter);
            arc(points, right - r.bottomRight.x, bottom - r.bottomRight.y, r.bottomRight.x,
                r.bottomRight.y, 0.0f, eighth, samplesPerQuarter);
            break;
        case Side::Bottom:
            arc(points, right - r.bottomRight.x, bottom - r.bottomRight.y, r.bottomRight.x,
                r.bottomRight.y, eighth, pi * 0.5f, samplesPerQuarter);
            arc(points, rect.x + r.bottomLeft.x, bottom - r.bottomLeft.y, r.bottomLeft.x,
                r.bottomLeft.y, pi * 0.5f, pi * 0.75f, samplesPerQuarter);
            break;
        case Side::Left:
            arc(points, rect.x + r.bottomLeft.x, bottom - r.bottomLeft.y, r.bottomLeft.x,
                r.bottomLeft.y, pi * 0.75f, pi, samplesPerQuarter);
            arc(points, rect.x + r.topLeft.x, rect.y + r.topLeft.y, r.topLeft.x, r.topLeft.y,
                pi, pi + eighth, samplesPerQuarter);
            break;
    }
    return points;
}

void Renderer::setColor(const Color& color) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
}

void Renderer::fillRect(const SDL_FRect& rect, const Color& color) {
    if (color.a == 0 || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    setColor(color);
    SDL_RenderFillRect(renderer_, &rect);
}

void Renderer::fillRect(const SDL_FRect& rect, const Shader& shader, const Radii& radii) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    if (shader.isSolid() && !radii.any()) {
        fillRect(rect, shader.colorAt(rect.x, rect.y));
        return;
    }
    if (shader.isSolid() && shader.colorAt(rect.x, rect.y).a == 0) {
        return;
    }
    emit(roundedOutline(rect, radii), shader);
}

void Renderer::emit(const std::vector<SDL_FPoint>& points, const Shader& shader) {
    if (points.size() < 3) {
        return;
    }
    if (const Color probe = shader.colorAt(points[0].x, points[0].y); probe.a == 0) {
        return;
    }
    if (shader.isImage()) {
        // The address mode is renderer state rather than texture state, and
        // SDL_RenderGeometry is the only thing it affects, so it is set here. Wrapping
        // is the default when a coordinate leaves 0..1, which is what a tiled
        // background wants, and what an untiled one has to ask not to get.
        const bool tileX = shader.repeat() == Repeat::Tile || shader.repeat() == Repeat::TileX;
        const bool tileY = shader.repeat() == Repeat::Tile || shader.repeat() == Repeat::TileY;
        SDL_SetRenderTextureAddressMode(
            renderer_, tileX ? SDL_TEXTURE_ADDRESS_WRAP : SDL_TEXTURE_ADDRESS_CLAMP,
            tileY ? SDL_TEXTURE_ADDRESS_WRAP : SDL_TEXTURE_ADDRESS_CLAMP);
    }
    if (shader.needsMesh()) {
        emitMesh(points, shader);
        return;
    }
    // Every shape the engine draws is convex, so a fan from the first vertex covers
    // it, and the corner colours then determine the whole triangle.
    emitFan(points, shader, points[0]);
}

void Renderer::emitFan(const std::vector<SDL_FPoint>& outline, const Shader& shader,
                       SDL_FPoint apex) {
    if (outline.size() < 3) {
        return;
    }
    std::vector<SDL_Vertex> vertices;
    vertices.reserve(outline.size() + 1);
    // An image shader carries its colour in the texture rather than in the vertices, so
    // the per-vertex colour is only the tint and the coordinates are what sample it.
    vertices.push_back(SDL_Vertex{apex, toFColor(shader.colorAt(apex.x, apex.y)),
                                  uvOf(shader, apex.x, apex.y)});
    for (const SDL_FPoint& p : outline) {
        vertices.push_back(
            SDL_Vertex{p, toFColor(shader.colorAt(p.x, p.y)), uvOf(shader, p.x, p.y)});
    }
    std::vector<int> indices;
    indices.reserve(outline.size() * 3);
    for (std::size_t i = 0; i < outline.size(); ++i) {
        indices.push_back(0);
        indices.push_back(static_cast<int>(i) + 1);
        indices.push_back(static_cast<int>((i + 1) % outline.size()) + 1);
    }
    SDL_RenderGeometry(renderer_, shader.texture(), vertices.data(),
                       static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));
}

void Renderer::emitRing(const std::vector<SDL_FPoint>& outer,
                        const std::vector<SDL_FPoint>& inner, const Color& color) {
    if (color.a == 0 || outer.size() != inner.size() || outer.size() < 3) {
        return;
    }
    // Both outlines were sampled at the same arc angles, so the ring is a strip
    // between corresponding points. The walk has to wrap past the last pair or the
    // side where the outline closes on itself is left bare.
    std::vector<SDL_Vertex> vertices;
    const std::size_t pairs = outer.size() + 1;  // the extra pair closes the loop
    vertices.reserve(pairs * 2);
    for (std::size_t i = 0; i < pairs; ++i) {
        const std::size_t j = i % outer.size();
        vertices.push_back(SDL_Vertex{outer[j], toFColor(color), SDL_FPoint{0.0f, 0.0f}});
        vertices.push_back(SDL_Vertex{inner[j], toFColor(color), SDL_FPoint{0.0f, 0.0f}});
    }
    std::vector<int> indices;
    indices.reserve(outer.size() * 6);
    for (std::size_t i = 0; i + 1 < pairs; ++i) {
        const int a = static_cast<int>(i) * 2;
        indices.insert(indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
    SDL_RenderGeometry(renderer_, nullptr, vertices.data(),
                       static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));
}

void Renderer::fillPolygon(const std::vector<SDL_FPoint>& points, const Color& color) {
    if (color.a == 0) {
        return;
    }
    emit(points, Shader::solid(color));
}

void Renderer::fillPolygon(const std::vector<SDL_FPoint>& points, const Shader& shader) {
    emit(points, shader);
}

void Renderer::fillTriangles(const std::vector<SDL_FPoint>& triangles, const Color& color) {
    if (color.a == 0 || triangles.size() < 3) {
        return;
    }
    std::vector<SDL_Vertex> vertices;
    vertices.reserve(triangles.size());
    for (const SDL_FPoint& p : triangles) {
        vertices.push_back(SDL_Vertex{p, toFColor(color), {0.0f, 0.0f}});
    }
    std::vector<int> indices(vertices.size());
    for (std::size_t k = 0; k < indices.size(); ++k) {
        indices[k] = static_cast<int>(k);
    }
    SDL_RenderGeometry(renderer_, nullptr, vertices.data(),
                       static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));
}

void Renderer::fillTriangles(const std::vector<SDL_FPoint>& triangles,
                             const Shader& shader) {
    if (triangles.size() < 3) {
        return;
    }
    for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
        emit({triangles[i], triangles[i + 1], triangles[i + 2]}, shader);
    }
}

void Renderer::fillCircle(float cx, float cy, float radius, const Color& color) {
    if (radius <= 0.0f || color.a == 0) {
        return;
    }
    // A fixed sample count makes a large circle visibly faceted and wastes vertices
    // on a small one, so the count follows the radius: one segment per few pixels of
    // circumference, which is where the chord error falls below half a pixel.
    const int samples = std::clamp(static_cast<int>(std::ceil(radius * 1.6f)), 8, 128);
    std::vector<SDL_FPoint> points;
    points.reserve(samples);
    const float twoPi = 2.0f * static_cast<float>(M_PI);
    for (int i = 0; i < samples; ++i) {
        const float a = twoPi * static_cast<float>(i) / static_cast<float>(samples);
        points.push_back(SDL_FPoint{cx + std::cos(a) * radius, cy + std::sin(a) * radius});
    }
    fillPolygon(points, color);
}

void Renderer::strokeRect(const SDL_FRect& rect, const Color& color, float thickness,
                          const Radii& radii, const SideDashes& dashes) {
    if (color.a == 0 || thickness <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    if (!dashes.uniform()) {
        // Each side is drawn on its own so it can carry its own pattern. The dash
        // phase restarts at every side, which only shows where the patterns differ.
        for (const Side side : {Side::Top, Side::Right, Side::Bottom, Side::Left}) {
            emitSide(rect, side, thickness, color, radii, dashes.of(side));
        }
        return;
    }
    if (dashes.top != Dash::Solid) {
        // Dashing walks the whole closed outline in one go, so the pattern carries on
        // around the corners the way CSS draws it.
        emitDashed(roundedOutline(rect, radii), thickness, color, dashes.top, true,
                   centreOf(rect));
        return;
    }
    if (!radii.any()) {
        // The common case: four axis-aligned bands between the box and the box inset
        // by the border width. The band is inward, not centred, so that this path and
        // the ring path below put the ink in the same place.
        const float t = std::min(thickness, std::min(rect.w, rect.h));
        setColor(color);
        const SDL_FRect sides[4] = {
            {rect.x, rect.y, rect.w, t},
            {rect.x, rect.y + rect.h - t, rect.w, t},
            {rect.x, rect.y + t, t, std::max(0.0f, rect.h - t * 2.0f)},
            {rect.x + rect.w - t, rect.y + t, t, std::max(0.0f, rect.h - t * 2.0f)},
        };
        for (const SDL_FRect& side : sides) {
            if (side.w > 0.0f && side.h > 0.0f) {
                SDL_RenderFillRect(renderer_, &side);
            }
        }
        return;
    }

    const std::vector<SDL_FPoint> outer = sampledOutline(rect, radii);
    // The inner edge follows the outer one concentrically, so each radius shrinks by
    // the border width and is then clamped into the smaller box.
    Radii inner = radii;
    for (Radii::Corner* c :
         {&inner.topLeft, &inner.topRight, &inner.bottomRight, &inner.bottomLeft}) {
        c->x = std::max(0.0f, c->x - thickness);
        c->y = std::max(0.0f, c->y - thickness);
    }
    const SDL_FRect innerRect{rect.x + thickness, rect.y + thickness,
                              std::max(0.0f, rect.w - thickness * 2.0f),
                              std::max(0.0f, rect.h - thickness * 2.0f)};
    const std::vector<SDL_FPoint> innerPoints = sampledOutline(innerRect, inner);
    emitRing(outer, innerPoints, color);
}

void Renderer::strokeSide(const SDL_FRect& rect, Side side, const Color& color, float thickness,
                          const Radii& radii, Dash dash) {
    if (color.a == 0 || thickness <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    emitSide(rect, side, thickness, color, radii, dash);
}

void Renderer::emitSide(const SDL_FRect& rect, Side side, float thickness, const Color& color,
                        const Radii& radii, Dash dash) {
    const std::vector<SDL_FPoint> outer = sideOutline(rect, radii, side);
    if (outer.size() < 2) {
        return;
    }
    if (dash != Dash::Solid) {
        emitDashed(outer, thickness, color, dash, false, centreOf(rect));
        return;
    }
    // The inner edge is the same side of the box inset by the border width, with each
    // radius reduced by the same amount so the two arcs stay concentric.
    Radii inner = radii;
    for (Radii::Corner* c :
         {&inner.topLeft, &inner.topRight, &inner.bottomRight, &inner.bottomLeft}) {
        c->x = std::max(0.0f, c->x - thickness);
        c->y = std::max(0.0f, c->y - thickness);
    }
    const SDL_FRect innerRect{rect.x + thickness, rect.y + thickness,
                              std::max(0.0f, rect.w - thickness * 2.0f),
                              std::max(0.0f, rect.h - thickness * 2.0f)};
    const std::vector<SDL_FPoint> innerPoints = sideOutline(innerRect, inner, side);
    emitRing(outer, innerPoints, color);
}

void Renderer::emitMesh(const std::vector<SDL_FPoint>& polygon, const Shader& shader) {
    if (polygon.size() < 3) {
        return;
    }
    float minX = polygon[0].x;
    float minY = polygon[0].y;
    float maxX = polygon[0].x;
    float maxY = polygon[0].y;
    for (const SDL_FPoint& p : polygon) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    const float sign = insideSign(polygon);
    const float step = std::max(0.5f, shader.meshStep());
    const int columns = std::max(1, static_cast<int>(std::ceil((maxX - minX) / step)));
    const int rows = std::max(1, static_cast<int>(std::ceil((maxY - minY) / step)));
    const float cellW = (maxX - minX) / columns;
    const float cellH = (maxY - minY) / rows;

    meshVertices_.clear();
    meshIndices_.clear();
    clipFront_.clear();
    clipBack_.clear();

    // A fan from one point cannot hold a gradient's peak: the triangles that reach
    // the middle of the shape are enormous, and the colour across one of them is a
    // straight line between two far-apart endpoints. A grid clipped to the outline
    // keeps every triangle the size of a cell, so the interpolation error is the same
    // everywhere and stays proportional to the cell rather than to the shape.
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const float x0 = minX + static_cast<float>(column) * cellW;
            const float y0 = minY + static_cast<float>(row) * cellH;
            const float x1 = x0 + cellW;
            const float y1 = y0 + cellH;
            const SDL_FPoint corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};

            bool whole = true;
            for (const SDL_FPoint& corner : corners) {
                if (!isInside(polygon, corner, sign)) {
                    whole = false;
                    break;
                }
            }

            const int base = static_cast<int>(meshVertices_.size());
            if (whole) {
                // The common case, and it needs no clipping at all.
                for (int i = 0; i < 4; ++i) {
                    meshVertices_.push_back(SDL_Vertex{
                        corners[i], toFColor(shader.colorAt(corners[i].x, corners[i].y)),
                        uvOf(shader, corners[i].x, corners[i].y)});
                }
                meshIndices_.insert(meshIndices_.end(), {base, base + 1, base + 2, base, base + 2,
                                                        base + 3});
                continue;
            }

            clipFront_.assign(std::begin(corners), std::end(corners));
            for (std::size_t i = 0; i < polygon.size() && !clipFront_.empty(); ++i) {
                clipToEdge(clipFront_, clipBack_, polygon[i], polygon[(i + 1) % polygon.size()],
                           sign);
            }
            if (clipFront_.size() < 3) {
                continue;
            }
            for (const SDL_FPoint& p : clipFront_) {
                meshVertices_.push_back(
                    SDL_Vertex{p, toFColor(shader.colorAt(p.x, p.y)), uvOf(shader, p.x, p.y)});
            }
            // The centre, plus one triangle per outline edge.
            const int centre = static_cast<int>(meshVertices_.size());
            const SDL_FPoint middle = centroid(clipFront_);
            meshVertices_.push_back(SDL_Vertex{middle,
                                               toFColor(shader.colorAt(middle.x, middle.y)),
                                               uvOf(shader, middle.x, middle.y)});
            for (std::size_t i = 0; i < clipFront_.size(); ++i) {
                meshIndices_.insert(meshIndices_.end(),
                                    {centre, base + static_cast<int>(i),
                                     base + static_cast<int>((i + 1) % clipFront_.size())});
            }
        }
    }
    if (meshIndices_.empty()) {
        return;
    }
    SDL_RenderGeometry(renderer_, shader.texture(), meshVertices_.data(),
                       static_cast<int>(meshVertices_.size()), meshIndices_.data(),
                       static_cast<int>(meshIndices_.size()));
}

void Renderer::emitDashed(const std::vector<SDL_FPoint>& outline, float thickness,
                          const Color& color, Dash dash, bool closed, const SDL_FPoint& interior) {
    if (outline.size() < 2 || thickness <= 0.0f || color.a == 0) {
        return;
    }
    // A dotted run is a disc centred half a width in from the path, so that it fills
    // the band instead of hanging over the outline.
    const float half = thickness * 0.5f;
    // A border band lives between the outline and the outline moved inward by the
    // border width, so a dashed run fills that same band rather than straddling the
    // path the way a stroked centreline would. Which way is inward is decided per
    // segment against `interior`, which keeps the answer right whether the path is a
    // whole box or a single side of one walked either way round.
    //
    // A dotted border is the same walk with a round cap, a dot as wide as the border
    // and a gap of the same length, so it differs in the rhythm as well as in shape.
    // Dashing keeps the longer, thinner rhythm.
    const bool dotted = dash == Dash::Dotted;
    const float period =
        dotted ? std::max(thickness * 2.0f, 1.0f) : std::max(thickness * kDashMultiplier, 1.0f) * 1.5f;
    const float on = dotted ? std::max(thickness, 1.0f) : period * (2.0f / 3.0f);
    const std::size_t count = outline.size();
    const std::size_t segments = closed ? count : count - 1;

    float consumed = 0.0f;
    for (std::size_t i = 0; i < segments; ++i) {
        const SDL_FPoint& a = outline[i];
        const SDL_FPoint& b = outline[(i + 1) % count];
        const float span = std::hypot(b.x - a.x, b.y - a.y);
        if (span <= 0.0f) {
            continue;
        }
        float t = 0.0f;
        while (t < span) {
            const float phase = std::fmod(consumed + t, period);
            if (phase >= on) {
                t += std::min(period - phase, span - t);
                continue;
            }
            // The whole lit stretch is consumed in one step. Advancing by the dot
            // width instead would lay every dot of the period down touching.
            const float run = std::min(on - phase, span - t);
            const float x0 = a.x + (b.x - a.x) * (t / span);
            const float y0 = a.y + (b.y - a.y) * (t / span);
            const float x1 = a.x + (b.x - a.x) * ((t + run) / span);
            const float y1 = a.y + (b.y - a.y) * ((t + run) / span);
            const float dx = (b.x - a.x) / span;
            const float dy = (b.y - a.y) / span;
            float nx = -dy;
            float ny = dx;
            if (nx * (interior.x - a.x) + ny * (interior.y - a.y) < 0.0f) {
                nx = -nx;
                ny = -ny;
            }
            if (dotted) {
                fillCircle((x0 + x1) * 0.5f + nx * half, (y0 + y1) * 0.5f + ny * half, half,
                           color);
            } else {
                const std::vector<SDL_FPoint> quad{{x0 + nx * thickness, y0 + ny * thickness},
                                                   {x1 + nx * thickness, y1 + ny * thickness},
                                                   {x1, y1},
                                                   {x0, y0}};
                fillPolygon(quad, color);
            }
            t += run;
        }
        consumed = std::fmod(consumed + span, period);
    }
}

void Renderer::drawTexture(SDL_Texture* texture, const SDL_FRect& destination,
                           const SDL_FRect* source, Color tint) {
    if (!texture || destination.w <= 0.0f || destination.h <= 0.0f || tint.a == 0) {
        return;
    }
    const bool modulated = !(tint == Color{255, 255, 255, 255});
    if (modulated) {
        SDL_SetTextureAlphaMod(texture, tint.a);
        SDL_SetTextureColorMod(texture, tint.r, tint.g, tint.b);
    }
    SDL_RenderTexture(renderer_, texture, source, &destination);
    if (modulated) {
        SDL_SetTextureAlphaMod(texture, 255);
        SDL_SetTextureColorMod(texture, 255, 255, 255);
    }
}

void Renderer::pushClip(const SDL_FRect& rect) {
    clipStack_.push_back(current_);
    const SDL_Rect incoming{static_cast<int>(std::floor(rect.x)),
                            static_cast<int>(std::floor(rect.y)),
                            static_cast<int>(std::ceil(rect.w)),
                            static_cast<int>(std::ceil(rect.h))};
    if (clipStack_.size() == 1) {
        current_ = incoming;
    } else {
        const int x = std::max(current_.x, incoming.x);
        const int y = std::max(current_.y, incoming.y);
        const int right = std::min(current_.x + current_.w, incoming.x + incoming.w);
        const int bottom = std::min(current_.y + current_.h, incoming.y + incoming.h);
        current_ = SDL_Rect{x, y, std::max(0, right - x), std::max(0, bottom - y)};
    }
    clipping_ = true;
    SDL_SetRenderClipRect(renderer_, &current_);
}

void Renderer::popClip() {
    if (clipStack_.empty()) {
        return;
    }
    current_ = clipStack_.back();
    clipStack_.pop_back();
    clipping_ = !clipStack_.empty();
    SDL_SetRenderClipRect(renderer_, clipping_ ? &current_ : nullptr);
}

void Renderer::strokePath(const std::vector<SDL_FPoint>& points, const Color& color,
                          float thickness, bool closed, Dash dash) {
    if (color.a == 0 || thickness <= 0.0f || points.size() < 2) {
        return;
    }
    if (dash != Dash::Solid) {
        emitDashed(points, thickness, color, dash, closed, centroid(points));
        return;
    }
    const float half = thickness * 0.5f;
    const std::size_t last = closed ? points.size() : points.size() - 1;
    for (std::size_t i = 0; i < last; ++i) {
        const SDL_FPoint& a = points[i];
        const SDL_FPoint& b = points[(i + 1) % points.size()];
        const float span = std::hypot(b.x - a.x, b.y - a.y);
        if (span <= 0.0f) {
            continue;
        }
        const float dx = (b.x - a.x) / span;
        const float dy = (b.y - a.y) / span;
        const std::vector<SDL_FPoint> quad{{a.x - dy * half, a.y + dx * half},
                                           {b.x - dy * half, b.y + dx * half},
                                           {b.x + dy * half, b.y - dx * half},
                                           {a.x + dy * half, a.y - dx * half}};
        fillPolygon(quad, color);
        if (thickness > 1.5f && i + 2 < points.size()) {
            // A thick polyline notches at each vertex without a join.
            fillCircle(b.x, b.y, half, color);
        }
    }
}

void Renderer::line(float x1, float y1, float x2, float y2, const Color& color,
                    float thickness) {
    strokePath({SDL_FPoint{x1, y1}, SDL_FPoint{x2, y2}}, color, thickness);
}

bool parseSvgViewBox(std::string_view text, float box[4]) {
    // Commas read as spaces; exactly four finite numbers with a live extent.
    std::string cleaned(text);
    for (char& c : cleaned) {
        if (c == ',') {
            c = ' ';
        }
    }
    std::istringstream in(cleaned);
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        if (!(in >> v[i]) || !std::isfinite(v[i])) {
            return false;
        }
    }
    std::string tail;
    if (in >> tail) {
        return false;
    }
    if (!(v[2] > 0.0f) || !(v[3] > 0.0f)) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        box[i] = v[i];
    }
    return true;
}

float svgTextPxSize(std::string_view size, float cssSize, float scaleX) {
    // A parsed, positive attribute in user units wins; anything else is the
    // CSS size the element would have used as an HTML box.
    std::size_t first = size.find_first_not_of(" \t\n\r\f");
    if (first != std::string_view::npos) {
        const std::string number(size.substr(first));
        char* stop = nullptr;
        const float value = std::strtof(number.c_str(), &stop);
        if (stop != number.c_str() && value > 0.0f) {
            // A trailing percent scales the inherited CSS size; a plain number
            // is user units through the viewport.
            const char* end = stop;
            while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' ||
                   *end == '\f') {
                ++end;
            }
            if (*end == '%') {
                return cssSize * value * 0.01f;
            }
            if (scaleX > 0.0f) {
                return value * scaleX;
            }
        }
    }
    return cssSize;
}

SDL_FPoint svgTextOrigin(const SvgView& view, float x, float y, std::string_view anchor,
                         float width, float ascent) {
    float px = view.x + x * view.sx;
    if (anchor == "middle") {
        px -= width * 0.5f;
    } else if (anchor == "end") {
        px -= width;
    }
    return SDL_FPoint{px, view.y + y * view.sy - ascent};
}

SvgView svgSubViewport(const SvgView& parent, float x, float y, float w, float h,
                       bool none, bool hasBox, float bvx, float bvy, float bvw,
                       float bvh) {
    const float ox = parent.x + x * parent.sx;
    const float oy = parent.y + y * parent.sy;
    if (!hasBox || !(bvw > 0.0f) || !(bvh > 0.0f)) {
        return SvgView{1.0f, 1.0f, ox, oy, w, h};
    }
    SvgView sub = svgViewport(w * parent.sx, h * parent.sy, bvx, bvy, bvw, bvh, none);
    sub.x += ox;
    sub.y += oy;
    return sub;
}

float svgLengthBase(std::string_view attr, float vbw, float vbh) {
    if (attr == "x" || attr == "x1" || attr == "x2" || attr == "width" || attr == "cx" ||
        attr == "rx") {
        return vbw;
    }
    if (attr == "y" || attr == "y1" || attr == "y2" || attr == "height" || attr == "cy" ||
        attr == "ry") {
        return vbh;
    }
    if (attr == "r" || attr == "stroke-width") {
        return std::sqrt(vbw * vbw + vbh * vbh) / 1.41421356f;
    }
    return 0.0f;
}

SDL_FRect svgImageFit(float sw, float sh, float dw, float dh, bool none) {
    if (!(sw > 0.0f) || !(sh > 0.0f) || !(dw > 0.0f) || !(dh > 0.0f)) {
        return SDL_FRect{0.0f, 0.0f, 0.0f, 0.0f};
    }
    if (none) {
        return SDL_FRect{0.0f, 0.0f, dw, dh};
    }
    const float scale = std::min(dw / sw, dh / sh);
    const float w = sw * scale;
    const float h = sh * scale;
    return SDL_FRect{(dw - w) * 0.5f, (dh - h) * 0.5f, w, h};
}

SvgView svgViewport(float vw, float vh, float vbx, float vby, float vbw, float vbh,
                    bool none) {
    SvgView view;
    view.vbw = vbw;
    view.vbh = vbh;
    if (!(vw > 0.0f) || !(vh > 0.0f) || !(vbw > 0.0f) || !(vbh > 0.0f)) {
        return view;
    }
    if (none) {
        view.sx = vw / vbw;
        view.sy = vh / vbh;
        view.x = -vbx * view.sx;
        view.y = -vby * view.sy;
        return view;
    }
    const float scale = std::min(vw / vbw, vh / vbh);
    view.sx = view.sy = scale;
    view.x = (vw - vbw * scale) * 0.5f - vbx * scale;
    view.y = (vh - vbh * scale) * 0.5f - vby * scale;
    return view;
}

namespace {

// The path `d` grammar lets numbers run together: "10-20.5e2" is three numbers
// with no separator. This scanner reads that form directly.
struct PathScanner {
    std::string_view text;
    std::size_t i = 0;

    static bool isSeparator(char c) {
        return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }
    void skipSeparators() {
        while (i < text.size() && isSeparator(text[i])) {
            ++i;
        }
    }
    // The next command letter, if any. A bare number means the previous command
    // repeats, which is the caller's business.
    bool command(char& out) {
        skipSeparators();
        if (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) {
            out = text[i];
            ++i;
            return true;
        }
        return false;
    }
    bool number(float& out) {
        skipSeparators();
        const std::size_t start = i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        bool digits = false;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
            digits = true;
        }
        if (i < text.size() && text[i] == '.') {
            ++i;
            while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
                ++i;
                digits = true;
            }
        }
        if (digits && i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
            const std::size_t mark = i;
            ++i;
            if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
                ++i;
            }
            if (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
                while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
                    ++i;
                }
            } else {
                i = mark;
            }
        }
        if (!digits) {
            i = start;
            return false;
        }
        out = std::strtof(std::string(text.substr(start, i - start)).c_str(), nullptr);
        return true;
    }
    // An arc flag is a single `0` or `1`, which may sit flush against the next
    // number ("a1 1 0 0110 10").
    bool flag(bool& out) {
        skipSeparators();
        if (i < text.size() && (text[i] == '0' || text[i] == '1')) {
            out = text[i] == '1';
            ++i;
            return true;
        }
        return false;
    }
    bool more() {
        skipSeparators();
        return i < text.size();
    }
};

// Segments for a curve, from the length of its control polygon: enough that the
// flattening error stays under a pixel at any sane scale, never more than 64.
int segmentsFor(float span) {
    const int n = static_cast<int>(std::ceil(std::fabs(span) / 6.0f));
    return std::clamp(n, 4, 64);
}

// Samples an elliptical arc in the endpoint parameterization, appending every
// point after the first (the caller already holds the start).
void appendArc(std::vector<SDL_FPoint>& out, float x1, float y1, float rx, float ry,
               float degrees, bool largeArc, bool sweep, float x2, float y2) {
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx <= 0.0f || ry <= 0.0f || (x1 == x2 && y1 == y2)) {
        out.push_back(SDL_FPoint{x2, y2});
        return;
    }
    const float phi = degrees * 3.14159265f / 180.0f;
    const float cosPhi = std::cos(phi);
    const float sinPhi = std::sin(phi);
    const float dx = (x1 - x2) * 0.5f;
    const float dy = (y1 - y2) * 0.5f;
    const float xp = cosPhi * dx + sinPhi * dy;
    const float yp = -sinPhi * dx + cosPhi * dy;
    // Scale the radii up when they cannot span the endpoints, as the spec says.
    const float lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lambda > 1.0f) {
        const float grow = std::sqrt(lambda);
        rx *= grow;
        ry *= grow;
    }
    const float rx2 = rx * rx;
    const float ry2 = ry * ry;
    const float num = rx2 * ry2 - rx2 * yp * yp - ry2 * xp * xp;
    const float den = rx2 * yp * yp + ry2 * xp * xp;
    float factor = std::sqrt(std::max(0.0f, num / den));
    if (largeArc == sweep) {
        factor = -factor;
    }
    const float cxp = factor * rx * yp / ry;
    const float cyp = -factor * ry * xp / rx;
    const float cx = cosPhi * cxp - sinPhi * cyp + (x1 + x2) * 0.5f;
    const float cy = sinPhi * cxp + cosPhi * cyp + (y1 + y2) * 0.5f;
    const auto angleOf = [&](float ux, float uy) {
        return std::atan2(uy, ux);
    };
    const float ux = (xp - cxp) / rx;
    const float uy = (yp - cyp) / ry;
    const float vx = (-xp - cxp) / rx;
    const float vy = (-yp - cyp) / ry;
    const float theta = angleOf(ux, uy);
    float sweepAngle = angleOf(vx, vy) - theta;
    if (!sweep && sweepAngle > 0.0f) {
        sweepAngle -= 2.0f * 3.14159265f;
    } else if (sweep && sweepAngle < 0.0f) {
        sweepAngle += 2.0f * 3.14159265f;
    }
    const int steps =
        std::clamp(static_cast<int>(std::ceil(std::fabs(sweepAngle) / (0.5f * 3.14159265f) *
                                         8.0f)),
                   8, 64);
    for (int k = 1; k <= steps; ++k) {
        const float a = theta + sweepAngle * static_cast<float>(k) / static_cast<float>(steps);
        out.push_back(SDL_FPoint{cx + rx * cosPhi * std::cos(a) - ry * sinPhi * std::sin(a),
                                 cy + rx * sinPhi * std::cos(a) + ry * cosPhi * std::sin(a)});
    }
}

}  // namespace

bool parseSvgPath(std::string_view d, std::vector<SvgSubpath>& out) {
    PathScanner scan{d, 0};
    SvgSubpath* current = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    // The start of the subpath in progress, for the shorthand curve commands.
    float subX = 0.0f;
    float subY = 0.0f;
    float lastCtrlX = 0.0f;
    float lastCtrlY = 0.0f;
    char prev = 0;
    char command = 0;

    const auto flush = [&]() {
        current = nullptr;
        prev = 0;
    };
    const auto start = [&](float px, float py) {
        // `current` always aliases the subpath being built, so opening the
        // next one is just appending: the last is already in `out`.
        out.emplace_back();
        current = &out.back();
        current->points.push_back(SDL_FPoint{px, py});
    };
    const auto lineTo = [&](float px, float py) {
        if (!current) {
            start(px, py);
            return;
        }
        current->points.push_back(SDL_FPoint{px, py});
    };
    // Reads `n` numbers, reporting failure without consuming past the problem.
    const auto numbers = [&](float* values, int n) {
        for (int k = 0; k < n; ++k) {
            if (!scan.number(values[k])) {
                return false;
            }
        }
        return true;
    };
    const auto cubic = [&](float x1, float y1, float x2, float y2, float px, float py) {
        const int steps = segmentsFor(std::hypot(x1 - x, y1 - y) + std::hypot(x2 - x1, y2 - y1) +
                                     std::hypot(px - x2, py - y2));
        for (int k = 1; k <= steps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(steps);
            const float u = 1.0f - t;
            lineTo(u * u * u * x + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * px,
                   u * u * u * y + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * py);
        }
        lastCtrlX = x2;
        lastCtrlY = y2;
    };
    const auto quad = [&](float x1, float y1, float px, float py) {
        const int steps = segmentsFor(std::hypot(x1 - x, y1 - y) + std::hypot(px - x1, py - y1));
        for (int k = 1; k <= steps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(steps);
            const float u = 1.0f - t;
            lineTo(u * u * x + 2 * u * t * x1 + t * t * px, u * u * y + 2 * u * t * y1 + t * t * py);
        }
        lastCtrlX = x1;
        lastCtrlY = y1;
    };
    const auto arc = [&](float rx, float ry, float degrees, bool largeArc, bool sweep,
                         float px, float py) {
        if (!current) {
            start(px, py);
            return;
        }
        appendArc(current->points, x, y, rx, ry, degrees, largeArc, sweep, px, py);
    };

    while (scan.more()) {
        char letter = 0;
        if (scan.command(letter)) {
            command = letter;
        } else if (!command || scan.i == 0) {
            // No command letter ever appeared.
            return !out.empty();
        } else {
            // A run of numbers repeats the previous command; `M` after itself
            // becomes an implicit `L`, as the spec spells out.
            if (command == 'M') {
                command = 'L';
            } else if (command == 'm') {
                command = 'l';
            }
        }
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        switch (command) {
            case 'M':
            case 'm': {
                float v[2] = {0.0f, 0.0f};
                if (!numbers(v, 2)) {
                    return true;
                }
                x = relative ? x + v[0] : v[0];
                y = relative ? y + v[1] : v[1];
                subX = x;
                subY = y;
                start(x, y);
                break;
            }
            case 'L':
            case 'l': {
                float v[2] = {0.0f, 0.0f};
                if (!numbers(v, 2)) {
                    return true;
                }
                x = relative ? x + v[0] : v[0];
                y = relative ? y + v[1] : v[1];
                lineTo(x, y);
                break;
            }
            case 'H':
            case 'h': {
                float v[1] = {0.0f};
                if (!numbers(v, 1)) {
                    return true;
                }
                x = relative ? x + v[0] : v[0];
                lineTo(x, y);
                break;
            }
            case 'V':
            case 'v': {
                float v[1] = {0.0f};
                if (!numbers(v, 1)) {
                    return true;
                }
                y = relative ? y + v[0] : v[0];
                lineTo(x, y);
                break;
            }
            case 'C':
            case 'c': {
                float v[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                if (!numbers(v, 6)) {
                    return true;
                }
                const float ax = relative ? x + v[0] : v[0];
                const float ay = relative ? y + v[1] : v[1];
                const float bx = relative ? x + v[2] : v[2];
                const float by = relative ? y + v[3] : v[3];
                const float px = relative ? x + v[4] : v[4];
                const float py = relative ? y + v[5] : v[5];
                cubic(ax, ay, bx, by, px, py);
                x = px;
                y = py;
                break;
            }
            case 'S':
            case 's': {
                float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (!numbers(v, 4)) {
                    return true;
                }
                const bool reflected = prev == 'C' || prev == 'c' || prev == 'S' || prev == 's';
                const float ax = reflected ? 2 * x - lastCtrlX : x;
                const float ay = reflected ? 2 * y - lastCtrlY : y;
                const float bx = relative ? x + v[0] : v[0];
                const float by = relative ? y + v[1] : v[1];
                const float px = relative ? x + v[2] : v[2];
                const float py = relative ? y + v[3] : v[3];
                cubic(ax, ay, bx, by, px, py);
                x = px;
                y = py;
                break;
            }
            case 'Q':
            case 'q': {
                float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (!numbers(v, 4)) {
                    return true;
                }
                const float ax = relative ? x + v[0] : v[0];
                const float ay = relative ? y + v[1] : v[1];
                const float px = relative ? x + v[2] : v[2];
                const float py = relative ? y + v[3] : v[3];
                quad(ax, ay, px, py);
                x = px;
                y = py;
                break;
            }
            case 'T':
            case 't': {
                float v[2] = {0.0f, 0.0f};
                if (!numbers(v, 2)) {
                    return true;
                }
                const bool reflected = prev == 'Q' || prev == 'q' || prev == 'T' || prev == 't';
                const float ax = reflected ? 2 * x - lastCtrlX : x;
                const float ay = reflected ? 2 * y - lastCtrlY : y;
                const float px = relative ? x + v[0] : v[0];
                const float py = relative ? y + v[1] : v[1];
                quad(ax, ay, px, py);
                x = px;
                y = py;
                break;
            }
            case 'A':
            case 'a': {
                float v[3] = {0.0f, 0.0f, 0.0f};
                if (!numbers(v, 3)) {
                    return true;
                }
                bool largeArc = false;
                bool sweep = false;
                if (!scan.flag(largeArc) || !scan.flag(sweep)) {
                    return true;
                }
                float v2[2] = {0.0f, 0.0f};
                if (!numbers(v2, 2)) {
                    return true;
                }
                const float px = relative ? x + v2[0] : v2[0];
                const float py = relative ? y + v2[1] : v2[1];
                arc(v[0], v[1], v[2], largeArc, sweep, px, py);
                x = px;
                y = py;
                break;
            }
            case 'Z':
            case 'z': {
                if (current) {
                    current->closed = true;
                }
                x = subX;
                y = subY;
                break;
            }
            default:
                // An unknown letter ends the path; whatever it described stands.
                flush();
                return !out.empty();
        }
        prev = command;
    }
    flush();
    return !out.empty();
}

}  // namespace sui

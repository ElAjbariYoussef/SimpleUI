// Draw API the layout and paint stages emit into. Backends sit behind this so the
// engine is not coupled to SDL draw calls directly.
//
// Everything works in window coordinates with y down. Shapes are emitted as
// triangle geometry rather than handed to SDL's rectangle calls, because a web
// stylesheet needs rounded corners, elliptical arcs, dashes, gradients, and an
// arbitrary 2D transform, and none of those exist as SDL primitives.
//
// A non-flat fill needs no offscreen buffer: every shape the engine draws is
// convex, so it can be triangulated as a fan and given a colour per vertex.
// Linear gradients are affine and therefore exact that way; radial ones are
// approximated closely enough by the same fan, since the triangles are small.
#pragma once

#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>

#include <cstdint>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace sui {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    friend bool operator==(Color a, Color b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }
};

// Straight (non-premultiplied) alpha, as the DOM uses.
Color withOpacity(Color color, float opacity);

// Multiplies the alpha of two colours, which is how a group's `opacity` reaches
// every drawing command inside it.
Color multiplyAlpha(Color color, float factor);

// The style layer stores SDL colours and the renderer draws `Color`, so every value
// crosses that boundary in one place.
Color toColor(SDL_Color color);

// Two radii per corner, so `border-radius: 10px 20px / 30px 40px` is expressible.
struct Radii {
    struct Corner {
        float x = 0.0f;
        float y = 0.0f;
    };
    Corner topLeft;
    Corner topRight;
    Corner bottomRight;
    Corner bottomLeft;

    bool any() const;
    bool uniform() const;

    // CSS reduces over-long radii by the smallest amount that stops neighbouring
    // corners overlapping, which is what keeps a 40px radius inside a 30px box.
    Radii clamped(float width, float height) const;

    static Radii all(float x, float y);
};

enum class Dash {
    Solid,
    Dashed,
    Dotted,
};

enum class Side : std::uint8_t {
    Top,
    Right,
    Bottom,
    Left,
};

// `border-style` is a per-side property, so the four edges can disagree. `all` is
// the shorthand for the common case where they do not.
struct SideDashes {
    Dash top = Dash::Solid;
    Dash right = Dash::Solid;
    Dash bottom = Dash::Solid;
    Dash left = Dash::Solid;

    bool uniform() const {
        return top == right && right == bottom && bottom == left;
    }
    Dash of(Side side) const {
        switch (side) {
            case Side::Top:
                return top;
            case Side::Right:
                return right;
            case Side::Bottom:
                return bottom;
            case Side::Left:
                return left;
        }
        return Dash::Solid;
    }
    static SideDashes all(Dash dash) { return SideDashes{dash, dash, dash, dash}; }
};

struct GradientStop {
    float offset = 0.0f;  // 0..1
    Color color;
};

// The gradient line in the same space as the shape being filled. An `angle` and a
// `side`/`corner` keyword are resolved into these by the paint stage.
struct LinearGradient {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    std::vector<GradientStop> stops;
};

// The ending shape of a radial gradient, which CSS lets be a circle or an ellipse
// and sizes to `closest-side` or `farthest-corner` of the box.
struct RadialGradient {
    float cx = 0.0f;
    float cy = 0.0f;
    float radiusX = 0.0f;
    float radiusY = 0.0f;
    float focalX = 0.0f;  // the focus; equal to cx/cy when the gradient is concentric
    float focalY = 0.0f;
    std::vector<GradientStop> stops;
};

// A conic sweep around a centre: `fromAngle` starts it clockwise from twelve
// o'clock, and `radius` hints how fast the field bends for the mesh step.
struct ConicalGradient {
    float cx = 0.0f;
    float cy = 0.0f;
    float fromAngle = 0.0f;
    float radius = 0.0f;
    std::vector<GradientStop> stops;
};

enum class Repeat {
    No,
    Tile,
    TileX,
    TileY,
};

// What a fill is made of. Textures are owned by whoever decoded them; the renderer
// only draws them.
class Shader {
public:
    static Shader solid(Color color);
    static Shader linear(const LinearGradient& gradient);
    static Shader radial(const RadialGradient& gradient);
    static Shader conical(const ConicalGradient& gradient);
    static Shader image(SDL_Texture* texture, const SDL_FRect& source,
                        const SDL_FRect& destination, Repeat repeat, Color tint);

    bool isSolid() const { return kind_ == Kind::Solid; }
    bool isImage() const { return kind_ == Kind::Image; }

    // Whether the colour needs a fine mesh rather than the outline's own corners.
    // A gradient is only affine in the vertex position when it has one ramp, so a
    // multi-stop ramp has to be sampled along the edges too.
    bool needsMesh() const;

    // How finely the mesh has to be cut to keep the interpolation error invisible.
    // A radial gradient bends twice as fast as its radius is small, so the step
    // follows the radius rather than being fixed.
    float meshStep() const;

    // The colour this shader shows at a point, in window coordinates.
    Color colorAt(float x, float y) const;

    SDL_Texture* texture() const { return texture_; }
    const SDL_FRect& source() const { return source_; }
    const SDL_FRect& destination() const { return destination_; }
    Repeat repeat() const { return repeat_; }
    Color tint() const { return tint_; }

private:
    enum class Kind { Solid, Linear, Radial, Conical, Image };
    Kind kind_ = Kind::Solid;
    Color solid_{};
    LinearGradient linear_{};
    RadialGradient radial_{};
    ConicalGradient conical_{};
    SDL_Texture* texture_ = nullptr;
    SDL_FRect source_{};
    SDL_FRect destination_{};
    Repeat repeat_ = Repeat::No;
    Color tint_{255, 255, 255, 255};
};

class Renderer {
public:
    explicit Renderer(SDL_Renderer* renderer) : renderer_(renderer) {}

    // Fills.
    void fillRect(const SDL_FRect& rect, const Color& color);
    void fillRect(const SDL_FRect& rect, const Shader& shader, const Radii& radii = {});
    void fillPolygon(const std::vector<SDL_FPoint>& points, const Color& color);
    void fillPolygon(const std::vector<SDL_FPoint>& points, const Shader& shader);
    void fillCircle(float cx, float cy, float radius, const Color& color);

    // Strokes. The band runs inward from the path, not centred on it, because that is
    // where a CSS border sits: inside the border box, taking space from the padding.
    void strokeRect(const SDL_FRect& rect, const Color& color, float thickness = 1.0f,
                    const Radii& radii = {}, const SideDashes& dashes = {});
    void strokePath(const std::vector<SDL_FPoint>& points, const Color& color,
                    float thickness = 1.0f, bool closed = false, Dash dash = Dash::Solid);
    void line(float x1, float y1, float x2, float y2, const Color& color,
              float thickness = 1.0f);

    // An axis-aligned image with no rounding, which is the common `background-image`
    // and `<img>` case and is worth a direct blit.
    void drawTexture(SDL_Texture* texture, const SDL_FRect& destination,
                     const SDL_FRect* source, Color tint);

    // Clipping. Clips nest and intersect; the renderer restores them.
    void pushClip(const SDL_FRect& rect);
    void popClip();

    SDL_Renderer* sdl() const { return renderer_; }

    void strokeSide(const SDL_FRect& rect, Side side, const Color& color, float thickness,
                    const Radii& radii, Dash dash);

    // Exposed so layout-adjacent code and tests can reason about the geometry
    // without a renderer.
    static std::vector<SDL_FPoint> roundedOutline(const SDL_FRect& rect, const Radii& radii,
                                                  int samplesPerCorner = 8);

    // The same outline, but always with the arcs sampled even where a radius is zero.
    // A ring zips an outer outline against an inner one point for point, so the two
    // have to agree on how many points that is; a zero radius collapsing to a single
    // corner would otherwise leave the pair unmatched.
    static std::vector<SDL_FPoint> sampledOutline(const SDL_FRect& rect, const Radii& radii,
                                                  int samplesPerCorner = 8);

    static std::vector<SDL_FPoint> fourCorners(const SDL_FRect& rect);

    // One side's share of that outline: the straight edge plus the half of each
    // corner arc that touches it. The four sides partition the outline, so drawing
    // them one at a time covers every pixel exactly once.
    static std::vector<SDL_FPoint> sideOutline(const SDL_FRect& rect, const Radii& radii,
                                               Side side, int samplesPerQuarter = 4);

private:
    void emit(const std::vector<SDL_FPoint>& points, const Shader& shader);
    void emitFan(const std::vector<SDL_FPoint>& points, const Shader& shader, SDL_FPoint apex);
    void emitMesh(const std::vector<SDL_FPoint>& polygon, const Shader& shader);
    void emitRing(const std::vector<SDL_FPoint>& outer, const std::vector<SDL_FPoint>& inner,
                  const Color& color);
    // `interior` is a point known to lie inside the shape; it decides which way the
    // band extends from the path.
    void emitDashed(const std::vector<SDL_FPoint>& outline, float thickness, const Color& color,
                    Dash dash, bool closed, const SDL_FPoint& interior);
    void emitSide(const SDL_FRect& rect, Side side, float thickness, const Color& color,
                  const Radii& radii, Dash dash);
    void setColor(const Color& color);

    SDL_Renderer* renderer_ = nullptr;
    std::vector<SDL_Rect> clipStack_;
    SDL_Rect current_{0, 0, 0, 0};
    bool clipping_ = false;
    // `emitDashed` draws one kind of dash and the caller says which, which is cheaper
    // than duplicating the walk.
    // Scratch for the mesh, kept across frames so a gradient does not reallocate a
    // few thousand vertices every frame.
    std::vector<SDL_Vertex> meshVertices_;
    std::vector<int> meshIndices_;
    std::vector<SDL_FPoint> clipFront_;
    std::vector<SDL_FPoint> clipBack_;
};

}  // namespace sui

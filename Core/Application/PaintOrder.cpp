#include "Core/Application/PaintOrder.h"

#include "CSS/StyleResolver.h"
#include "Rendering/ColorMatrix.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace sui {
namespace {
struct HitAffine {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;
};
inline HitAffine hitIdentity() { return HitAffine{1,0,0,1,0,0}; }
inline HitAffine hitCompose(const HitAffine& o, const HitAffine& i) {
    return HitAffine{o.a*i.a+o.c*i.b, o.b*i.a+o.d*i.b, o.a*i.c+o.c*i.d, o.b*i.c+o.d*i.d, o.a*i.e+o.c*i.f+o.e, o.b*i.e+o.d*i.f+o.f};
}
struct HitCtx {
    SDL_FRect clip{0,0,0,0};
    HitAffine transform{1,0,0,1,0,0};
    // Under a flattened-3D group the affine above maps window points into the
    // group's parent frame, `quad`/`quadRect` map that frame back onto the
    // surface rect, and `inner` maps onward through any 2D groups below.
    bool proj = false;
    ProjectedQuad quad{};
    SDL_FRect quadRect{0.0f, 0.0f, 0.0f, 0.0f};
    HitAffine inner{1,0,0,1,0,0};
};
inline HitCtx hitBase() { return HitCtx{{0,0,0,0},{1,0,0,1,0,0}}; }
inline bool hitPointIn(const SDL_FRect& r,float x,float y){ if(r.w<=0||r.h<=0)return true; return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h; }
inline bool hitToLocal(const HitAffine& m,float x,float y,float* lx,float* ly){ float det=m.a*m.d-m.b*m.c; if(std::fabs(det)<1e-18f)return false; float dx=x-m.e,dy=y-m.f; *lx=(m.d*dx-m.c*dy)/det; *ly=(-m.b*dx+m.a*dy)/det; return true; }
inline void hitApply(const HitAffine& m,float x,float y,float* px,float* py){ *px=m.a*x+m.c*y+m.e; *py=m.b*x+m.d*y+m.f; }
inline bool hitBox(const HitCtx& ctx,const PaintItem& item,float x,float y){ if(!hitPointIn(item.clip,x,y))return false; float lx,ly; if(ctx.proj){ if(!unprojectPoint(ctx.quad,ctx.quadRect.x,ctx.quadRect.y,ctx.quadRect.w,ctx.quadRect.h,x,y,lx,ly))return false; if(!hitToLocal(ctx.inner,lx,ly,&lx,&ly))return false; } else if(!hitToLocal(ctx.transform,x,y,&lx,&ly))return false; float bx=item.el->layoutX()-item.dx,by=item.el->layoutY()-item.dy,bw=item.el->layoutWidth(),bh=item.el->layoutHeight(); if(lx<bx||lx>=bx+bw||ly<by||ly>=by+bh)return false; if(item.clipShape.kind!=ResolvedClip::Kind::None&&!clipContains(item.clipShape,x,y))return false; return true; }
} // namespace

namespace {

// The environment a subtree paints under: the tightest clip its clipping
// ancestors impose, the scroll and translation offsets that move it on screen,
// and the group alpha laid over it.
struct PaintEnv {
    SDL_FRect clip{0.0f, 0.0f, 0.0f, 0.0f};
    float dx = 0.0f;
    float dy = 0.0f;
    float opacity = 1.0f;
    // The `perspective` length funnelling children's 3D through a viewer;
    // 0 is `none`.
    float perspective = 0.0f;
};

// How an element's descendants are ordered relative to its own box. CSS splits
// the stacking contexts it opens into two kinds, and the difference decides
// whether a positioned descendant may climb out of the group.
enum class GroupKind {
    None,    // no group: descendants are ordered around the box, in the context found in
    Real,    // a true context: everything below it stays inside
    Pseudo   // `position` with `z-index: auto`: in-flow content stays inside,
             // but positioned descendants rejoin the context around it
};

// One entry in a context's list: the element's own box, plus the subgroup it
// opens when it establishes a context. Only a group root fills `inner`, and it is
// emitted -- box first, then everything the group orders -- the moment that
// group's turn comes.
struct PaintEntry {
    PaintItem item;
    bool group = false;
    // The alpha a composited group blends its offscreen surface back with: the
    // root's `opacity` times any `filter: opacity()` in its chain. 1 means the
    // group needs no surface and its box and contents stay in the flat list.
    float groupAlpha = 1.0f;
    // When the group root carries a scale() or rotate(), the surface is mapped
    // to the screen by the affine `m[6]` about the resolved transform-origin
    // point `origin` (in window coordinates). A flattened 3D transform rides
    // `mat3d`/`persp3d`/`originZ` instead (see `projectQuad`).
    bool transformed = false;
    float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    bool projective = false;
    float mat3d[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                       0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    float persp3d = 0.0f;
    float originZ = 0.0f;
    SDL_FPoint origin{0.0f, 0.0f};
    // A `filter: blur(x)` radius in px; forces a surface so the executor can
    // blur the readback.
    float blur = 0.0f;
    // A `filter:` colour-shift chain forces a surface too, and hands the
    // composed affine matrix the executor applies to the readback.
    bool shift = false;
    float filter[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    // A `filter: drop-shadow()` silhouette to blit under the content; forces a
    // surface so the executor can trace the group's own shape.
    DropShadowFilter dropShadow{};
    // A `mix-blend-mode` compositing the group against its backdrop.
    ComputedStyle::MixBlendMode blend = ComputedStyle::MixBlendMode::Normal;
    // `isolation: isolate`: the group opens even when nothing else about it
    // composites, so its descendants blend with its content alone.
    bool isolated = false;
    std::vector<PaintEntry> inner;
};

SDL_FRect intersectClip(const SDL_FRect& a, const SDL_FRect& b) {
    if (a.w <= 0.0f || a.h <= 0.0f) {
        return b;
    }
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return a;
    }
    const float x = std::max(a.x, b.x);
    const float y = std::max(a.y, b.y);
    return {x, y, std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - x),
            std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - y)};
}


// A 2D affine map, x' = a*x + c*y + e, y' = b*x + d*y + f. Enough for the
// translate/scale/rotate a CSS `transform` can express; the translation part
// lives in e/f so list ordering stays exact (a translate written before a
// rotate rotates the shifted box about the origin, one written after rotates
// first).
struct Affine {
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    float e = 0.0f;
    float f = 0.0f;
};

// `outer` carried out after `inner`: the matrix product outer . inner.
Affine combine(const Affine& outer, const Affine& inner) {
    return {outer.a * inner.a + outer.c * inner.b,
            outer.b * inner.a + outer.d * inner.b,
            outer.a * inner.c + outer.c * inner.d,
            outer.b * inner.c + outer.d * inner.d,
            outer.a * inner.e + outer.c * inner.f + outer.e,
            outer.b * inner.e + outer.d * inner.f + outer.f};
}

// Parses a CSS `transform` list. `translate()` and its single-axis forms can
// keep folding into the offset the painter already threads through the tree
// (handed back in `tx`/`ty`), while every function -- including the translate
// -- accumulates into `affine` in CSS order (multiplied left to right, so the
// rightmost function reaches a point first) for groups that need a surface.
// Returns whether a matrix worth carrying landed: false when the list has no
// scale/rotate, or its product degenerated to the identity (a lone
// `rotate(0deg)`). A percentage axis in `translate()` is left at zero because
// it resolves against the box and is the rarer spelling.
bool transformEffects(std::string_view transform, Affine& affine, float& tx, float& ty,
                      float boxW = 0.0f, float boxH = 0.0f) {
    Affine acc;
    bool any = false;
    constexpr float kPi = 3.14159265f;
    std::size_t at = 0;
    while (at < transform.size()) {
        const std::size_t open = transform.find('(', at);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t close = transform.find(')', open);
        if (close == std::string_view::npos) {
            break;
        }
        std::size_t nameStart = open;
        while (nameStart > 0 &&
               (std::isalnum(static_cast<unsigned char>(transform[nameStart - 1])) ||
                transform[nameStart - 1] == '-')) {
            --nameStart;
        }
        const std::string_view name = transform.substr(nameStart, open - nameStart);
        const std::string_view body = transform.substr(open + 1, close - open - 1);
        auto number = [](std::string_view piece, float& out) {
            const std::string owned(piece);
            char* stop = nullptr;
            const float value = std::strtof(owned.c_str(), &stop);
            if (stop != owned.c_str() && stop[0] != '%') {
                out = value;
            }
        };
        auto numberOrPercent = [](std::string_view piece, float ref, float& out) {
            const std::string owned(piece);
            char* stop = nullptr;
            const float value = std::strtof(owned.c_str(), &stop);
            if (stop != owned.c_str()) {
                if (stop[0] == '%') {
                    out = value * 0.01f * ref;
                } else {
                    out = value;
                }
            }
        };
        auto angleToRad = [&](std::string_view piece) -> float {
            const std::string owned(piece);
            char* stop = nullptr;
            const float value = std::strtof(owned.c_str(), &stop);
            if (stop == owned.c_str()) return 0.0f;
            while (*stop && std::isspace(static_cast<unsigned char>(*stop))) ++stop;
            const std::string_view unit(stop);
            if (unit.find("rad") != std::string_view::npos) return value;
            if (unit.find("turn") != std::string_view::npos) return value * 2.0f * kPi;
            if (unit.find("grad") != std::string_view::npos) return value * kPi / 200.0f;
            return value * kPi / 180.0f;
        };
        if (name == "translate" || name == "translateX" || name == "translateY") {
            const std::size_t comma = body.find(',');
            const std::string_view first = body.substr(0, comma);
            const std::string_view second =
                comma == std::string_view::npos ? std::string_view() : body.substr(comma + 1);
            float x = 0.0f, y = 0.0f;
            numberOrPercent(first, boxW, x);
            numberOrPercent(second, boxH, y);
            if (name == "translateY") {
                y = x;
                x = 0.0f;
            } else if (name == "translateX") {
                y = 0.0f;
            }
            tx += x;
            ty += y;
            acc = combine(acc, Affine{1.0f, 0.0f, 0.0f, 1.0f, x, y});
        } else if (name == "scale" || name == "scaleX" || name == "scaleY") {
            float sx = 1.0f, sy = 1.0f;
            if (name == "scaleX") {
                number(body, sx);
            } else if (name == "scaleY") {
                number(body, sy);
            } else {
                const std::size_t comma = body.find(',');
                number(body.substr(0, comma), sx);
                if (comma != std::string_view::npos) {
                    number(body.substr(comma + 1), sy);
                } else {
                    sy = sx;
                }
            }
            acc = combine(acc, Affine{sx, 0.0f, 0.0f, sy, 0.0f, 0.0f});
            any = true;
        } else if (name == "rotate") {
            const float rad = angleToRad(body);
            acc = combine(acc,
                          Affine{std::cos(rad), std::sin(rad), -std::sin(rad), std::cos(rad), 0.0f,
                                 0.0f});
            any = true;
        } else if (name == "skew" || name == "skewX" || name == "skewY") {
            float ax = 0.0f, ay = 0.0f;
            if (name == "skewX") {
                ax = angleToRad(body);
            } else if (name == "skewY") {
                ay = angleToRad(body);
            } else {
                const std::size_t comma = body.find(',');
                ax = angleToRad(body.substr(0, comma));
                if (comma != std::string_view::npos) {
                    ay = angleToRad(body.substr(comma + 1));
                }
            }
            acc = combine(acc, Affine{1.0f, std::tan(ay), std::tan(ax), 1.0f, 0.0f, 0.0f});
            any = true;
        } else if (name == "matrix") {
            std::vector<std::string_view> parts;
            std::size_t pstart = 0;
            for (std::size_t pi = 0; pi <= body.size(); ++pi) {
                if (pi == body.size() || body[pi] == ',') {
                    parts.push_back(body.substr(pstart, pi - pstart));
                    pstart = pi + 1;
                }
            }
            if (parts.size() == 6) {
                float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;
                number(parts[0], a);
                number(parts[1], b);
                number(parts[2], c);
                number(parts[3], d);
                numberOrPercent(parts[4], boxW, e);
                numberOrPercent(parts[5], boxH, f);
                acc = combine(acc, Affine{a, b, c, d, e, f});
                any = true;
            }
        }
        at = close + 1;
    }
    affine = acc;
    if (!any) {
        return false;
    }
    constexpr float kEps = 1e-5f;
    return std::fabs(affine.a - 1.0f) > kEps || std::fabs(affine.b) > kEps ||
           std::fabs(affine.c) > kEps || std::fabs(affine.d - 1.0f) > kEps ||
           std::fabs(affine.e) > kEps || std::fabs(affine.f) > kEps;
}

// A 4x4 row-major matrix: p' = M . p on column vectors. Only `transform`
// lists that use 3D functions take this path; pure-2D lists stay on the
// affine one above, bit for bit.
struct Mat4 {
    float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

// `outer` carried out after `inner`: the matrix product outer . inner.
Mat4 combine4(const Mat4& outer, const Mat4& inner) {
    Mat4 out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r * 4 + c] = outer.m[r * 4] * inner.m[c] +
                               outer.m[r * 4 + 1] * inner.m[4 + c] +
                               outer.m[r * 4 + 2] * inner.m[8 + c] +
                               outer.m[r * 4 + 3] * inner.m[12 + c];
        }
    }
    return out;
}

Mat4 translate4(float x, float y, float z) {
    Mat4 out;
    out.m[3] = x;
    out.m[7] = y;
    out.m[11] = z;
    return out;
}

Mat4 scale4(float x, float y, float z) {
    Mat4 out;
    out.m[0] = x;
    out.m[5] = y;
    out.m[10] = z;
    return out;
}

Mat4 rotateX4(float rad) {
    Mat4 out;
    const float c = std::cos(rad), s = std::sin(rad);
    out.m[5] = c;
    out.m[6] = -s;
    out.m[9] = s;
    out.m[10] = c;
    return out;
}

Mat4 rotateY4(float rad) {
    Mat4 out;
    const float c = std::cos(rad), s = std::sin(rad);
    out.m[0] = c;
    out.m[2] = s;
    out.m[8] = -s;
    out.m[10] = c;
    return out;
}

Mat4 rotateZ4(float rad) {
    Mat4 out;
    const float c = std::cos(rad), s = std::sin(rad);
    out.m[0] = c;
    out.m[1] = -s;
    out.m[4] = s;
    out.m[5] = c;
    return out;
}

// Axis-angle rotation (CSS `rotate3d`): a zero axis is the identity.
Mat4 rotateAxis4(float x, float y, float z, float rad) {
    Mat4 out;
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len <= 0.0f) {
        return out;
    }
    x /= len;
    y /= len;
    z /= len;
    const float c = std::cos(rad), s = std::sin(rad), t = 1.0f - c;
    out.m[0] = t * x * x + c;
    out.m[1] = t * x * y - s * z;
    out.m[2] = t * x * z + s * y;
    out.m[4] = t * x * y + s * z;
    out.m[5] = t * y * y + c;
    out.m[6] = t * y * z - s * x;
    out.m[8] = t * x * z - s * y;
    out.m[9] = t * y * z + s * x;
    out.m[10] = t * z * z + c;
    return out;
}

// CSS `perspective(d)`: points divide by 1 - z/d, so nearer z looms larger.
Mat4 perspective4(float d) {
    Mat4 out;
    out.m[14] = -1.0f / d;
    return out;
}

float angleRad(std::string_view piece) {
    constexpr float kPi = 3.14159265f;
    const std::string owned(piece);
    char* stop = nullptr;
    const float value = std::strtof(owned.c_str(), &stop);
    if (stop == owned.c_str()) {
        return 0.0f;
    }
    while (*stop && std::isspace(static_cast<unsigned char>(*stop))) {
        ++stop;
    }
    const std::string_view unit(stop);
    if (unit.find("rad") != std::string_view::npos) {
        return value;
    }
    if (unit.find("turn") != std::string_view::npos) {
        return value * 2.0f * kPi;
    }
    if (unit.find("grad") != std::string_view::npos) {
        return value * kPi / 200.0f;
    }
    return value * kPi / 180.0f;
}

bool numberPiece(std::string_view piece, float& out) {
    const std::string owned(piece);
    char* stop = nullptr;
    const float value = std::strtof(owned.c_str(), &stop);
    if (stop == owned.c_str()) {
        return false;
    }
    out = value;
    return true;
}

// Comma-separated floats, trimmed; false unless exactly `count` parse.
bool numberList(std::string_view body, float* out, std::size_t count) {
    std::size_t found = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= body.size(); ++i) {
        if (i == body.size() || body[i] == ',') {
            if (found >= count) {
                return false;
            }
            std::string_view piece = body.substr(start, i - start);
            while (!piece.empty() &&
                   std::isspace(static_cast<unsigned char>(piece.front()))) {
                piece.remove_prefix(1);
            }
            while (!piece.empty() &&
                   std::isspace(static_cast<unsigned char>(piece.back()))) {
                piece.remove_suffix(1);
            }
            if (!numberPiece(piece, out[found])) {
                return false;
            }
            ++found;
            start = i + 1;
        }
    }
    return found == count;
}

// The comma parts of a function body, trimmed. Units stay attached: lengths
// like `10px` keep their suffix, which is what tells a z length from a number.
std::vector<std::string_view> commaParts(std::string_view body) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= body.size(); ++i) {
        if (i == body.size() || body[i] == ',') {
            std::string_view piece = body.substr(start, i - start);
            while (!piece.empty() &&
                   std::isspace(static_cast<unsigned char>(piece.front()))) {
                piece.remove_prefix(1);
            }
            while (!piece.empty() &&
                   std::isspace(static_cast<unsigned char>(piece.back()))) {
                piece.remove_suffix(1);
            }
            parts.push_back(piece);
            start = i + 1;
        }
    }
    return parts;
}

// A bare number that may trail a length unit (`10px` reads 10): the 2D list's
// leniency, for the slots where CSS wants a plain number but authors write px.
bool looseNumber(std::string_view piece, float& out) {
    const std::string owned(piece);
    char* stop = nullptr;
    const float value = std::strtof(owned.c_str(), &stop);
    if (stop == owned.c_str() || *stop == '%') {
        return false;
    }
    out = value;
    return true;
}

// Parses a CSS `transform` list into a 4x4, handling the 2D functions as
// z=0-plane matrices too. Returns true when the list needs the projective
// path: any 3D function or `perspective()`. A non-positive `perspective()`
// invalidates the whole list (identity, false, invalid set): pure-2D lists
// stay on the affine path even under a parent perspective, whose z=0 plane
// they never leave.
bool transformEffects3D(std::string_view transform, Mat4& mat, float boxW, float boxH,
                        bool& invalid) {
    Mat4 acc;
    bool dimensional = false;
    invalid = false;
    std::size_t at = 0;
    const auto lengthOrPercent = [](std::string_view piece, float ref) {
        const std::string owned(piece);
        char* stop = nullptr;
        const float value = std::strtof(owned.c_str(), &stop);
        if (stop == owned.c_str()) {
            return 0.0f;
        }
        return stop[0] == '%' ? value * 0.01f * ref : value;
    };
    while (at < transform.size()) {
        const std::size_t open = transform.find('(', at);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t close = transform.find(')', open);
        if (close == std::string_view::npos) {
            break;
        }
        std::size_t nameStart = open;
        while (nameStart > 0 &&
               (std::isalnum(static_cast<unsigned char>(transform[nameStart - 1])) ||
                transform[nameStart - 1] == '-')) {
            --nameStart;
        }
        const std::string_view name = transform.substr(nameStart, open - nameStart);
        const std::string_view body = transform.substr(open + 1, close - open - 1);
        const std::size_t comma = body.find(',');
        const std::string_view first = body.substr(0, comma);
        const std::string_view second =
            comma == std::string_view::npos ? std::string_view() : body.substr(comma + 1);
        if (name == "translate" || name == "translateX" || name == "translateY") {
            float x = lengthOrPercent(first, boxW);
            float y = comma == std::string_view::npos ? 0.0f : lengthOrPercent(second, boxH);
            if (name == "translateY") {
                y = x;
                x = 0.0f;
            } else if (name == "translateX") {
                y = 0.0f;
            }
            acc = combine4(acc, translate4(x, y, 0.0f));
        } else if (name == "translateZ") {
            float z = 0.0f;
            looseNumber(body, z);
            acc = combine4(acc, translate4(0.0f, 0.0f, z));
            dimensional = true;
        } else if (name == "translate3d") {
            const std::vector<std::string_view> parts = commaParts(body);
            if (parts.size() == 3) {
                // Percentages in x/y resolve against the box, like the 2D path.
                float z = 0.0f;
                looseNumber(parts[2], z);
                acc = combine4(acc, translate4(lengthOrPercent(parts[0], boxW),
                                               lengthOrPercent(parts[1], boxH), z));
                dimensional = true;
            }
        } else if (name == "scale" || name == "scaleX" || name == "scaleY") {
            float sx = 1.0f, sy = 1.0f;
            if (name == "scaleX") {
                looseNumber(body, sx);
            } else if (name == "scaleY") {
                looseNumber(body, sy);
            } else {
                looseNumber(first, sx);
                if (comma != std::string_view::npos) {
                    looseNumber(second, sy);
                } else {
                    sy = sx;
                }
            }
            acc = combine4(acc, scale4(sx, sy, 1.0f));
        } else if (name == "scaleZ") {
            float sz = 1.0f;
            looseNumber(body, sz);
            acc = combine4(acc, scale4(1.0f, 1.0f, sz));
            dimensional = true;
        } else if (name == "scale3d") {
            const std::vector<std::string_view> parts = commaParts(body);
            if (parts.size() == 3) {
                float sx = 1.0f, sy = 1.0f, sz = 1.0f;
                looseNumber(parts[0], sx);
                looseNumber(parts[1], sy);
                looseNumber(parts[2], sz);
                acc = combine4(acc, scale4(sx, sy, sz));
                dimensional = true;
            }
        } else if (name == "rotate" || name == "rotateZ") {
            acc = combine4(acc, rotateZ4(angleRad(body)));
        } else if (name == "rotateX") {
            acc = combine4(acc, rotateX4(angleRad(body)));
            dimensional = true;
        } else if (name == "rotateY") {
            acc = combine4(acc, rotateY4(angleRad(body)));
            dimensional = true;
        } else if (name == "rotate3d") {
            // The angle carries a unit, so only the axis goes through the
            // comma reader; the tail parses as an angle.
            std::size_t splits[3] = {0, 0, 0};
            std::size_t found = 0;
            for (std::size_t i = 0; i < body.size() && found < 3; ++i) {
                if (body[i] == ',') {
                    splits[found++] = i;
                }
            }
            if (found == 3) {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                if (numberPiece(body.substr(0, splits[0]), x) &&
                    numberPiece(body.substr(splits[0] + 1, splits[1] - splits[0] - 1), y) &&
                    numberPiece(body.substr(splits[1] + 1, splits[2] - splits[1] - 1), z)) {
                    acc = combine4(acc, rotateAxis4(x, y, z,
                                                    angleRad(body.substr(splits[2] + 1))));
                    dimensional = true;
                }
            }
        } else if (name == "skew" || name == "skewX" || name == "skewY") {
            float ax = 0.0f, ay = 0.0f;
            if (name == "skewX") {
                ax = angleRad(body);
            } else if (name == "skewY") {
                ay = angleRad(body);
            } else {
                ax = angleRad(first);
                if (comma != std::string_view::npos) {
                    ay = angleRad(second);
                }
            }
            Mat4 skew;
            skew.m[1] = std::tan(ax);
            skew.m[4] = std::tan(ay);
            acc = combine4(acc, skew);
        } else if (name == "matrix") {
            // The 2D leniency: a-f read bare numbers, e/f resolve percentages.
            const std::vector<std::string_view> parts = commaParts(body);
            if (parts.size() == 6) {
                float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
                looseNumber(parts[0], a);
                looseNumber(parts[1], b);
                looseNumber(parts[2], c);
                looseNumber(parts[3], d);
                const float e = lengthOrPercent(parts[4], boxW);
                const float f = lengthOrPercent(parts[5], boxH);
                Mat4 item;
                item.m[0] = a;
                item.m[1] = c;
                item.m[3] = e;
                item.m[4] = b;
                item.m[5] = d;
                item.m[7] = f;
                acc = combine4(acc, item);
            }
        } else if (name == "matrix3d") {
            float column[16];
            if (numberList(body, column, 16)) {
                Mat4 item;
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        item.m[r * 4 + c] = column[c * 4 + r];
                    }
                }
                acc = combine4(acc, item);
                dimensional = true;
            }
        } else if (name == "perspective") {
            float d = 0.0f;
            looseNumber(body, d);
            if (!(d > 0.0f)) {
                invalid = true;
                mat = Mat4();
                return false;
            }
            acc = combine4(acc, perspective4(d));
            dimensional = true;
        }
        at = close + 1;
    }
    mat = acc;
    return dimensional;
}

// Projects one surface point through a flattened 3D transform: q = M . (p - O)
// + O, w-divided, then scaled about the origin's x/y by the viewer distance.
// Returns the screen point and whether it fell behind the viewer. Identity
// matrices map every point onto itself whatever the origin holds.
SDL_FPoint projectPoint(const float mat[16], float perspective, float ox, float oy,
                        float oz, float x, float y, bool& behind) {
    const float px = x - ox, py = y - oy, pz = -oz;
    const float vx = mat[0] * px + mat[1] * py + mat[2] * pz + mat[3];
    const float vy = mat[4] * px + mat[5] * py + mat[6] * pz + mat[7];
    const float vz = mat[8] * px + mat[9] * py + mat[10] * pz + mat[11];
    const float vw = mat[12] * px + mat[13] * py + mat[14] * pz + mat[15];
    behind = false;
    if (!(vw > 1e-6f)) {
        // Behind the camera (or edge-on): visible, collapsed onto the origin.
        behind = true;
        return SDL_FPoint{ox, oy};
    }
    const float qx = vx / vw + ox;
    const float qy = vy / vw + oy;
    const float qz = vz / vw + oz;
    if (perspective > 0.0f) {
        const float denom = perspective - qz;
        if (!(denom > 1e-6f)) {
            behind = true;
            return SDL_FPoint{ox, oy};
        }
        // A point rushing at the viewer looms, but never past a sane bound.
        float scale = perspective / denom;
        scale = std::min(std::max(scale, -64.0f), 64.0f);
        return SDL_FPoint{ox + scale * (qx - ox), oy + scale * (qy - oy)};
    }
    return SDL_FPoint{qx, qy};
}

// Solves the rect-to-quad homography by direct linear transform (8 equations,
// h[8] = 1), for hit-testing back through a projected group.
bool rectToQuadHomo(float rectX, float rectY, float rectW, float rectH,
                    const SDL_FPoint quad[4], float h[9]) {
    if (rectW <= 0.0f || rectH <= 0.0f) {
        return false;
    }
    // Source corners in unit space pair with quad corners in order.
    const float u[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    const float v[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float a[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        const float x = quad[i].x, y = quad[i].y;
        a[2 * i][0] = -u[i];
        a[2 * i][1] = -v[i];
        a[2 * i][2] = -1.0f;
        a[2 * i][6] = x * u[i];
        a[2 * i][7] = x * v[i];
        a[2 * i][8] = -x;
        a[2 * i + 1][3] = -u[i];
        a[2 * i + 1][4] = -v[i];
        a[2 * i + 1][5] = -1.0f;
        a[2 * i + 1][6] = y * u[i];
        a[2 * i + 1][7] = y * v[i];
        a[2 * i + 1][8] = -y;
    }
    // Partial-pivot Gauss on 8 equations in h[0..7].
    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 8; ++row) {
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) {
                pivot = row;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-12f) {
            return false;
        }
        if (pivot != col) {
            for (int k = col; k < 9; ++k) {
                std::swap(a[col][k], a[pivot][k]);
            }
        }
        for (int row = 0; row < 8; ++row) {
            if (row == col) {
                continue;
            }
            const float factor = a[row][col] / a[col][col];
            for (int k = col; k < 9; ++k) {
                a[row][k] -= factor * a[col][k];
            }
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (std::fabs(a[i][i]) < 1e-12f) {
            return false;
        }
        h[i] = a[i][8] / a[i][i];
    }
    h[8] = 1.0f;
    // From rect space back to unit space, then through the solved quad map.
    float s[9] = {1.0f / rectW, 0.0f,         -rectX / rectW,
                  0.0f,         1.0f / rectH, -rectY / rectH,
                  0.0f,         0.0f,         1.0f};
    float out[9] = {};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] =
                h[r * 3] * s[c] + h[r * 3 + 1] * s[3 + c] + h[r * 3 + 2] * s[6 + c];
        }
    }
    for (int i = 0; i < 9; ++i) {
        h[i] = out[i];
    }
    return true;
}

bool homoInverse(const float h[9], float inv[9]) {
    const float det = h[0] * (h[4] * h[8] - h[5] * h[7]) -
                      h[1] * (h[3] * h[8] - h[5] * h[6]) +
                      h[2] * (h[3] * h[7] - h[4] * h[6]);
    if (std::fabs(det) < 1e-18f) {
        return false;
    }
    inv[0] = (h[4] * h[8] - h[5] * h[7]) / det;
    inv[1] = (h[2] * h[7] - h[1] * h[8]) / det;
    inv[2] = (h[1] * h[5] - h[2] * h[4]) / det;
    inv[3] = (h[5] * h[6] - h[3] * h[8]) / det;
    inv[4] = (h[0] * h[8] - h[2] * h[6]) / det;
    inv[5] = (h[2] * h[3] - h[0] * h[5]) / det;
    inv[6] = (h[3] * h[7] - h[4] * h[6]) / det;
    inv[7] = (h[1] * h[6] - h[0] * h[7]) / det;
    inv[8] = (h[0] * h[4] - h[1] * h[3]) / det;
    return true;
}

// An element's value is "set" when it says something other than the initial
// `none`. `transform` and `filter` default to the empty string, but a rule may
// name `none` explicitly, which must not open a group.
bool declares(const std::string& value) {
    return !value.empty() && value != "none";
}

// Classifies the group an element opens. `opacity` under 1, a `transform` and a
// `filter` are unambiguous stacking contexts in CSS. A positioned box with a
// `z-index` other than zero is one too; `z-index: auto` and an explicit `0` both
// read zero here, and CSS treats only the former as *not* a context, so both
// fall into `Pseudo`, which keeps the engine's ordering for the `auto` case and
// differs from CSS only where a positioned descendant would rejoin the context
// around it -- CSS lets it, the engine keeps it inside.
GroupKind classifyGroup(const ComputedStyle& s) {
    if (s.opacity < 1.0f || declares(s.transform) || declares(s.filter) ||
        s.isolate || s.mixBlendMode != ComputedStyle::MixBlendMode::Normal) {
        return GroupKind::Real;
    }
    // A `clip-path` composites its subtree through a surface to mask it, so it
    // groups for real: descendants must not escape above the mask. A mask with
    // applicable layers, or a border mask, does the same for its own alpha pass.
    if (s.clipPath.shape != ComputedStyle::ClipPath::Shape::None ||
        hasMaskSurface(s.maskImages) || hasMaskBorderSurface(s.maskBorder)) {
        return GroupKind::Real;
    }
    if (s.positioned) {
        return s.zIndex != 0 ? GroupKind::Real : GroupKind::Pseudo;
    }
    return GroupKind::None;
}

PaintOp boxOp(const PaintItem& item) {
    return PaintOp{PaintOp::Kind::Box, item, {}, 1.0f};
}

// The BeginGroup op an entry opens: carries the alpha to blend the surface back
// with and, when the group root transforms, the affine mapping the surface to
// the screen about the resolved transform-origin point. `blur` hands the
// executor a `filter: blur()` radius to apply to the readback; `shift` hands it
// a composed colour-shift matrix when the filter chain has one; `dropShadow`
// hands it a `filter: drop-shadow()` silhouette to blit under the content.
PaintOp groupStart(const PaintItem& item, float alpha, bool transformed, const float m[6],
                   SDL_FPoint origin, float blur, bool shift, const float filter[12],
                   const DropShadowFilter& dropShadow, ComputedStyle::MixBlendMode blend) {
    PaintOp op;
    op.kind = PaintOp::Kind::BeginGroup;
    op.item = item;
    op.alpha = alpha;
    op.transformed = transformed;
    op.origin = origin;
    op.projective = item.projective;
    for (int i = 0; i < 16; ++i) {
        op.mat3d[i] = item.mat3d[i];
    }
    op.persp3d = item.persp3d;
    op.originZ = item.originZ;
    op.blur = blur;
    op.shift = shift;
    op.dropShadow = dropShadow;
    op.blend = blend;
    for (int i = 0; i < 6; ++i) {
        op.m[i] = m[i];
    }
    for (int i = 0; i < 12; ++i) {
        op.filter[i] = filter[i];
    }
    return op;
}

SDL_FRect unionRect(const SDL_FRect& a, const SDL_FRect& b) {
    if (a.w <= 0.0f || a.h <= 0.0f) {
        return b;
    }
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return a;
    }
    const float x = std::min(a.x, b.x);
    const float y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
}

// The reference box a `clip-path` resolves against, in window coordinates:
// the element's own box shifted the way it paints.
SDL_FRect clipReferenceBox(const Element& el, ComputedStyle::ClipPath::Box box,
                           float dx, float dy) {
    switch (box) {
        case ComputedStyle::ClipPath::Box::Margin:
        case ComputedStyle::ClipPath::Box::Border:
            return SDL_FRect{el.layoutX() - dx, el.layoutY() - dy, el.layoutWidth(),
                             el.layoutHeight()};
        case ComputedStyle::ClipPath::Box::Padding:
            return SDL_FRect{el.paddingX() - dx, el.paddingY() - dy, el.paddingWidth(),
                             el.paddingHeight()};
        case ComputedStyle::ClipPath::Box::Content:
            return SDL_FRect{el.contentX() - dx, el.contentY() - dy, el.contentWidth(),
                             el.contentHeight()};
    }
    return SDL_FRect{el.layoutX() - dx, el.layoutY() - dy, el.layoutWidth(),
                     el.layoutHeight()};
}

float clipRadialSize(ComputedStyle::ClipPath::Radial radial, float explicitSize,
                     float centerX, float centerY, float x, float y, float w, float h,
                     bool allSides, bool horizontal) {
    using Radial = ComputedStyle::ClipPath::Radial;
    const float left = centerX - x;
    const float right = x + w - centerX;
    const float top = centerY - y;
    const float bottom = y + h - centerY;
    // A circle reads every side; an ellipse reads its own axis.
    const float nearSide =
        allSides ? std::min(std::min(left, right), std::min(top, bottom))
                 : (horizontal ? std::min(left, right) : std::min(top, bottom));
    const float farSide =
        allSides ? std::max(std::max(left, right), std::max(top, bottom))
                 : (horizontal ? std::max(left, right) : std::max(top, bottom));
    switch (radial) {
        case Radial::Explicit:
            return explicitSize;
        case Radial::ClosestSide:
            return nearSide;
        case Radial::FarthestSide:
            return farSide;
        case Radial::ClosestCorner:
        case Radial::FarthestCorner: {
            const float corners[4] = {
                std::sqrt(left * left + top * top), std::sqrt(right * right + top * top),
                std::sqrt(left * left + bottom * bottom),
                std::sqrt(right * right + bottom * bottom)};
            float picked = corners[0];
            for (float c : corners) {
                picked = radial == Radial::ClosestCorner ? std::min(picked, c)
                                                         : std::max(picked, c);
            }
            return picked;
        }
    }
    return explicitSize;
}

// Resolves a parsed shape against its reference box into surface pixels.
// Lengths resolve the CSS way: x against the width, y against the height,
// radii against the normalized diagonal, em/rem against the font sizes.
ResolvedClip resolveClipPath(const ComputedStyle::ClipPath& clip, const Element& el,
                             float dx, float dy, float fontSize, float rootFontSize) {
    using CP = ComputedStyle::ClipPath;
    ResolvedClip out;
    if (clip.shape == CP::Shape::None) {
        return out;
    }
    const SDL_FRect ref = clipReferenceBox(el, clip.box, dx, dy);
    const auto px = [&](const Length& l, float base) {
        return l.resolve(base, fontSize, rootFontSize);
    };
    if (clip.shape == CP::Shape::Inset) {
        const float top = px(clip.inset[0], ref.h);
        const float right = px(clip.inset[1], ref.w);
        const float bottom = px(clip.inset[2], ref.h);
        const float left = px(clip.inset[3], ref.w);
        out.kind = ResolvedClip::Kind::Rect;
        out.x = ref.x + left;
        out.y = ref.y + top;
        out.w = std::max(0.0f, ref.w - left - right);
        out.h = std::max(0.0f, ref.h - top - bottom);
        const float cornerBase =
            std::min(ref.w, ref.h) > 0.0f ? std::min(ref.w, ref.h) : 0.0f;
        out.radius = std::max(0.0f, px(clip.insetRadius, cornerBase));
        return out;
    }
    const float cx = ref.x + px(clip.centerX, ref.w);
    const float cy = ref.y + px(clip.centerY, ref.h);
    if (clip.shape == CP::Shape::Circle) {
        float radius = clip.radius == CP::Radial::Explicit
                           ? px(clip.radiusLength,
                                std::sqrt(ref.w * ref.w + ref.h * ref.h) /
                                    std::sqrt(2.0f))
                           : clipRadialSize(clip.radius, 0.0f, cx, cy, ref.x, ref.y, ref.w,
                                            ref.h, true, true);
        out.kind = ResolvedClip::Kind::Circle;
        out.cx = cx;
        out.cy = cy;
        out.rx = out.ry = out.radius = std::max(0.0f, radius);
        return out;
    }
    if (clip.shape == CP::Shape::Ellipse) {
        const float rx = clip.radiusX == CP::Radial::Explicit
                             ? px(clip.radiusXLength, ref.w)
                             : clipRadialSize(clip.radiusX, 0.0f, cx, cy, ref.x, ref.y,
                                              ref.w, ref.h, false, true);
        const float ry = clip.radiusY == CP::Radial::Explicit
                             ? px(clip.radiusYLength, ref.h)
                             : clipRadialSize(clip.radiusY, 0.0f, cx, cy, ref.x, ref.y,
                                              ref.w, ref.h, false, false);
        out.kind = ResolvedClip::Kind::Ellipse;
        out.cx = cx;
        out.cy = cy;
        out.rx = std::max(0.0f, rx);
        out.ry = std::max(0.0f, ry);
        return out;
    }
    out.kind = ResolvedClip::Kind::Polygon;
    out.evenOdd = clip.evenOdd;
    for (const auto& point : clip.points) {
        out.points.push_back(
            SDL_FPoint{ref.x + px(point.x, ref.w), ref.y + px(point.y, ref.h)});
    }
    return out;
}

// How far a box's painting can reach beyond its layout box, so an offscreen
// surface does not cut a shadow or outline short at its edge.
float paintOutset(const ComputedStyle& s, float rootFontSize) {
    float amount = 0.0f;
    for (const ComputedStyle::BoxShadow& shadow : s.boxShadows) {
        if (shadow.inset) {
            continue;
        }
        const float blur = std::max(0.0f, shadow.blurRadius.resolve(0.0f, s.fontSize, rootFontSize));
        const float spread = std::max(0.0f, shadow.spread.resolve(0.0f, s.fontSize, rootFontSize));
        const float offset = std::max(std::fabs(shadow.offsetX.resolve(0.0f, s.fontSize, rootFontSize)),
                                      std::fabs(shadow.offsetY.resolve(0.0f, s.fontSize, rootFontSize)));
        amount = std::max(amount, blur + spread + offset);
    }
    for (const ComputedStyle::TextShadow& shadow : s.textShadows) {
        const float blur = std::max(0.0f, shadow.blurRadius.resolve(0.0f, s.fontSize, rootFontSize));
        const float offset = std::max(std::fabs(shadow.offsetX.resolve(0.0f, s.fontSize, rootFontSize)),
                                      std::fabs(shadow.offsetY.resolve(0.0f, s.fontSize, rootFontSize)));
        amount = std::max(amount, blur + offset);
    }
    if (s.outline.style != ComputedStyle::BorderStyleNone) {
        const float width = s.outline.width.resolve(0.0f, s.fontSize, rootFontSize);
        const float offset = std::max(0.0f, s.outline.offset.resolve(0.0f, s.fontSize, rootFontSize));
        amount = std::max(amount, width + offset);
    }
    return amount;
}

// Fills in the `bounds` each BeginGroup op carries. The content is gathered
// from the group's own boxes: the union of what each paints -- its layout box
// clipped by its ancestor clip, then padded by whatever it draws outside that
// (shadows, outlines) -- clamped to the group root's own clip, the tightest
// rect the group's ancestors allow. The group root's box sits immediately after
// its BeginGroup, so it owns the clip the whole group is confined to.
void computeGroupBounds(std::vector<PaintOp>& ops, const StyleResolver& resolver) {
    const float rootFontSize = resolver.rootFontSize();
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].kind == PaintOp::Kind::BeginGroup) {
            open.push_back(i);
        } else if (ops[i].kind == PaintOp::Kind::EndGroup) {
            const std::size_t begin = open.back();
            open.pop_back();
            const SDL_FRect clip = ops[begin + 1].item.clip;
            SDL_FRect bounds{};
            for (std::size_t j = begin + 1; j < i; ++j) {
                if (ops[j].kind != PaintOp::Kind::Box) {
                    continue;
                }
                const PaintItem& item = ops[j].item;
                SDL_FRect painted{item.el->layoutX() - item.dx, item.el->layoutY() - item.dy,
                                  item.el->layoutWidth(), item.el->layoutHeight()};
                painted = intersectClip(painted, item.clip);
                if (painted.w <= 0.0f || painted.h <= 0.0f) {
                    continue;
                }
                const float outset = paintOutset(resolver.resolve(*item.el), rootFontSize);
                bounds = unionRect(bounds, {painted.x - outset, painted.y - outset,
                                            painted.w + 2.0f * outset, painted.h + 2.0f * outset});
            }
            ops[begin].bounds =
                bounds.w > 0.0f && bounds.h > 0.0f ? intersectClip(bounds, clip) : SDL_FRect{};
        }
    }
}

// Walks one document into per-context lists. Discovery is depth-first, so `order`
// stays document order across the whole walk even though each list is sorted on
// its own.
struct Collector {
    const StyleResolver& resolver;
    std::size_t nextOrder = 0;

    // What one element contributes to the walk: its box when it paints, the
    // environment its children inherit, and the group it opens.
    struct BoxState {
        bool paints = false;
        bool positioned = false;
        GroupKind kind = GroupKind::None;
        float groupAlpha = 1.0f;
        bool transformed = false;
        float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        // A flattened 3D transform takes the projective path instead: the full
        // list matrix, the viewer distance, and the origin's z. `culled` drops
        // a backface-hidden subtree outright.
        bool projective = false;
        float mat3d[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        float persp3d = 0.0f;
        float originZ = 0.0f;
        bool culled = false;
        SDL_FPoint origin{0.0f, 0.0f};
        float blur = 0.0f;
        bool shift = false;
        float filter[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                            0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        DropShadowFilter dropShadow{};
        // A `mix-blend-mode` that composites the group against its backdrop.
        ComputedStyle::MixBlendMode blend = ComputedStyle::MixBlendMode::Normal;
        bool isolated = false;
        PaintItem item;
        PaintEnv child;
    };

    BoxState resolve(Element& el, const PaintEnv& env) {
        BoxState box;
        box.child = env;
        if (!el.isVisual()) {
            // Non-visual wrappers (<root>, <head>) still contain drawable descendants.
            return box;
        }
        const ComputedStyle s = resolver.resolve(el);
        const bool drawable = s.display != ComputedStyle::None && s.visible;
        const float groupAlpha = s.opacity * filterOpacity(s.filter);
        // A parent's `perspective` funnels every descendant's 3D through its
        // viewer, display or no display.
        box.child.perspective = s.perspective;
        // A scale() or rotate() cannot be folded into offsets the way translate
        // can, so its context composites offscreen just like an alpha under 1
        // does, and the whole group is mapped to the screen by the matrix at the
        // surface blit.
        Affine matrix;
        float tx = 0.0f, ty = 0.0f;
        // A list with 3D functions takes the projective path instead; a
        // non-positive `perspective()` invalidates the whole list. Pure-2D
        // lists never leave the affine path below, bit for bit.
        Mat4 mat4;
        bool invalid3d = false;
        bool hasProjective = false;
        if (s.transform.find("3d") != std::string_view::npos ||
            s.transform.find("rotateX") != std::string_view::npos ||
            s.transform.find("rotateY") != std::string_view::npos ||
            s.transform.find("translateZ") != std::string_view::npos ||
            s.transform.find("scaleZ") != std::string_view::npos ||
            s.transform.find("perspective") != std::string_view::npos) {
            hasProjective = transformEffects3D(s.transform, mat4, el.layoutWidth(),
                                               el.layoutHeight(), invalid3d);
        }
        bool hasMatrix = false;
        if (!invalid3d && !hasProjective) {
            hasMatrix = transformEffects(s.transform, matrix, tx, ty,
                                         el.layoutWidth(), el.layoutHeight());
        }
        // A `filter: blur()` has to diffuse the box's rendered surface, which
        // only an offscreen texture can provide, so it too forces a surface; the
        // radius rides the BeginGroup op for the executor to apply.
        const float blur = filterBlur(s.filter);
        // A `filter:` colour-shift chain (brightness/contrast/...) likewise has
        // to hit the rendered surface, so it forces a surface and rides the
        // BeginGroup op as one composed affine matrix.
        const bool hasShift = filterColorMatrix(s.filter, box.filter);
        box.shift = hasShift;
        // A `filter: drop-shadow()` traces the group's own silhouette, which
        // only a surface can capture, so it forces one too; the shadow's
        // offset, blur and colour ride the BeginGroup op for the executor.
        const DropShadowFilter dropShadow = filterDropShadow(s.filter);
        // An `opacity` under 1 -- or a `filter: opacity()` -- composites its whole
        // context through an offscreen surface, so its alpha leaves the environment
        // here and is carried on the BeginGroup op instead; the box and everything
        // they hold paint at full strength into that surface. A box that never
        // paints (say `display:none`) still hands its alpha on, as it always did,
        // since there is no surface for it.
        const bool composited =
            drawable && (groupAlpha < 1.0f || hasMatrix || hasProjective || blur > 0.0f ||
                         hasShift || dropShadow.present ||
                         s.mixBlendMode != ComputedStyle::MixBlendMode::Normal);
        if (!composited) {
            box.child.opacity *= groupAlpha;
        }
        // The group alpha and the translation land on the box as well as on what
        // it contains, so both are folded in before the item is taken. A matrix
        // group leaves its translate in the matrix instead -- folded twice, it
        // would move the group twice over. A projective group keeps the whole
        // list in its matrix for the same reason.
        if (!hasMatrix && !hasProjective) {
            box.child.dx += tx;
            box.child.dy += ty;
        }
        const bool clips = drawable && clipsAnything(s);
        if (clips) {
            // `overflow` crops to the padding box, which in CSS includes the padding.
            box.child.clip = intersectClip(env.clip,
                                           SDL_FRect{el.paddingX(), el.paddingY(),
                                                     el.paddingWidth(), el.paddingHeight()});
        }
        if (!drawable) {
            // A hidden box paints nothing of its own, but its descendants are still
            // walked and already carry its alpha and translation.
            return box;
        }
        box.paints = true;
        box.positioned = s.positioned;
        box.kind = classifyGroup(s);
        box.groupAlpha = composited ? groupAlpha : 1.0f;
        box.transformed = hasMatrix;
        box.projective = hasProjective;
        box.blur = composited ? blur : 0.0f;
        box.dropShadow = composited ? dropShadow : DropShadowFilter{};
        box.blend = composited ? s.mixBlendMode : ComputedStyle::MixBlendMode::Normal;
        box.isolated = s.isolate;
        box.m[0] = matrix.a;
        box.m[1] = matrix.b;
        box.m[2] = matrix.c;
        box.m[3] = matrix.d;
        box.m[4] = matrix.e;
        box.m[5] = matrix.f;
        if (hasProjective) {
            for (int i = 0; i < 16; ++i) {
                box.mat3d[i] = mat4.m[i];
            }
            box.persp3d = env.perspective;
            const float rootFontSize = resolver.rootFontSize();
            box.originZ = s.transformOrigin.z.resolve(0.0f, s.fontSize, rootFontSize);
        }
        PaintItem& item = box.item;
        item.el = &el;
        item.resolver = &resolver;
        item.zIndex = s.positioned ? s.zIndex : 0;
        item.layer = !s.positioned ? 0 : (s.zIndex < 0 ? -1 : (s.zIndex > 0 ? 2 : 1));
        item.order = nextOrder++;
        // `overflow` crops the element's own content as well as its descendants,
        // so the clip has already included this element by the time it is stored.
        item.clip = box.child.clip;
        item.dx = box.child.dx;
        item.dy = box.child.dy;
        item.opacity = box.child.opacity;
        item.pointerEventsNone = s.pointerEventsNone;
        // A `clip-path` shape resolves against its reference box now, in the
        // same window coordinates, so paint and hit-testing share one answer.
        if (s.clipPath.shape != ComputedStyle::ClipPath::Shape::None) {
            item.clipShape = resolveClipPath(s.clipPath, el, item.dx, item.dy, s.fontSize,
                                             resolver.rootFontSize());
        }
        // Mask layers ride along untouched: the executor evaluates them
        // against the surface. Modes, composites, geometry, and the border
        // mask ride too, matched by position at apply time.
        if (hasMaskSurface(s.maskImages) || hasMaskBorderSurface(s.maskBorder)) {
            item.maskImages = s.maskImages;
            item.maskModes = s.maskModes;
            item.maskComposites = s.maskComposites;
            item.maskSizes = s.maskSizes;
            item.maskPositions = s.maskPositions;
            item.maskRepeats = s.maskRepeats;
            item.maskOrigins = s.maskOrigins;
            item.maskClips = s.maskClips;
            item.maskBorder = s.maskBorder;
        }
        // The point the transform turns about: the resolved transform-origin,
        // measured from the box's own top-left (which scroll offsets have already
        // moved out of `dx`/`dy`).
        const float rootFontSize = resolver.rootFontSize();
        const float originX = s.transformOrigin.x.resolve(el.layoutWidth(), s.fontSize,
                                                          rootFontSize);
        const float originY = s.transformOrigin.y.resolve(el.layoutHeight(), s.fontSize,
                                                          rootFontSize);
        box.origin = {item.el->layoutX() - item.dx + originX,
                      item.el->layoutY() - item.dy + originY};
        if (hasProjective) {
            // The projective payload rides the item for the executor and the
            // hit test. A backface-hidden group facing away culls its whole
            // subtree here, so paint and hit-testing agree by construction.
            item.projective = true;
            for (int i = 0; i < 16; ++i) {
                item.mat3d[i] = box.mat3d[i];
            }
            item.persp3d = box.persp3d;
            item.originZ = box.originZ;
            const float bx = item.el->layoutX() - item.dx;
            const float by = item.el->layoutY() - item.dy;
            const ProjectedQuad facing = projectQuad(
                box.mat3d, box.persp3d, box.origin.x, box.origin.y, box.originZ, bx,
                by, item.el->layoutWidth(), item.el->layoutHeight());
            if (s.backfaceHidden && facing.facesAway) {
                box.culled = true;
                return box;
            }
        }
        if (clips) {
            // What the clip keeps is shifted by this box's own scroll offsets, and
            // only its contents move; the box itself stays where layout put it.
            box.child.dx += s.overflowX != ComputedStyle::Visible ? el.scrollLeft() : 0.0f;
            box.child.dy += s.overflowY != ComputedStyle::Visible ? el.scrollTop() : 0.0f;
        }
        return box;
    }

    void walkChildren(Element& el, const PaintEnv& env, std::vector<PaintEntry>& target,
                      std::vector<PaintEntry>* escape) {
        for (const auto& child : el.children()) {
            if (child && !child->isTextNode()) {
                walk(*child, env, target, escape);
            }
        }
    }

    // Appends `el`'s box when it paints, then orders its descendants. `escape` is
    // the list a positioned descendant of a pseudo-context rejoins; it is null
    // inside a real context and at the root, where nothing escapes above.
    void walk(Element& el, const PaintEnv& env, std::vector<PaintEntry>& target,
              std::vector<PaintEntry>* escape) {
        BoxState box = resolve(el, env);
        if (box.culled) {
            // A backface-hidden group facing away paints nothing, children
            // included: the plane turned its back on the viewer.
            return;
        }
        if (!box.paints) {
            walkChildren(el, box.child, target, escape);
            return;
        }
        // A positioned box found inside a pseudo-context belongs to the context
        // around that pseudo-context, not to the group it was found in.
        std::vector<PaintEntry>* into = &target;
        if (escape && box.positioned) {
            into = escape;
        }
        PaintEntry entry;
        entry.item = std::move(box.item);
        if (box.kind == GroupKind::None) {
            // Not a group: children share the context this box was found in, and
            // a positioned child may still escape through it.
            walkChildren(el, box.child, *into, escape);
            into->push_back(std::move(entry));
            return;
        }
        entry.group = true;
        entry.groupAlpha = box.groupAlpha;
        entry.transformed = box.transformed;
        entry.origin = box.origin;
        entry.projective = box.projective;
        for (int i = 0; i < 16; ++i) {
            entry.mat3d[i] = box.mat3d[i];
        }
        entry.persp3d = box.persp3d;
        entry.originZ = box.originZ;
        entry.blur = box.blur;
        entry.shift = box.shift;
        entry.dropShadow = box.dropShadow;
        entry.blend = box.blend;
        entry.isolated = box.isolated;
        for (int i = 0; i < 12; ++i) {
            entry.filter[i] = box.filter[i];
        }
        entry.m[0] = box.m[0];
        entry.m[1] = box.m[1];
        entry.m[2] = box.m[2];
        entry.m[3] = box.m[3];
        entry.m[4] = box.m[4];
        entry.m[5] = box.m[5];
        // A pseudo-context holds its in-flow content but lets positioned
        // descendants rejoin the list its own box sits in; a real one holds all.
        std::vector<PaintEntry>* childEscape = box.kind == GroupKind::Pseudo ? into : nullptr;
        walkChildren(el, box.child, entry.inner, childEscape);
        into->push_back(std::move(entry));
    }

    // Appends one context's entries in order -- layer, then `z-index`, then
    // document order -- with each group's box immediately followed by everything
    // the group orders. A group carrying an alpha under 1 is bracketed by
    // BeginGroup/EndGroup operations around its boxes, so the executor can
    // composite them offscreen; anything else stays a flat sequence.
    void emit(std::vector<PaintEntry>& entries, std::vector<PaintOp>& out) {
        std::stable_sort(entries.begin(), entries.end(),
                         [](const PaintEntry& a, const PaintEntry& b) {
                             if (a.item.layer != b.item.layer) {
                                 return a.item.layer < b.item.layer;
                             }
                             if (a.item.zIndex != b.item.zIndex) {
                                 return a.item.zIndex < b.item.zIndex;
                             }
                             return a.item.order < b.item.order;
                         });
        for (PaintEntry& entry : entries) {
            if (entry.group &&
                (entry.groupAlpha < 1.0f || entry.transformed || entry.projective ||
                 entry.blur > 0.0f || entry.shift || entry.dropShadow.present ||
                 entry.blend != ComputedStyle::MixBlendMode::Normal || entry.isolated ||
                 entry.item.clipShape.kind != ResolvedClip::Kind::None ||
                 hasMaskSurface(entry.item.maskImages) ||
                 hasMaskBorderSurface(entry.item.maskBorder))) {
                out.push_back(groupStart(entry.item, entry.groupAlpha, entry.transformed, entry.m,
                                        entry.origin, entry.blur, entry.shift, entry.filter,
                                        entry.dropShadow, entry.blend));
                out.push_back(boxOp(entry.item));
                emit(entry.inner, out);
                out.push_back(PaintOp{PaintOp::Kind::EndGroup, entry.item, {}, 1.0f});
            } else {
                out.push_back(boxOp(entry.item));
                emit(entry.inner, out);
            }
        }
    }
};

}  // namespace

// A `filter: opacity(x)` is the one filter the painter can honour without an
// offscreen surface: it scales the same group alpha `opacity` already threads
// down. Other filter functions are left for a renderer that can blur or colour-
// shift a surface. Returns 1 when there is no opacity() in the chain.
float filterOpacity(std::string_view filter) {
    float result = 1.0f;
    std::size_t at = 0;
    while ((at = filter.find("opacity(", at)) != std::string_view::npos) {
        at += 8;
        const std::size_t end = filter.find(')', at);
        if (end == std::string_view::npos) {
            break;
        }
        const std::string_view number = filter.substr(at, end - at);
        char* stop = nullptr;
        const std::string owned(number);
        const float value = std::strtof(owned.c_str(), &stop);
        if (stop != owned.c_str()) {
            result *= value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        }
        at = end + 1;
    }
    return result;
}

// The `)` that closes the `(` just after `open`, letting a `drop-shadow()`'s
// color argument -- rgba(), hsl(), color-mix() and friends -- keep its own
// nested parentheses inside the body.
std::size_t closingParen(std::string_view text, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')' && --depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

// A `filter: drop-shadow(x y blur color)` shadows the group's own silhouette --
// its alpha shape, not its box -- so the group still needs an offscreen
// surface. The executor colours the readback with `color`, softens it by
// `blur` px, and blits a shadow texture offset by (x, y) under the content; the
// shadow may spill past the group's bounds, so it is not confined the way blur
// is. The offset and blur are parsed as bare numbers (em is flattened to px,
// like the other filter lengths); a missing color stays opaque near-black.
// Returns present=false unless both offsets parse; only the first
// drop-shadow() in the chain is honoured.
DropShadowFilter filterDropShadow(std::string_view filter) {
    DropShadowFilter shadow;
    std::size_t at = 0;
    while ((at = filter.find("drop-shadow(", at)) != std::string_view::npos) {
        const std::size_t close = closingParen(filter, at + 11);
        if (close == std::string_view::npos) {
            break;
        }
        const std::string_view body = filter.substr(at + 12, close - at - 12);
        at = close + 1;
        if (shadow.present) {
            continue;
        }
        // Split the body on whitespace that is not inside parentheses, so a
        // functional color (rgba()/color-mix()) stays one token.
        std::vector<std::string_view> tokens;
        std::size_t begin = 0;
        int depth = 0;
        for (std::size_t i = 0; i <= body.size(); ++i) {
            const bool end = i == body.size();
            const char ch = end ? ' ' : body[i];
            if (!end) {
                if (ch == '(') {
                    ++depth;
                } else if (ch == ')') {
                    --depth;
                }
            }
            if (end || (std::isspace(static_cast<unsigned char>(ch)) && depth == 0)) {
                if (i > begin) {
                    tokens.push_back(body.substr(begin, i - begin));
                }
                begin = i + 1;
            }
        }
        auto asLength = [](std::string_view token, float& value) {
            if (token.empty() || token.find('(') != std::string_view::npos) {
                return false;
            }
            const std::string owned(token);
            char* stop = nullptr;
            const float parsed = std::strtof(owned.c_str(), &stop);
            if (stop == owned.c_str()) {
                return false;
            }
            value = parsed;
            return true;
        };
        if (tokens.size() < 2) {
            continue;
        }
        float dx = 0.0f, dy = 0.0f;
        if (!asLength(tokens[0], dx) || !asLength(tokens[1], dy)) {
            continue;
        }
        shadow.dx = dx;
        shadow.dy = dy;
        bool blurSet = false;
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            float value = 0.0f;
            if (!blurSet && asLength(tokens[i], value)) {
                shadow.blur = std::max(0.0f, value);
                blurSet = true;
            } else {
                SDL_Color color;
                if (parseColor(tokens[i], color)) {
                    shadow.color = color;
                }
            }
        }
        shadow.present = true;
    }
    return shadow;
}


// Whether any mask layer needs a compositing surface: gradients evaluate
// against it and url() sources sample decoded bytes, while `none` layers and
// an empty list paint flat.
bool hasMaskSurface(const std::vector<ComputedStyle::MaskImage>& layers) {
    using Kind = ComputedStyle::MaskImage::Kind;
    for (const auto& layer : layers) {
        if (layer.kind == Kind::LinearGradient || layer.kind == Kind::RadialGradient ||
            layer.kind == Kind::ConicalGradient || layer.kind == Kind::Url) {
            return true;
        }
    }
    return false;
}

bool hasMaskBorderSurface(const ComputedStyle::MaskBorder& border) {
    return border.source.kind != ComputedStyle::MaskImage::Kind::None;
}

ProjectedQuad projectQuad(const float mat[16], float perspective, float ox, float oy,
                          float oz, float rectX, float rectY, float rectW, float rectH) {
    ProjectedQuad quad;
    bool behind = false;
    quad.corners[0] = projectPoint(mat, perspective, ox, oy, oz, rectX, rectY, behind);
    quad.facesAway = behind;
    quad.corners[1] =
        projectPoint(mat, perspective, ox, oy, oz, rectX + rectW, rectY, behind);
    quad.facesAway = quad.facesAway || behind;
    quad.corners[2] = projectPoint(mat, perspective, ox, oy, oz, rectX + rectW,
                                   rectY + rectH, behind);
    quad.facesAway = quad.facesAway || behind;
    quad.corners[3] =
        projectPoint(mat, perspective, ox, oy, oz, rectX, rectY + rectH, behind);
    quad.facesAway = quad.facesAway || behind;
    if (!quad.facesAway) {
        // A flipped winding faces away: the plane turned its back.
        const float before = rectW * rectH;
        float after = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const SDL_FPoint& a = quad.corners[i];
            const SDL_FPoint& b = quad.corners[(i + 1) % 4];
            after += a.x * b.y - b.x * a.y;
        }
        quad.facesAway = (before > 0.0f) != (after > 0.0f);
    }
    return quad;
}
bool unprojectPoint(const ProjectedQuad& quad, float rectX, float rectY, float rectW,
                    float rectH, float x, float y, float& lx, float& ly) {
    float h[9];
    if (!rectToQuadHomo(rectX, rectY, rectW, rectH, quad.corners, h)) {
        return false;
    }
    float inv[9];
    if (!homoInverse(h, inv)) {
        return false;
    }
    const float w = inv[6] * x + inv[7] * y + inv[8];
    if (std::fabs(w) < 1e-12f) {
        return false;
    }
    lx = (inv[0] * x + inv[1] * y + inv[2]) / w;
    ly = (inv[3] * x + inv[4] * y + inv[5]) / w;
    return true;
}

// A `filter: blur(x)` asks for the group's rendered surface to be diffused by
// x pixels. The painter cannot blur a flat box list, so it does what it can
// here: mark the box for an offscreen surface (like an opacity under 1 does)
// and hand the radius to the executor, which reads the surface back, blurs it,
// and blits the result. Returns 0 when the chain has no blur(). Shared with
// the backdrop filter, which diffuses what is behind the box instead.
float filterBlur(std::string_view filter) {
    float radius = 0.0f;
    std::size_t at = 0;
    while ((at = filter.find("blur(", at)) != std::string_view::npos) {
        at += 5;
        const std::size_t end = filter.find(')', at);
        if (end == std::string_view::npos) {
            break;
        }
        const std::string_view number = filter.substr(at, end - at);
        char* stop = nullptr;
        const std::string owned(number);
        const float value = std::strtof(owned.c_str(), &stop);
        if (stop != owned.c_str()) {
            radius = std::max(radius, value);
        }
        at = end + 1;
    }
    return radius;
}

bool clipsAnything(const ComputedStyle& s) {
    return s.overflowX != ComputedStyle::Visible || s.overflowY != ComputedStyle::Visible;
}

bool createsPaintGroup(const ComputedStyle& s) {
    return classifyGroup(s) != GroupKind::None;
}

std::vector<PaintOp> computePaintOps(Element& root, const StyleResolver& resolver) {
    Collector collector{resolver, 0};
    std::vector<PaintOp> out;
    // The document's context opens with the root's own box, then orders everything
    // else around it: a context paints its background before any child's turn.
    // Nothing escapes above the root, so its children carry no escape list. A
    // root that composites (say `opacity` on the whole document) brackets the
    // whole frame.
    const Collector::BoxState box = collector.resolve(root, PaintEnv{});
    const ComputedStyle::MixBlendMode rootBlend =
        box.item.el ? resolver.resolve(*box.item.el).mixBlendMode
                    : ComputedStyle::MixBlendMode::Normal;
    if (box.paints && (box.groupAlpha < 1.0f || box.transformed || box.projective ||
                       box.blur > 0.0f || box.shift || box.dropShadow.present ||
                       rootBlend != ComputedStyle::MixBlendMode::Normal ||
                       box.item.clipShape.kind != ResolvedClip::Kind::None ||
                       hasMaskSurface(box.item.maskImages) ||
                       hasMaskBorderSurface(box.item.maskBorder))) {
        out.push_back(groupStart(box.item, box.groupAlpha, box.transformed, box.m, box.origin,
                                box.blur, box.shift, box.filter, box.dropShadow, rootBlend));
    }
    if (box.paints) {
        out.push_back(boxOp(box.item));
    }
    std::vector<PaintEntry> entries;
    collector.walkChildren(root, box.child, entries, nullptr);
    collector.emit(entries, out);
    if (box.paints && (box.groupAlpha < 1.0f || box.transformed || box.projective ||
                       box.blur > 0.0f || box.shift || box.dropShadow.present)) {
        out.push_back(PaintOp{PaintOp::Kind::EndGroup, box.item, {}, 1.0f});
    }
    computeGroupBounds(out, resolver);
    return out;
}

std::vector<PaintOp> spliceFrameOps(const std::vector<PaintOp>& host,
                                    const std::vector<FrameSplice>& frames) {
    std::vector<PaintOp> ops;
    if (frames.empty()) {
        return host;
    }
    ops.reserve(host.size());
    for (const PaintOp& op : host) {
        ops.push_back(op);
        if (op.kind != PaintOp::Kind::Box || !op.item.el) {
            continue;
        }
        const FrameSplice* splice = nullptr;
        for (const FrameSplice& candidate : frames) {
            if (candidate.frame == op.item.el) {
                splice = &candidate;
                break;
            }
        }
        if (!splice || !splice->document || !splice->document->root() ||
            !splice->resolver) {
            continue;
        }
        // The frame viewport is the element's content box: a frame crops to it
        // the way an `overflow: hidden` box crops its children.
        const SDL_FRect viewport{op.item.el->contentX(), op.item.el->contentY(),
                                 op.item.el->contentWidth(), op.item.el->contentHeight()};
        if (viewport.w <= 0.0f || viewport.h <= 0.0f) {
            continue;
        }
        // A host scroll moves the whole frame with the box; the frame's own
        // scrolls already ride its items.
        const float dx = op.item.dx;
        const float dy = op.item.dy;
        std::vector<PaintOp> inner =
            computePaintOps(*splice->document->root(), *splice->resolver);
        for (PaintOp& inner_op : inner) {
            inner_op.item.dx += dx;
            inner_op.item.dy += dy;
            inner_op.item.frame = splice->state;
            SDL_FRect clip = inner_op.item.clip;
            if (op.item.clip.w > 0.0f && op.item.clip.h > 0.0f) {
                clip = intersectClip(clip, op.item.clip);
            }
            inner_op.item.clip = intersectClip(clip, viewport);
            // A `clip-path` on the frame element clips the box; the frame's own
            // shapes win over it, and two of them on one frame stay a
            // simplification.
            if (inner_op.item.clipShape.kind == ResolvedClip::Kind::None) {
                inner_op.item.clipShape = op.item.clipShape;
            }
            if (inner_op.kind == PaintOp::Kind::BeginGroup) {
                // Group bounds and the transform origin are in window
                // coordinates with the host's scroll already folded in.
                inner_op.bounds.x -= dx;
                inner_op.bounds.y -= dy;
                inner_op.origin.x -= dx;
                inner_op.origin.y -= dy;
            }
            ops.push_back(std::move(inner_op));
        }
    }
    return ops;
}

Element* hitTestPaintOrder(const std::vector<PaintOp>& ops, float x, float y) {
    if (ops.empty()) return nullptr;
    std::vector<HitCtx> ctx(ops.size());
    std::vector<HitCtx> stack;
    stack.push_back(hitBase());
    for (size_t i = 0; i < ops.size(); ++i) {
        ctx[i] = stack.back();
        const auto& op = ops[i];
        if (op.kind == PaintOp::Kind::BeginGroup) {
            HitAffine g{op.m[0],op.m[1],op.m[2],op.m[3],op.m[4],op.m[5]};
            HitCtx inner = stack.back();
            if (op.projective && !stack.back().proj) {
                // A flattened-3D group maps its window-space bounds through
                // its own quad; anything projective deeper rides along
                // unwarped, and 2D groups below compose inward as usual.
                inner.proj = true;
                inner.quad = projectQuad(op.mat3d, op.persp3d, op.origin.x, op.origin.y,
                                         op.originZ, op.bounds.x, op.bounds.y,
                                         op.bounds.w, op.bounds.h);
                inner.quadRect = op.bounds;
                inner.inner = HitAffine{1,0,0,1,0,0};
            } else if (inner.proj) {
                inner.inner = hitCompose(inner.inner, g);
            } else {
                inner.transform = hitCompose(stack.back().transform, g);
            }
            stack.push_back(inner);
        } else if (op.kind == PaintOp::Kind::EndGroup) {
            if (stack.size() > 1) stack.pop_back(); else stack.back() = hitBase();
        }
    }
    for (int i = (int)ops.size()-1; i>=0; --i) {
        const auto& op = ops[i];
        if (op.kind == PaintOp::Kind::Box && !op.item.pointerEventsNone && hitBox(ctx[i], op.item, x, y)) {
            return op.item.el;
        }
    }
    return nullptr;
}

}  // namespace sui

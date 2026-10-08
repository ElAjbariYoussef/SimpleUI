#include "Core/Application/PaintOrder.h"

#include "CSS/StyleResolver.h"
#include "SUI/DOM/Element.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace sui {
namespace {

// The environment a subtree paints under: the tightest clip its clipping
// ancestors impose, the scroll and translation offsets that move it on screen,
// and the group alpha laid over it.
struct PaintEnv {
    SDL_FRect clip{0.0f, 0.0f, 0.0f, 0.0f};
    float dx = 0.0f;
    float dy = 0.0f;
    float opacity = 1.0f;
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
    // point `origin` (in window coordinates).
    bool transformed = false;
    float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    SDL_FPoint origin{0.0f, 0.0f};
    // A `filter: blur(x)` radius in px; forces a surface so the executor can
    // blur the readback.
    float blur = 0.0f;
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

// A `filter: blur(x)` asks for the group's rendered surface to be diffused by
// x pixels. The painter cannot blur a flat box list, so it does what it can
// here: mark the box for an offscreen surface (like an opacity under 1 does)
// and hand the radius to the executor, which reads the surface back, blurs it,
// and blits the result. Returns 0 when the chain has no blur().
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
bool transformEffects(std::string_view transform, Affine& affine, float& tx, float& ty) {
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
        if (name == "translate" || name == "translateX" || name == "translateY") {
            const std::size_t comma = body.find(',');
            const std::string_view first = body.substr(0, comma);
            const std::string_view second =
                comma == std::string_view::npos ? std::string_view() : body.substr(comma + 1);
            float x = 0.0f, y = 0.0f;
            number(first, x);
            number(second, y);
            if (name == "translateY") {
                y = x;
                x = 0.0f;
            } else if (name == "translateX") {
                y = 0.0f;
            }
            tx += x;
            ty += y;
            acc = combine(acc, Affine{1.0f, 0.0f, 0.0f, 1.0f, x, y});
        } else if (name == "scale") {
            const std::size_t comma = body.find(',');
            float sx = 1.0f, sy = 1.0f;
            number(body.substr(0, comma), sx);
            if (comma != std::string_view::npos) {
                number(body.substr(comma + 1), sy);
            } else {
                sy = sx;
            }
            acc = combine(acc, Affine{sx, 0.0f, 0.0f, sy, 0.0f, 0.0f});
            any = true;
        } else if (name == "rotate") {
            float degrees = 0.0f;
            number(body, degrees);
            const float rad = degrees * kPi / 180.0f;
            acc = combine(acc,
                          Affine{std::cos(rad), std::sin(rad), -std::sin(rad), std::cos(rad), 0.0f,
                                 0.0f});
            any = true;
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
    if (s.opacity < 1.0f || declares(s.transform) || declares(s.filter)) {
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
// executor a `filter: blur()` radius to apply to the readback.
PaintOp groupStart(const PaintItem& item, float alpha, bool transformed, const float m[6],
                   SDL_FPoint origin, float blur) {
    PaintOp op;
    op.kind = PaintOp::Kind::BeginGroup;
    op.item = item;
    op.alpha = alpha;
    op.transformed = transformed;
    op.origin = origin;
    op.blur = blur;
    for (int i = 0; i < 6; ++i) {
        op.m[i] = m[i];
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
        SDL_FPoint origin{0.0f, 0.0f};
        float blur = 0.0f;
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
        // A scale() or rotate() cannot be folded into offsets the way translate
        // can, so its context composites offscreen just like an alpha under 1
        // does, and the whole group is mapped to the screen by the matrix at the
        // surface blit.
        Affine matrix;
        float tx = 0.0f, ty = 0.0f;
        const bool hasMatrix = transformEffects(s.transform, matrix, tx, ty);
        // A `filter: blur()` has to diffuse the box's rendered surface, which
        // only an offscreen texture can provide, so it too forces a surface; the
        // radius rides the BeginGroup op for the executor to apply.
        const float blur = filterBlur(s.filter);
        // An `opacity` under 1 -- or a `filter: opacity()` -- composites its whole
        // context through an offscreen surface, so its alpha leaves the environment
        // here and is carried on the BeginGroup op instead; the box and everything
        // they hold paint at full strength into that surface. A box that never
        // paints (say `display:none`) still hands its alpha on, as it always did,
        // since there is no surface for it.
        const bool composited = drawable && (groupAlpha < 1.0f || hasMatrix || blur > 0.0f);
        if (!composited) {
            box.child.opacity *= groupAlpha;
        }
        // The group alpha and the translation land on the box as well as on what
        // it contains, so both are folded in before the item is taken. A matrix
        // group leaves its translate in the matrix instead -- folded twice, it
        // would move the group twice over.
        if (!hasMatrix) {
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
        box.blur = composited ? blur : 0.0f;
        box.m[0] = matrix.a;
        box.m[1] = matrix.b;
        box.m[2] = matrix.c;
        box.m[3] = matrix.d;
        box.m[4] = matrix.e;
        box.m[5] = matrix.f;
        PaintItem& item = box.item;
        item.el = &el;
        item.zIndex = s.positioned ? s.zIndex : 0;
        item.layer = !s.positioned ? 0 : (s.zIndex < 0 ? -1 : (s.zIndex > 0 ? 2 : 1));
        item.order = nextOrder++;
        // `overflow` crops the element's own content as well as its descendants,
        // so the clip has already included this element by the time it is stored.
        item.clip = box.child.clip;
        item.dx = box.child.dx;
        item.dy = box.child.dy;
        item.opacity = box.child.opacity;
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
        entry.blur = box.blur;
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
            if (entry.group && (entry.groupAlpha < 1.0f || entry.transformed || entry.blur > 0.0f)) {
                out.push_back(groupStart(entry.item, entry.groupAlpha, entry.transformed, entry.m,
                                        entry.origin, entry.blur));
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
    if (box.paints && (box.groupAlpha < 1.0f || box.transformed || box.blur > 0.0f)) {
        out.push_back(groupStart(box.item, box.groupAlpha, box.transformed, box.m, box.origin,
                                box.blur));
    }
    if (box.paints) {
        out.push_back(boxOp(box.item));
    }
    std::vector<PaintEntry> entries;
    collector.walkChildren(root, box.child, entries, nullptr);
    collector.emit(entries, out);
    if (box.paints && (box.groupAlpha < 1.0f || box.transformed || box.blur > 0.0f)) {
        out.push_back(PaintOp{PaintOp::Kind::EndGroup, box.item, {}, 1.0f});
    }
    computeGroupBounds(out, resolver);
    return out;
}

}  // namespace sui

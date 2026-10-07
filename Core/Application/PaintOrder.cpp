#include "Core/Application/PaintOrder.h"

#include "CSS/StyleResolver.h"
#include "SUI/DOM/Element.h"

#include <algorithm>
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

// Pulls the pixel translation out of a `transform` list. Only `translate()` and
// its single-axis forms are honoured: they can be folded into the scroll offset
// the painter already threads through the tree, whereas a scale or rotate would
// need a real matrix on every draw. A percentage axis is left at zero because it
// resolves against the box and is the rarer spelling.
void transformTranslate(std::string_view transform, float& dx, float& dy) {
    std::size_t at = 0;
    while ((at = transform.find("translate", at)) != std::string_view::npos) {
        const std::size_t open = transform.find('(', at);
        if (open == std::string_view::npos) {
            return;
        }
        const std::size_t close = transform.find(')', open);
        if (close == std::string_view::npos) {
            return;
        }
        const std::string_view body = transform.substr(open + 1, close - open - 1);
        const std::size_t comma = body.find(',');
        const std::string_view first = body.substr(0, comma);
        const std::string_view second =
            comma == std::string_view::npos ? std::string_view() : body.substr(comma + 1);
        auto px = [](std::string_view piece, float& out) {
            const std::string owned(piece);
            char* stop = nullptr;
            const float value = std::strtof(owned.c_str(), &stop);
            if (stop != owned.c_str() && stop[0] != '%') {
                out = value;
            }
        };
        const bool xOnly = transform.compare(at, 10, "translateX") == 0;
        const bool yOnly = transform.compare(at, 10, "translateY") == 0;
        float tx = 0.0f, ty = 0.0f;
        px(first, tx);
        px(second, ty);
        if (xOnly) {
            dx += tx;
        } else if (yOnly) {
            dy += tx;
        } else {
            dx += tx;
            dy += ty;
        }
        at = close + 1;
    }
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
        // The group alpha and the translation land on the box as well as on what
        // it contains, so both are folded in before the item is taken.
        box.child.opacity *= s.opacity;
        box.child.opacity *= filterOpacity(s.filter);
        transformTranslate(s.transform, box.child.dx, box.child.dy);
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
        // A pseudo-context holds its in-flow content but lets positioned
        // descendants rejoin the list its own box sits in; a real one holds all.
        std::vector<PaintEntry>* childEscape = box.kind == GroupKind::Pseudo ? into : nullptr;
        walkChildren(el, box.child, entry.inner, childEscape);
        into->push_back(std::move(entry));
    }

    // Appends one context's entries in order -- layer, then `z-index`, then
    // document order -- with each group's box immediately followed by everything
    // the group orders.
    void emit(std::vector<PaintEntry>& entries, std::vector<PaintItem>& out) {
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
            out.push_back(entry.item);
            if (entry.group) {
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

std::vector<PaintItem> computePaintOrder(Element& root, const StyleResolver& resolver) {
    Collector collector{resolver, 0};
    std::vector<PaintItem> out;
    // The document's context opens with the root's own box, then orders everything
    // else around it: a context paints its background before any child's turn.
    // Nothing escapes above the root, so its children carry no escape list.
    const Collector::BoxState box = collector.resolve(root, PaintEnv{});
    if (box.paints) {
        out.push_back(box.item);
    }
    std::vector<PaintEntry> entries;
    collector.walkChildren(root, box.child, entries, nullptr);
    collector.emit(entries, out);
    return out;
}

}  // namespace sui

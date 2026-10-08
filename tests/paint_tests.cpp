// Paint order: which box a frame draws on top of which, how a stacking context
// keeps its subtree together, and where an `opacity` below 1 switches to
// offscreen compositing. No device is needed -- computePaintOps is a pure walk
// over a laid-out document -- so this runs in the ordinary test pass.

#include "Core/Application/PaintOrder.h"
#include "CSS/Parser.h"
#include "CSS/StyleResolver.h"
#include "Layout/BlockLayout.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

void checkNear(float actual, float expected, float tolerance, const char* what) {
    if (std::fabs(actual - expected) > tolerance) {
        std::printf("FAIL: %s (got %.2f, expected %.2f)\n", what, actual, expected);
        ++failures;
    } else {
        std::printf("ok:   %s (%.2f)\n", what, actual);
    }
}

using sui::ComputedStyle;
using sui::Document;
using sui::Element;
using sui::PaintItem;

std::unique_ptr<Document> parseOrNull(const char* source) {
    sui::ParseError error;
    auto document = sui::MarkupParser::parse(source, error);
    if (!document) {
        std::printf("FAIL: could not parse document: %s\n", error.message.c_str());
        ++failures;
    }
    return document;
}

// The document laid out at a fixed viewport, the way the application does it.
void layoutRoot(Document& document, sui::StyleResolver& resolver) {
    sui::LayoutInput input;
    input.resolver = &resolver;
    input.font = nullptr;
    input.viewportWidth = 400.0f;
    input.viewportHeight = 600.0f;
    sui::BlockLayout().layout(document.root(), input);
}

// Flattens the op sequence back into boxes, multiplying each enclosing group's
// alpha back into the box the way the flat painter used to, so every ordering
// check reads the same on both sides of the offscreen switch.
std::vector<PaintItem> flattened(const std::vector<sui::PaintOp>& ops) {
    std::vector<PaintItem> items;
    std::vector<float> groupAlpha;
    for (const sui::PaintOp& op : ops) {
        if (op.kind == sui::PaintOp::Kind::BeginGroup) {
            groupAlpha.push_back(op.alpha);
        } else if (op.kind == sui::PaintOp::Kind::EndGroup) {
            groupAlpha.pop_back();
        } else {
            PaintItem item = op.item;
            for (float alpha : groupAlpha) {
                item.opacity *= alpha;
            }
            items.push_back(item);
        }
    }
    return items;
}

std::vector<PaintItem> orderOf(Document& document, sui::StyleResolver& resolver) {
    layoutRoot(document, resolver);
    return flattened(sui::computePaintOps(*document.root(), resolver));
}

// The first BeginGroup op, for tests that assert on the composite sequence.
std::size_t beginGroupAt(const std::vector<sui::PaintOp>& ops) {
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup) {
            return i;
        }
    }
    return ops.size();
}

// Position of `el` in the paint list, or -1 when it does not paint at all.
int paintIndex(const std::vector<PaintItem>& items, Element* el) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].el == el) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const PaintItem* paintOf(const std::vector<PaintItem>& items, Element* el) {
    for (const PaintItem& item : items) {
        if (item.el == el) {
            return &item;
        }
    }
    return nullptr;
}

// A plain document keeps document order inside each layer, with negative
// `z-index` below the in-flow boxes and positive `z-index` above them -- the
// flat ordering the engine shipped with, still required where no alpha opens a
// composited group.
void testPlainOrder() {
    auto document = parseOrNull(R"(<sui><body>
        <div id="a"></div><div id="b"></div>
        <div id="n" style="position:absolute;z-index:-1"></div>
        <div id="p" style="position:absolute;z-index:2"></div>
    </body></sui>)");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    const auto items = orderOf(*document, resolver);
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* n = document->getElementById("n");
    Element* p = document->getElementById("p");
    if (!a || !b || !n || !p) {
        check(false, "plain document parsed");
        return;
    }
    const int ia = paintIndex(items, a);
    const int ib = paintIndex(items, b);
    const int in = paintIndex(items, n);
    const int ip = paintIndex(items, p);
    check(ia >= 0 && ib >= 0 && in >= 0 && ip >= 0, "every box of a plain document paints");
    check(in < ia && ia < ib && ib < ip,
          "negative z-index below, then document order, then positive z-index");
}

// A positioned box paints its own background before the content it holds: the
// group's box comes first, its children ordered after it. A flat list sorted by
// layer put the in-flow children *below* the positioned parent's background.
void testPositionedGroupsInFlow() {
    auto document = parseOrNull(R"(<sui><body>
        <div id="w" style="position:relative;background:#f00">
            <div id="c"></div>
        </div>
        <div id="s"></div>
    </body></sui>)");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    const auto items = flattened(ops);
    Element* w = document->getElementById("w");
    Element* c = document->getElementById("c");
    Element* s = document->getElementById("s");
    if (!w || !c || !s) {
        check(false, "positioned document parsed");
        return;
    }
    check(paintIndex(items, s) < paintIndex(items, w),
          "in-flow boxes paint below a positioned box");
    check(paintIndex(items, w) < paintIndex(items, c),
          "a positioned box paints before the content it holds");
    bool flat = true;
    for (const sui::PaintOp& op : ops) {
        flat = flat && op.kind == sui::PaintOp::Kind::Box;
    }
    check(flat, "position groups carry no alpha and stay in the flat list");
}

// A stacking context opened by `opacity` is atomic: the positive `z-index`
// inside it cannot climb above a lower `z-index` that is a sibling of the
// context, and the group alpha reaches everything the group contains.
void testNestedGroupIsAtomic() {
    auto document = parseOrNull(R"(<sui><body>
        <div id="g" style="opacity:0.5">
            <div id="in" style="position:absolute;z-index:9"></div>
        </div>
        <div id="sib" style="position:absolute;z-index:1"></div>
    </body></sui>)");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    const auto items = orderOf(*document, resolver);
    Element* g = document->getElementById("g");
    Element* in = document->getElementById("in");
    Element* sib = document->getElementById("sib");
    if (!g || !in || !sib) {
        check(false, "opacity group document parsed");
        return;
    }
    const int ig = paintIndex(items, g);
    const int iin = paintIndex(items, in);
    const int isib = paintIndex(items, sib);
    check(ig >= 0 && iin >= 0 && isib >= 0, "opacity group and its neighbours paint");
    check(ig < iin && iin < isib,
          "z-index inside an opacity group stays below the group's sibling");
    const PaintItem* inner = paintOf(items, in);
    check(inner && std::fabs(inner->opacity - 0.5f) < 0.01f,
          "the group alpha reaches a descendant of the group");
}

// CSS lets `position` with `z-index: auto` send its positioned descendants back
// out to the context around it, so their `z-index` competes there: a sibling
// with a lower `z-index` still paints below them.
void testEscapeKeepsZOrder() {
    auto document = parseOrNull(R"(<sui><body>
        <div id="w" style="position:relative">
            <div id="in" style="position:absolute;z-index:5"></div>
        </div>
        <div id="sib" style="position:absolute;z-index:1"></div>
    </body></sui>)");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    const auto items = orderOf(*document, resolver);
    Element* w = document->getElementById("w");
    Element* in = document->getElementById("in");
    Element* sib = document->getElementById("sib");
    if (!w || !in || !sib) {
        check(false, "escape document parsed");
        return;
    }
    check(paintIndex(items, w) < paintIndex(items, sib) &&
              paintIndex(items, sib) < paintIndex(items, in),
          "a positioned child rejoins the context around its positioned parent");
}

// What opens a group. `none` -- the initial value and an explicit keyword -- must
// not count; transform and filter are per-box and never inherited.
void testGroupDetection() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="plain"></div>
        <div id="half" style="opacity:0.5"></div>
        <div id="full" style="opacity:1"></div>
        <div id="abs" style="position:absolute"></div>
        <div id="zpos" style="position:absolute;z-index:3"></div>
        <div id="rel" style="position:relative"></div>
        <div id="tr" style="transform:translateX(4px)"></div>
        <div id="trnone" style="transform:none"></div>
        <div id="fi" style="filter:opacity(0.5)"></div>
        <div id="finone" style="filter:none"></div>
    </body></sui>)HTML");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    struct Case {
        const char* id;
        bool opens;
        const char* what;
    };
    const Case cases[] = {
        {"plain", false, "a plain box opens no group"},
        {"half", true, "opacity under 1 opens a group"},
        {"full", false, "opacity of 1 opens no group"},
        {"abs", true, "position:absolute opens a group"},
        {"zpos", true, "a positioned z-index opens a group"},
        {"rel", true, "position:relative opens a group"},
        {"tr", true, "a transform opens a group"},
        {"trnone", false, "transform:none opens no group"},
        {"fi", true, "a filter opens a group"},
        {"finone", false, "filter:none opens no group"},
    };
    for (const Case& c : cases) {
        Element* el = document->getElementById(c.id);
        if (!el) {
            check(false, c.what);
            continue;
        }
        check(sui::createsPaintGroup(resolver.resolve(*el)) == c.opens, c.what);
    }
}

// transform and filter are properties of the box they are declared on: a
// descendant inherits the *effect* by being carried along, not by re-reading the
// string. The offset and the alpha must appear once, not once per level.
void testEffectsDontCompound() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="t" style="transform:translateX(10px)">
            <div id="k"></div>
        </div>
        <div id="f" style="filter:opacity(0.5)">
            <div id="j"></div>
        </div>
    </body></sui>)HTML");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    const auto items = orderOf(*document, resolver);
    Element* t = document->getElementById("t");
    Element* k = document->getElementById("k");
    Element* f = document->getElementById("f");
    Element* j = document->getElementById("j");
    if (!t || !k || !f || !j) {
        check(false, "transform/filter document parsed");
        return;
    }
    const PaintItem* it = paintOf(items, t);
    const PaintItem* ik = paintOf(items, k);
    const PaintItem* iff = paintOf(items, f);
    const PaintItem* ij = paintOf(items, j);
    if (!it || !ik || !iff || !ij) {
        check(false, "transform/filter boxes paint");
        return;
    }
    checkNear(it->dx, 10.0f, 0.01f, "translateX lands on the box it is declared on");
    checkNear(ik->dx, 10.0f, 0.01f, "a child is carried by the translate, not shifted again");
    checkNear(iff->opacity, 0.5f, 0.01f, "filter opacity lands on the box it is declared on");
    checkNear(ij->opacity, 0.5f, 0.01f, "a child carries the group alpha once");
}

// An `opacity` under 1 brackets its boxes with offscreen operations: BeginGroup,
// the group root's own box (painted into its own surface), the content, then
// EndGroup. The boxes inside carry no alpha -- it rides the BeginGroup op -- and
// the bounds wrap the whole group.
void testGroupBecomesOffscreenBlit() {
    auto document = parseOrNull(R"(<sui><body>
        <div id="g" style="opacity:0.5;width:100px;height:60px">
            <div id="c" style="width:40px;height:20px"></div>
        </div>
    </body></sui>)");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    Element* g = document->getElementById("g");
    Element* c = document->getElementById("c");
    if (!g || !c) {
        check(false, "offscreen document parsed");
        return;
    }
    const std::size_t begin = beginGroupAt(ops);
    const bool sequence =
        begin != ops.size() && ops.size() == begin + 4 &&
        ops[begin + 1].kind == sui::PaintOp::Kind::Box && ops[begin + 1].item.el == g &&
        ops[begin + 2].kind == sui::PaintOp::Kind::Box && ops[begin + 2].item.el == c &&
        ops[begin + 3].kind == sui::PaintOp::Kind::EndGroup;
    check(sequence, "an opacity group emits Begin, its box, its content, then End");
    check(begin != ops.size() && ops[begin].alpha == 0.5f,
          "the group alpha rides the BeginGroup op");
    check(begin + 1 < ops.size() && ops[begin + 1].item.opacity == 1.0f &&
              ops[begin + 2].item.opacity == 1.0f,
          "boxes paint at full strength inside a composited group");
    checkNear(ops[begin].bounds.w, 100.0f, 2.0f, "group bounds fit the horizontal extent");
    checkNear(ops[begin].bounds.h, 60.0f, 2.0f, "group bounds fit the vertical extent");
}

// Nested alphas composite in nested surfaces: each BeginGroup carries only its
// own alpha, and flattening the sequence multiplies them back together.
void testNestedGroupAlphas() {
    auto document = parseOrNull(R"(<sui><body>
        <div id="g" style="opacity:0.7">
            <div id="h" style="opacity:0.3"><div id="i"></div></div>
        </div>
    </body></sui>)");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    const std::size_t begin = beginGroupAt(ops);
    // Begin(g) Box(g) Begin(h) Box(h) Box(i) End End
    const bool sequence =
        begin != ops.size() && ops.size() == begin + 7 &&
        ops[begin + 2].kind == sui::PaintOp::Kind::BeginGroup &&
        ops[begin + 5].kind == sui::PaintOp::Kind::EndGroup &&
        ops[begin + 6].kind == sui::PaintOp::Kind::EndGroup;
    check(sequence, "nested opacity groups nest their begin/end operations");
    check(begin + 2 < ops.size() && ops[begin].alpha == 0.7f && ops[begin + 2].alpha == 0.3f,
          "each nested group carries only its own alpha");
    const auto items = flattened(ops);
    Element* g = document->getElementById("g");
    Element* i = document->getElementById("i");
    if (!g || !i) {
        check(false, "nested document parsed");
        return;
    }
    const PaintItem* ig = paintOf(items, g);
    const PaintItem* ii = paintOf(items, i);
    check(ig && ii && std::fabs(ig->opacity - 0.7f) < 0.01f,
          "the outer alpha reaches the group root");
    check(ii && std::fabs(ii->opacity - 0.21f) < 0.01f,
          "nested alphas multiply for the deepest box");
}

// The surface has to hold what a group paints beyond its boxes: an outset
// shadow sticks out past them, so `bounds` pads the boxes by blur + spread +
// offset on every side.
void testGroupBoundsIncludePaint() {
    auto document = parseOrNull(R"(<sui><body>
        <div id="g" style="opacity:0.5;width:100px;height:50px;
            box-shadow:8px 9px 10px #000000"></div>
    </body></sui>)");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    Element* g = document->getElementById("g");
    if (!g) {
        check(false, "shadow document parsed");
        return;
    }
    const std::size_t begin = beginGroupAt(ops);
    if (begin == ops.size()) {
        check(false, "shadow group gets an offscreen operation");
        return;
    }
    const float outset = 10.0f + std::max(8.0f, 9.0f);  // blur + max offset
    checkNear(ops[begin].bounds.x, g->layoutX() - outset, 1.5f,
              "bounds start left of the box by the shadow extent");
    checkNear(ops[begin].bounds.w, g->layoutWidth() + 2.0f * outset, 3.0f,
              "bounds reach past the shadow's other side");
    checkNear(ops[begin].bounds.h, g->layoutHeight() + 2.0f * outset, 3.0f,
              "bounds reach below the shadow");
}

// A clipping ancestor bounds the surface to what the group is allowed to paint,
// even when the group's own boxes -- or the shadows they cast -- stick far out
// past it.
void testGroupClampedToClip() {
    auto document = parseOrNull(R"(<sui><body>
        <div id="outer" style="overflow:hidden;width:60px;height:60px">
            <div id="g" style="opacity:0.5;width:200px;height:200px;
                box-shadow:8px 9px 10px #000000"></div>
        </div>
    </body></sui>)");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    const std::size_t begin = beginGroupAt(ops);
    if (begin == ops.size()) {
        check(false, "clipped group gets an offscreen operation");
        return;
    }
    const sui::PaintOp& group = ops[begin];
    check(group.bounds.w <= 60.5f && group.bounds.h <= 60.5f,
          "group bounds are clamped to the ancestor clip");
    check(group.bounds.x >= -0.5f && group.bounds.y >= -0.5f,
          "group bounds start inside the clip");
}

}  // namespace

int main() {
    testPlainOrder();
    testPositionedGroupsInFlow();
    testNestedGroupIsAtomic();
    testEscapeKeepsZOrder();
    testGroupDetection();
    testEffectsDontCompound();
    testGroupBecomesOffscreenBlit();
    testNestedGroupAlphas();
    testGroupBoundsIncludePaint();
    testGroupClampedToClip();
    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all paint order checks passed\n");
    return 0;
}
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

// A scale() or rotate() composites the whole context through a surface too, and
// the affine rides the BeginGroup op -- the executor maps the surface with it --
// with alpha 1, so the group is bracketed even though no opacity is at play.
void testScaleAndRotateComposite() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="s" style="transform:scale(2);width:100px;height:50px">
            <div id="c"></div>
        </div>
        <div id="r" style="transform:rotate(45deg);width:100px;height:50px">
            <div id="d"></div>
        </div>
    </body></sui>)HTML");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    Element* s = document->getElementById("s");
    Element* r = document->getElementById("r");
    if (!s || !r) {
        check(false, "transform document parsed");
        return;
    }
    const std::size_t sb = beginGroupAt(ops);
    std::size_t rb = ops.size();
    for (std::size_t i = sb + 1; i < ops.size(); ++i) {
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup) {
            rb = i;
            break;
        }
    }
    const bool sequence =
        sb != ops.size() && rb != ops.size() && sb + 3 < ops.size() && rb + 3 < ops.size() &&
        ops[sb + 1].item.el == s && ops[sb + 2].item.el == document->getElementById("c") &&
        ops[sb + 3].kind == sui::PaintOp::Kind::EndGroup &&
        ops[rb + 1].item.el == r && ops[rb + 3].kind == sui::PaintOp::Kind::EndGroup;
    check(sequence, "scale and rotate groups each emit Begin, box, content, then End");
    // The safe indices keep this test a discriminator even when a bug stops the
    // groups from compositing: reads land on the first box instead of out of
    // bounds, and their identity matrix / zero centre still fail the checks.
    const std::size_t si = sb < ops.size() ? sb : 0;
    const std::size_t ri = rb < ops.size() ? rb : 0;
    check(si == sb && ops[si].transformed && ops[si].alpha == 1.0f,
          "scale composite carries alpha 1 on its transformed BeginGroup");
    checkNear(ops[si].m[0], 2.0f, 0.01f, "scale(2) lands in the matrix's horizontal axis");
    checkNear(ops[si].m[3], 2.0f, 0.01f, "scale(2) lands in the matrix's vertical axis");
    checkNear(ops[si].m[1], 0.0f, 0.01f, "a pure scale skews neither axis");
    checkNear(ops[si].origin.x, 50.0f, 0.5f, "transform origin at the box's horizontal centre");
    checkNear(ops[si].origin.y, 25.0f, 0.5f, "transform origin at the box's vertical centre");
    checkNear(ops[si].bounds.w, 100.0f, 2.0f, "surface stays pre-transform sized horizontally");
    checkNear(ops[si].bounds.h, 50.0f, 2.0f, "surface stays pre-transform sized vertically");
    check(ri == rb && ops[ri].transformed && ops[ri].alpha == 1.0f,
          "rotate 45deg composites offscreen without alpha");
    const float c45 = std::cos(3.14159265f / 4.0f);
    checkNear(ops[ri].m[0], c45, 0.01f, "rotate(45deg) matrix cosine on the x axis");
    checkNear(ops[ri].m[1], c45, 0.01f, "rotate(45deg) matrix sine on the y axis");
    checkNear(ops[ri].m[2], -c45, 0.01f, "rotate(45deg) matrix axes reach the other way");
}

// A matrix group keeps its translate out of the offset fold: the translate rides
// the surface blit, and folding it into the members as well would move them
// twice. Members of the scaled group therefore carry no translate, while a
// translate sitting alone is still folded exactly as before.
void testTransformedTranslateRidesMatrix() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="g" style="transform:scale(2) translateX(10px);width:40px;height:20px">
            <div id="c" style="width:10px;height:10px"></div>
        </div>
        <div id="t" style="transform:translateX(10px)">
            <div id="k"></div>
        </div>
    </body></sui>)HTML");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    Element* g = document->getElementById("g");
    Element* c = document->getElementById("c");
    Element* k = document->getElementById("k");
    if (!g || !c || !k) {
        check(false, "mixed transform document parsed");
        return;
    }
    const std::size_t begin = beginGroupAt(ops);
    check(begin != ops.size() && ops[begin].transformed,
          "scale plus translate composites as one matrix group");
    checkNear(ops[begin].m[4], 20.0f, 0.01f,
              "the translate rides inside the matrix, scaled with the group");
    const sui::PaintItem* ic = nullptr;
    for (const sui::PaintOp& op : ops) {
        if (op.kind == sui::PaintOp::Kind::Box && op.item.el == c) {
            ic = &op.item;
        }
    }
    check(ic && std::fabs(ic->dx) < 0.01f,
          "the translate is not folded into a member of a matrix group");
    const sui::PaintItem* it = nullptr;
    for (const sui::PaintOp& op : ops) {
        if (op.kind == sui::PaintOp::Kind::Box && op.item.el == k) {
            it = &op.item;
        }
    }
    check(it && std::fabs(it->dx - 10.0f) < 0.01f,
          "a translate alone still folds into its children");
}

// `transform-origin` moves the point the matrix turns about: a scaled box grows
// out of a corner or an edge instead of its centre. Percentages resolve against
// the border box, keyword edges too, and a single value leaves the other axis
// centred.
void testTransformOriginOffsetsAxis() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="corner" style="transform:scale(2);transform-origin:0 0;width:100px;height:50px">
            <div id="c1"></div>
        </div>
        <div id="edge" style="transform:scale(2);transform-origin:right bottom;width:100px;height:50px">
            <div id="c2"></div>
        </div>
        <div id="lean" style="transform:scale(2);transform-origin:25px 30px;width:100px;height:50px">
            <div id="c3"></div>
        </div>
    </body></sui>)HTML");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    Element* corner = document->getElementById("corner");
    Element* edge = document->getElementById("edge");
    Element* lean = document->getElementById("lean");
    if (!corner || !edge || !lean) {
        check(false, "transform-origin document parsed");
        return;
    }
    // The three groups walk in document order: corner, then edge, then lean, at
    // flow positions (0,0), (0,50) and (0,100).
    std::vector<std::size_t> groups;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup) {
            groups.push_back(i);
        }
    }
    check(groups.size() == 3u, "each transform box composites its own group");
    if (groups.size() != 3u) {
        return;
    }
    checkNear(ops[groups[0]].origin.x, 0.0f, 0.5f, "transform-origin:0 0 puts the pivot at the corner");
    checkNear(ops[groups[0]].origin.y, 0.0f, 0.5f, "transform-origin:0 0 vertically at the top");
    checkNear(ops[groups[1]].origin.x, 100.0f, 0.5f, "keyword right resolves to the far edge in x");
    checkNear(ops[groups[1]].origin.y, 100.0f, 0.5f, "keyword bottom counts down the box then the flow");
    checkNear(ops[groups[2]].origin.x, 25.0f, 0.5f, "a pixel x is kept as a pixel");
    checkNear(ops[groups[2]].origin.y, 130.0f, 0.5f, "a pixel y lands on the explicitly named spot");
}

// The transform list applies left to right, rightmost first -- CSS multiplied
// order -- and the translate rides inside the matrix either way. `translateX
// then rotate` turns the shifted result, so its e is the 10 and f is zero; the
// same pair the other way rotates first, so the 10 lands in f instead. Both
// differ from dropping the translate entirely, which the old folding did.
void testTransformListOrder() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="tr" style="transform:translateX(10px) rotate(90deg);width:40px;height:20px">
            <div id="c1"></div>
        </div>
        <div id="rt" style="transform:rotate(90deg) translateX(10px);width:40px;height:20px">
            <div id="c2"></div>
        </div>
    </body></sui>)HTML");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    Element* tr = document->getElementById("tr");
    Element* rt = document->getElementById("rt");
    if (!tr || !rt) {
        check(false, "transform order document parsed");
        return;
    }
    for (std::size_t i = 0; i + 4 < ops.size(); ++i) {
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup && ops[i + 1].item.el == tr) {
            check(ops[i].transformed, "translate-then-rotate composites");
            checkNear(ops[i].m[4], 10.0f, 0.01f,
                      "translate written first lands in the matrix's e");
            checkNear(ops[i].m[5], 0.0f, 0.01f, "translate written first leaves f empty");
        }
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup && ops[i + 1].item.el == rt) {
            checkNear(ops[i].m[4], 0.0f, 0.01f, "rotate first keeps e empty");
            checkNear(ops[i].m[5], 10.0f, 0.01f,
                      "rotate first bends the translate into f");
        }
    }
    check(tr != nullptr, "the transform order groups are found");
}

// A `filter: blur()` cannot diffuse a flat box list, so it opens an offscreen
// group the way an alpha under 1 does, and the radius rides the BeginGroup op
// for the executor. When the same box also lowers opacity, both land on the
// one op. A plain box opens no group.
void testFilterBlurComposites() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="blur" style="filter:blur(4px);width:40px;height:20px">
            <div id="c"></div>
        </div>
        <div id="combo" style="opacity:0.5;filter:blur(2px);width:40px;height:20px">
            <div id="c2"></div>
        </div>
        <div id="plain" style="width:40px;height:20px">
            <div id="c3"></div>
        </div>
    </body></sui>)HTML");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    Element* blur = document->getElementById("blur");
    Element* combo = document->getElementById("combo");
    Element* plain = document->getElementById("plain");
    if (!blur || !combo || !plain) {
        check(false, "filter blur document parsed");
        return;
    }
    bool sawBlur = false;
    bool sawCombo = false;
    for (std::size_t i = 0; i + 4 < ops.size(); ++i) {
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup &&
            ops[i + 1].item.el == blur) {
            sawBlur = true;
            check(ops[i].blur > 3.9f && ops[i].blur < 4.1f,
                  "a blur group carries its radius on the BeginGroup op");
            checkNear(ops[i].alpha, 1.0f, 0.001f, "a blur alone keeps full alpha");
            check(!ops[i].transformed, "blur alone opens no affine group");
        }
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup &&
            ops[i + 1].item.el == combo) {
            sawCombo = true;
            checkNear(ops[i].alpha, 0.5f, 0.001f,
                      "opacity and blur share the same BeginGroup");
            check(ops[i].blur > 1.9f && ops[i].blur < 2.1f,
                  "the combined group carries its blur too");
        }
    }
    bool sawPlain = false;
    for (const sui::PaintOp& op : ops) {
        if (op.kind == sui::PaintOp::Kind::Box && op.item.el == plain) {
            sawPlain = true;
        }
    }
    check(sawBlur, "the blur box opens a group");
    check(sawCombo, "the opacity-plus-blur box opens a group");
    check(sawPlain, "a plain box is still painted");
}

// A `filter:` colour-shift chain has to hit the rendered surface, so it forces
// an offscreen group like blur does, and the composed matrix rides the
// BeginGroup op. The op's values are the exact spec matrices, so the assertion
// pins both the parse and the ordering.
void testFilterShiftComposites() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="bright" style="filter:brightness(0.5);width:40px;height:20px">
            <div id="c"></div>
        </div>
        <div id="invert" style="filter:invert(1) contrast(2);width:40px;height:20px">
            <div id="c2"></div>
        </div>
        <div id="plain" style="width:40px;height:20px">
            <div id="c3"></div>
        </div>
    </body></sui>)HTML");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    Element* bright = document->getElementById("bright");
    Element* invert = document->getElementById("invert");
    Element* plain = document->getElementById("plain");
    if (!bright || !invert || !plain) {
        check(false, "filter shift document parsed");
        return;
    }
    bool sawBright = false;
    bool sawInvert = false;
    for (std::size_t i = 0; i + 4 < ops.size(); ++i) {
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup &&
            ops[i + 1].item.el == bright) {
            sawBright = true;
            check(ops[i].shift, "the brightened box carries a shift");
            checkNear(ops[i].filter[0], 0.5f, 0.001f, "brightness scales the red row");
            checkNear(ops[i].filter[5], 0.5f, 0.001f, "brightness scales the green row");
            checkNear(ops[i].filter[3], 0.0f, 0.001f, "a pure brightness has no offset");
            checkNear(ops[i].alpha, 1.0f, 0.001f, "a shift alone keeps full alpha");
            check(ops[i].blur < 0.001f, "a pure shift group carries no blur");
        }
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup &&
            ops[i + 1].item.el == invert) {
            sawInvert = true;
            // invert(1) then contrast(2): -2 per diagonal, offset 2·255-128 = 382.
            check(ops[i].shift, "the invert-contrast box carries a shift");
            checkNear(ops[i].filter[0], -2.0f, 0.001f, "the chain multiplies to -2");
            checkNear(ops[i].filter[3], 382.0f, 0.001f, "the chain's offsets compose");
        }
    }
    bool sawPlain = false;
    for (const sui::PaintOp& op : ops) {
        if (op.kind == sui::PaintOp::Kind::Box && op.item.el == plain) {
            sawPlain = true;
        }
    }
    check(sawBright, "the brightness box opens a group");
    check(sawInvert, "the invert-contrast box opens a group");
    check(sawPlain, "a plain box is still painted");
}

// A `filter: drop-shadow()` has to trace the group's own silhouette, so it
// forces an offscreen group like the other filters, and the shadow's offset,
// blur and colour all ride the BeginGroup op.
void testFilterDropShadowComposites() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="shadow" style="filter:drop-shadow(4px 6px 8px #00000080);width:40px;height:20px">
            <div id="c"></div>
        </div>
        <div id="goneg" style="filter:drop-shadow(-2px 3px 0px red);width:40px;height:20px">
            <div id="c2"></div>
        </div>
        <div id="combo" style="filter:drop-shadow(2px 2px 2px #000) brightness(0.5);width:40px;height:20px">
            <div id="c3"></div>
        </div>
        <div id="plain" style="width:40px;height:20px">
            <div id="c4"></div>
        </div>
    </body></sui>)HTML");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);
    Element* shadow = document->getElementById("shadow");
    Element* goneg = document->getElementById("goneg");
    Element* combo = document->getElementById("combo");
    Element* plain = document->getElementById("plain");
    if (!shadow || !goneg || !combo || !plain) {
        check(false, "filter drop-shadow document parsed");
        return;
    }
    bool sawShadow = false;
    bool sawNeg = false;
    bool sawCombo = false;
    for (std::size_t i = 0; i + 4 < ops.size(); ++i) {
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup &&
            ops[i + 1].item.el == shadow) {
            sawShadow = true;
            check(ops[i].dropShadow.present, "the drop-shadow box carries a shadow");
            checkNear(ops[i].dropShadow.dx, 4.0f, 0.001f, "the shadow's x offset rides the op");
            checkNear(ops[i].dropShadow.dy, 6.0f, 0.001f, "the shadow's y offset rides the op");
            checkNear(ops[i].dropShadow.blur, 8.0f, 0.001f, "the shadow's blur rides the op");
            checkNear(ops[i].dropShadow.color.a, 128.0f, 0.5f,
                      "the shadow's hex alpha is kept");
        }
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup &&
            ops[i + 1].item.el == goneg) {
            sawNeg = true;
            checkNear(ops[i].dropShadow.dx, -2.0f, 0.001f, "a negative shadow offset parses");
            checkNear(ops[i].dropShadow.dy, 3.0f, 0.001f, "the y offset stays positive");
            checkNear(ops[i].dropShadow.color.r, 255.0f, 0.5f, "the named colour is parsed");
            checkNear(ops[i].dropShadow.color.a, 255.0f, 0.5f, "a named colour is opaque");
        }
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup &&
            ops[i + 1].item.el == combo) {
            sawCombo = true;
            check(ops[i].dropShadow.present && ops[i].shift,
                  "drop-shadow and a colour shift share one group");
        }
    }
    bool sawPlain = false;
    for (const sui::PaintOp& op : ops) {
        if (op.kind == sui::PaintOp::Kind::Box && op.item.el == plain) {
            sawPlain = true;
        }
    }
    check(sawShadow, "the drop-shadow box opens a group");
    check(sawNeg, "the negative-offset drop-shadow box opens a group");
    check(sawCombo, "the drop-shadow-plus-shift box opens a group");
    check(sawPlain, "a plain box is still painted");
}

void testHitTestPaintOrder() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"container\" style=\"position:relative;width:200px;height:200px\">"
        "<div id=\"under\" style=\"position:absolute;left:10px;top:10px;width:100px;height:100px;z-index:1\"></div>"
        "<div id=\"over\" style=\"position:absolute;left:10px;top:10px;width:100px;height:100px;z-index:10\"></div>"
        "</div>"
        "<div id=\"clipper\" style=\"overflow:hidden;width:50px;height:50px;position:relative\">"
        "<div id=\"clipped\" style=\"position:absolute;left:60px;top:10px;width:50px;height:50px\"></div>"
        "</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);

    Element* over = document->getElementById("over");
    Element* under = document->getElementById("under");
    Element* clipped = document->getElementById("clipped");

    Element* hit = sui::hitTestPaintOrder(ops, 50.0f, 50.0f);
    check(hit == over, "topmost element with higher z-index wins paint-order hit test");

    Element* hitClipped = sui::hitTestPaintOrder(ops, 70.0f, 20.0f);
    check(hitClipped != clipped, "element outside ancestor clip is not hit");

    Element* hitMiss = sui::hitTestPaintOrder(ops, 500.0f, 500.0f);
    check(hitMiss == nullptr, "out of bounds hit returns null");
}

void testExtendedTransformFunctions() {
    auto document = parseOrNull(R"HTML(<sui><body>
        <div id="sx" style="transform:scaleX(2);width:100px;height:50px"></div>
        <div id="sy" style="transform:scaleY(3);width:100px;height:50px"></div>
        <div id="skewx" style="transform:skewX(45deg);width:100px;height:50px"></div>
        <div id="mat" style="transform:matrix(1, 0, 0, 1, 30, 40);width:100px;height:50px"></div>
        <div id="rotturn" style="transform:rotate(0.25turn);width:100px;height:50px"></div>
    </body></sui>)HTML");
    if (!document) return;
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*document, resolver);
    const auto ops = sui::computePaintOps(*document->root(), resolver);

    Element* sx = document->getElementById("sx");
    Element* sy = document->getElementById("sy");
    Element* skewx = document->getElementById("skewx");
    Element* mat = document->getElementById("mat");
    Element* rotturn = document->getElementById("rotturn");

    for (std::size_t i = 0; i + 1 < ops.size(); ++i) {
        if (ops[i].kind == sui::PaintOp::Kind::BeginGroup) {
            Element* el = ops[i + 1].item.el;
            if (el == sx) {
                check(ops[i].transformed, "scaleX composites");
                checkNear(ops[i].m[0], 2.0f, 0.01f, "scaleX sets m0 to 2");
                checkNear(ops[i].m[3], 1.0f, 0.01f, "scaleX leaves m3 at 1");
            } else if (el == sy) {
                check(ops[i].transformed, "scaleY composites");
                checkNear(ops[i].m[0], 1.0f, 0.01f, "scaleY leaves m0 at 1");
                checkNear(ops[i].m[3], 3.0f, 0.01f, "scaleY sets m3 to 3");
            } else if (el == skewx) {
                check(ops[i].transformed, "skewX composites");
                checkNear(ops[i].m[2], 1.0f, 0.01f, "skewX(45deg) sets tan(45deg)=1 in m2");
            } else if (el == mat) {
                check(ops[i].transformed, "matrix composites");
                checkNear(ops[i].m[4], 30.0f, 0.01f, "matrix sets tx=30 in m4");
                checkNear(ops[i].m[5], 40.0f, 0.01f, "matrix sets ty=40 in m5");
            } else if (el == rotturn) {
                check(ops[i].transformed, "rotate with turn units composites");
                checkNear(ops[i].m[1], 1.0f, 0.01f, "0.25turn (90deg) sets sin(90)=1 in m1");
            }
        }
    }
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
    testScaleAndRotateComposite();
    testTransformedTranslateRidesMatrix();
    testTransformOriginOffsetsAxis();
    testTransformListOrder();
    testFilterBlurComposites();
    testFilterShiftComposites();
    testFilterDropShadowComposites();
    testHitTestPaintOrder();
    testExtendedTransformFunctions();
    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all paint order checks passed\n");
    return 0;
}
// Paint order: which box a frame draws on top of which, and how a stacking
// context keeps its subtree together. No device is needed -- computePaintOrder is
// a pure walk over a laid-out document -- so this runs in the ordinary pass.

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

std::vector<PaintItem> orderOf(Document& document, sui::StyleResolver& resolver) {
    layoutRoot(document, resolver);
    return sui::computePaintOrder(*document.root(), resolver);
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
// flat ordering the engine shipped with, still required where no context opens.
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
    const auto items = orderOf(*document, resolver);
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

}  // namespace

int main() {
    testPlainOrder();
    testPositionedGroupsInFlow();
    testNestedGroupIsAtomic();
    testEscapeKeepsZOrder();
    testGroupDetection();
    testEffectsDontCompound();
    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all paint order checks passed\n");
    return 0;
}

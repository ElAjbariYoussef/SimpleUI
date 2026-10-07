// CSS parsing, cascade, and layout tests. Layout runs without a font so the
// assertions stay independent of any bundled asset; the engine falls back to
// fixed average advances in that case.

#include "CSS/Parser.h"
#include "CSS/PseudoElements.h"
#include "CSS/StyleResolver.h"
#include "Layout/BlockLayout.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"

#include <cmath>
#include <cstdio>
#include <string>

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

std::unique_ptr<Document> parseOrNull(const char* source) {
    sui::ParseError error;
    auto document = sui::MarkupParser::parse(source, error);
    if (!document) {
        std::printf("FAIL: could not parse document: %s\n", error.message.c_str());
        ++failures;
    }
    return document;
}

Element* layoutRoot(Document& document, sui::StyleResolver& resolver, float width) {
    sui::LayoutInput input;
    input.resolver = &resolver;
    input.font = nullptr;
    input.viewportWidth = width;
    input.viewportHeight = 600.0f;
    sui::BlockLayout().layout(document.root(), input);
    return document.root();
}

void testRuleParsing() {
    const auto rules = sui::CSSParser::parse(R"(
        /* a comment */
        @supports (display: grid) { .ignored { color: #fff; } }
        .sidebar, #main > p { color: #123456; font-size: 18px }
        p { margin: 4px 8px !important; }
    )");
    check(rules.size() == 3, "comments and unimplemented at-rules skipped");
    if (rules.size() < 3) {
        return;
    }
    check(rules[0].selector.parts.size() == 1 && rules[1].selector.parts.size() == 2,
          "comma-separated selector split into two rules");
    check(rules[1].selector.combinators.size() == 1 &&
              rules[1].selector.combinators[0] == sui::Combinator::Child,
          "child combinator recorded");
    bool foundImportant = false;
    for (const auto& decl : rules[2].declarations) {
        if (decl.property == "margin" && decl.important) {
            foundImportant = true;
            check(decl.value == "4px 8px", "!important stripped from the value");
        }
    }
    check(foundImportant, "!important flag recorded");
}

void testSelectorMatching() {
    auto document = parseOrNull(
        "<sui><body><div class=\"sidebar wide\" id=\"main\">"
        "<p>text</p></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "div { color: #111111 } .sidebar { color: #222222 } #main { color: #333333 } "
        ".missing { color: #444444 } div p { color: #555555 } div > p { color: #666666 }"));

    Element* div = document->getElementById("main");
    Element* paragraph = document->querySelector("p");
    check(div != nullptr && paragraph != nullptr, "elements found");
    if (!div || !paragraph) {
        return;
    }

    const auto divStyle = resolver.resolve(*div);
    check(divStyle.color.r == 0x33, "id rule beats class and type");
    const auto paraStyle = resolver.resolve(*paragraph);
    check(paraStyle.color.r == 0x66, "child combinator matches");
    check(!selectorMatches(sui::CSSParser::parseSelector("body > p"), *paragraph),
          "child combinator does not match a grandchild");
    check(selectorMatches(sui::CSSParser::parseSelector("div#main.wide"), *div),
          "compound selector matches one element carrying every part");
    check(!selectorMatches(sui::CSSParser::parseSelector("div span"), *div),
          "a compound selector is not a descendant selector");
    check(selectorMatches(sui::CSSParser::parseSelector("*"), *div),
          "universal selector matches anything");
    check(selectorMatches(sui::CSSParser::parseSelector("body div p"), *paragraph),
          "descendant chain of any depth matches");
    // An unsupported pseudo-class must disable the whole selector, not be dropped:
    // `#t:hover` matching every `#t` would be worse than not matching at all.
    check(!selectorMatches(sui::CSSParser::parseSelector("h1:hover"), *paragraph),
          "a selector with an unevaluated pseudo-class matches nothing");
    check(!selectorMatches(sui::CSSParser::parseSelector("div::before"), *div),
          "a pseudo-element selector matches nothing");
    check(selectorMatches(sui::CSSParser::parseSelector("div:first-child"), *div),
          "first-child matches the first child of its parent");
    check(selectorMatches(sui::CSSParser::parseSelector("p:nth-child(1)"), *paragraph),
          "nth-child(1) matches the only child");
    check(selectorMatches(sui::CSSParser::parseSelector("p:nth-child(odd)"), *paragraph),
          "nth-child(odd) matches the first child");
    check(!selectorMatches(sui::CSSParser::parseSelector("p:nth-child(even)"), *paragraph),
          "nth-child(even) misses the first child");
    check(selectorMatches(sui::CSSParser::parseSelector("p:not(.missing)"), *paragraph),
          ":not() matches when the inner selector misses");
    check(!selectorMatches(sui::CSSParser::parseSelector("p:not(p)"), *paragraph),
          ":not() rejects when the inner selector hits");
    check(selectorMatches(sui::CSSParser::parseSelector("p:is(.missing, p)"), *paragraph),
          ":is() matches when any branch matches");
    check(!selectorMatches(sui::CSSParser::parseSelector("p:is(.missing, .absent)"), *paragraph),
          ":is() rejects when no branch matches");
}

void testInteractivePseudoClasses() {
    auto document = parseOrNull(
        "<sui><body><div id=\"panel\"><button id=\"btn\"><span id=\"label\">Go</span>"
        "</button></div></body></sui>");
    if (!document) {
        return;
    }
    Element* panel = document->getElementById("panel");
    Element* btn = document->getElementById("btn");
    Element* label = document->getElementById("label");
    check(panel && btn && label, "pseudo-class elements found");
    if (!panel || !btn || !label) {
        return;
    }

    const auto hoverPanel = sui::CSSParser::parseSelector("#panel:hover");
    const auto hoverBtn = sui::CSSParser::parseSelector("#btn:hover");
    // A descendant selector, which is how "the pointer is somewhere inside me"
    // gets written in CSS: `#panel:hover #btn` lights the button whenever anything
    // inside the panel is hovered.
    const auto panelHoverBtn = sui::CSSParser::parseSelector("#panel:hover #btn");
    const auto activeBtn = sui::CSSParser::parseSelector("#btn:active");
    const auto focusBtn = sui::CSSParser::parseSelector("#btn:focus");

    sui::PseudoState none;
    check(!selectorMatches(hoverBtn, *btn, &none),
          ":hover matches nothing when nothing is hovered");
    check(!selectorMatches(focusBtn, *btn, &none),
          ":focus matches nothing when nothing is focused");

    // A compound `#btn:hover` is one element: the button, and only when the button
    // itself is what the pointer is on or inside.
    sui::PseudoState overLabel;
    overLabel.hovered = label;
    check(selectorMatches(hoverBtn, *btn, &overLabel),
          "#btn:hover matches the button while a child is hovered");
    check(selectorMatches(sui::CSSParser::parseSelector("#label:hover"), *label,
                          &overLabel),
          ":hover matches the hovered element");

    // The panel is hovered while anything inside it is hovered.
    check(selectorMatches(hoverPanel, *panel, &overLabel),
          ":hover matches an ancestor of the hovered element");
    check(selectorMatches(panelHoverBtn, *btn, &overLabel),
          "#panel:hover #btn matches a descendant of the hovered panel");

    // The chain does not run the other way: hovering the panel does not light the
    // button inside it, which is what `:hover` (not `:hover-within`) means.
    sui::PseudoState overPanel;
    overPanel.hovered = panel;
    check(selectorMatches(hoverPanel, *panel, &overPanel),
          ":hover matches the hovered container itself");
    check(!selectorMatches(hoverBtn, *btn, &overPanel),
          ":hover does not leak down to children");
    check(!selectorMatches(panelHoverBtn, *btn, &overPanel),
          "a hovered parent does not style its descendants");
    check(!selectorMatches(hoverPanel, *label, &overPanel),
          "an ancestor selector does not match a descendant");

    // `:active` behaves like `:hover` while the button is held.
    sui::PseudoState pressed;
    pressed.active = btn;
    check(selectorMatches(activeBtn, *btn, &pressed), ":active matches the pressed element");

    // `:focus` belongs to one element only.
    sui::PseudoState focused;
    focused.focused = btn;
    check(selectorMatches(focusBtn, *btn, &focused), ":focus matches the focused element");
    check(!selectorMatches(sui::CSSParser::parseSelector("#panel:focus"), *panel,
                           &focused),
          ":focus does not match an ancestor of the focused element");

    // The state drives the cascade: a hovered button picks up its hover colour.
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0} #btn{width:60px;height:20px} #btn:hover{background-color:#ff0000}"));
    check(resolver.resolve(*btn).backgroundColor.r != 0xff,
          "a button is not hover-styled at rest");
    check(resolver.setHovered(btn), "hovering reports the state change");
    check(!resolver.setHovered(btn), "hovering the same element again reports no change");
    check(resolver.resolve(*btn).backgroundColor.r == 0xff,
          "hovering applies the :hover rule");
    check(resolver.setHovered(nullptr), "moving away reports the state change");
    check(resolver.resolve(*btn).backgroundColor.r != 0xff,
          "moving away drops the :hover rule");
    check(resolver.generation() > 0, "pseudo state changes bump the resolver generation");

    // A descendant selector sees the same state, which is how a hover highlight is
    // usually written.
    sui::StyleResolver nested;
    nested.setRules(sui::CSSParser::parse(
        "#panel:hover #btn{background-color:#00ff00}"));
    check(nested.resolve(*btn).backgroundColor.g != 0xff, "descendant hover is off at rest");
    nested.setHovered(label);
    check(nested.resolve(*btn).backgroundColor.g == 0xff,
          "a descendant selector lights up from a child being hovered");
}

void testAttributeSelectors() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\" data-role=\"panel aside\" data-lang=\"en-GB\""
        " data-url=\"https://example.com/page.html\" data-open></div>"
        "<div id=\"b\" data-role=\"panel\" data-lang=\"en\"></div></body></sui>");
    if (!document) {
        return;
    }
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    check(a && b, "attribute-selector elements found");
    if (!a || !b) {
        return;
    }
    const auto sel = [](const char* text) { return sui::CSSParser::parseSelector(text); };
    const auto on = [&](const char* text, Element& e) { return selectorMatches(sel(text), e); };

    // Presence and exact match, the two that were already right.
    check(on("[data-role]", *a), "attribute presence matches");
    check(!on("[data-open]", *b), "presence misses an absent attribute");
    check(on("[data-open]", *a), "a valueless attribute is a presence test");
    check(on("[data-role=\"panel aside\"]", *a), "attribute value matches when quoted");
    check(on("[data-role=panel]", *b), "attribute value matches when unquoted");
    check(!on("[data-role=\"panel aside\"]", *b), "exact match fails on a different value");
    check(!on("[data-lang=\"en\"]", *a), "exact match does not prefix-match");

    // Every operator must do what it says. Each one used to fall through to exact
    // equality, so `[data-lang^="en"]` failed against `en-GB`.
    check(on("[data-lang^=\"en\"]", *a), "^= matches a prefix");
    check(on("[data-url^=\"https://\"]", *a), "^= matches a multi-character prefix");
    check(!on("[data-url^=\"http/\"]", *a), "^= is not a naive contains");
    check(on("[data-url$=\"page.html\"]", *a), "$= matches a suffix");
    check(!on("[data-url$=\"https\"]", *a), "$= does not match a prefix");
    check(on("[data-url*=\"example.com\"]", *a), "*= matches a substring");
    check(on("[data-url*=\"/page\"]", *a), "*= matches a substring mid-word");
    check(!on("[data-url*=\"nope\"]", *a), "*= misses an absent substring");
    check(on("[data-role~=\"aside\"]", *a), "~= matches one word of a list");
    check(on("[data-role~=\"panel\"]", *a), "~= matches every word in a list");
    check(on("[data-role~=\"panel\"]", *b), "~= works on a single-word value");
    check(!on("[data-role~=\"panels\"]", *a), "~= is whole words only");
    check(on("[data-lang|=\"en\"]", *a), "|= matches the full value when no separator follows");
    check(on("[data-lang|=\"en\"]", *b), "|= matches the exact value");
    check(!on("[data-lang|=\"e\"]", *a), "|= needs a following hyphen");
    check(on("[data-lang|=\"en-GB\"]", *a), "|= still matches the exact value");

    // The spec's empty-value behaviour: everything begins with, ends with and contains
    // the empty string, so these match on presence alone.
    check(on("[data-url^=\"\"]", *a), "^= with an empty value matches on presence");
    check(on("[data-url$=\"\"]", *a), "$= with an empty value matches on presence");
    check(on("[data-url*=\"\"]", *a), "*= with an empty value matches on presence");
    check(!on("[data-role~=\"\"]", *a), "~= with an empty value matches no word");

    // A suffix longer than the value must not wrap the offset.
    check(!on("[data-url$=\"https://example.com/does-not-fit\"]", *a),
          "$= with a value longer than the attribute does not crash");

    // Malformed operators must not silently degrade into equality.
    check(sel("[data-role ~ panel]").unsupported, "an operator with no = is malformed");
    check(sel("[data-role panel]").unsupported, "space-separated matching is unsupported");
    check(!sel("[data-role=panel]").unsupported, "a well-formed selector stays supported");

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0} div{width:10px;height:5px} [data-lang^=\"en\"]{width:99px}"));
    const auto s = resolver.resolve(*a);
    checkNear(s.width.resolve(0.0f, s.fontSize, 16.0f), 99.0f, 0.01f,
              "an attribute rule beats a type rule");
}

void testCascade() {
    auto document = parseOrNull(
        "<sui><body><div class=\"a\" id=\"b\" style=\"color:#0000ff;\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "div { color: #010101 } .a { color: #020202 } #b { color: #030303 }"));
    Element* div = document->getElementById("b");
    check(div != nullptr, "div found");
    if (!div) {
        return;
    }
    const auto style = resolver.resolve(*div);
    check(style.color.b == 0xff && style.color.r == 0x00,
          "inline style beats every stylesheet rule");
}

void testPropertyParsing() {
    auto document = parseOrNull("<sui><body><div>x</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "div { width: 120px; height: 40px; margin: 5px 10px; padding: 2px 4px; "
        "background-color: #0a141e; color: rgb(255, 128, 0); display: inline-block; "
        "line-height: 2; font-weight: bold; border-color: #ff0000 }"));
    Element* div = document->querySelector("div");
    check(div != nullptr, "div found");
    if (!div) {
        return;
    }
    const auto s = resolver.resolve(*div);
    checkNear(s.width.resolve(), 120.0f, 0.01f, "width parsed");
    checkNear(s.margin.top.resolve(), 5.0f, 0.01f, "2-value margin top");
    checkNear(s.margin.left.resolve(), 10.0f, 0.01f, "2-value margin left");
    checkNear(s.padding.right.resolve(), 4.0f, 0.01f, "2-value padding right");
    check(s.backgroundColor.r == 0x0a && s.backgroundColor.g == 0x14 &&
              s.backgroundColor.b == 0x1e,
          "hex background parsed");
    check(s.color.r == 255 && s.color.g == 128 && s.color.b == 0,
          "rgb() color parsed");
    check(s.display == sui::ComputedStyle::InlineBlock, "display parsed");
    checkNear(s.lineHeight, 2.0f, 0.01f, "line-height parsed");
    check(s.fontBold, "font-weight: bold");
    check(s.borderColor.r == 0xff, "border-color parsed");
}

void testBlockStacking() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"></div><div id=\"b\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } div { margin: 10px; height: 30px }"));
    layoutRoot(*document, resolver, 200.0f);

    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    check(a != nullptr && b != nullptr, "boxes found");
    if (!a || !b) {
        return;
    }
    checkNear(a->layoutX(), 10.0f, 0.01f, "first box x includes its margin");
    checkNear(a->layoutY(), 10.0f, 0.01f, "first box y includes its margin");
    checkNear(b->layoutY(), 60.0f, 0.01f, "second box stacks below the first");
    checkNear(document->root()->layoutHeight(), 100.0f, 0.01f,
              "root height sums children and margins");
}

void testNestedContainer() {
    auto document = parseOrNull(
        "<sui><body><div id=\"outer\"><div id=\"inner\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #outer { padding: 20px; margin: 5px } "
        "#inner { height: 10px; margin: 2px }"));
    layoutRoot(*document, resolver, 300.0f);

    Element* outer = document->getElementById("outer");
    Element* inner = document->getElementById("inner");
    check(outer != nullptr && inner != nullptr, "nested boxes found");
    if (!outer || !inner) {
        return;
    }
    checkNear(outer->layoutX(), 5.0f, 0.01f, "outer border-box x");
    checkNear(inner->layoutX(), 5.0f + 20.0f + 2.0f, 0.01f,
              "inner is offset by the parent padding plus its own margin");
    checkNear(inner->layoutY(), 5.0f + 20.0f + 2.0f, 0.01f,
              "inner y is relative to the parent content box");
    checkNear(outer->layoutHeight(), 20.0f + 2.0f + 10.0f + 2.0f + 20.0f, 0.01f,
              "outer height grows to fit the child");
}

void testTextIndent() {
    auto document = parseOrNull(
        "<sui><body><p id=\"a\">alpha beta</p></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("#a { text-indent: 20px }"));
    layoutRoot(*document, resolver, 400.0f);

    Element* p = document->getElementById("a");
    check(p != nullptr && !p->textRuns().empty(), "indented paragraph laid out");
    if (p && !p->textRuns().empty()) {
        check(p->textRuns()[0].x > p->layoutX() + 19.0f,
              "text-indent pushes the first text run right");
    }
}

void testTextWrappingAndBr() {
    auto document = parseOrNull(
        "<sui><body><p id=\"t\">alpha beta gamma delta epsilon zeta eta theta</p>"
        "<p id=\"u\">one<br />two</p></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(
        sui::CSSParser::parse("body { margin: 0; font-size: 16px } p { width: 120px }"));
    layoutRoot(*document, resolver, 200.0f);

    Element* wrapped = document->getElementById("t");
    Element* broken = document->getElementById("u");
    check(wrapped != nullptr && broken != nullptr, "paragraphs found");
    if (!wrapped || !broken) {
        return;
    }

    check(wrapped->textRuns().size() == 8, "every word produced a text run");
    // 8 words at ~8px each plus spaces cannot fit two per 120px line in a 16px
    // font, so the paragraph must be more than one line tall.
    check(wrapped->layoutHeight() > 16.0f * 1.2f,
          "wrapped text is taller than a single line");

    // one / two on separate lines => 2 lines => height is two line boxes.
    check(broken->textRuns().size() == 2, "br keeps both text segments");
    checkNear(broken->layoutHeight(), 16.0f * 1.2f * 2.0f, 0.5f,
              "br produces a second line");
    check(broken->textRuns()[0].y < broken->textRuns()[1].y,
          "text after br sits on a lower line");
}

void testDisplayNoneIsSkipped() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"></div><div id=\"b\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } div { height: 20px } #a { display: none }"));
    layoutRoot(*document, resolver, 200.0f);

    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    check(a != nullptr && b != nullptr, "boxes found");
    if (!a || !b) {
        return;
    }
    checkNear(a->layoutHeight(), 0.0f, 0.01f, "display:none box has no height");
    checkNear(b->layoutY(), 0.0f, 0.01f, "display:none box takes no vertical space");
}

void testInheritance() {
    auto document = parseOrNull(
        "<sui><body><div id=\"box\"><p id=\"p\">x</p>"
        "<h1 id=\"h\">y</h1></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { color: #ff0000; font-size: 20px; line-height: 2.0; text-align: center }"));

    Element* div = document->getElementById("box");
    Element* p = document->getElementById("p");
    Element* h = document->getElementById("h");
    check(div && p && h, "elements found");
    if (!div || !p || !h) {
        return;
    }

    const auto divStyle = resolver.resolve(*div);
    const auto pStyle = resolver.resolve(*p);
    const auto hStyle = resolver.resolve(*h);

    check(divStyle.fontSize == 20.0f && divStyle.lineHeight == 2.0f,
          "author values reach the div");
    check(pStyle.fontSize == 20.0f, "font-size inherits into a p with no UA size");
    check(pStyle.lineHeight == 2.0f, "line-height inherits into a p");
    check(pStyle.color.r == 0xff, "color inherits into a p");
    check(pStyle.textAlign == sui::ComputedStyle::Center, "text-align inherits");
    check(hStyle.fontSize == 32.0f, "h1 keeps its UA font-size instead of inheriting");
    check(hStyle.lineHeight == 2.0f, "h1 still inherits line-height");
    check(hStyle.fontBold, "h1 UA bold survives inheritance");
    check(hStyle.color.r == 0xff, "h1 inherits the body color");
}

void testChildOverridesInherited() {
    auto document = parseOrNull(
        "<sui><body><p class=\"small\">x</p></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(
        sui::CSSParser::parse("body { font-size: 20px } p { font-size: 10px }"
                               " p.small { font-size: 8px }"));
    Element* p = document->querySelector("p");
    check(p != nullptr, "p found");
    if (!p) {
        return;
    }
    checkNear(resolver.resolve(*p).fontSize, 8.0f, 0.01f,
              "the most specific rule wins over an ancestor's value");
}

void testDefaultStyles() {
    sui::ComputedStyle body = sui::defaultStyleFor("body");
    check(body.display == sui::ComputedStyle::Block, "body defaults to block");
    sui::ComputedStyle span = sui::defaultStyleFor("span");
    check(span.display == sui::ComputedStyle::Inline, "span defaults to inline");
    sui::ComputedStyle button = sui::defaultStyleFor("button");
    check(button.display == sui::ComputedStyle::InlineBlock,
          "button defaults to inline-block");
    sui::ComputedStyle heading = sui::defaultStyleFor("h1");
    check(heading.fontSize > body.fontSize, "h1 is larger than body text");
    check(heading.fontBold, "h1 is bold");
}

// Phrasing tags that defaulted to block. Each one forced a line break, and the text
// either side of it laid out into no run at all, so it was never drawn -- correct
// markup lost its content rather than simply missing its decoration.
void testPhrasingTagsStayInline() {
    const char* phrasing[] = {"u",   "s",    "big", "kbd", "samp", "var", "q",
                              "cite", "abbr", "mark", "del", "ins",  "time", "data",
                              "bdi",  "bdo",  "wbr"};
    for (const char* tag : phrasing) {
        const std::string what = std::string("<") + tag + "> defaults to inline";
        const sui::ComputedStyle s = sui::defaultStyleFor(tag);
        check(s.display == sui::ComputedStyle::Inline, what.c_str());
    }

    const sui::ComputedStyle mark = sui::defaultStyleFor("mark");
    check(mark.backgroundColor.r == 255 && mark.backgroundColor.b == 0,
          "<mark> gets the browser's yellow background");
    check(mark.specified & sui::ComputedStyle::SpecifiedColor,
          "<mark>'s colour is flagged as set so inheritance leaves it alone");

    auto document =
        parseOrNull("<sui><body><p id=\"t\">alpha <u>beta</u> gamma</p></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, resolver, 400.0f);
    Element* p = document->getElementById("t");
    check(p != nullptr, "paragraph found");
    if (!p) {
        return;
    }
    // Before the fix this was 0: the block-level <u> pushed its siblings out of the
    // inline run list, and only "beta" was ever drawn.
    check(p->textRuns().size() == 3, "text either side of an inline tag still lays out");
    check(p->layoutHeight() <= 16.0f * 1.2f + 0.5f, "the paragraph stays one line box");
    const auto& runs = p->textRuns();
    check(runs.size() == 3 && runs[0].text.find("alpha") != std::string::npos &&
              runs[1].text.find("beta") != std::string::npos &&
              runs[2].text.find("gamma") != std::string::npos,
          "all three pieces survive in document order");
}

namespace {
// Captures log lines so a test can assert what the engine said. SDL routes every
// SDL_Log* call through one function, so this is the only way to check a warning.
std::vector<std::string> capturedLog;
std::vector<std::string> resolvedLog;

void captureLog(void*, int, SDL_LogPriority, const char* message) {
    if (message) {
        capturedLog.emplace_back(message);
    }
}
}  // namespace

// An unsupported property is dropped by CSS anyway, but silently is what makes a
// stylesheet undiagnosable here: there is no devtools panel to show a warning.
void testUnknownPropertyIsReported() {
    SDL_LogOutputFunction previous = nullptr;
    void* previousUserdata = nullptr;
    SDL_GetLogOutputFunction(&previous, &previousUserdata);
    SDL_SetLogOutputFunction(captureLog, nullptr);
    // The default filter can sit above WARN depending on how the process started.
    const SDL_LogPriority previousPriority =
        SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_VERBOSE);

    capturedLog.clear();

    auto document = parseOrNull("<sui><body><div id=\"x\">x</div></body></sui>");
    if (document) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("div { cursor: pointer; width: 10px }"));
        resolver.resolve(*document->getElementById("x"));
        resolvedLog = capturedLog;
        const std::size_t afterFirst = capturedLog.size();

        // Resolve again: resolve() walks every element of every stale frame, so
        // without dedupe a single unsupported property would flood the log forever.
        resolver.resolve(*document->getElementById("x"));
        resolver.resolve(*document->getElementById("x"));
        check(capturedLog.size() == afterFirst,
              "repeating the resolve does not repeat the warning");
    }

    auto mentions = [](const char* property) {
        for (const auto& line : resolvedLog) {
            if (line.find(property) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    check(mentions("cursor"), "an unsupported property is logged by name");
    check(!mentions("width"), "a supported property is not reported");

    // A different unsupported property is its own report; dedupe is per name.
    capturedLog.clear();
    if (document) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("div { -nonexistent-prop: 1 }"));
        resolver.resolve(*document->getElementById("x"));
    }
    check(capturedLog.size() > 0, "each unsupported property is reported once");

    SDL_SetLogOutputFunction(previous, previousUserdata);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, previousPriority);
}

void testLengthUnits() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"outer\"><div id=\"half\"></div><div id=\"em\"></div>"
        "<div id=\"rem\"></div><div id=\"padded\"></div></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRootFontSize(20.0f);
    resolver.setRules(sui::CSSParser::parse(
        "#outer { width: 400px }"
        "#half { width: 50% }"
        "#em { font-size: 10px; width: 5em }"
        "#rem { width: 2rem }"
        "#padded { padding: 10% 0 10% 0 }"));
    layoutRoot(*document, resolver, 800.0f);

    Element* outer = document->getElementById("outer");
    Element* half = document->getElementById("half");
    Element* em = document->getElementById("em");
    Element* rem = document->getElementById("rem");
    Element* padded = document->getElementById("padded");
    if (!outer || !half || !em || !rem || !padded) {
        check(false, "unit test elements found");
        return;
    }

    checkNear(half->layoutWidth(), 200.0f, 0.5f,
              "a percentage width resolves against the containing block");
    checkNear(half->layoutX(), outer->layoutX(), 0.5f,
              "the percentage child sits at its parent's content edge");
    checkNear(em->layoutWidth(), 50.0f, 0.5f, "em resolves against the element's own font size");
    checkNear(rem->layoutWidth(), 40.0f, 0.5f, "rem resolves against the root font size");

    // A percentage padding resolves against the containing block's inline size
    // even on the top and bottom edges, which is the rule browsers apply.
    checkNear(resolveBox(resolver.resolve(*padded), 400.0f, 600.0f, 20.0f).padding.top, 40.0f,
              0.5f, "a percentage padding uses the containing block width on the block axis");
    checkNear(resolveBox(resolver.resolve(*padded), 400.0f, 600.0f, 20.0f).padding.bottom, 40.0f,
              0.5f, "a percentage bottom padding resolves the same way");

    // The computed style keeps the unit, so a script can read it back.
    check(sui::StyleResolver::propertyValue(resolver.resolve(*half), "width") == "50%",
          "a computed percentage reads back with its unit");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*rem), "width") == "2rem",
          "a computed rem reads back with its unit");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*outer), "height") == "auto",
          "an unset height reads back as auto");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*outer), "max-width") == "none",
          "an unset max-width reads back as none");
}

void testAbsoluteAndKeywordUnits() {
    auto document = parseOrNull(
        "<sui><body><div id=\"d\" style=\"width: auto; max-width: none; "
        "padding: 12pt\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    Element* d = document->getElementById("d");
    if (!d) {
        check(false, "d found");
        return;
    }
    const auto s = resolver.resolve(*d);
    check(s.width.isAuto(), "the auto keyword means auto, not zero");
    check(s.maxWidth.isAuto(), "the none keyword means no maximum");
    // 12pt is 16px at the CSS 96/72 ratio.
    checkNear(s.padding.top.resolve(), 16.0f, 0.01f, "pt converts to px");
}

void testFontRelativeSizes() {
    auto document = parseOrNull(
        "<sui><body><div id=\"base\"><span id=\"em\"></span>"
        "<span id=\"pct\"></span><span id=\"root\"></span></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRootFontSize(20.0f);
    resolver.setRules(sui::CSSParser::parse(
        "#base { font-size: 10px }"
        "#em { font-size: 2em }"
        "#pct { font-size: 150% }"
        "#root { font-size: 1.5rem }"));
    Element* base = document->getElementById("base");
    Element* em = document->getElementById("em");
    Element* pct = document->getElementById("pct");
    Element* root = document->getElementById("root");
    if (!base || !em || !pct || !root) {
        check(false, "font-relative elements found");
        return;
    }

    // Font size is the one length style resolution can finish alone, so it comes
    // back as a plain number with no containing block needed.
    checkNear(resolver.resolve(*em).fontSize, 20.0f, 0.01f,
              "em font-size resolves against the parent's font size");
    checkNear(resolver.resolve(*pct).fontSize, 15.0f, 0.01f,
              "a percentage font-size resolves against the parent's font size");
    checkNear(resolver.resolve(*root).fontSize, 30.0f, 0.01f,
              "rem font-size resolves against the root font size");
    checkNear(resolver.resolve(*base).fontSize, 10.0f, 0.01f,
              "the parent keeps the size the children are relative to");
}

void testLineHeightUnits() {
    auto document =
        parseOrNull("<sui><body><div id=\"a\"></div><div id=\"b\"></div>"
                    "<div id=\"c\"></div><div id=\"d\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { font-size: 16px; line-height: 24px }"
        "#b { font-size: 16px; line-height: 1.5 }"
        "#c { font-size: 20px; line-height: 2em }"
        "#d { line-height: normal }"));
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* c = document->getElementById("c");
    Element* d = document->getElementById("d");
    if (!a || !b || !c || !d) {
        check(false, "line-height elements found");
        return;
    }

    // line-height is stored as a multiplier, so a length has to become a ratio.
    // Reading "24px" as a multiplier of 24 would be wildly wrong.
    checkNear(resolver.resolve(*a).lineHeight, 1.5f, 0.01f,
              "a px line-height becomes a ratio against the font size");
    checkNear(resolver.resolve(*b).lineHeight, 1.5f, 0.01f,
              "a unitless line-height is used as-is");
    checkNear(resolver.resolve(*c).lineHeight, 2.0f, 0.01f,
              "an em line-height becomes a ratio against the font size");
    checkNear(resolver.resolve(*d).lineHeight, 1.2f, 0.01f, "normal restores the default ratio");
}

void testBoxSizing() {
    auto document = parseOrNull(
        "<sui><body><div id=\"content\"></div><div id=\"border\"></div>"
        "<div id=\"tall\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#content { width: 200px; padding: 10px; border-width: 5px }"
        "#border { width: 200px; padding: 10px; border-width: 5px; "
        "box-sizing: border-box }"
        "#tall { height: 100px; padding: 10px; border-width: 5px; "
        "box-sizing: border-box }"));
    Element* content = document->getElementById("content");
    Element* border = document->getElementById("border");
    Element* tall = document->getElementById("tall");
    if (!content || !border || !tall) {
        check(false, "box-sizing elements found");
        return;
    }
    check(!document->root() || true, "layout ran");
    layoutRoot(*document, resolver, 800.0f);

    // content-box: the authored width is the content box, so chrome adds to it.
    checkNear(content->layoutWidth(), 230.0f, 0.5f,
              "content-box adds padding and border to the authored width");
    // border-box: the authored width already includes them, so it stays 200.
    checkNear(border->layoutWidth(), 200.0f, 0.5f,
              "border-box keeps the authored width as the outer width");
    checkNear(tall->layoutHeight(), 100.0f, 0.5f,
              "border-box applies to height as well");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*border), "box-sizing") ==
              "border-box",
          "box-sizing reads back");
}

void testPercentageMinMax() {
    auto document = parseOrNull(
        "<sui><body><div id=\"box\"><div id=\"child\"></div>"
        "<div id=\"minwins\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#box { width: 200px }"
        "#child { min-width: 50%; max-width: 75% }"
        "#minwins { min-width: 75%; max-width: 50% }"));
    Element* box = document->getElementById("box");
    Element* child = document->getElementById("child");
    Element* minwins = document->getElementById("minwins");
    if (!box || !child || !minwins) {
        check(false, "min/max elements found");
        return;
    }
    const sui::UsedBox used =
        resolveBox(resolver.resolve(*child), 200.0f, 600.0f, 16.0f);
    checkNear(used.minWidth, 100.0f, 0.5f, "a percentage min-width resolves");
    checkNear(used.maxWidth, 150.0f, 0.5f, "a percentage max-width resolves");

    layoutRoot(*document, resolver, 800.0f);
    // A block child fills its 200px parent, then clamps: max-width wins when it
    // is the tighter bound...
    checkNear(child->layoutWidth(), 150.0f, 0.5f,
              "a percentage max-width clamps a block that would otherwise fill");
    // ...and min-width wins when they conflict the other way.
    checkNear(minwins->layoutWidth(), 150.0f, 0.5f,
              "min-width wins over a tighter max-width");
}

void testPercentNestedLevels() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"><div id=\"b\"><div id=\"c\"></div></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { width: 400px } #b { width: 50% } #c { width: 50% }"));
    Element* c = document->getElementById("c");
    if (!c) {
        check(false, "c found");
        return;
    }
    layoutRoot(*document, resolver, 800.0f);
    // Each level halves the previous one: 400 -> 200 -> 100.
    checkNear(c->layoutWidth(), 100.0f, 0.5f,
              "a nested percentage resolves against its own parent, not the root");
}

void testCommentsInsideDeclarationBlocks() {
    // A comment anywhere in a block used to be glued onto the following property
    // name, which silently dropped that declaration.
    const auto rules = sui::CSSParser::parse(R"(
.sidebar {
    background-color: #27272a;
    /* explains the gutter */
    padding: 0.75rem;
    /* and the gap below it */
    margin-bottom: 0.75rem;
}
.other {
    padding /* inline */: 1rem;
}
)");
    check(rules.size() == 2, "comments between rules still produce two rules");
    if (rules.size() < 2) {
        return;
    }
    check(rules[0].declarations.size() == 3,
          "a comment does not swallow the declaration after it");
    check(rules[0].declarations.size() == 3 &&
              rules[0].declarations[1].property == "padding",
          "the property after a comment is read cleanly");
    check(rules[0].declarations.size() == 3 &&
              rules[0].declarations[1].value == "0.75rem",
          "the value after a comment is read cleanly");
    check(rules[1].declarations.size() == 1 && rules[1].declarations[0].property == "padding",
          "a comment inside a declaration reads cleanly too");
}

void testUnitsEndToEnd() {
    // The bug this covers was only visible once a stylesheet used rem units: the
    // comment above the declaration was part of the property name, so the rule
    // resolved to nothing at all.
    auto document = parseOrNull("<sui><body><div id=\"s\" class=\"sidebar\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRootFontSize(16.0f);
    resolver.setRules(sui::CSSParser::parse(
        ".sidebar {\n"
        "    background-color: #27272a;\n"
        "    /* the gutter scales with the user's text size */\n"
        "    padding: 0.75rem;\n"
        "    margin-bottom: 0.75rem;\n"
        "}"));
    Element* s = document->getElementById("s");
    if (!s) {
        check(false, "s found");
        return;
    }
    const auto style = resolver.resolve(*s);
    check(style.backgroundColor.r == 0x27, "the colour before the comment still applies");
    const sui::UsedBox used = resolveBox(style, 900.0f, 640.0f, resolver.rootFontSize());
    checkNear(used.padding.left, 12.0f, 0.01f,
              "a rem padding declared after a comment applies");
    checkNear(used.margin.bottom, 12.0f, 0.01f,
              "the declaration after that one applies too");
}

void testFlexRowBasics() {
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "<div id=\"c\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 600px }"
        "#a { width: 100px; height: 100px } #b { width: 200px; height: 120px } #c { width: 150px; height: 150px }"));
    layoutRoot(*document, resolver, 800.0f);

    Element* row = document->getElementById("row");
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* c = document->getElementById("c");
    if (!row || !a || !b || !c) {
        check(false, "flex row elements found");
        return;
    }
    // A row lays its children out side by side starting at the content edge.
    checkNear(a->layoutX(), 0.0f, 0.5f, "a row places its first item at the content edge");
    checkNear(b->layoutX(), 100.0f, 0.5f, "the second item follows the first");
    checkNear(c->layoutX(), 300.0f, 0.5f, "the third item follows the second");
    checkNear(a->layoutWidth(), 100.0f, 0.5f, "an item keeps the width it asked for");
    // The container is as tall as its tallest item.
    checkNear(row->layoutHeight(), 150.0f, 0.5f,
              "a row is as tall as its tallest item");
}

void testFlexColumn() {
    auto document = parseOrNull(
        "<sui><body><div id=\"col\"><div id=\"a\"></div><div id=\"b\"></div>"
        "</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #col { display: flex; flex-direction: column; width: 400px }"
        "#a { height: 40px } #b { height: 60px }"));
    layoutRoot(*document, resolver, 800.0f);

    Element* col = document->getElementById("col");
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    if (!col || !a || !b) {
        check(false, "flex column elements found");
        return;
    }
    // A column stacks downward instead of sideways.
    checkNear(a->layoutY(), 0.0f, 0.5f, "a column places its first item at the top");
    checkNear(b->layoutY(), 40.0f, 0.5f, "the second item is below the first");
    checkNear(b->layoutHeight(), 60.0f, 0.5f, "an item keeps the height it asked for");
    // Both items share the cross axis, and align-items stretches by default, so
    // each fills the container's width.
    checkNear(a->layoutWidth(), 400.0f, 0.5f,
              "a stretched item fills the cross axis");
    checkNear(col->layoutHeight(), 100.0f, 0.5f,
              "a column is as tall as the sum of its items");
}

void testFlexGrowAndShrink() {
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 600px }"
        "#a { flex: 1 } #b { flex: 3 }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    if (!a || !b) {
        check(false, "grow elements found");
        return;
    }
    // All 600px is free, shared 1:3.
    checkNear(a->layoutWidth(), 150.0f, 0.5f, "flex: 1 takes a quarter of the row");
    checkNear(b->layoutWidth(), 450.0f, 0.5f, "flex: 3 takes three quarters");
    checkNear(b->layoutX(), 150.0f, 0.5f, "the grown items sit side by side");

    // Now overflow the row and check that shrinking is weighted by base size.
    auto document2 = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "</div></body></sui>");
    if (!document2) {
        return;
    }
    sui::StyleResolver resolver2;
    resolver2.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 300px }"
        "#a { width: 100px; height: 10px } #b { width: 400px; height: 10px }"));
    layoutRoot(*document2, resolver2, 800.0f);
    Element* a2 = document2->getElementById("a");
    Element* b2 = document2->getElementById("b");
    if (!a2 || !b2) {
        check(false, "shrink elements found");
        return;
    }
    // 200px of overflow shared in proportion to the 100 and 400 base sizes, so the
    // small item gives up 40 and keeps 60 while the large one gives up 160.
    checkNear(a2->layoutWidth(), 60.0f, 0.5f,
              "an overflowing item shrinks by its share of the base size");
    checkNear(b2->layoutWidth(), 240.0f, 0.5f, "the larger item gives up more room");
}

void testFlexBasisAndShrinkZero() {
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 300px }"
        "#a { flex: 0 0 120px } #b { flex: 1 }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    if (!a || !b) {
        check(false, "basis elements found");
        return;
    }
    // flex-basis sets the size directly and shrink: 0 keeps it there even though
    // the two items together overflow the row.
    checkNear(a->layoutWidth(), 120.0f, 0.5f, "flex-basis decides the item's size");
    checkNear(b->layoutWidth(), 180.0f, 0.5f, "the remaining space goes to flex: 1");
}

void testFlexGap() {
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "<div id=\"c\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 600px; gap: 20px }"
        "#a, #b, #c { width: 100px; height: 10px }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* c = document->getElementById("c");
    if (!a || !b || !c) {
        check(false, "gap elements found");
        return;
    }
    // Three 100px items plus two 20px gaps leave 60px unused, which
    // justify-content: flex-start leaves at the end.
    checkNear(b->layoutX(), 120.0f, 0.5f, "gap separates the first pair");
    checkNear(c->layoutX(), 240.0f, 0.5f, "gap separates the second pair");
    checkNear(a->layoutX(), 0.0f, 0.5f, "flex-start leaves no space before the first item");

    // row-gap must not move items sideways, and column-gap must.
    auto document2 = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div></div></body></sui>");
    if (!document2) {
        return;
    }
    sui::StyleResolver resolver2;
    resolver2.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; row-gap: 30px; column-gap: 5px; width: 300px }"
        "#a { width: 50px; height: 10px }"));
    Element* a2 = document2->getElementById("a");
    if (!a2) {
        return;
    }
    layoutRoot(*document2, resolver2, 800.0f);
    checkNear(a2->layoutX(), 0.0f, 0.5f, "row-gap does not affect the inline axis");
}

void testJustifyContent() {
    const char* css =
        "body { margin: 0 } #row { display: flex; width: 400px; %s } #a { width: 100px; height: 10px } "
        "#b { width: 100px; height: 10px }";

    struct Case {
        const char* value;
        float expectA;
        float expectB;
        const char* what;
    };
    const Case cases[] = {
        {"justify-content: flex-start;", 0.0f, 100.0f, "flex-start packs at the start"},
        {"justify-content: flex-end;", 200.0f, 300.0f,
         "flex-end packs at the end"},
        {"justify-content: center;", 100.0f, 200.0f,
         "center puts the free space on both sides"},
        {"justify-content: space-between;", 0.0f, 300.0f,
         "space-between pushes the items apart"},
        {"justify-content: space-around;", 50.0f, 250.0f,
         "space-around gives each item room on both sides"},
        {"justify-content: space-evenly;", 66.7f, 233.3f,
         "space-evenly splits the space evenly"},
    };

    for (const Case& c : cases) {
        auto document = parseOrNull(
            "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(std::string("body { margin: 0 } #row { display: flex; width: 400px; ") +
                                                c.value +
                                                " } #a { width: 100px; height: 10px } "
                                                "#b { width: 100px; height: 10px }"));
        layoutRoot(*document, resolver, 800.0f);
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        if (!a || !b) {
            check(false, "justify elements found");
            return;
        }
        checkNear(a->layoutX(), c.expectA, 1.0f, c.what);
        checkNear(b->layoutX(), c.expectB, 1.0f, c.what);
    }
}

void testFlexAlignItems() {
    struct Case {
        const char* value;
        float expectY;
        const char* what;
    };
    // The row is 200px tall and the single item 20px, so 180px is free to align.
    const Case cases[] = {
        {"align-items: stretch;", 0.0f, "stretch starts at the line's cross edge"},
        {"align-items: flex-start;", 0.0f, "flex-start pins to the cross start"},
        {"align-items: center;", 90.0f, "center halves the free cross space"},
        {"align-items: flex-end;", 180.0f, "flex-end pins to the cross end"},
    };
    for (const Case& c : cases) {
        auto document = parseOrNull(
            "<sui><body><div id=\"row\"><div id=\"a\"></div></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            std::string("body { margin: 0 } #row { display: flex; width: 400px; height: 200px; ") + c.value +
            " } #a { width: 100px; height: 20px }"));
        Element* a = document->getElementById("a");
        if (!a) {
            check(false, "align element found");
            return;
        }
        layoutRoot(*document, resolver, 800.0f);
        checkNear(a->layoutY(), c.expectY, 1.0f, c.what);
    }

    // `align-self` on the item beats the container's `align-items`.
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 400px; height: 200px; align-items: flex-start }"
        "#a { width: 100px; height: 20px; align-self: flex-end }"));
    Element* a = document->getElementById("a");
    if (!a) {
        return;
    }
    layoutRoot(*document, resolver, 800.0f);
    checkNear(a->layoutY(), 180.0f, 1.0f, "align-self overrides align-items");
}

void testFlexWrap() {
    // Four 200px items in a 500px row wrap onto two lines of two.
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "<div id=\"c\"></div><div id=\"d\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; flex-wrap: wrap; width: 500px }"
        "#a, #b, #c, #d { width: 200px; height: 30px }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* row = document->getElementById("row");
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* c = document->getElementById("c");
    Element* d = document->getElementById("d");
    if (!row || !a || !b || !c || !d) {
        check(false, "wrap elements found");
        return;
    }
    checkNear(b->layoutX(), 200.0f, 0.5f, "the second item stays on the first line");
    // The third would end at 600px, past the 500px row, so it starts a new line.
    checkNear(c->layoutX(), 0.0f, 0.5f, "a wrapping item returns to the cross start");
    checkNear(c->layoutY(), 30.0f, 0.5f, "the new line sits below the first");
    checkNear(d->layoutX(), 200.0f, 0.5f, "the fourth continues on the second line");
    checkNear(row->layoutHeight(), 60.0f, 0.5f, "the container is as tall as both lines");

    // Without wrapping the same items overflow instead of stacking.
    auto document2 = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"c\"></div>"
        "</div></body></sui>");
    if (!document2) {
        return;
    }
    sui::StyleResolver resolver2;
    resolver2.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 500px }"
        "#a, #c { width: 200px; height: 30px }"));
    layoutRoot(*document2, resolver2, 800.0f);
    Element* a2 = document2->getElementById("a");
    Element* c2 = document2->getElementById("c");
    if (!a2 || !c2) {
        return;
    }
    checkNear(c2->layoutY(), 0.0f, 0.5f, "a no-wrap container keeps every item on one line");
    checkNear(c2->layoutX(), 200.0f, 0.5f, "a no-wrap container lets its items overflow");
}

void testFlexReverseDirections() {
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; flex-direction: row-reverse; width: 400px }"
        "#a { width: 100px; height: 10px } #b { width: 100px; height: 10px }"));
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    if (!a || !b) {
        check(false, "row-reverse elements found");
        return;
    }
    layoutRoot(*document, resolver, 800.0f);
    // The first item ends up at the far end, the second before it.
    checkNear(a->layoutX(), 300.0f, 0.5f, "row-reverse puts the first item at the end");
    checkNear(b->layoutX(), 200.0f, 0.5f, "the second item precedes it");

    // column-reverse stacks upward from the bottom of a fixed-height container.
    auto document2 = parseOrNull(
        "<sui><body><div id=\"col\"><div id=\"a\"></div><div id=\"b\"></div>"
        "</div></body></sui>");
    if (!document2) {
        return;
    }
    sui::StyleResolver resolver2;
    resolver2.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #col { display: flex; flex-direction: column-reverse; height: 300px }"
        "#a { height: 50px } #b { height: 50px }"));
    Element* a2 = document2->getElementById("a");
    Element* b2 = document2->getElementById("b");
    if (!a2 || !b2) {
        return;
    }
    layoutRoot(*document2, resolver2, 800.0f);
    checkNear(a2->layoutY(), 250.0f, 0.5f,
              "column-reverse puts the first item at the bottom");
    checkNear(b2->layoutY(), 200.0f, 0.5f, "the second item sits above it");
}

void testFlexItemMarginsAndPadding() {
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 400px }"
        "#a { width: 100px; height: 10px; margin-left: 20px }"
        "#b { width: 100px; height: 10px }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    if (!a || !b) {
        check(false, "margin elements found");
        return;
    }
    // A margin pushes the item along the main axis and takes room of its own.
    checkNear(a->layoutX(), 20.0f, 0.5f, "a margin offsets the item from the line start");
    checkNear(b->layoutX(), 120.0f, 0.5f, "the margin is included in the item's footprint");

    // Padding is inside the item, so it must not move the next item.
    auto document2 = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "</div></body></sui>");
    if (!document2) {
        return;
    }
    sui::StyleResolver resolver2;
    resolver2.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 400px }"
        "#a { width: 100px; height: 10px; padding: 5px }"
        "#b { width: 100px; height: 10px }"));
    Element* a2 = document2->getElementById("a");
    Element* b2 = document2->getElementById("b");
    if (!a2 || !b2) {
        return;
    }
    layoutRoot(*document2, resolver2, 800.0f);
    checkNear(b2->layoutX(), 110.0f, 0.5f,
              "padding adds to the item's outer width, moving the next one");
    checkNear(a2->layoutWidth(), 110.0f, 0.5f, "the padded item is wider by its padding");
}

void testFlexMinMaxClamp() {
    // The items ask for 300px each in a 400px row, so something has to give. A
    // min-width stops that from going too far.
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 400px }"
        "#a { flex: 1; min-width: 250px } #b { flex: 1 }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    if (!a || !b) {
        check(false, "min-width elements found");
        return;
    }
    checkNear(a->layoutWidth(), 250.0f, 0.5f, "min-width stops an item shrinking further");
    checkNear(b->layoutWidth(), 150.0f, 0.5f, "the other item absorbs the shortfall");
}

void testFlexInlineFlexShrinks() {
    // `inline-flex` is a flex container that sits in the inline flow, so it must
    // size itself to its contents rather than filling the line.
    auto document = parseOrNull(
        "<sui><body><p id=\"line\">before <span id=\"f\"><span id=\"a\"></span>"
        "<span id=\"b\"></span></span> after</p></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #f { display: inline-flex; gap: 10px }"
        "#a { width: 60px; height: 20px } #b { width: 60px; height: 20px }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* f = document->getElementById("f");
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    if (!f || !a || !b) {
        check(false, "inline-flex elements found");
        return;
    }
    checkNear(f->layoutWidth(), 130.0f, 0.5f,
              "inline-flex shrinks to its items plus the gap");
    checkNear(b->layoutX() - a->layoutX(), 70.0f, 0.5f,
              "inline-flex lays its items out as a row");
}

void testFlexPropertyParsing() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div>"
        "<div id=\"d\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { display: flex; flex-direction: column-reverse; flex-wrap: wrap-reverse; "
        "justify-content: space-evenly; align-items: baseline; gap: 7px }"
        "#b { display: inline-flex }"
        "#c { flex: 2 0 50px }"
        "#d { flex: none }"));
    auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("a", "display") == "flex", "display: flex parses");
    check(value("b", "display") == "inline-flex", "display: inline-flex parses");
    check(value("a", "gap") == "", "gap has no computed-value form yet");
    check(value("a", "flex-direction") == "column-reverse",
          "flex-direction reads back");
    check(value("a", "justify-content") == "space-evenly", "justify-content reads back");
    check(value("a", "align-items") == "baseline", "align-items reads back");
    check(value("a", "flex-wrap") == "wrap-reverse", "flex-wrap reads back");

    Element* c = document->getElementById("c");
    if (c) {
        const auto s = resolver.resolve(*c);
        checkNear(s.flex.grow, 2.0f, 0.01f, "the flex shorthand sets grow");
        checkNear(s.flex.shrink, 0.0f, 0.01f, "the flex shorthand sets shrink");
        checkNear(s.flex.basis.resolve(400.0f, 16.0f, 16.0f), 50.0f, 0.01f,
                  "the flex shorthand sets basis");
    }
    Element* d = document->getElementById("d");
    if (d) {
        const auto s = resolver.resolve(*d);
        checkNear(s.flex.grow, 0.0f, 0.01f, "flex: none does not grow");
        checkNear(s.flex.shrink, 0.0f, 0.01f, "flex: none does not shrink");
    }
}

void testFlexIsBlockLevel() {
    // `display: flex` puts the box in the block formatting context, so a flex
    // container sits on its own line and its sibling lands below it. Only
    // `inline-flex` behaves like an inline-level box.
    auto document = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div></div>"
        "<div id=\"after\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 200px; height: 50px }"
        "#a { width: 20px; height: 20px } #after { height: 30px }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* row = document->getElementById("row");
    Element* after = document->getElementById("after");
    if (!row || !after) {
        check(false, "flex block-level elements found");
        return;
    }
    checkNear(row->layoutY(), 0.0f, 0.5f, "a flex container starts a new block");
    checkNear(after->layoutY(), 50.0f, 0.5f,
              "the block after a flex container starts below it");
    checkNear(after->layoutWidth(), 800.0f, 0.5f,
              "the block after a flex container still fills the parent");
}

void testFlexNestedContainer() {
    auto document = parseOrNull(
        "<sui><body><div id=\"outer\"><div id=\"mid\"><div id=\"a\"></div>"
        "<div id=\"b\"></div></div><div id=\"side\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #outer { display: flex; width: 500px }"
        "#mid { display: flex; flex: 2 } #side { flex: 1 }"
        "#a { flex: 1; height: 10px } #b { flex: 1; height: 10px }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* mid = document->getElementById("mid");
    Element* side = document->getElementById("side");
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    if (!mid || !side || !a || !b) {
        check(false, "nested flex elements found");
        return;
    }
    checkNear(mid->layoutWidth(), 333.33f, 1.0f, "the first item takes two parts of the row");
    checkNear(side->layoutX(), 333.33f, 1.0f, "the second item follows it");
    // Inside `mid` the space is split again between its two children.
    checkNear(b->layoutX() - a->layoutX(), 166.67f, 1.0f,
              "a nested flex container splits its own share");
}

void testImportantAndCssWideKeywords() {
    auto document = parseOrNull(
        "<sui><body><div style=\"width:300px;padding:20px\">"
        "<p id=\"t\">x</p></div></body></sui>");
    if (!document) {
        return;
    }
    Element* t = document->getElementById("t");
    check(t != nullptr, "important/keyword elements found");
    if (!t) {
        return;
    }

    // `!important` is a cascade level, so a type rule carrying it beats a later id
    // rule that does not.
    sui::StyleResolver r1;
    r1.setRules(sui::CSSParser::parse("p{color:#0000ff !important} #t{color:#ff0000}"));
    check(r1.resolve(*t).color.b == 0xff && r1.resolve(*t).color.r == 0x00,
          "!important beats a stronger ordinary selector");

    // Without it, specificity decides as usual.
    sui::StyleResolver r2;
    r2.setRules(sui::CSSParser::parse("p{color:#0000ff} #t{color:#ff0000}"));
    check(r2.resolve(*t).color.r == 0xff, "an ordinary id rule still beats a type rule");

    // `!important` inside the style attribute outranks every rule.
    sui::ParseError e3;
    auto d3 = parseOrNull(
        "<sui><body><p id=\"t\" style=\"color:#00ff00 !important\">x</p></body></sui>");
    if (d3) {
        sui::StyleResolver r3;
        r3.setRules(sui::CSSParser::parse("#t{color:#ff0000}"));
        const auto s = r3.resolve(*d3->getElementById("t"));
        check(s.color.g == 0xff, "an inline !important beats every rule");
    }

    // `inherit` takes the parent's value, including for a property that does not
    // normally inherit.
    sui::StyleResolver r4;
    r4.setRules(sui::CSSParser::parse("#t{width:inherit}"));
    check(r4.resolve(*t).width.isAuto() == false,
          "width:inherit produces a length instead of auto");
    checkNear(r4.resolve(*t).width.resolve(0.0f, 16.0f, 16.0f), 300.0f, 0.01f,
              "width:inherit takes the parent's width");

    // `unset` inherits for an inheritable property...
    sui::StyleResolver r5;
    r5.setRules(sui::CSSParser::parse("body{color:#00ff00} #t{color:unset}"));
    check(r5.resolve(*t).color.g == 0xff, "color:unset inherits");

    // ...and does nothing for one that does not.
    sui::StyleResolver r6;
    r6.setRules(sui::CSSParser::parse("#t{width:80px} #t{width:unset}"));
    checkNear(r6.resolve(*t).width.resolve(0.0f, 16.0f, 16.0f), 80.0f, 0.01f,
              "width:unset leaves the earlier declaration alone");

    // `initial` and `revert` are not implemented: an unparseable token resolves to
    // auto rather than to a number, and for a border side that is where the UA
    // starts, so nothing moves.
    sui::StyleResolver r7;
    r7.setRules(sui::CSSParser::parse("#t{border-width-left:initial}"));
    check(r7.resolve(*t).borderWidth.left.isAuto(),
          "border-width-left is untouched by an unknown keyword");
}

// These four were readable through propertyValue() but had no setter branch, so
// writing one was reported as an unknown property while reading it back looked
// correct. CSS spells them `border-*-width`; the engine's own spelling is
// `border-width-*`, and both are now accepted either way round.
void testBorderWidthLonghands() {
    auto document = parseOrNull("<sui><body><div id=\"t\">x</div></body></sui>");
    if (!document) {
        return;
    }
    Element* t = document->getElementById("t");
    if (!t) {
        return;
    }

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#t{border-left-width:4px;border-top-width:6px;border-width-right:8px}"));
    const auto s = resolver.resolve(*t);
    checkNear(s.borderWidth.left.resolve(0.0f, 16.0f, 16.0f), 4.0f, 0.01f,
              "border-left-width sets the left side");
    checkNear(s.borderWidth.top.resolve(0.0f, 16.0f, 16.0f), 6.0f, 0.01f,
              "border-top-width sets the top side");
    checkNear(s.borderWidth.right.resolve(0.0f, 16.0f, 16.0f), 8.0f, 0.01f,
              "the legacy border-width-right spelling still sets the right side");
    check(s.borderWidth.bottom.isAuto(), "the sides no rule names are left alone");

    check(sui::StyleResolver::propertyValue(s, "border-left-width") == "4px",
          "the CSS spelling reads back");
    check(sui::StyleResolver::propertyValue(s, "border-width-left") == "4px",
          "the legacy spelling reads back too");
}

void testPositioning() {
    // An absolute box resolves against the padding box of the nearest positioned
    // ancestor, which is only known once the flow has placed that ancestor.
    auto document = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"stage\" style=\"position:relative;width:200px;height:100px;"
        "padding:10px;border-width:5px\">"
        "<div id=\"box\" style=\"position:absolute;left:30px;top:40px;"
        "width:50px;height:20px\"></div>"
        "</div>"
        "<div id=\"after\" style=\"height:10px\"></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    layoutRoot(*document, resolver, 400.0f);
    Element* stage = document->getElementById("stage");
    Element* box = document->getElementById("box");
    Element* after = document->getElementById("after");
    check(stage && box && after, "positioning elements found");
    if (!stage || !box || !after) {
        return;
    }

    // The stage's padding box starts at 5 + 10 and is 200 + 20 wide.
    checkNear(stage->layoutX(), 0.0f, 0.01f, "the positioned ancestor is at the flow position");
    checkNear(box->layoutX(), 45.0f, 0.01f, "position:absolute left is relative to the padding box");
    checkNear(box->layoutY(), 55.0f, 0.01f, "position:absolute top is relative to the padding box");
    checkNear(box->layoutWidth(), 50.0f, 0.01f, "an absolute box keeps its own width");

    // An out-of-flow box contributes nothing to its parent's height, so the sibling
    // below the stage sits right under it rather than under the absolute child.
    checkNear(after->layoutY(), stage->layoutY() + stage->layoutHeight(), 0.01f,
              "an absolute box takes no space in its parent");
    check(box->layoutHeight() < stage->layoutHeight(),
          "the absolute box does not stretch the stage");

    // `right` and `bottom` anchor the far edges instead.
    auto corner = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"s2\" style=\"position:relative;width:200px;height:100px\">"
        "<div id=\"b2\" style=\"position:absolute;right:10px;bottom:20px;"
        "width:50px;height:30px\"></div>"
        "</div></body></sui>");
    if (corner) {
        sui::StyleResolver r2;
        layoutRoot(*corner, r2, 400.0f);
        Element* b2 = corner->getElementById("b2");
        checkNear(b2->layoutX(), 140.0f, 0.01f, "position:absolute right anchors the far edge");
        checkNear(b2->layoutY(), 50.0f, 0.01f, "position:absolute bottom anchors the far edge");
    }

    // With no positioned ancestor the containing block is the viewport.
    auto viewport = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"b3\" style=\"position:absolute;left:15px;top:25px;width:40px;height:10px\">"
        "</div></body></sui>");
    if (viewport) {
        sui::StyleResolver r3;
        layoutRoot(*viewport, r3, 400.0f);
        Element* b3 = viewport->getElementById("b3");
        checkNear(b3->layoutX(), 15.0f, 0.01f, "the viewport is the containing block by default");
        checkNear(b3->layoutY(), 25.0f, 0.01f, "the viewport anchors top too");
    }

    // Both edges given means the size comes from them.
    auto stretched = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"b4\" style=\"position:absolute;left:10px;right:10px;top:0;bottom:0\">"
        "</div></body></sui>");
    if (stretched) {
        sui::StyleResolver r4;
        layoutRoot(*stretched, r4, 400.0f);
        Element* b4 = stretched->getElementById("b4");
        checkNear(b4->layoutWidth(), 380.0f, 0.01f,
                  "left and right together set the width");
        checkNear(b4->layoutHeight(), 600.0f, 0.01f,
                  "top and bottom together set the height to the viewport");
    }

    // `relative` moves the box after the flow has placed it and leaves its space
    // alone, so the sibling below does not move.
    auto shifted = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"r1\" style=\"position:relative;left:20px;top:5px;height:10px\"></div>"
        "<div id=\"r2\" style=\"height:10px\"></div></body></sui>");
    if (shifted) {
        sui::StyleResolver r5;
        layoutRoot(*shifted, r5, 400.0f);
        Element* r1 = shifted->getElementById("r1");
        Element* r2 = shifted->getElementById("r2");
        checkNear(r1->layoutX(), 20.0f, 0.01f, "position:relative left offsets the box");
        checkNear(r1->layoutY(), 5.0f, 0.01f, "position:relative top offsets the box");
        checkNear(r2->layoutY(), 10.0f, 0.01f,
                  "a relative box leaves the space it occupied in the flow");
    }

    // `right` and `bottom` subtract rather than add.
    auto negative = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"r3\" style=\"position:relative;right:20px;bottom:5px;height:10px\"></div>"
        "</body></sui>");
    if (negative) {
        sui::StyleResolver r6;
        layoutRoot(*negative, r6, 400.0f);
        Element* r3 = negative->getElementById("r3");
        checkNear(r3->layoutX(), -20.0f, 0.01f, "position:relative right subtracts");
        checkNear(r3->layoutY(), -5.0f, 0.01f, "position:relative bottom subtracts");
    }

    // `left` wins over `right`, which is the one direction CSS resolves by leaving
    // the pair over-specified.
    auto both = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"r4\" style=\"position:relative;left:7px;right:99px;height:10px\"></div>"
        "</body></sui>");
    if (both) {
        sui::StyleResolver r7;
        layoutRoot(*both, r7, 400.0f);
        checkNear(both->getElementById("r4")->layoutX(), 7.0f, 0.01f,
                  "left wins when both offsets are given");
    }
}

void testWhiteSpace() {
    const char* source =
        "<sui><body style=\"margin:0\">"
        "<div id=\"t\" style=\"width:80px\">one two three four five</div>"
        "</body></sui>";

    // 80px fits about two of these words, so the box is one line tall when wrapping
    // is off and several lines tall when it is on.
    const char* const cases[] = {"normal", "nowrap", "pre", "pre-wrap"};
    for (const char* value : cases) {
        auto document = parseOrNull(source);
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(
            sui::CSSParser::parse(std::string("#t{white-space:") + value + "}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* box = document->getElementById("t");
        const std::string what = std::string("white-space:") + value;
        const std::size_t runs = box->textRuns().size();
        std::printf("     %-22s -> %zu runs, height %.1f\n", what.c_str(), runs,
                    box->layoutHeight());

        if (std::string_view(value) == "nowrap") {
            // One line, one line height, whatever the words do.
            check(runs == 5, "white-space:nowrap keeps every word but never breaks");
            check(box->layoutHeight() < 40.0f,
                  "white-space:nowrap leaves the text on a single line");
        }
        if (std::string_view(value) == "normal") {
            check(runs > 1, "white-space:normal breaks into several runs");
            check(box->layoutHeight() > 40.0f, "white-space:normal wraps onto more lines");
        }
        if (std::string_view(value) == "pre") {
            check(runs == 1, "white-space:pre takes the line whole");
            check(box->layoutHeight() < 40.0f, "white-space:pre never wraps");
        }
        if (std::string_view(value) == "pre-wrap") {
            check(runs > 1, "white-space:pre-wrap still wraps");
            check(box->layoutHeight() > 40.0f, "white-space:pre-wrap fills more than one line");
        }
    }

    // A newline in the text is a line break for `pre` and `pre-wrap`, and ordinary
    // whitespace for everything else.
    const char* broken =
        "<sui><body style=\"margin:0\">"
        "<div id=\"t\" style=\"width:400px;white-space:%s;padding:0;margin:0\">"
        "one\ntwo</div></body></sui>";
    for (const char* value : {"pre", "pre-wrap", "normal"}) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), broken, value);
        auto document = parseOrNull(buffer);
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        layoutRoot(*document, resolver, 400.0f);
        Element* box = document->getElementById("t");
        const bool keeps = std::string_view(value) != "normal";
        const std::string label = keeps ? "a newline in white-space:" + std::string(value) +
                                             " breaks the line"
                                        : "a newline in normal white-space is just a space";
        const float oneLine = box->textRuns().front().height;
        checkNear(box->layoutHeight(), keeps ? 2 * oneLine : oneLine, 0.5f, label.c_str());
    }
}

void testPseudoElements() {
    auto document = parseOrNull(
        "<sui><body><div id=\"card\"><span id=\"kid\">k</span></div></body></sui>");
    if (!document) {
        return;
    }
    Element* card = document->getElementById("card");
    if (!card) {
        return;
    }

    // Parsing.
    const auto before = sui::CSSParser::parseSelector("#card::before");
    check(!before.unsupported, "::before is a supported selector");
    check(before.parts.back().kind == sui::Selector::Kind::PseudoElement,
          "::before parses as a pseudo-element part");
    check(before.parts.back().value == "::before", "the pseudo-element part keeps its name");
    check(sui::CSSParser::parseSelector("#card::after").parts.back().value == "::after",
          "::after parses too");
    check(sui::CSSParser::parseSelector("#card::marker").unsupported,
          "an ungenerated pseudo-element matches nothing");
    check(sui::CSSParser::parseSelector("#card::first-line").unsupported,
          "::first-line matches nothing");

    // A pseudo-element counts in the type group, so `#card::before` outranks
    // `#card` exactly as `#card span` would.
    check(sui::StyleResolver::specificityOf(sui::CSSParser::parseSelector("#card::before")) ==
              sui::StyleResolver::specificityOf(sui::CSSParser::parseSelector("#card")) + 1,
          "a pseudo-element counts in the type group");

    // Generation.
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#card::before { content: \"A\" } #card::after { content: \"B\" }"));
    sui::generatePseudoElements(*document, resolver);

    check(card->children().size() == 3, "::before and ::after become children");
    check(card->children()[0]->tag() == "::before", "::before comes first");
    check(card->children()[1]->id() == "kid", "the real content keeps its place");
    check(card->children()[2]->tag() == "::after", "::after comes last");
    check(card->children()[0]->textContent() == "A", "::before carries its content");
    check(card->children()[2]->textContent() == "B", "::after carries its content");
    check(card->children()[0]->isPseudoElement(), "a generated box is marked as one");
    check(!card->children()[1]->isPseudoElement(), "real content is not marked as one");
    check(resolver.resolve(*card->children()[0]).display == sui::ComputedStyle::Inline,
          "a generated box joins the inline flow");

    // A generated box is styled by the rule that made it and inherits the rest from
    // the element it hangs off, exactly as the element itself is.
    sui::StyleResolver styled;
    styled.setRules(sui::CSSParser::parse(
        "body { color: #00ff00 } #card::before { content: \"A\"; font-size: 30px }"));
    sui::generatePseudoElements(*document, styled);
    check(styled.resolve(*card->children()[0]).fontSize > 29.0f,
          "::before takes its own font-size");
    check(styled.resolve(*card->children()[0]).color.g == 0xff,
          "::before inherits the document colour");
    check(selectorMatches(sui::CSSParser::parseSelector("#card"), *card->children()[0]),
          "a rule on the originating element applies to its ::before");
    check(!selectorMatches(sui::CSSParser::parseSelector("body"), *card->children()[0]),
          "a rule for an unrelated ancestor does not apply to the box");
    check(!selectorMatches(sui::CSSParser::parseSelector("#card::after"),
                           *card->children()[0]),
          "#card::after does not match the ::before box");
    check(selectorMatches(sui::CSSParser::parseSelector("#card::before"),
                          *card->children()[0]),
          "#card::before matches the ::before box");
    check(!selectorMatches(sui::CSSParser::parseSelector("span"), *card->children()[0]),
          "a type selector for the original content misses a generated box");

    // No `content` means no box at all, which is also what `content: none` means.
    sui::StyleResolver bare;
    bare.setRules(sui::CSSParser::parse("#card::before { color: #ff0000 }"));
    sui::generatePseudoElements(*document, bare);
    check(card->children().size() == 1, "a rule with no content generates nothing");
    sui::StyleResolver none;
    none.setRules(sui::CSSParser::parse("#card::before { content: none }"));
    sui::generatePseudoElements(*document, none);
    check(card->children().size() == 1, "content: none generates nothing");

    // Rebuilding replaces rather than accumulating, or every frame would double up.
    sui::generatePseudoElements(*document, resolver);
    check(card->children().size() == 3, "regenerating does not duplicate the boxes");
    check(card->children()[0]->textContent() == "A", "regenerating still puts ::before first");
    check(card->children()[1]->id() == "kid", "regenerating keeps the real content in order");
}

}  // namespace

// Resolves the style of every child of the body against `resolver`, so a stylesheet can
// be checked positionally: the nth element matches the nth rule in the test source.
std::vector<ComputedStyle> bodyStyles(Document& document, sui::StyleResolver& resolver) {
    Element* body = document.querySelector("body");
    std::vector<ComputedStyle> styles;
    if (!body) {
        return styles;
    }
    for (const auto& child : body->children()) {
        if (child) {
            styles.push_back(resolver.resolve(*child));
        }
    }
    return styles;
}

void testBackgroundColourParsing() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { background-color: #f0a8 }
        .b { background-color: #ff00aa80 }
        .c { background-color: red }
        .d { background-color: GREEN }
        .e { background-color: transparent }
        .f { background-color: green }
        .g { background-color: lime }
        .h { background-color: #ABCDEF }
        .i { background-color: not-a-colour }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);

    auto document = parseOrNull("<sui><body>"
                                "<div class=\"a\"></div><div class=\"b\"></div>"
                                "<div class=\"c\"></div><div class=\"d\"></div>"
                                "<div class=\"e\"></div><div class=\"f\"></div>"
                                "<div class=\"g\"></div><div class=\"h\"></div>"
                                "<div class=\"i\"></div></body></sui>");
    if (!document) {
        return;
    }
    Element* root = document->root();
    const std::vector<ComputedStyle> styles = bodyStyles(*document, resolver);
    if (styles.size() < 9) {
        check(false, "background colour elements found");
        return;
    }
    const SDL_Color& a = styles[0].backgroundColor;
    check(a.r == 255 && a.g == 0 && a.b == 170 && a.a == 136, "#f0a8 short-form hex");
    const SDL_Color& b = styles[1].backgroundColor;
    check(b.r == 255 && b.b == 170 && b.a == 128, "eight-digit hex carries alpha");
    check(styles[2].backgroundColor.r == 255 && styles[2].backgroundColor.g == 0,
          "the name red");
    check(styles[3].backgroundColor.g == 128, "a colour name is case-insensitive");
    check(styles[4].backgroundColor.a == 0, "transparent is a zero alpha");
    // CSS `green` is the darker #008000 and `lime` is the brighter #00ff00, which is
    // the opposite of what most people expect and the one that shows up in a gradient.
    check(styles[5].backgroundColor.g == 128 && styles[5].backgroundColor.b == 0,
          "the name green is #008000, not lime");
    check(styles[6].backgroundColor.g == 255, "the name lime is #00ff00");
    const SDL_Color& h = styles[7].backgroundColor;
    check(h.r == 0xab && h.g == 0xcd && h.b == 0xef, "hex digits are case-insensitive");
    check(styles[8].backgroundColor.r == 0 && styles[8].backgroundColor.g == 0 &&
              styles[8].backgroundColor.b == 0 && styles[8].backgroundColor.a == 0,
          "an unparseable colour falls back to transparent");
}

void testLinearGradientParsing() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { background-image: linear-gradient(red, blue) }
        .b { background-image: linear-gradient(to right, red, blue) }
        .c { background-image: linear-gradient(90deg, red, blue) }
        .d { background-image: linear-gradient(0.5turn, red, blue) }
        .e { background-image: linear-gradient(100grad, red, blue) }
        .f { background-image: linear-gradient(1.57rad, red, blue) }
        .g { background-image: linear-gradient(to bottom right, red, blue) }
        .h { background-image: linear-gradient(red 20%, blue 80%) }
        .i { background-image: linear-gradient(red, blue, lime) }
        .j { background-image: linear-gradient(sideways, red, blue) }
        .k { background-image: linear-gradient(red) }
        .l { background-image: linear-gradient(to right 90deg, red, blue) }
        .m { background-image: linear-gradient(#fff) }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div><div class=\"h\"></div><div class=\"i\"></div>"
        "<div class=\"j\"></div><div class=\"k\"></div><div class=\"l\"></div>"
        "<div class=\"m\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 13) {
        check(false, "linear gradient elements found");
        return;
    }
    using Kind = sui::ComputedStyle::BackgroundImage::Kind;
    // No prelude means `to bottom`, which is 180 degrees clockwise from the top.
    check(styles[0].backgroundImages.size() == 1 &&
              styles[0].backgroundImages[0].kind == Kind::LinearGradientImage,
          "a bare linear-gradient still parses");
    checkNear(styles[0].backgroundImages[0].linear.angle, 180.0f, 0.01f,
              "a bare linear-gradient points down");
    check(styles[0].backgroundImages[0].linear.stops.size() == 2,
          "two stops for two colours");
    checkNear(styles[1].backgroundImages[0].linear.angle, 90.0f, 0.01f, "to right is 90deg");
    checkNear(styles[2].backgroundImages[0].linear.angle, 90.0f, 0.01f, "90deg parses");
    checkNear(styles[3].backgroundImages[0].linear.angle, 180.0f, 0.01f, "a turn converts");
    checkNear(styles[4].backgroundImages[0].linear.angle, 90.0f, 0.01f, "a grad converts");
    checkNear(styles[5].backgroundImages[0].linear.angle, 90.0f, 0.5f, "a rad converts");
    // Angles are clockwise from "to top", so a diagonal sits halfway between the two
    // edges it names: bottom is 180 and right is 90, putting the corner at 135.
    checkNear(styles[6].backgroundImages[0].linear.angle, 135.0f, 0.01f,
              "to bottom right is 135deg");
    check(styles[7].backgroundImages[0].linear.stops.size() == 2 &&
              styles[7].backgroundImages[0].linear.stops[0].offset.unit ==
                  sui::Length::Percent &&
              styles[7].backgroundImages[0].linear.stops[0].offset.value == 20.0f,
          "an explicit stop offset is kept as a percentage");
    check(styles[8].backgroundImages[0].linear.stops.size() == 3,
          "three colours give three stops");
    // An unauthored offset has to stay distinguishable from an authored 0%.
    check(styles[0].backgroundImages[0].linear.stops[0].offset.unit == sui::Length::Auto,
          "an unauthored offset is Auto, not 0%");
    check(styles[9].backgroundImages[0].kind == Kind::None,
          "a bad direction drops the whole gradient");
    check(styles[10].backgroundImages[0].kind == Kind::LinearGradientImage,
          "a single colour is still a gradient");
    check(styles[11].backgroundImages[0].kind == Kind::None,
          "two directions in one prelude is not a direction");
    check(styles[12].backgroundImages[0].kind == Kind::LinearGradientImage,
          "a one-stop gradient paints as a flat fill");
}

void testRadialGradientParsing() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { background-image: radial-gradient(white, black) }
        .b { background-image: radial-gradient(circle closest-side, white, black) }
        .c { background-image: radial-gradient(ellipse farthest-side at 20% 30%, white, black) }
        .d { background-image: radial-gradient(circle at 10px 20px 30px, white, black) }
        .e { background-image: radial-gradient(ellipse 40px 20px at 10% 10%, white, black) }
        .f { background-image: radial-gradient(white) }
        .g { background-image: radial-gradient(closest-corner, white, black) }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 7) {
        check(false, "radial gradient elements found");
        return;
    }
    using Kind = sui::ComputedStyle::BackgroundImage::Kind;
    using Radial = sui::ComputedStyle::RadialGradient;
    check(styles[0].backgroundImages[0].kind == Kind::RadialGradientImage &&
              styles[0].backgroundImages[0].radial.shape == Radial::Circle &&
              styles[0].backgroundImages[0].radial.extent == Radial::FarthestCorner,
          "the default radial is a circle reaching the farthest corner");
    // The centre defaults to the middle of the box, which has to be a percentage or it
    // lands 50px from the left edge instead of halfway across.
    check(styles[0].backgroundImages[0].radial.centreX.unit == sui::Length::Percent &&
              styles[0].backgroundImages[0].radial.centreX.value == 50.0f,
          "the default centre is 50%, not 50px");
    check(styles[1].backgroundImages[0].radial.shape == Radial::Circle &&
              styles[1].backgroundImages[0].radial.extent == Radial::ClosestSide,
          "circle and closest-side both parse");
    check(styles[2].backgroundImages[0].radial.shape == Radial::Ellipse &&
              styles[2].backgroundImages[0].radial.extent == Radial::FarthestSide &&
              styles[2].backgroundImages[0].radial.centreX.value == 20.0f,
          "ellipse, farthest-side, and a centre all parse");
    check(styles[3].backgroundImages[0].radial.hasRadii &&
              styles[3].backgroundImages[0].radial.radiusX.unit == sui::Length::Px &&
              styles[3].backgroundImages[0].radial.radiusX.value == 30.0f,
          "one radius on a circle");
    check(styles[4].backgroundImages[0].radial.hasRadii &&
              styles[4].backgroundImages[0].radial.radiusX.value == 40.0f &&
              styles[4].backgroundImages[0].radial.radiusY.value == 20.0f,
          "two radii on an ellipse");
    check(styles[5].backgroundImages[0].kind == Kind::RadialGradientImage &&
              styles[5].backgroundImages[0].radial.stops.size() == 1,
          "a one-stop radial paints as a flat fill");
    check(styles[6].backgroundImages[0].radial.extent == Radial::ClosestCorner,
          "an extent with no shape takes the default shape");
}

// The seven background longhands are parallel lists that CSS matches up by
// position, repeating whichever is shortest, so one value can serve every layer.
void testBackgroundListsAreParallel() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { background-image: url(one.png), linear-gradient(red, blue);
             background-size: 10px }
        .b { background-image: linear-gradient(red, blue);
             background-repeat: no-repeat, repeat-x }
        .c { background-image: linear-gradient(red, blue);
             background-position: right 4px bottom 8px }
        .d { background-image: linear-gradient(red, blue), url(one.png);
             background-clip: content-box }
        .e { background-image: none }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 5) {
        check(false, "background list elements found");
        return;
    }
    using Kind = sui::ComputedStyle::BackgroundImage::Kind;
    using SizeMode = sui::ComputedStyle::BackgroundSize::Mode;
    check(styles[0].backgroundImages.size() == 2 && styles[0].backgroundSizes.size() == 1,
          "one size value serves both layers");
    check(styles[0].backgroundImages[0].kind == Kind::Url &&
              styles[0].backgroundImages[1].kind == Kind::LinearGradientImage,
          "the first layer in the list is the topmost");
    check(styles[0].backgroundSizes[0].mode == SizeMode::Explicit &&
              styles[0].backgroundSizes[0].width.value == 10.0f,
          "a lone size value sets the width and leaves the height auto");
    check(styles[0].backgroundSizes[0].height.unit == sui::Length::Auto,
          "an unstated size axis stays auto");
    check(styles[1].backgroundRepeats.size() == 2 &&
              !styles[1].backgroundRepeats[0].x && !styles[1].backgroundRepeats[0].y &&
              styles[1].backgroundRepeats[1].x && !styles[1].backgroundRepeats[1].y,
          "a repeat pair splits into one entry per axis");
    check(styles[2].backgroundPositions.size() == 1 &&
              styles[2].backgroundPositions[0].horizontal ==
                  sui::ComputedStyle::BackgroundPosition::End &&
              styles[2].backgroundPositions[0].vertical ==
                  sui::ComputedStyle::BackgroundPosition::End,
          "the four-value position form names both edges");
    // A length after `right` or `bottom` measures back from the edge, so it is stored
    // negative and added to the leftover space rather than to the origin.
    checkNear(styles[2].backgroundPositions[0].offsetX.value, -4.0f, 0.01f,
              "an offset after right counts back from the edge");
    checkNear(styles[2].backgroundPositions[0].offsetY.value, -8.0f, 0.01f,
              "an offset after bottom counts back from the edge");
    check(styles[3].backgroundClips.size() == 1 &&
              styles[3].backgroundClips[0] ==
                  sui::ComputedStyle::BackgroundContentBox,
          "a lone clip value serves both layers");
    check(styles[4].backgroundImages.size() == 1 &&
              styles[4].backgroundImages[0].kind == Kind::None,
          "none is a layer of its own rather than an absent list");
}

void testBackgroundSizeAndPositionKeywords() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { background-size: cover }
        .b { background-size: contain }
        .c { background-size: auto 40px }
        .d { background-size: 30% }
        .e { background-size: 10px 20px }
        .f { background-size: 10px 20px 30px }
        .g { background-position: center }
        .h { background-position: left top }
        .i { background-position: 50% 50% }
        .j { background-position: right }
        .k { background-position: 10px }
        .l { background-position: left 5px }
        .m { background-position: centre }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div><div class=\"h\"></div><div class=\"i\"></div>"
        "<div class=\"j\"></div><div class=\"k\"></div><div class=\"l\"></div>"
        "<div class=\"m\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 13) {
        check(false, "background keyword elements found");
        return;
    }
    using SizeMode = sui::ComputedStyle::BackgroundSize::Mode;
    using Keyword = sui::ComputedStyle::BackgroundPosition::Keyword;
    check(styles[0].backgroundSizes[0].mode == SizeMode::Cover, "cover parses");
    check(styles[1].backgroundSizes[0].mode == SizeMode::Contain, "contain parses");
    check(styles[2].backgroundSizes[0].mode == SizeMode::Explicit &&
              styles[2].backgroundSizes[0].width.unit == sui::Length::Auto &&
              styles[2].backgroundSizes[0].height.value == 40.0f,
          "auto with a length keeps the two axes apart");
    check(styles[3].backgroundSizes[0].width.unit == sui::Length::Percent,
          "a percentage size stays a percentage");
    check(styles[4].backgroundSizes[0].width.value == 10.0f &&
              styles[4].backgroundSizes[0].height.value == 20.0f,
          "two lengths give both axes");
    check(styles[5].backgroundSizes[0].mode == SizeMode::AutoSize,
          "a third length is an invalid value, not a shift");
    check(styles[6].backgroundPositions[0].horizontal == Keyword::Center &&
              styles[6].backgroundPositions[0].vertical == Keyword::Center,
          "a lone centre applies to both axes");
    check(styles[7].backgroundPositions[0].horizontal == Keyword::Start &&
              styles[7].backgroundPositions[0].vertical == Keyword::Start,
          "left top is the start corner on both axes");
    check(styles[8].backgroundPositions[0].offsetX.unit == sui::Length::Percent &&
              styles[8].backgroundPositions[0].offsetY.unit == sui::Length::Percent,
          "two percentages are lengths, not keywords");
    check(styles[9].backgroundPositions[0].horizontal == Keyword::End &&
              styles[9].backgroundPositions[0].vertical == Keyword::Start,
          "a lone right names the horizontal axis only");
    check(styles[10].backgroundPositions[0].offsetX.value == 10.0f,
          "a lone length applies to the horizontal axis");
    check(styles[11].backgroundPositions[0].horizontal == Keyword::Start &&
              styles[11].backgroundPositions[0].offsetY.value == 5.0f,
          "left takes the horizontal slot and the length the vertical one");
    checkNear(styles[12].backgroundPositions[0].horizontal == Keyword::Center ? 1.0f : 0.0f,
              1.0f, 0.01f, "the British spelling of centre is accepted");
}

// A flex item's own cross size is a content-box length, so the border and padding
// have to come off the line's cross extent when it stretches to fill it, and must not
// be added a second time when the size is written back onto the item.
void testFlexCrossSizeExcludesChrome() {
    auto document = parseOrNull("<sui><body><div id=\"row\">"
                                "<div id=\"bordered\"></div><div id=\"auto\"></div>"
                                "</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 400px }"
        "#bordered { width: 100px; height: 70px; border: 1px solid #fff; padding: 6px }"
        "#auto { width: 50px }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* bordered = document->getElementById("bordered");
    Element* autoHeight = document->getElementById("auto");
    Element* row = document->getElementById("row");
    if (!bordered || !autoHeight || !row) {
        check(false, "flex cross-size elements found");
        return;
    }
    checkNear(bordered->layoutWidth(), 114.0f, 0.5f, "the main axis keeps its own chrome");
    checkNear(bordered->layoutHeight(), 84.0f, 0.5f,
              "the cross axis adds the border and padding once");
    // The auto-height item stretches to the line's cross extent, which is the tallest
    // item's outer height, so it ends up the same height as its neighbour.
    checkNear(autoHeight->layoutHeight(), 84.0f, 0.5f, "a stretched item matches the line");
    checkNear(row->layoutHeight(), 84.0f, 0.5f, "the line is as tall as its items");
}

void testFlexBoxSizingIsNotAppliedTwice() {
    auto document = parseOrNull("<sui><body><div id=\"row\">"
                                "<div id=\"a\"></div><div id=\"b\"></div></div>"
                                "<div id=\"col\"><div id=\"c\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 400px }"
        "#col { display: flex; flex-direction: column; width: 400px }"
        "#a, #b, #c { box-sizing: border-box; width: 100px; height: 70px;"
        "             border: 1px solid #fff; padding: 6px }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* c = document->getElementById("c");
    if (!a || !b || !c) {
        check(false, "flex border-box elements found");
        return;
    }
    checkNear(a->layoutWidth(), 100.0f, 0.5f, "a border-box row item keeps its width");
    checkNear(a->layoutHeight(), 70.0f, 0.5f, "a border-box row item keeps its height");
    checkNear(b->layoutWidth(), 100.0f, 0.5f, "the cross axis is not reduced twice");
    checkNear(b->layoutHeight(), 70.0f, 0.5f, "the cross axis is not reduced twice");
    checkNear(c->layoutWidth(), 100.0f, 0.5f, "a border-box column item keeps its width");
    checkNear(c->layoutHeight(), 70.0f, 0.5f, "a border-box column item keeps its height");
}

// `@media` was dropped whole, so a stylesheet written for a browser could not
// adapt to the window: every breakpoint in it silently never applied.
void testMediaQueries() {
    const auto rules = sui::CSSParser::parse(
        ".a { color: #fff }"
        "@media screen { .b { color: #fff } }"
        "@media (max-width: 768px) { .c { color: #fff } }"
        "@media (min-width: 600px) and (max-width: 900px) { .d { color: #fff } }"
        "@media (min-width: 600px), (min-height: 600px) { .e { color: #fff } }"
        "@media not print { .f { color: #fff } }"
        "@media (orientation: landscape) { .g { color: #fff } }"
        "@media (hover: hover) { .h { color: #fff } }"
        "@media print { .i { color: #fff } }"
        "@media (max-height: 50vw) { .j { color: #fff } }");
    check(rules.size() == 10, "an @media block contributes its rules instead of being skipped");
    if (rules.size() < 10) {
        return;
    }

    check(rules[0].media.empty(), "a rule outside any @media is unconditional");
    check(rules[1].media.size() == 1 && rules[1].media[0].type == "screen",
          "@media screen records the media type");

    const auto& upper = rules[2].media[0];
    check(upper.features.size() == 1, "(max-width: 768px) is one feature");
    check(upper.features[0].kind == sui::MediaFeature::Kind::Width &&
              upper.features[0].op == sui::MediaFeature::Op::Le &&
              upper.features[0].number == 768.0f,
          "max-width is read as a width upper bound");
    check(rules[3].media[0].features.size() == 2, "and joins two features into one query");
    check(rules[4].media.size() == 2, "a comma joins two alternatives");
    check(rules[5].media[0].negate, "not is kept so it can negate the whole query");
    check(rules[6].media[0].features.size() == 1 &&
              rules[6].media[0].features[0].kind == sui::MediaFeature::Kind::Orientation,
          "orientation is recognised");
    check(rules[7].media[0].features.size() == 1 &&
              rules[7].media[0].features[0].kind == sui::MediaFeature::Kind::Unknown,
          "a feature the engine has no value for is kept as unknown rather than dropped");
    check(rules[8].media[0].type == "print", "@media print records its type");
    check(rules[9].media[0].features[0].unit == sui::MediaFeature::Unit::Vw &&
              rules[9].media[0].features[0].kind == sui::MediaFeature::Kind::Height,
          "a viewport unit is kept for evaluation against the viewport");

    const auto matches = [](const sui::Rule& rule, float w, float h) {
        return sui::mediaMatches(rule.media, w, h);
    };
    check(matches(rules[1], 700, 500), "the screen type matches what the engine draws to");
    check(!matches(rules[8], 700, 500), "the print type does not match a screen engine");
    check(matches(rules[2], 700, 500), "max-width matches below the breakpoint");
    check(!matches(rules[2], 800, 500), "max-width misses above the breakpoint");
    check(matches(rules[2], 768, 500), "max-width is inclusive at the breakpoint");
    check(matches(rules[3], 700, 500), "a range matches inside itself");
    check(!matches(rules[3], 500, 500), "a range misses below itself");
    check(!matches(rules[3], 950, 500), "a range misses above itself");
    check(matches(rules[4], 700, 500), "the first alternative matches on its own");
    check(matches(rules[4], 500, 700), "the second alternative matches on its own");
    check(!matches(rules[4], 500, 500), "neither alternative matching means the query does not");
    check(matches(rules[5], 700, 500), "not print matches a screen");
    check(matches(rules[6], 1000, 500), "orientation: landscape matches a wide viewport");
    check(!matches(rules[6], 500, 1000), "orientation: landscape misses a tall one");
    check(!matches(rules[7], 700, 500), "an unsupported feature makes its query match nothing");
    check(!matches(rules[9], 900, 500),
          "vw resolves against the viewport width even in a height feature");
    check(matches(rules[9], 900, 400),
          "the same query matches once the height falls under that width-derived bound");

    // Nesting has to hold at both levels.
    const auto nested = sui::CSSParser::parse(
        "@media (min-width: 600px) { @media (orientation: landscape) { .n { color: #fff } } }");
    check(nested.size() == 1, "a nested @media still produces its rule");
    if (nested.size() == 1) {
        check(nested[0].media.size() == 1 && nested[0].media[0].features.size() == 2,
              "a nested @media is merged with its parent rather than replacing it");
        check(sui::mediaMatches(nested[0].media, 700, 500), "a merged query needs both levels");
        check(!sui::mediaMatches(nested[0].media, 700, 900), "a merged query fails on the inner level");
        check(!sui::mediaMatches(nested[0].media, 500, 500), "a merged query fails on the outer level");
    }

    // End to end: which rule wins depends on the viewport, and changing it makes
    // the next resolve see the other breakpoint.
    auto document = parseOrNull("<sui><body><div id=\"t\">x</div></body></sui>");
    Element* t = document ? document->getElementById("t") : nullptr;
    if (!t) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#t { width: 10px }"
        "@media (min-width: 600px) { #t { width: 200px } }"));
    resolver.setViewportSize(500, 400);
    checkNear(resolver.resolve(*t).width.resolve(0.0f, 16.0f, 16.0f), 10.0f, 0.01f,
              "below the breakpoint the plain rule wins");
    const std::uint64_t before = resolver.generation();
    check(resolver.setViewportSize(900, 400), "a viewport that actually changes is reported");
    check(!resolver.setViewportSize(900, 400), "repeating the same size changes nothing");
    check(resolver.generation() > before, "changing the viewport bumps the generation");
    checkNear(resolver.resolve(*t).width.resolve(0.0f, 16.0f, 16.0f), 200.0f, 0.01f,
              "above the breakpoint the @media rule wins");
    check(!resolver.setViewportSize(0, 400), "a zero dimension is not accepted as a viewport");
}

// Custom properties (`--name`) and `var()`: values are raw tokens, the cascade
// decides them before anything is substituted, and an unresolved `var()` drops
// the whole declaration at computed-value time.
void testCustomProperties() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"t1\">a</div><div id=\"t2\">b</div><div id=\"t3\">c</div>"
        "<div id=\"t4\">d</div><div id=\"t5\">e</div><div id=\"t6\">f</div>"
        "<div id=\"t7\">g</div><div id=\"t8\" style=\"--k: 3px; width: var(--k)\">h</div>"
        "<div id=\"t9\">i</div><div id=\"t10\">j</div><div id=\"t11\">k</div>"
        "<div id=\"t12\">l</div><div id=\"t13\">m</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { --accent: #00ff00; color: var(--accent) }"
        "#t1 { --w: 40px; width: var(--w) }"
        "#t2 { --x: 4px; width: var(--x) }"
        "#t2 { --x: 8px }"
        "#t3 { width: var(--y) }"
        "#t3 { --y: 40px }"
        "#t4 { width: var(--never, 30px) }"
        "#t5 { --a: 5px; width: var(--never2, var(--a)) }"
        "#t6 { width: var(--nope); height: 9px; color: #ff0000 }"
        "#t7 { --x: 40px; width: var(--x) }"
        "#t7 { width: 12px }"
        "#t9 { --a: var(--b); --b: var(--a); width: var(--a); height: 5px }"
        "#t11 { --x: 1px; width: var(--x) }"
        "#t11 { --x: 9px !important }"
        "#t13 { color: var(--accent); height: 7px }"
        "#t10 { width: var(--x) }"
        "@media (min-width: 600px) { #t10 { --x: 50px } }"));

    auto width = [&](const char* id) {
        Element* el = document->getElementById(id);
        if (!el) {
            return -1.0f;
        }
        return resolver.resolve(*el).width.resolve(0.0f, 16.0f, 16.0f);
    };
    auto throughId = [&](const char* id) {
        Element* el = document->getElementById(id);
        return el ? resolver.resolve(*el) : sui::ComputedStyle{};
    };

    Element* t1 = document->getElementById("t1");
    Element* t2 = document->getElementById("t2");
    Element* t4 = document->getElementById("t4");
    Element* t5 = document->getElementById("t5");
    Element* t6 = document->getElementById("t6");
    Element* t10 = document->getElementById("t10");
    Element* t11 = document->getElementById("t11");
    check(t1 && t2 && t4 && t5 && t6 && t10 && t11, "elements found");
    if (!t1 || !t2 || !t4 || !t5 || !t6 || !t10 || !t11) {
        return;
    }

    checkNear(width("t1"), 40.0f, 0.01f, "var(--w) resolves to the custom property value");
    check(resolver.propertyValue(resolver.resolve(*t1), "--w") == "40px",
          "a custom property reads back from the computed style");
    check(resolver.propertyValue(resolver.resolve(*t2), "--x") == "8px",
          "the later --x wins the custom-property cascade");
    checkNear(width("t2"), 8.0f, 0.01f,
              "width: var(--x) sees the --x that was declared after it");
    checkNear(width("t3"), 40.0f, 0.01f,
              "a consuming rule that precedes the definition still gets the value");
    checkNear(width("t4"), 30.0f, 0.01f, "an undefined var falls back to its fallback");
    checkNear(width("t5"), 5.0f, 0.01f, "a fallback may itself be a var()");
    checkNear(width("t6"), 0.0f, 0.01f,
              "an undefined var with no fallback drops the declaration");
    const sui::ComputedStyle s6 = throughId("t6");
    checkNear(s6.height.resolve(0.0f, 16.0f, 16.0f), 9.0f, 0.01f,
              "sibling declarations in the same rule still apply");
    check(s6.color.r == 0xff, "a colour after the dropped declaration still applies");

    const sui::ComputedStyle body = [&]() {
        Element* b = document->querySelector("body");
        return b ? resolver.resolve(*b) : sui::ComputedStyle{};
    }();
    check(body.color.g == 0xff, "body consumes its own inherited --accent");
    const sui::ComputedStyle s13 = throughId("t13");
    check(s13.color.g == 0xff && s13.color.a == 0xff,
          "a child consumes a custom property inherited from its ancestor");

    checkNear(width("t7"), 12.0f, 0.01f,
              "a later literal beats an earlier var() at the same specificity");
    checkNear(width("t8"), 3.0f, 0.01f,
              "a custom property from an inline style resolves too");
    const sui::ComputedStyle s9 = throughId("t9");
    checkNear(s9.width.resolve(0.0f, 16.0f, 16.0f), 0.0f, 0.01f,
              "a --a: var(--b); --b: var(--a) cycle drops the consuming declaration");
    checkNear(s9.height.resolve(0.0f, 16.0f, 16.0f), 5.0f, 0.01f,
              "a declaration next to a cycled one still applies");
    checkNear(width("t11"), 9.0f, 0.01f, "an important custom property beats a normal one");

    resolver.setViewportSize(500, 400);
    const float narrow = resolver.resolve(*t10).width.resolve(0.0f, 16.0f, 16.0f);
    resolver.setViewportSize(700, 400);
    const float wide = resolver.resolve(*t10).width.resolve(0.0f, 16.0f, 16.0f);
    checkNear(narrow, 0.0f, 0.01f,
              "below the breakpoint --x never exists, so the var() drops");
    checkNear(wide, 50.0f, 0.01f,
              "above the breakpoint --x is defined and the var() resolves");

    // The none case: a warning exists but the stylesheet still resolves. A custom
    // property must not trip the unknown-property report.
    auto clean = sui::CSSParser::parse("#a { --pool: 4px }");
    sui::StyleResolver probe;
    probe.setRules(clean);
    check(!probe.rules().empty() && probe.rules()[0].usesCustom,
          "a custom property marks its rule for the two-pass path");
    auto plain = sui::CSSParser::parse("#a { width: 4px }");
    probe.setRules(plain);
    check(!probe.rules()[0].usesCustom, "a rule without one does not pay for the path");
}

// --- Transitions and animations (M5) ---

// Animation time comes from the resolver's clock source, so the tests step a
// frame by moving a fake clock instead of sleeping on the real one.
double fakeNowMs = 0.0;
double fakeClock() {
    return fakeNowMs;
}

void testEasingAndTimingFunctions() {
    using sui::Easing;
    check(Easing::parse("linear").type == Easing::Type::Linear,
          "linear parses as a linear ramp");
    checkNear(Easing::parse("linear").apply(0.25f), 0.25f, 0.001f, "linear is identity");
    check(Easing::parse("ease").type == Easing::Type::CubicBezier,
          "ease folds onto a cubic-bezier");
    checkNear(Easing::parse("ease-in-out").apply(0.5f), 0.5f, 0.001f,
              "a symmetric curve maps 0.5 to 0.5");
    checkNear(Easing::parse("steps(4)").apply(0.3f), 0.25f, 0.001f,
              "jump-end steps floor to the previous step");
    checkNear(Easing::parse("steps(4, start)").apply(0.3f), 0.5f, 0.001f,
              "jump-start steps jump to the next step");
    check(Easing::parse("step-start").stepStart && Easing::parse("step-end").steps == 1,
          "step-start/step-end are single steps");
    const Easing bezier = Easing::parse("cubic-bezier(0.25, 0.1, 0.25, 1)");
    check(bezier.type == Easing::Type::CubicBezier && bezier.y1 == 0.1f && bezier.x2 == 0.25f,
          "cubic-bezier() numbers are read");
    checkNear(Easing::parse("garbage").apply(0.5f), 0.5f, 0.001f,
              "an unparsable timing function fails to a linear ramp");
    checkNear(Easing::parse("cubic-bezier(1, 2, 5, 0)").apply(0.5f), 0.5f, 0.001f,
              "an out-of-range x axis is rejected as linear");

    float ms = -1.0f;
    check(sui::parseTimeMs("0.5s", ms) && std::fabs(ms - 500.0f) < 0.001f,
          "`0.5s` is 500 milliseconds");
    check(sui::parseTimeMs("250ms", ms) && std::fabs(ms - 250.0f) < 0.001f,
          "`250ms` is 250 milliseconds");
    check(sui::parseTimeMs("0", ms) && ms == 0.0f, "a bare zero is a valid time");
    check(!sui::parseTimeMs("5", ms), "a bare non-zero number is not a time");
    const auto parts = sui::splitCommaList("width 0.3s, color 1s, cubic-bezier(0, 1, 1, 0)");
    check(parts.size() == 3 && parts[0] == "width 0.3s" && parts[1] == "color 1s",
          "top-level comma split leaves function arguments intact");
    const auto all = sui::expandTransitionProperty("all");
    check(all.size() > 20, "`all` expands to the whole animatable table");
    const auto margins = sui::expandTransitionProperty("margin");
    check(margins.size() == 4 && margins[2] == "margin-bottom", "`margin` expands to its edges");
}

void testTransitionDeclarations() {
    auto document = parseOrNull("<sui><body><div id=\"t\">x</div></body></sui>");
    Element* t = document ? document->getElementById("t") : nullptr;
    if (!t) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#t { transition: width 0.3s ease 0.2s, color 1s; }"));
    const ComputedStyle s = resolver.resolve(*t);
    check(s.transitionProperties.size() == 2 && s.transitionProperties[0] == "width" &&
              s.transitionProperties[1] == "color",
          "a shorthand list pairs one entry per property");
    checkNear(s.transitionDurationsMs[0], 300.0f, 0.01f, "the first duration is kept");
    checkNear(s.transitionDurationsMs[1], 1000.0f, 0.01f, "the second duration is kept");
    checkNear(s.transitionDelaysMs[0], 200.0f, 0.01f, "a second time is the delay");
    check(s.transitionEasings[0].type == sui::Easing::Type::CubicBezier,
          "an easing word parses per part");
    check(s.transitionEasings[1].type == sui::Easing::Type::Linear,
          "a missing easing defaults to linear");

    sui::StyleResolver r2;
    r2.setRules(sui::CSSParser::parse(
        "#t { transition: width 0.3s; transition-property: color, opacity }"));
    const ComputedStyle s2 = r2.resolve(*t);
    check(s2.transitionProperties.size() == 2 && s2.transitionProperties[1] == "opacity",
          "transition-property replaces only the property list");
    checkNear(sui::listAt(s2.transitionDurationsMs, 1), 300.0f, 0.01f,
              "a shorter list repeats its last entry");
    checkNear(sui::listAt(s2.transitionDelaysMs, 1), 0.0f, 0.01f, "the repeated entry keeps its delay");

    sui::StyleResolver r3;
    r3.setRules(sui::CSSParser::parse(
        "body { transition: width 0.7s } #t { transition: inherit }"));
    const ComputedStyle s3 = r3.resolve(*t);
    check(s3.transitionProperties.size() == 1 && s3.transitionProperties[0] == "width",
          "transition: inherit copies the parent's list");
    checkNear(s3.transitionDurationsMs[0], 700.0f, 0.01f,
              "inherit carries the duration through the round trip");

    sui::StyleResolver r4;
    r4.setRules(sui::CSSParser::parse(
        "#t { transition: width 0.3s; transition-property: none }"));
    check(r4.resolve(*t).transitionProperties.empty(),
          "transition-property: none disables transitions");

    sui::StyleResolver r5;
    r5.setRules(sui::CSSParser::parse("#t { animation: pulse 2s ease 0.5s infinite alternate both }"));
    const ComputedStyle s5 = r5.resolve(*t);
    check(s5.animationNames.size() == 1 && s5.animationNames[0] == "pulse",
          "the animation name is read first");
    checkNear(s5.animationDurationsMs[0], 2000.0f, 0.01f, "the animation duration is read");
    checkNear(s5.animationDelaysMs[0], 500.0f, 0.01f, "the animation delay is read");
    check(s5.animationIterations[0] < 0.0f, "infinite folds onto a negative iteration count");
    check(s5.animationDirections[0] == "alternate" && s5.animationFills[0] == "both",
          "direction and fill keywords land in their lists");
}

void testTransitions() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"b\" style=\"width:100px;height:32px;background-color:#ffff00\">x</div>"
        "<div id=\"r\" style=\"width:100px\">x</div>"
        "<div id=\"o\" style=\"opacity:1\">x</div>"
        "</body></sui>");
    Element* b = document ? document->getElementById("b") : nullptr;
    Element* r = document ? document->getElementById("r") : nullptr;
    Element* o = document ? document->getElementById("o") : nullptr;
    if (!b || !r || !o) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setTimeSource(fakeClock);
    resolver.setRules(sui::CSSParser::parse(
        "#b { transition: width 1s linear, background-color 1s linear; }"
        "#r { transition: width 1s linear; }"
        "#o { transition: opacity 1s linear 0.5s; }"));
    const auto width = [](const ComputedStyle& s) {
        return s.width.resolve(0.0f, 16.0f, 16.0f);
    };

    fakeNowMs = 1000.0;
    const ComputedStyle base = resolver.resolve(*b);
    checkNear(width(base), 100.0f, 0.01f, "the baseline resolve keeps the author width");

    // Change the targets; the change is detected on the next resolve, whose
    // frame still shows the old value (progress is 0 at the change instant).
    b->style().setProperty("width", "200px");
    b->style().setProperty("background-color", "#0000ff");
    fakeNowMs = 1500.0;
    const ComputedStyle atChange = resolver.resolve(*b);
    checkNear(width(atChange), 100.0f, 0.01f, "the change frame holds the from-value");
    fakeNowMs = 2000.0;
    const ComputedStyle mid = resolver.resolve(*b);
    checkNear(width(mid), 150.0f, 0.5f, "halfway through a 1s linear look");
    check(mid.backgroundColor.a == 255 && mid.backgroundColor.r == 128 &&
              mid.backgroundColor.b == 128,
          "the background colour blends at the midpoint");
    fakeNowMs = 3000.0;
    const ComputedStyle done = resolver.resolve(*b);
    checkNear(width(done), 200.0f, 0.01f, "the run lands exactly on the target");
    check(done.backgroundColor.b == 255 && done.backgroundColor.r == 0,
          "the colour reaches the target");

    // A resting page: nothing moves, so nothing keeps the loop awake.
    fakeNowMs = 4000.0;
    const ComputedStyle rested = resolver.resolve(*b);
    checkNear(width(rested), 200.0f, 0.01f, "an idle element is unchanged");
    check(!resolver.hasRunningAnimations(), "no runs are left after a finished transition");

    // Retargeting mid-flight restarts from where the eye is, not from the base.
    // `r` runs its own timeline here, so its later frames cannot disturb the
    // idle checks above, which must see only `b`'s finished run.
    fakeNowMs = 4000.0;
    resolver.resolve(*r);  // anchor: no change yet, holds the author width
    r->style().setProperty("width", "200px");
    fakeNowMs = 4500.0;
    resolver.resolve(*r);  // the change frame holds the from-value
    fakeNowMs = 5000.0;
    checkNear(width(resolver.resolve(*r)), 150.0f, 0.5f, "the original run is mid-flight");
    r->style().setProperty("width", "300px");
    const ComputedStyle restarted = resolver.resolve(*r);  // same instant
    checkNear(width(restarted), 150.0f, 0.5f, "restart shows the displayed value");
    fakeNowMs = 5250.0;
    const ComputedStyle restartedMid = resolver.resolve(*r);
    checkNear(width(restartedMid), 187.5f, 0.5f, "restart eases from the displayed value");
    fakeNowMs = 6250.0;
    const ComputedStyle secondDone = resolver.resolve(*r);
    checkNear(width(secondDone), 300.0f, 0.01f, "the restarted run finishes on target");

    // Delay: the run holds its from-value through the delay, then moves.
    fakeNowMs = 1000.0;
    resolver.resolve(*o);  // baseline
    o->style().setProperty("opacity", "0.5");
    fakeNowMs = 1500.0;
    checkNear(resolver.resolve(*o).opacity, 1.0f, 0.01f, "the delay holds the from-value");
    fakeNowMs = 2250.0;
    checkNear(resolver.resolve(*o).opacity, 0.875f, 0.01f,
              "a quarter of the run has elapsed after the delay");
    fakeNowMs = 3200.0;
    checkNear(resolver.resolve(*o).opacity, 0.5f, 0.01f, "the delayed run finishes on target");
}

void testKeyframeAnimations() {
    const auto width = [](const ComputedStyle& s) {
        return s.width.resolve(0.0f, 16.0f, 16.0f);
    };
    const std::string pulse =
        "@keyframes pulse {"
        "  from { width: 100px; opacity: 0 }"
        "  50% { width: 400px; opacity: 0.5 }"
        "  to { width: 100px; opacity: 1 }"
        "} ";

    // Fill, finish, and restart stay on the only element this resolver animates,
    // so `hasRunningAnimations` observes exactly that element's timeline.
    auto kdoc = parseOrNull("<sui><body><div id=\"k\" style=\"width:100px\">x</div></body></sui>");
    Element* k = kdoc ? kdoc->getElementById("k") : nullptr;
    if (!k) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setTimeSource(fakeClock);
    std::vector<sui::Rule> rules;
    std::vector<sui::KeyframeBlock> keyframes;
    sui::CSSParser::parseWithKeyframes(pulse + "#k { animation: pulse 2s linear both }",
                                       rules, keyframes);
    check(keyframes.size() == 1 && keyframes[0].name == "pulse",
          "@keyframes hands its block to the resolver");
    check(keyframes[0].frames.size() == 3 && keyframes[0].frames[1].offset == 0.5f &&
              keyframes[0].frames[2].offset == 1.0f,
          "frames are sorted by offset with from=0 and to=1");
    resolver.setRules(std::move(rules));
    resolver.setKeyframes(std::move(keyframes));

    fakeNowMs = 0.0;
    resolver.resolve(*k);
    check(resolver.hasRunningAnimations(), "a keyframe animation keeps the loop awake");

    fakeNowMs = 500.0;
    const ComputedStyle s1 = resolver.resolve(*k);
    checkNear(width(s1), 250.0f, 0.5f, "the 0%-50% segment is half-blended at 0.25");
    checkNear(s1.opacity, 0.25f, 0.01f, "opacity blends across the same segment");
    fakeNowMs = 1000.0;
    checkNear(width(resolver.resolve(*k)), 400.0f, 0.5f, "the 50% keyframe is exact");
    fakeNowMs = 1500.0;
    const ComputedStyle s3 = resolver.resolve(*k);
    checkNear(width(s3), 250.0f, 0.5f, "the 50%-100% segment returns downward");
    checkNear(s3.opacity, 0.75f, 0.01f, "opacity keeps rising while width returns");
    fakeNowMs = 2000.0;
    const ComputedStyle ended = resolver.resolve(*k);
    checkNear(width(ended), 100.0f, 0.01f, "fill forwards holds the last frame");
    checkNear(ended.opacity, 1.0f, 0.01f, "the after-end fill holds opacity");
    check(!resolver.hasRunningAnimations(), "a finished animation goes idle");

    // Changing the animation configuration restarts the timeline from zero, so
    // the change frame still holds the from-value; the new duration shows up on
    // the frames that follow, and the run goes idle when the new span elapses.
    k->style().setProperty("animation", "pulse 1s linear both");
    fakeNowMs = 2500.0;
    const ComputedStyle changed = resolver.resolve(*k);
    checkNear(width(changed), 100.0f, 0.01f, "a config change restarts the timeline");
    check(resolver.hasRunningAnimations(), "the restarted run is running again");
    fakeNowMs = 2750.0;
    const ComputedStyle shortened = resolver.resolve(*k);
    checkNear(width(shortened), 250.0f, 0.5f, "the shortened run eases at the new duration");
    fakeNowMs = 3500.0;
    resolver.resolve(*k);
    check(!resolver.hasRunningAnimations(), "the shortened run also finishes idle");

    // Infinite alternate lives in its own resolver, so its never-ending loop
    // cannot keep the idle checks above awake.
    auto adoc = parseOrNull("<sui><body><div id=\"a\" style=\"width:100px\">x</div></body></sui>");
    Element* a = adoc ? adoc->getElementById("a") : nullptr;
    if (!a) {
        return;
    }
    sui::StyleResolver ra;
    ra.setTimeSource(fakeClock);
    ra.setRules(sui::CSSParser::parse(pulse + "#a { animation: pulse 2s linear infinite alternate }"));
    ra.setKeyframes([&] {
        std::vector<sui::Rule> ignored;
        std::vector<sui::KeyframeBlock> akeysInner;
        sui::CSSParser::parseWithKeyframes(
            pulse + "#a { animation: pulse 2s linear infinite alternate }", ignored, akeysInner);
        return akeysInner;
    }());
    fakeNowMs = 0.0;
    ra.resolve(*a);  // anchor the timeline
    fakeNowMs = 2500.0;
    const ComputedStyle oddReverse = ra.resolve(*a);
    checkNear(oddReverse.opacity, 0.75f, 0.01f, "the first odd iteration runs reversed");
    fakeNowMs = 4500.0;
    const ComputedStyle evenNormal = ra.resolve(*a);
    checkNear(evenNormal.opacity, 0.25f, 0.01f, "the next even iteration runs forward");

    // A delay with backwards fill holds the first frame, then plays; its own
    // resolver keeps the short timeline out of the others.
    auto ddoc = parseOrNull("<sui><body><div id=\"d\" style=\"width:100px\">x</div></body></sui>");
    Element* d = ddoc ? ddoc->getElementById("d") : nullptr;
    if (!d) {
        return;
    }
    sui::StyleResolver rd;
    rd.setTimeSource(fakeClock);
    rd.setRules(sui::CSSParser::parse(pulse + "#d { animation: pulse 2s linear 1s backwards }"));
    rd.setKeyframes([&] {
        std::vector<sui::Rule> ignored;
        std::vector<sui::KeyframeBlock> dkeysInner;
        sui::CSSParser::parseWithKeyframes(
            pulse + "#d { animation: pulse 2s linear 1s backwards }", ignored, dkeysInner);
        return dkeysInner;
    }());
    fakeNowMs = 0.0;
    rd.resolve(*d);  // anchor the timeline
    fakeNowMs = 500.0;
    const ComputedStyle beforeStart = rd.resolve(*d);
    checkNear(width(beforeStart), 100.0f, 0.01f, "backwards fill applies the first frame");
    checkNear(beforeStart.opacity, 0.0f, 0.01f, "backwards fill applies the first values");
    fakeNowMs = 1500.0;
    const ComputedStyle afterStart = rd.resolve(*d);
    checkNear(width(afterStart), 250.0f, 0.5f, "the run begins where the timeline says");
    checkNear(afterStart.opacity, 0.25f, 0.01f, "progress restarts after the delay");
    fakeNowMs = 4000.0;
    const ComputedStyle reverted = rd.resolve(*d);
    checkNear(width(reverted), 100.0f, 0.01f,
              "backwards fill does not hold past the animation's end");
    checkNear(reverted.opacity, 1.0f, 0.01f, "the element returns to its own style");
}

void testCalc() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"a\" style=\"width:calc(100% - 40px)\"></div>"
        "<div id=\"b\" style=\"width:calc(2em + 1rem)\"></div>"
        "<div id=\"c\" style=\"width:min(100px, 50%)\"></div>"
        "<div id=\"d\" style=\"width:max(100px, 50%)\"></div>"
        "<div id=\"e\" style=\"width:clamp(80px, 50%, 300px)\"></div>"
        "<div id=\"f\" style=\"width:calc((100% - 2rem) / 2)\"></div>"
        "<div id=\"g\" style=\"width:calc(100% - -10px)\"></div>"
        "<div id=\"h\" style=\"width:calc(50% / 2)\"></div>"
        "<div id=\"i\" style=\"width:calc(100px * 100px)\"></div>"
        "<div id=\"k\" style=\"width:calc(50%)\"></div>"
        "<div id=\"m\" style=\"margin: calc(50% - 10px) 4px\"></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRootFontSize(16.0f);
    resolver.setRules(sui::CSSParser::parse("#a,#b,#c,#d,#e,#f,#g,#h,#i,#j,#k,#m {}"));
    const auto find = [&](const char* id) { return document->getElementById(id); };
    const sui::ComputedStyle a = resolver.resolve(*find("a"));
    checkNear(a.width.resolve(400.0f, 0.0f, 0.0f), 360.0f, 0.01f,
              "calc % and px fold against the containing block");
    const sui::ComputedStyle b = resolver.resolve(*find("b"));
    checkNear(b.width.resolve(0.0f, 10.0f, 20.0f), 40.0f, 0.01f,
              "calc mixes em and rem against their font bases");
    const sui::ComputedStyle c = resolver.resolve(*find("c"));
    checkNear(c.width.resolve(100.0f, 0.0f, 0.0f), 50.0f, 0.01f,
              "min() picks the smaller resolved operand");
    checkNear(c.width.resolve(400.0f, 0.0f, 0.0f), 100.0f, 0.01f,
              "min() lets the percentage win when it is the smaller");
    const sui::ComputedStyle d = resolver.resolve(*find("d"));
    checkNear(d.width.resolve(400.0f, 0.0f, 0.0f), 200.0f, 0.01f,
              "max() picks the larger resolved operand");
    const sui::ComputedStyle e = resolver.resolve(*find("e"));
    checkNear(e.width.resolve(100.0f, 0.0f, 0.0f), 80.0f, 0.01f,
              "clamp() floors at its minimum");
    checkNear(e.width.resolve(400.0f, 0.0f, 0.0f), 200.0f, 0.01f,
              "clamp() passes values between its bounds");
    const sui::ComputedStyle f = resolver.resolve(*find("f"));
    checkNear(f.width.resolve(400.0f, 16.0f, 16.0f), 184.0f, 0.01f,
              "a parenthesised calc divides after fixing the containing block");
    const sui::ComputedStyle g = resolver.resolve(*find("g"));
    checkNear(g.width.resolve(400.0f, 0.0f, 0.0f), 410.0f, 0.01f,
              "a minus right after the operator subtracts a negative");
    const sui::ComputedStyle h = resolver.resolve(*find("h"));
    checkNear(h.width.resolve(400.0f, 0.0f, 0.0f), 100.0f, 0.01f,
              "a percentage divides by a unitless number");
    const sui::ComputedStyle i = resolver.resolve(*find("i"));
    check(i.width.isAuto(), "calc px times px is invalid and drops to auto");
    const sui::ComputedStyle k = resolver.resolve(*find("k"));
    checkNear(k.width.resolve(400.0f, 0.0f, 0.0f), 200.0f, 0.01f,
              "calc(50%) collapses to the plain percentage");
    const sui::ComputedStyle m = resolver.resolve(*find("m"));
    checkNear(m.margin.top.resolve(400.0f, 0.0f, 0.0f), 190.0f, 0.01f,
              "a calc survives a top-level space-split margin list");
    checkNear(m.margin.left.resolve(400.0f, 0.0f, 0.0f), 4.0f, 0.01f,
              "the plain part of the list keeps its own unit");
    check(resolver.propertyValue(a, "width") == "calc(100% - 40px)",
          "a calc width serialises back to css");
    check(resolver.propertyValue(c, "width") == "min(100px, 50%)",
          "min() serialises with its arguments");
    check(resolver.propertyValue(k, "width") == "50%",
          "a collapsed calc serialises as the plain length");

    // Layout: the child resolves this calc against its containing block.
    auto layoutDoc = parseOrNull(
        "<sui><body><div id=\"parent\" style=\"width:400px\">"
        "<div id=\"child\" style=\"width:calc(100% - 20px)\"></div>"
        "</div></body></sui>");
    if (!layoutDoc) {
        return;
    }
    sui::StyleResolver layoutResolver;
    layoutResolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    layoutRoot(*layoutDoc, layoutResolver, 800.0f);
    Element* child = layoutDoc->getElementById("child");
    if (!child) {
        check(false, "calc layout child found");
        return;
    }
    checkNear(child->layoutWidth(), 380.0f, 0.01f,
              "layout resolves a calc width against the containing block");

    // A custom property feeds an operand the same way its literal text would.
    auto varDoc = parseOrNull(
        "<sui><body><div id=\"v\"></div></body></sui>");
    if (!varDoc) {
        return;
    }
    sui::StyleResolver varResolver;
    varResolver.setRules(sui::CSSParser::parse(
        "#v { --n: 2; width: calc(100% / var(--n)); --side: 4px; "
        "margin: calc(50% - var(--side)); }"));
    Element* v = varDoc->getElementById("v");
    if (!v) {
        check(false, "var-fed calc element found");
        return;
    }
    const sui::ComputedStyle vs = varResolver.resolve(*v);
    checkNear(vs.width.resolve(400.0f, 0.0f, 0.0f), 200.0f, 0.01f,
              "var() supplies the unitless divisor of a calc");
    checkNear(vs.margin.top.resolve(400.0f, 0.0f, 0.0f), 196.0f, 0.01f,
              "var() inside calc substitutes before the expression is parsed");
}

void testScroll() {
    // A clipping box reports its padding box as `client`, the furthest content
    // edge as `scroll`, and clamps its offsets into the gap between them.
    auto document = parseOrNull(
        "<sui><body><div id=\"s\" style=\"width:200px; height:100px; overflow-y:auto\">"
        "<div id=\"tall\" style=\"height:300px\"></div>"
        "</div><div id=\"plain\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    layoutRoot(*document, resolver, 400.0f);
    Element* s = document->getElementById("s");
    if (!s) {
        check(false, "scroll fixture element found");
        return;
    }
    checkNear(s->clientHeight(), 100.0f, 0.01f, "clientHeight is the padding box");
    checkNear(s->scrollHeight(), 300.0f, 0.01f, "scrollHeight reaches the overflowing child");
    checkNear(s->scrollWidth(), 200.0f, 0.01f, "scrollWidth floors at the client width");
    checkNear(s->scrollTop(), 0.0f, 0.01f, "scrollTop starts at zero");
    checkNear(s->scrollLeft(), 0.0f, 0.01f, "scrollLeft starts at zero");
    check(s->scrollHeight() >= s->clientHeight(), "scrollHeight never shrinks below clientHeight");

    s->setScrollTop(999.0f);
    checkNear(s->scrollTop(), 200.0f, 0.01f, "setScrollTop clamps at the end of the range");
    s->setScrollTop(-5.0f);
    checkNear(s->scrollTop(), 0.0f, 0.01f, "setScrollTop clamps below zero");
    s->setScrollTop(60.0f);
    checkNear(s->scrollTop(), 60.0f, 0.01f, "an in-range scrollTop sticks");
    // The horizontal axis is `visible`, so it cannot hold an offset at all.
    s->setScrollLeft(50.0f);
    checkNear(s->scrollLeft(), 0.0f, 0.01f, "a visible axis ignores a scroll write");

    // The computed overflow keeps `auto` distinct from `scroll`.
    const sui::ComputedStyle sv = resolver.resolve(*s);
    check(sv.overflowY == sui::ComputedStyle::Auto, "overflow: auto is its own computed value");
    check(sv.overflowX == sui::ComputedStyle::Visible, "the other axis stays visible");

    // A box that never clips reports honest extents but is not scrollable.
    Element* plain = document->getElementById("plain");
    if (!plain) {
        check(false, "plain box found");
        return;
    }
    plain->setScrollTop(30.0f);
    checkNear(plain->scrollTop(), 0.0f, 0.01f, "an overflow-visible box cannot scroll");

    // Horizontal overflow lets scrollLeft move; a positioned child escapes the
    // content box without flexbox shrinking it to fit.
    auto wideDoc = parseOrNull(
        "<sui><body><div id=\"h\" style=\"position:relative; width:100px; height:50px; "
        "overflow-x:auto\">"
        "<div id=\"wide\" style=\"position:absolute; left:300px; width:120px\"></div>"
        "</div></body></sui>");
    if (!wideDoc) {
        return;
    }
    sui::StyleResolver wideResolver;
    layoutRoot(*wideDoc, wideResolver, 400.0f);
    Element* h = wideDoc->getElementById("h");
    if (!h) {
        check(false, "wide scroll fixture element found");
        return;
    }
    checkNear(h->scrollWidth(), 420.0f, 0.01f, "scrollWidth reaches the escaping child");
    checkNear(h->scrollHeight(), 50.0f, 0.01f, "scrollHeight floors at the client height");
    h->setScrollLeft(999.0f);
    checkNear(h->scrollLeft(), 320.0f, 0.01f, "setScrollLeft clamps to the horizontal range");
    h->setScrollTop(10.0f);
    checkNear(h->scrollTop(), 0.0f, 0.01f, "a non-scrolling axis rejects the write");
    const std::string overflowReadback = wideResolver.propertyValue(wideResolver.resolve(*h), "overflow");
    check(overflowReadback == "auto visible", "the overflow pair reads back per axis");
}

int main() {
    testEasingAndTimingFunctions();
    testTransitionDeclarations();
    testTransitions();
    testKeyframeAnimations();
    testCalc();
    testScroll();
    testRuleParsing();
    testSelectorMatching();
    testInteractivePseudoClasses();
    testAttributeSelectors();
    testPositioning();
    testWhiteSpace();
    testPseudoElements();
    testImportantAndCssWideKeywords();
    testBorderWidthLonghands();
    testMediaQueries();
    testCustomProperties();
    testCascade();
    testPropertyParsing();
    testBlockStacking();
    testNestedContainer();
    testTextWrappingAndBr();
    testTextIndent();
    testDisplayNoneIsSkipped();
    testInheritance();
    testChildOverridesInherited();
    testDefaultStyles();
    testPhrasingTagsStayInline();
    testUnknownPropertyIsReported();
    testCommentsInsideDeclarationBlocks();
    testUnitsEndToEnd();
    testFlexRowBasics();
    testFlexColumn();
    testFlexGrowAndShrink();
    testFlexBasisAndShrinkZero();
    testFlexGap();
    testJustifyContent();
    testFlexAlignItems();
    testFlexWrap();
    testFlexReverseDirections();
    testFlexItemMarginsAndPadding();
    testFlexMinMaxClamp();
    testFlexInlineFlexShrinks();
    testFlexPropertyParsing();
    testFlexIsBlockLevel();
    testFlexNestedContainer();
    testLengthUnits();
    testAbsoluteAndKeywordUnits();
    testFontRelativeSizes();
    testLineHeightUnits();
    testBoxSizing();
    testPercentageMinMax();
    testPercentNestedLevels();
    testBackgroundColourParsing();
    testLinearGradientParsing();
    testRadialGradientParsing();
    testBackgroundListsAreParallel();
    testBackgroundSizeAndPositionKeywords();
    testFlexCrossSizeExcludesChrome();
    testFlexBoxSizingIsNotAppliedTwice();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}

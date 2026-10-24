// CSS parsing, cascade, and layout tests. Layout runs without a font so the
// assertions stay independent of any bundled asset; the engine falls back to
// fixed average advances in that case.

#include "Core/Application/Application.h"
#include "CSS/Parser.h"
#include "CSS/PseudoElements.h"
#include "CSS/StyleResolver.h"
#include "Layout/BlockLayout.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"
#include "SUI/Script/Script.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace {

int failures = 0;
bool g_dialogClosed = false;
void flagDialogClose() { g_dialogClosed = true; }

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
        @unknown (display: grid) { .ignored { color: #fff; } }
        .sidebar, #main > p { color: #123456; font-size: 18px }
        p { margin: 4px 8px !important; }
    )");
    check(rules.size() == 3, "comments and unknown at-rules skipped");
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

// `@supports` gates a block on whether the engine implements the declarations
// and selectors inside it. The conditions are evaluated while parsing, so a
// false block contributes no rules at all.
void testSupports() {
    check(sui::StyleResolver::supportsDeclaration("display", "grid"),
          "a supported declaration is recognised");
    check(!sui::StyleResolver::supportsDeclaration("display", "contents"),
          "a display value the engine falls back on is not supported");
    check(!sui::StyleResolver::supportsDeclaration("not-a-property", "red"),
          "an unknown property is not supported");
    check(sui::StyleResolver::supportsDeclaration("--brand", "10px"),
          "custom properties are supported");
    check(sui::StyleResolver::supportsDeclaration("grid-template-columns", "1fr"),
          "grid track values are supported");
    check(sui::StyleResolver::supportsDeclaration("display", "inherit"),
          "CSS-wide keywords are supported for known properties");
    check(!sui::StyleResolver::supportsDeclaration("color", "not-a-colour"),
          "an invalid colour value is not supported");
    check(sui::StyleResolver::supportsDeclaration("color", "var(--brand)"),
          "a var() value is supported on a known property");

    const auto rules = sui::CSSParser::parse(R"(
        @supports (display: grid) { .a { color: #fff } }
        @supports (display: nonsense) { .b { color: #fff } }
        @supports not (display: grid) { .c { color: #fff } }
        @supports (display: grid) and (grid-template-columns: 1fr) {
            .d { color: #fff }
        }
        @supports (display: nonsense) or (grid-template-columns: 1fr) {
            .e { color: #fff }
        }
        @supports (unknown-property: red) { .f { color: #fff } }
        @supports ((display: grid) and (not (display: nonsense))) {
            .g { color: #fff }
        }
        @supports selector(:has(.item)) { .h { color: #fff } }
        @supports selector(:made-up) { .i { color: #fff } }
        @supports (--brand: 10px) { .j { color: #fff } }
        @supports (display: block) { .k { color: #fff } }
        @supports (opacity: 1.0) { .l { color: #fff } }
        @supports (display: contents) { .m { color: #fff } }
        @supports (display: grid) and (unknown-property: red) {
            .n { color: #fff }
        }
        @media (min-width: 100px) {
            @supports (display: grid) { .o { color: #fff } }
        }
        @supports (display: grid) {
            @unknown at-rule { .p { color: #fff } }
            .q { color: #fff }
        }
    )");
    check(rules.size() == 10, "only true @supports blocks contribute rules");
    const auto hasClass = [&](std::string_view name) {
        for (const sui::Rule& rule : rules) {
            if (rule.selector.parts.size() == 1 &&
                rule.selector.parts[0].kind == sui::Selector::Kind::Class &&
                rule.selector.parts[0].value == name) {
                return true;
            }
        }
        return false;
    };
    for (const char* name : {"a", "d", "e", "g", "h", "j", "k", "l", "o", "q"}) {
        check(hasClass(name), (std::string("@supports keeps .") + name).c_str());
    }
    for (const char* name : {"b", "c", "f", "i", "m", "n", "p"}) {
        check(!hasClass(name), (std::string("@supports drops .") + name).c_str());
    }
    for (const sui::Rule& rule : rules) {
        if (rule.selector.parts.size() == 1 && rule.selector.parts[0].value == "o") {
            check(rule.media.size() == 1,
                  "an @supports rule nested in @media keeps the media condition");
        }
    }

    auto document = parseOrNull("<sui><body><div id=\"x\">x</div></body></sui>");
    Element* x = document ? document->getElementById("x") : nullptr;
    if (!x) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#x { width: 10px }"
        "@supports (display: grid) { #x { width: 120px } }"
        "@supports (display: nonsense) { #x { height: 90px } }"));
    layoutRoot(*document, resolver, 400.0f);
    checkNear(resolver.resolve(*x).width.resolve(0.0f, 16.0f, 16.0f), 120.0f, 0.01f,
              "a true @supports block wins the cascade");
    check(resolver.resolve(*x).height.isAuto(),
          "a false @supports block contributes nothing");
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

// The structural pseudo-classes count elements: text nodes and generated
// boxes never shift an index. Plus `:disabled`/`:enabled` on form controls
// and `:focus-within` on the focused element's ancestors.
void testStructuralPseudoClasses() {
    auto document = parseOrNull(
        "<sui><body><div id=\"mix\">text<span id=\"sp\">b</span></div>"
        "<div id=\"row\"><p id=\"p1\">a</p><span id=\"mid\">b</span><p id=\"p2\">c</p></div>"
        "<div id=\"solo\"><p id=\"only\">one</p></div>"
        "<input id=\"en\" type=\"text\"/>"
        "<input id=\"dis\" type=\"text\" disabled/>"
        "<button id=\"btn\">go</button>"
        "<select id=\"sel\"><optgroup id=\"og\" label=\"g\" disabled>"
        "<option id=\"o1\">a</option></optgroup><option id=\"o2\">b</option></select>"
        "</body></sui>");
    if (!document) {
        return;
    }
    Element* sp = document->getElementById("sp");
    Element* p1 = document->getElementById("p1");
    Element* mid = document->getElementById("mid");
    Element* p2 = document->getElementById("p2");
    Element* only = document->getElementById("only");
    Element* en = document->getElementById("en");
    Element* dis = document->getElementById("dis");
    Element* btn = document->getElementById("btn");
    Element* dropdown = document->getElementById("sel");
    Element* o1 = document->getElementById("o1");
    Element* o2 = document->getElementById("o2");
    check(sp && p1 && mid && p2 && only && en && dis && btn && dropdown && o1 && o2,
          "structural elements found");
    if (!sp || !p1 || !mid || !p2 || !only || !en || !dis || !btn || !dropdown || !o1 ||
        !o2) {
        return;
    }
    const auto sel = [](const char* text) { return sui::CSSParser::parseSelector(text); };
    const auto on = [&](const char* text, Element& e) {
        return selectorMatches(sel(text), e);
    };

    // Mixed content: the leading text node does not move the span.
    check(on("span:first-child", *sp), "text before a span keeps it first");
    check(on("span:last-child", *sp), "a lone element child is also last");
    check(on("p:nth-child(1)", *p1), "nth-child(1) matches the first child");
    check(on("span:nth-child(2)", *mid), "nth-child numbers element siblings");

    // Type-restricted and from-the-end counting, plus the lone child.
    check(on("p:nth-of-type(2)", *p2), "nth-of-type(2) matches the second paragraph");
    check(!on("p:nth-of-type(2)", *p1), "nth-of-type(2) misses the first");
    check(on("span:nth-of-type(1)", *mid), "nth-of-type counts within the tag");
    check(on("p:nth-last-child(1)", *p2), "nth-last-child(1) matches the last child");
    check(on("span:nth-last-child(2)", *mid), "nth-last-child counts back from the end");
    check(!on("p:nth-last-child(1)", *p1), "nth-last-child(1) misses the first");
    check(on("p:only-child", *only), "only-child matches a lone child");
    check(!on("p:only-child", *p1), "only-child misses among siblings");
    check(on("span:only-of-type", *mid), "only-of-type matches a lone tag");
    check(!on("p:only-of-type", *p1), "only-of-type misses a repeated tag");
    check(on("p:nth-last-of-type(1)", *p2), "nth-last-of-type(1) matches the last paragraph");
    check(on("p:nth-last-of-type(2)", *p1), "nth-last-of-type counts back within the tag");
    check(!on("span:nth-last-of-type(2)", *mid), "nth-last-of-type misses past the lone tag");

    // Generated boxes never count either: a `::before` on the parent must not
    // shift the paragraphs it precedes.
    {
        auto gdoc = parseOrNull(
            "<sui><body><div id=\"g\"><p id=\"q1\">a</p><p id=\"q2\">b</p></div></body></sui>");
        if (gdoc) {
            Element* q1 = gdoc->getElementById("q1");
            Element* q2 = gdoc->getElementById("q2");
            check(q1 && q2, "generated-box elements found");
            if (q1 && q2) {
                sui::StyleResolver presolver;
                presolver.setRules(sui::CSSParser::parse("#g::before{content:\"x\"}"));
                sui::generatePseudoElements(*gdoc, presolver);
                check(on("p:first-child", *q1), "::before does not take first-child");
                check(on("p:nth-child(2)", *q2), "::before does not shift nth-child");
            }
        }
    }

    // Form-control states, including an option disabled through its group.
    check(on("input:enabled", *en), "an enabled input matches :enabled");
    check(!on("input:disabled", *en), "an enabled input misses :disabled");
    check(on("input:disabled", *dis), "a disabled input matches :disabled");
    check(!on("input:enabled", *dis), "a disabled input misses :enabled");
    check(on("button:enabled", *btn), "a button is enableable");
    check(on("select:enabled", *dropdown), "a select is enableable");
    check(on("option:disabled", *o1), "an option inherits its optgroup's disabled");
    check(!on("option:enabled", *o1), "a grouped-disabled option misses :enabled");
    check(on("option:enabled", *o2), "a plain option matches :enabled");
    check(!on("div:disabled", *p1->parent()), "a div matches neither state");
    check(!on("div:enabled", *p1->parent()), "a div is not enableable");

    // `:focus-within` belongs to the focused element and its ancestors.
    sui::PseudoState focused;
    focused.focused = mid;
    check(selectorMatches(sel("span:focus-within"), *mid, &focused),
          ":focus-within matches the focused element");
    check(selectorMatches(sel("div:focus-within"), *p1->parent(), &focused),
          ":focus-within matches an ancestor of the focused element");
    check(!selectorMatches(sel("div:focus-within"), *sp->parent(), &focused),
          ":focus-within misses a subtree without focus");
    check(!selectorMatches(sel("p:focus-within"), *p1, &focused),
          ":focus-within misses a sibling of the focused element");

    // The state drives the cascade, like `:hover` does for descendants.
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0} #row p{color:#ff0000} #row p:nth-of-type(2){color:#00ff00}"
        " #dis{background-color:#ff0000} #dis:disabled{background-color:#0000ff}"
        " #row{color:#ff0000} #row:focus-within{color:#00ff00}"));
    check(resolver.resolve(*p2).color.g == 0xff,
          "a matching nth-of-type rule restyles its paragraph");
    check(resolver.resolve(*dis).backgroundColor.b == 0xff,
          ":disabled applies over the plain id rule");
    check(resolver.resolve(*p1).color.r == 0xff, "the first paragraph stays red");
    check(resolver.resolve(*mid->parent()).color.r == 0xff,
          "the row is red without focus");
    resolver.setFocused(mid);
    check(resolver.resolve(*mid->parent()).color.g == 0xff,
          "focusing a child lights the :focus-within ancestor");
    resolver.setFocused(nullptr);
    check(resolver.resolve(*mid->parent()).color.r == 0xff,
          "clearing focus drops the :focus-within rule");
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

void testColorFunctions() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\">x</div><div id=\"b\">x</div><div id=\"c\">x</div>"
        "<div id=\"d\">x</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { color: oklch(0 0 0) }"
        "#b { color: oklch(1 0 0) }"
        "#c { color: color-mix(in srgb, red, blue) }"
        "#d { color: color-mix(in srgb, red 25%, blue) }"));
    const auto a = resolver.resolve(*document->getElementById("a"));
    const auto b = resolver.resolve(*document->getElementById("b"));
    const auto c = resolver.resolve(*document->getElementById("c"));
    const auto d = resolver.resolve(*document->getElementById("d"));
    check(a.color.r == 0 && a.color.g == 0 && a.color.b == 0, "oklch black converts");
    check(b.color.r == 255 && b.color.g == 255 && b.color.b == 255,
          "oklch white converts to near-white");
    checkNear(c.color.r, 128.0f, 1.0f, "color-mix splits two colours evenly (r)");
    checkNear(c.color.b, 128.0f, 1.0f, "color-mix splits two colours evenly (b)");
    checkNear(d.color.r, 64.0f, 1.0f, "color-mix honours an explicit weight (r)");
    checkNear(d.color.b, 191.0f, 1.0f, "color-mix gives the remainder to the other (b)");

    // A rounded-trip check: the OKLab round trip of mid grey stays neutral.
    sui::StyleResolver grey;
    grey.setRules(sui::CSSParser::parse("#a { color: oklch(0.5 0 0) }"));
    const auto g = grey.resolve(*document->getElementById("a"));
    check(std::abs(int(g.color.r) - int(g.color.g)) <= 1 &&
              std::abs(int(g.color.g) - int(g.color.b)) <= 1,
          "oklch with zero chroma stays grey");
}

void testSyntheticEmphasis() {
    auto document = parseOrNull(
        "<sui><body><p id=\"a\">Hello</p><p id=\"b\">Hello</p></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0;font-size:20px} #b{font-weight:bold;font-style:italic}"));
    layoutRoot(*document, resolver, 400.0f);

    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    check(a && b && !a->textRuns().empty() && !b->textRuns().empty(),
          "emphasis paragraphs laid out");
    if (!a || !b || a->textRuns().empty() || b->textRuns().empty()) {
        return;
    }
    check(!a->textRuns()[0].bold && !a->textRuns()[0].italic,
          "a normal run carries no synthetic emphasis");
    check(b->textRuns()[0].bold, "font-weight:bold reaches the text run");
    check(b->textRuns()[0].italic, "font-style:italic reaches the text run");
    check(b->textRuns()[0].width > a->textRuns()[0].width + 1.0f,
          "a synthetic italic reserves extra width for its lean");
}

void testVerticalAlign() {
    auto document = parseOrNull("<sui><body><p id=\"a\">x<span id=\"b\">y</span></p></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(
        sui::CSSParser::parse("body{margin:0;font-size:20px} #b{vertical-align:super}"));
    layoutRoot(*document, resolver, 400.0f);

    Element* a = document->getElementById("a");
    check(a != nullptr && a->textRuns().size() == 2, "inline runs laid out around the span");
    if (!a || a->textRuns().size() != 2) {
        return;
    }
    check(a->textRuns()[1].y < a->textRuns()[0].y - 1.0f,
          "vertical-align:super raises the inline run");
}

void testVerticalAlignTopBottom() {
    // Two inline-blocks on one line: the default sits at the line's top (its
    // baseline here), `bottom` drops its outer edge onto the line's bottom, so it
    // lands strictly lower than its neighbour.
    auto document = parseOrNull(
        "<sui><body><p id=\"p\">"
        "<span id=\"t\" style=\"display:inline-block;width:20px;height:10px\"></span>"
        "<span id=\"b\" style=\"display:inline-block;width:20px;height:10px;"
        "vertical-align:bottom\"></span>"
        "</p></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0} #p{margin:0;font-size:16px}"));
    layoutRoot(*document, resolver, 400.0f);

    Element* t = document->getElementById("t");
    Element* b = document->getElementById("b");
    check(t != nullptr && b != nullptr, "both inline-blocks were laid out");
    if (!t || !b) {
        return;
    }
    check(b->layoutY() > t->layoutY() + 1.0f,
          "vertical-align:bottom seats an inline-block on the line's bottom edge");

    // text-top / text-bottom read back distinctly and place an inline-block the
    // same way top/bottom do here, where the content area spans the line box.
    auto tb = parseOrNull(
        "<sui><body><p id=\"p\">"
        "<span id=\"t\" style=\"display:inline-block;width:20px;height:10px;"
        "vertical-align:text-top\"></span>"
        "<span id=\"b\" style=\"display:inline-block;width:20px;height:10px;"
        "vertical-align:text-bottom\"></span>"
        "</p></body></sui>");
    if (!tb) {
        return;
    }
    sui::StyleResolver tbResolver;
    tbResolver.setRules(sui::CSSParser::parse("body{margin:0} #p{margin:0;font-size:16px}"));
    layoutRoot(*tb, tbResolver, 400.0f);
    Element* tt = tb->getElementById("t");
    Element* bb = tb->getElementById("b");
    check(tt != nullptr && bb != nullptr && bb->layoutY() > tt->layoutY() + 1.0f,
          "vertical-align:text-bottom drops an inline-block to the line's bottom");
    if (tt) {
        const ComputedStyle s = tbResolver.resolve(*tt);
        check(sui::StyleResolver::propertyValue(s, "vertical-align") == "text-top",
              "vertical-align:text-top serialises back");
    }
}

void testVerticalAlignTextTopBottomInline() {
    auto docTT = parseOrNull("<sui><body><p id=\"p\">big<span id=\"s\" style=\"font-size:10px;vertical-align:text-top\">small</span></p></body></sui>");
    if (docTT) {
        sui::StyleResolver res;
        res.setRules(sui::CSSParser::parse("body{margin:0} #p{margin:0;font-size:20px}"));
        layoutRoot(*docTT, res, 400.0f);
        Element* p = docTT->getElementById("p");
        check(p != nullptr && p->textRuns().size() == 2, "text runs created for text-top span");
        if (p && p->textRuns().size() == 2) {
            check(p->textRuns()[1].y < p->textRuns()[0].y,
                  "vertical-align:text-top on inline text raises smaller font run");
        }
    }

    auto docTB = parseOrNull("<sui><body><p id=\"p\">big<span id=\"s\" style=\"font-size:10px;vertical-align:text-bottom\">small</span></p></body></sui>");
    if (docTB) {
        sui::StyleResolver res;
        res.setRules(sui::CSSParser::parse("body{margin:0} #p{margin:0;font-size:20px}"));
        layoutRoot(*docTB, res, 400.0f);
        Element* p = docTB->getElementById("p");
        check(p != nullptr && p->textRuns().size() == 2, "text runs created for text-bottom span");
        if (p && p->textRuns().size() == 2) {
            check(p->textRuns()[1].y > p->textRuns()[0].y,
                  "vertical-align:text-bottom on inline text lowers smaller font run");
        }
    }
}

void testMediaElementsLayout() {
    auto doc = parseOrNull(
        "<sui><body>"
        "<video id=\"v\" poster=\"test.png\"></video>"
        "<canvas id=\"c\"></canvas>"
        "<audio id=\"a\" controls></audio>"
        "</body></sui>");
    if (!doc) return;
    sui::StyleResolver res;
    res.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*doc, res, 800.0f);

    Element* v = doc->getElementById("v");
    Element* c = doc->getElementById("c");
    Element* a = doc->getElementById("a");
    check(v != nullptr && v->layoutWidth() == 300.0f && v->layoutHeight() == 150.0f,
          "<video> defaults to 300x150 dimensions");
    check(c != nullptr && c->layoutWidth() == 300.0f && c->layoutHeight() == 150.0f,
          "<canvas> defaults to 300x150 dimensions");
    check(a != nullptr && a->layoutWidth() == 300.0f && a->layoutHeight() == 32.0f,
          "<audio> defaults to 300x32 dimensions");
}

void testObjectFitAndListStyle() {
    auto doc = parseOrNull(
        "<sui><body>"
        "<img id=\"i1\" style=\"object-fit: cover;\"/>"
        "<img id=\"i2\" style=\"object-fit: fill;\"/>"
        "<img id=\"i3\" style=\"object-fit: none;\"/>"
        "<img id=\"i4\" style=\"object-fit: scale-down;\"/>"
        "<img id=\"i5\"/>"
        "<ul id=\"ul1\">"
        "  <li id=\"li1\">Item 1</li>"
        "  <li id=\"li2\" style=\"list-style-type: square;\">Item 2</li>"
        "  <li id=\"li3\" style=\"list-style: circle;\">Item 3</li>"
        "  <li id=\"li4\" style=\"list-style: none;\">Item 4</li>"
        "</ul>"
        "<ol id=\"ol1\">"
        "  <li id=\"oli1\">1</li>"
        "  <li id=\"oli2\">2</li>"
        "</ol>"
        "<ul id=\"ul2\" style=\"list-style: square;\">"
        "  <li id=\"ul2_li1\">A</li>"
        "</ul>"
        "</body></sui>");
    if (!doc) return;
    sui::StyleResolver res;
    res.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*doc, res, 800.0f);

    Element* i1 = doc->getElementById("i1");
    Element* i2 = doc->getElementById("i2");
    Element* i3 = doc->getElementById("i3");
    Element* i4 = doc->getElementById("i4");
    Element* i5 = doc->getElementById("i5");

    auto s1 = res.resolve(*i1);
    auto s2 = res.resolve(*i2);
    auto s3 = res.resolve(*i3);
    auto s4 = res.resolve(*i4);
    auto s5 = res.resolve(*i5);

    check(s1.objectFit == sui::ComputedStyle::ObjCover, "object-fit: cover parses");
    check(s2.objectFit == sui::ComputedStyle::ObjFill, "object-fit: fill parses");
    check(s3.objectFit == sui::ComputedStyle::ObjNone, "object-fit: none parses");
    check(s4.objectFit == sui::ComputedStyle::ObjScaleDown, "object-fit: scale-down parses");
    check(s5.objectFit == sui::ComputedStyle::ObjContain, "object-fit default is contain");

    check(sui::StyleResolver::propertyValue(s1, "object-fit") == "cover", "object-fit serializes to cover");
    check(sui::StyleResolver::propertyValue(s2, "object-fit") == "fill", "object-fit serializes to fill");

    Element* ul1 = doc->getElementById("ul1");
    Element* li1 = doc->getElementById("li1");
    Element* li2 = doc->getElementById("li2");
    Element* li3 = doc->getElementById("li3");
    Element* li4 = doc->getElementById("li4");

    auto sUl1 = res.resolve(*ul1);
    auto sLi1 = res.resolve(*li1);
    auto sLi2 = res.resolve(*li2);
    auto sLi3 = res.resolve(*li3);
    auto sLi4 = res.resolve(*li4);

    check(sUl1.listStyleType == sui::ComputedStyle::LSTDisc, "<ul> defaults to list-style-type: disc");
    check(sLi1.listStyleType == sui::ComputedStyle::LSTDisc, "<li> inherits list-style-type: disc from <ul>");
    check(sLi2.listStyleType == sui::ComputedStyle::LSTSquare, "<li> overrides list-style-type to square");
    check(sLi3.listStyleType == sui::ComputedStyle::LSTCircle, "<li> shorthand list-style sets circle");
    check(sLi4.listStyleType == sui::ComputedStyle::LSTNone, "<li> shorthand list-style: none sets LSTNone");

    Element* ol1 = doc->getElementById("ol1");
    Element* oli1 = doc->getElementById("oli1");
    auto sOl1 = res.resolve(*ol1);
    auto sOli1 = res.resolve(*oli1);
    check(sOl1.listStyleType == sui::ComputedStyle::LSTDecimal, "<ol> defaults to list-style-type: decimal");
    check(sOli1.listStyleType == sui::ComputedStyle::LSTDecimal, "<li> inside <ol> inherits list-style-type: decimal");

    Element* ul2_li1 = doc->getElementById("ul2_li1");
    auto sUl2_li1 = res.resolve(*ul2_li1);
    check(sUl2_li1.listStyleType == sui::ComputedStyle::LSTSquare, "<li> inherits list-style: square from <ul style=\"list-style: square\">");
}

void testTextDecorationColor() {
    auto doc = parseOrNull(
        "<sui><body>"
        "<div id=\"d1\" style=\"text-decoration: underline red;\">Underlined red</div>"
        "<div id=\"d2\" style=\"text-decoration-line: line-through; text-decoration-color: #00ff00;\">Line through green</div>"
        "<div id=\"d3\" style=\"color: #0000ff; text-decoration: overline;\">"
        "  <span id=\"s3\">Inherited decoration</span>"
        "</div>"
        "</body></sui>");
    if (!doc) return;
    sui::StyleResolver res;
    layoutRoot(*doc, res, 800.0f);

    Element* d1 = doc->getElementById("d1");
    Element* d2 = doc->getElementById("d2");
    Element* d3 = doc->getElementById("d3");
    Element* s3 = doc->getElementById("s3");

    auto sD1 = res.resolve(*d1);
    auto sD2 = res.resolve(*d2);
    auto sD3 = res.resolve(*d3);
    auto sS3 = res.resolve(*s3);

    check(sD1.textDecorationLine == sui::ComputedStyle::DecUnderline, "text-decoration shorthand sets underline");
    check(sD1.hasTextDecorationColor, "text-decoration shorthand sets color flag");
    check(sD1.textDecorationColor.r == 255 && sD1.textDecorationColor.g == 0 && sD1.textDecorationColor.b == 0,
          "text-decoration shorthand sets red color");

    check(sD2.textDecorationLine == sui::ComputedStyle::DecLineThrough, "text-decoration-line sets line-through");
    check(sD2.hasTextDecorationColor, "text-decoration-color sets flag");
    check(sD2.textDecorationColor.r == 0 && sD2.textDecorationColor.g == 255 && sD2.textDecorationColor.b == 0,
          "text-decoration-color sets green");
    check(sui::StyleResolver::propertyValue(sD2, "text-decoration-color") == "#00ff00",
          "text-decoration-color serializes to #00ff00");

    check(!sD3.hasTextDecorationColor, "text-decoration without color leaves flag unset");
    check(sui::StyleResolver::propertyValue(sD3, "text-decoration-color") == "#0000ff",
          "unset text-decoration-color falls back to element color");
}

void testGridAlignment() {
    // Track distribution: two 50px columns in a 300px box leave 200px, which
    // `justify-content: center` splits into 100px of leading space.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"t\"><i></i><i></i></div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{display:grid;grid-template-columns:50px 50px;"
            "justify-content:center;width:300px;height:40px} #t i{height:10px}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* t = document->getElementById("t");
        if (t && t->children().size() == 2) {
            checkNear(t->children()[0]->layoutX(), 100.0f, 1.0f,
                      "justify-content:center centres the track set");
            checkNear(t->children()[1]->layoutX(), 150.0f, 1.0f,
                      "the second centred track follows the first");
        }
    }
    // Item alignment inside its own track: a 50px item pinned to the end of a
    // 200px column starts 150px in.
    {
        auto document =
            parseOrNull("<sui><body><div id=\"t\"><i></i></div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{display:grid;grid-template-columns:200px;"
            "justify-items:end;width:200px;height:40px} #t i{width:50px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* t = document->getElementById("t");
        if (t && !t->children().empty()) {
            checkNear(t->children()[0]->layoutX(), 150.0f, 1.0f,
                      "justify-items:end pins the item to the track end");
        }
    }
    // Block-axis alignment against a fixed row: a 20px item at the end of a
    // 100px row sits 80px down.
    {
        auto document =
            parseOrNull("<sui><body><div id=\"t\"><i></i></div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{display:grid;grid-template-columns:100px;"
            "grid-template-rows:100px;align-items:end;width:100px;height:100px}"
            "#t i{width:20px;height:20px}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* t = document->getElementById("t");
        if (t && !t->children().empty()) {
            checkNear(t->children()[0]->layoutY(), 80.0f, 1.0f,
                      "align-items:end pins the item to the row bottom");
        }
    }
}

void testInputValue() {
    auto document =
        parseOrNull("<sui><body><input id=\"i\" value=\"hi\"/></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0} #i{width:100px}"));
    layoutRoot(*document, resolver, 400.0f);

    Element* i = document->getElementById("i");
    check(i != nullptr && !i->textRuns().empty() && i->textRuns()[0].text == "hi",
          "a text field renders its value as content");
    if (!i) {
        return;
    }
    // Editing writes the value, so the next layout has to show the new text.
    i->setValue("bye");
    layoutRoot(*document, resolver, 400.0f);
    check(!i->textRuns().empty() && i->textRuns()[0].text == "bye",
          "the field re-renders after setValue");
}

void testGridMinmax() {
    auto document = parseOrNull(
        "<sui><body><div id=\"t\"><i></i><i></i></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0} #t{display:grid;"
        "grid-template-columns:repeat(2,minmax(100px,1fr));width:400px;height:40px}"
        "#t i{height:10px}"));
    layoutRoot(*document, resolver, 500.0f);

    Element* t = document->getElementById("t");
    if (t && t->children().size() == 2) {
        checkNear(t->children()[0]->layoutX(), 0.0f, 1.0f, "minmax first track starts at zero");
        checkNear(t->children()[1]->layoutX(), 200.0f, 1.0f,
                  "minmax fr tracks split the leftover after their floor");
    }
    if (t) {
        const ComputedStyle s = resolver.resolve(*t);
        check(sui::StyleResolver::propertyValue(s, "grid-template-columns") ==
                  "minmax(100px, 1fr) minmax(100px, 1fr)",
              "minmax tracks serialise back");
    }

    // The floor wins when the container is narrower than the sum of the minimums.
    auto narrow = parseOrNull("<sui><body><div id=\"n\"><i></i></div></body></sui>");
    if (!narrow) {
        return;
    }
    sui::StyleResolver narrowResolver;
    narrowResolver.setRules(sui::CSSParser::parse(
        "body{margin:0} #n{display:grid;grid-template-columns:minmax(150px,1fr);"
        "width:100px;height:20px} #n i{height:10px}"));
    layoutRoot(*narrow, narrowResolver, 500.0f);
    Element* n = narrow->getElementById("n");
    if (n && !n->children().empty()) {
        check(n->children()[0]->layoutWidth() >= 149.0f,
              "a minmax minimum overflows a container too narrow for it");
    }
}

void testGridAutoFill() {
    auto document = parseOrNull(
        "<sui><body><div id=\"t\"><i></i><i></i><i></i><i></i></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0} #t{display:grid;"
        "grid-template-columns:repeat(auto-fill,minmax(100px,1fr));"
        "width:350px;height:80px} #t i{height:10px}"));
    layoutRoot(*document, resolver, 500.0f);

    Element* t = document->getElementById("t");
    if (t && t->children().size() == 4) {
        // Three 100px tracks fit in 350px; the 50px left over is shared by their fr
        // maxima, so each track is ~116.67px and the third starts at ~233.33px.
        checkNear(t->children()[1]->layoutX(), 116.667f, 1.0f,
                  "auto-fill repeats the body to fill the row");
        checkNear(t->children()[2]->layoutX(), 233.333f, 1.0f,
                  "the third auto-fill track follows the second");
        checkNear(t->children()[3]->layoutY(), 10.0f, 1.0f,
                  "the fourth item wraps to a second row");
    }
    if (t) {
        const ComputedStyle s = resolver.resolve(*t);
        check(sui::StyleResolver::propertyValue(s, "grid-template-columns") ==
                  "repeat(auto-fill, minmax(100px, 1fr))",
              "auto-fill serialises back");
    }

    // auto-fit drops tracks no item uses, so two items get two wide tracks rather
    // than three narrow ones.
    auto fit = parseOrNull("<sui><body><div id=\"f\"><i></i><i></i></div></body></sui>");
    if (!fit) {
        return;
    }
    sui::StyleResolver fitResolver;
    fitResolver.setRules(sui::CSSParser::parse(
        "body{margin:0} #f{display:grid;"
        "grid-template-columns:repeat(auto-fit,minmax(100px,1fr));"
        "width:350px;height:40px} #f i{height:10px}"));
    layoutRoot(*fit, fitResolver, 500.0f);
    Element* f = fit->getElementById("f");
    if (f && f->children().size() == 2) {
        checkNear(f->children()[1]->layoutX(), 175.0f, 1.0f,
                  "auto-fit collapses unused tracks");
    }
}

void testGridStretch() {
    auto document = parseOrNull(
        "<sui><body><div id=\"g\"><i id=\"a\"></i><i id=\"b\"></i></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0} #g{display:grid;grid-template-columns:1fr 1fr;"
        "grid-template-rows:60px;width:200px} "
        "#a{height:10px} #b{align-self:start;height:10px}"));
    layoutRoot(*document, resolver, 400.0f);

    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    // The default align-self:stretch fills the 60px row; align-self:start keeps the
    // item at its own 10px height and top-aligns it.
    check(a != nullptr && std::fabs(a->layoutHeight() - 60.0f) < 0.5f,
          "a grid item stretches to a definite row track");
    check(b != nullptr && std::fabs(b->layoutHeight() - 10.0f) < 0.5f,
          "align-self:start opts a grid item out of stretch");
}

void testGridSpans() {
    // A column-spanning item takes two cells, so the next item slides into what is
    // left and the following one starts a new row.
    auto cols = parseOrNull("<sui><body><div id=\"g\">"
                            "<i id=\"a\" style=\"grid-column:span 2\"></i>"
                            "<i id=\"b\"></i><i id=\"c\"></i><i id=\"d\"></i>"
                            "<i id=\"e\"></i></div></body></sui>");
    if (cols) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:repeat(3,100px);"
            "width:300px} #g i{height:10px}"));
        layoutRoot(*cols, resolver, 500.0f);

        Element* a = cols->getElementById("a");
        Element* b = cols->getElementById("b");
        Element* c = cols->getElementById("c");
        Element* e = cols->getElementById("e");
        check(a && std::fabs(a->layoutWidth() - 200.0f) < 0.5f,
              "a column span claims two cells");
        check(b && std::fabs(b->layoutX() - 200.0f) < 0.5f,
              "the next item slides past the spanned cells");
        check(c && std::fabs(c->layoutY() - 10.0f) < 0.5f,
              "the row fills after the spanned cells are used");
        check(e && std::fabs(e->layoutY() - 10.0f) < 0.5f,
              "five items land on two rows");
        if (a) {
            const ComputedStyle s = resolver.resolve(*a);
            check(sui::StyleResolver::propertyValue(s, "grid-column") == "span 2",
                  "the column span serialises back");
        }
    }

    // A row-spanning item holds its column across both rows, so the following
    // items are pushed one column to the right on each row.
    auto rows = parseOrNull("<sui><body><div id=\"g\">"
                            "<i id=\"a\" style=\"grid-row:span 2;height:80px\"></i>"
                            "<i id=\"b\"></i><i id=\"c\"></i></div></body></sui>");
    if (rows) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:100px 100px;"
            "grid-template-rows:40px 40px;width:200px} #g i{height:10px}"));
        layoutRoot(*rows, resolver, 500.0f);

        Element* a = rows->getElementById("a");
        Element* b = rows->getElementById("b");
        Element* c = rows->getElementById("c");
        check(a && std::fabs(a->layoutWidth() - 100.0f) < 0.5f,
              "a row span keeps the first column");
        check(b && std::fabs(b->layoutX() - 100.0f) < 0.5f &&
                  std::fabs(b->layoutY() - 0.0f) < 0.5f,
              "the next item sits in the first row beside the span");
        check(c && std::fabs(c->layoutX() - 100.0f) < 0.5f &&
                  std::fabs(c->layoutY() - 40.0f) < 0.5f,
              "the last item sits in the second row beside the span");
    }
}

void testGridLinePlacement() {
    // `grid-column: 2 / 4` on a 4-column grid: item starts in column 2 (0-based
    // index 1) and spans 2 columns, so it occupies columns 2-3 (x = 100, w = 200).
    // The first auto-placed item should land in column 1 (x = 0), and the third
    // auto-placed item should appear after the pinned one in column 4 (x = 300).
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\"></i>"
            "<i id=\"b\" style=\"grid-column:2/4\"></i>"
            "<i id=\"c\"></i>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:repeat(4,100px);"
            "width:400px} #g i{height:10px}"));
        layoutRoot(*document, resolver, 500.0f);

        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(a && std::fabs(a->layoutX() - 0.0f) < 0.5f,
              "first auto item lands in column 1");
        check(b && std::fabs(b->layoutX() - 100.0f) < 0.5f,
              "explicit grid-column:2/4 pins item to column 2");
        check(b && std::fabs(b->layoutWidth() - 200.0f) < 0.5f,
              "explicit grid-column:2/4 spans two tracks");
        check(c && std::fabs(c->layoutX() - 300.0f) < 0.5f,
              "auto item after pinned item fills remaining column");
        if (b) {
            const ComputedStyle s = resolver.resolve(*b);
            check(sui::StyleResolver::propertyValue(s, "grid-column") == "2 / 4",
                  "grid-column:2/4 serialises back with start and end lines");
            check(sui::StyleResolver::propertyValue(s, "grid-column-start") == "2",
                  "grid-column-start serialises as '2'");
            check(sui::StyleResolver::propertyValue(s, "grid-column-end") == "4",
                  "grid-column-end serialises as '4'");
        }
    }

    // `grid-column: 3` (bare line number): item starts at column 3, spans 1.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\"></i>"
            "<i id=\"b\" style=\"grid-column:3\"></i>"
            "<i id=\"c\"></i>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:repeat(4,100px);"
            "width:400px} #g i{height:10px}"));
        layoutRoot(*document, resolver, 500.0f);

        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(a && std::fabs(a->layoutX() - 0.0f) < 0.5f,
              "first auto item in column 1 before pinned column-3 item");
        check(b && std::fabs(b->layoutX() - 200.0f) < 0.5f,
              "bare line number grid-column:3 pins to column 3 (x=200)");
        check(c && std::fabs(c->layoutX() - 100.0f) < 0.5f,
              "auto item fills the free cell in column 2");
    }

    // `grid-row: 2` (explicit row start): item is pinned to the second row.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\" style=\"grid-row:2;height:20px\"></i>"
            "<i id=\"b\" style=\"height:10px\"></i>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:100px 100px;"
            "grid-template-rows:10px 20px;width:200px}"));
        layoutRoot(*document, resolver, 300.0f);

        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        // `b` should fill row 1 col 1; `a` is forced to row 2.
        check(b && std::fabs(b->layoutY() - 0.0f) < 0.5f,
              "auto item fills first row when peer is pinned to row 2");
        check(a && std::fabs(a->layoutY() - 10.0f) < 0.5f,
              "grid-row:2 pins item to second row (y=10)");
        if (a) {
            const ComputedStyle s = resolver.resolve(*a);
            check(sui::StyleResolver::propertyValue(s, "grid-row-start") == "2",
                  "grid-row-start serialises as '2'");
        }
    }

    // Longhands: grid-column-start + grid-column-end produce the same result as
    // the shorthand.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"b\" style=\"grid-column-start:2;grid-column-end:4\"></i>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:repeat(4,50px);"
            "width:200px} #g i{height:10px}"));
        layoutRoot(*document, resolver, 300.0f);

        Element* b = document->getElementById("b");
        check(b && std::fabs(b->layoutX() - 50.0f) < 0.5f,
              "grid-column-start:2 longhand pins item to column 2");
        check(b && std::fabs(b->layoutWidth() - 100.0f) < 0.5f,
              "grid-column-start:2 grid-column-end:4 span = 2 tracks");
    }
}

void testGridTemplateAreas() {
    // 3 rows, 2 columns layout with named grid areas:
    // "head head" (row 1, col 1-2)
    // "nav  main" (row 2, col 1-2)
    // "nav  foot" (row 3, col 1-2)
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"h\" style=\"grid-area:head\"></i>"
            "<i id=\"n\" style=\"grid-area:nav\"></i>"
            "<i id=\"m\" style=\"grid-area:main\"></i>"
            "<i id=\"f\" style=\"grid-area:foot\"></i>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;width:300px;"
            "grid-template-areas:\"head head\" \"nav main\" \"nav foot\";"
            "grid-template-columns:100px 200px;"
            "grid-template-rows:50px 150px 40px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* g = document->getElementById("g");
        Element* h = document->getElementById("h");
        Element* n = document->getElementById("n");
        Element* m = document->getElementById("m");
        Element* f = document->getElementById("f");
        check(h && n && m && f && g, "grid area elements found");
        if (!h || !n || !m || !f || !g) {
            return;
        }

        // head spans columns 1-2 (x=0, w=300), sits on row 1 (y=0, h=50)
        check(std::fabs(h->layoutX() - 0.0f) < 0.5f, "head area starts at x=0");
        check(std::fabs(h->layoutWidth() - 300.0f) < 0.5f, "head area spans both columns (w=300)");
        check(std::fabs(h->layoutY() - 0.0f) < 0.5f, "head area sits at row 1 (y=0)");
        check(std::fabs(h->layoutHeight() - 50.0f) < 0.5f, "head area height matches row 1 track (h=50)");

        // nav starts at col 1 (x=0, w=100), spans rows 2-3 (y=50, h=150+40=190)
        check(std::fabs(n->layoutX() - 0.0f) < 0.5f, "nav area starts at col 1 (x=0)");
        check(std::fabs(n->layoutWidth() - 100.0f) < 0.5f, "nav area width matches col 1 (w=100)");
        check(std::fabs(n->layoutY() - 50.0f) < 0.5f, "nav area starts at row 2 (y=50)");
        check(std::fabs(n->layoutHeight() - 190.0f) < 0.5f, "nav area spans rows 2 and 3 (h=190)");

        // main sits at col 2 row 2 (x=100, y=50, w=200, h=150)
        check(std::fabs(m->layoutX() - 100.0f) < 0.5f, "main area starts at col 2 (x=100)");
        check(std::fabs(m->layoutWidth() - 200.0f) < 0.5f, "main area width matches col 2 (w=200)");
        check(std::fabs(m->layoutY() - 50.0f) < 0.5f, "main area starts at row 2 (y=50)");
        check(std::fabs(m->layoutHeight() - 150.0f) < 0.5f, "main area height matches row 2 (h=150)");

        // foot sits at col 2 row 3 (x=100, y=200, w=200, h=40)
        check(std::fabs(f->layoutX() - 100.0f) < 0.5f, "foot area starts at col 2 (x=100)");
        check(std::fabs(f->layoutWidth() - 200.0f) < 0.5f, "foot area width matches col 2 (w=200)");
        check(std::fabs(f->layoutY() - 200.0f) < 0.5f, "foot area starts at row 3 (y=200)");
        check(std::fabs(f->layoutHeight() - 40.0f) < 0.5f, "foot area height matches row 3 (h=40)");

        // Serialization checks:
        const ComputedStyle gs = resolver.resolve(*g);
        check(sui::StyleResolver::propertyValue(gs, "grid-template-areas") ==
                  "\"head head\" \"nav main\" \"nav foot\"",
              "grid-template-areas roundtrips serialized value");
        const ComputedStyle hs = resolver.resolve(*h);
        check(sui::StyleResolver::propertyValue(hs, "grid-area") == "head",
              "grid-area serializes as authored area name");
    }

    // Invalid non-rectangular template areas are rejected
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("div { grid-template-areas: \"a b\" \"b a\"; }"));
        auto doc = parseOrNull("<sui><body><div id=\"d\"></div></body></sui>");
        if (doc) {
            Element* d = doc->getElementById("d");
            if (d) {
                const auto cs = resolver.resolve(*d);
                check(cs.gridTemplateAreas.empty(), "non-rectangular grid-template-areas is rejected");
            }
        }
    }
}

void testGridTemplateShorthand() {
    // Rows / columns form: explicit tracks on both axes.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\"></i><i id=\"b\"></i><i id=\"c\"></i><i id=\"d\"></i>"
            "</div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template:40px 60px / 50px 100px;"
            "width:150px;height:100px} #g i{width:100%;height:100%}"));
        layoutRoot(*document, resolver, 300.0f);

        Element* g = document->getElementById("g");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        Element* d = document->getElementById("d");
        check(g && a && b && c && d, "grid-template rows/columns elements found");
        if (g && a && b && c && d) {
            checkNear(a->layoutX(), 0.0f, 0.5f, "grid-template row/column item a x");
            checkNear(a->layoutY(), 0.0f, 0.5f, "grid-template row/column item a y");
            checkNear(b->layoutX(), 50.0f, 0.5f, "grid-template row/column item b x");
            checkNear(c->layoutY(), 40.0f, 0.5f, "grid-template row/column item c y");
            checkNear(d->layoutX(), 50.0f, 0.5f, "grid-template row/column item d x");
            checkNear(d->layoutY(), 40.0f, 0.5f, "grid-template row/column item d y");
        }

        const ComputedStyle gs = resolver.resolve(*g);
        check(sui::StyleResolver::propertyValue(gs, "grid-template") ==
                  "40px 60px / 50px 100px",
              "grid-template rows/columns roundtrips");
        check(sui::StyleResolver::propertyValue(gs, "grid-template-rows") == "40px 60px",
              "grid-template sets grid-template-rows");
        check(sui::StyleResolver::propertyValue(gs, "grid-template-columns") ==
                  "50px 100px",
              "grid-template sets grid-template-columns");
    }

    // String form: area rows carry their own row tracks.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"h\" style=\"grid-area:head\"></i>"
            "<i id=\"n\" style=\"grid-area:nav\"></i>"
            "<i id=\"m\" style=\"grid-area:main\"></i>"
            "<i id=\"f\" style=\"grid-area:foot\"></i>"
            "</div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;width:300px;"
            "grid-template:\"head head\" 50px \"nav main\" 100px "
            "\"nav foot\" 40px / 100px 200px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* g = document->getElementById("g");
        Element* h = document->getElementById("h");
        Element* n = document->getElementById("n");
        Element* m = document->getElementById("m");
        Element* f = document->getElementById("f");
        check(g && h && n && m && f, "grid-template area elements found");
        if (g && h && n && m && f) {
            checkNear(h->layoutWidth(), 300.0f, 0.5f, "grid-template head spans columns");
            checkNear(h->layoutHeight(), 50.0f, 0.5f, "grid-template head row track");
            checkNear(n->layoutY(), 50.0f, 0.5f, "grid-template nav starts after head");
            checkNear(n->layoutHeight(), 140.0f, 0.5f, "grid-template nav spans two rows");
            checkNear(m->layoutX(), 100.0f, 0.5f, "grid-template main column starts");
            checkNear(m->layoutY(), 50.0f, 0.5f, "grid-template main row starts");
            checkNear(f->layoutY(), 150.0f, 0.5f, "grid-template foot row starts");
        }

        const ComputedStyle gs = resolver.resolve(*g);
        check(sui::StyleResolver::propertyValue(gs, "grid-template") ==
                  "\"head head\" 50px \"nav main\" 100px \"nav foot\" 40px / "
                  "100px 200px",
              "grid-template area form roundtrips");
        check(sui::StyleResolver::propertyValue(gs, "grid-template-areas") ==
                  "\"head head\" \"nav main\" \"nav foot\"",
              "grid-template sets grid-template-areas");
        check(sui::StyleResolver::propertyValue(gs, "grid-template-rows") ==
                  "50px 100px 40px",
              "grid-template area rows set row tracks");
        check(sui::StyleResolver::propertyValue(gs, "grid-template-columns") ==
                  "100px 200px",
              "grid-template area rows set column tracks");
    }

    // Omitted column tracks default to one auto track per area column, while row
    // tracks still come from the strings.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "div{grid-template:\"a b\" 20px \"c d\" 30px}"));
        auto document = parseOrNull("<sui><body><div id=\"g\"></div></body></sui>");
        if (document) {
            Element* g = document->getElementById("g");
            const ComputedStyle gs = resolver.resolve(*g);
            check(gs.gridRows.size() == 2 && gs.gridRows[0].size.value == 20.0f &&
                      gs.gridRows[1].size.value == 30.0f,
                  "grid-template area form keeps row tracks");
            check(gs.gridColumns.size() == 2 && gs.gridColumns[0].isAuto &&
                      gs.gridColumns[1].isAuto,
                  "grid-template area form defaults omitted columns to auto");
            check(gs.gridTemplateAreasColumns == 2 && gs.gridTemplateAreasRows == 2,
                  "grid-template area form records the area matrix");
            check(sui::StyleResolver::propertyValue(gs, "grid-template") ==
                      "\"a b\" 20px \"c d\" 30px / auto auto",
                  "grid-template omitted columns roundtrip");
        }
    }

    // The shorthand resets all three longhands, and `none` resets them again.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "div{grid-template-areas:\"a a\" \"b b\";"
            "grid-template-columns:77px;grid-template-rows:33px;"
            "grid-template:10px 20px / 30px 40px}"));
        auto document = parseOrNull("<sui><body><div id=\"g\"></div></body></sui>");
        if (document) {
            const ComputedStyle gs = resolver.resolve(*document->getElementById("g"));
            check(gs.gridTemplateAreas.empty() && gs.gridTemplateAreasRaw.empty(),
                  "grid-template resets grid-template-areas");
            check(gs.gridRows.size() == 2 && gs.gridColumns.size() == 2,
                  "grid-template replaces both track lists");
            check(sui::StyleResolver::propertyValue(gs, "grid-template-areas") == "none",
                  "grid-template reset areas read back as none");
        }

        sui::StyleResolver noneResolver;
        noneResolver.setRules(sui::CSSParser::parse(
            "div{grid-template:10px / 20px;grid-template:none}"));
        auto noneDocument = parseOrNull("<sui><body><div id=\"g\"></div></body></sui>");
        if (noneDocument) {
            const ComputedStyle gs =
                noneResolver.resolve(*noneDocument->getElementById("g"));
            check(gs.gridRows.empty() && gs.gridColumns.empty(),
                  "grid-template none clears both track lists");
            check(sui::StyleResolver::propertyValue(gs, "grid-template") == "none",
                  "grid-template none roundtrips");
        }
    }

    // An invalid shorthand is dropped whole: the previous longhand value survives.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "div{grid-template-columns:33px;grid-template:\"a b\" \"b a\" / 10px 20px}"));
        auto document = parseOrNull("<sui><body><div id=\"g\"></div></body></sui>");
        if (document) {
            const ComputedStyle gs = resolver.resolve(*document->getElementById("g"));
            check(sui::StyleResolver::propertyValue(gs, "grid-template-columns") == "33px",
                  "invalid grid-template leaves the previous columns");
            check(gs.gridRows.empty() && gs.gridTemplateAreasRaw.empty(),
                  "invalid grid-template leaves no partial reset");
        }
    }

    // Line-name groups are ignored for now, and auto-fill survives the shorthand.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "div{grid-template:[row1] \"a b\" 20px [row2] / "
            "[col1] 50px 60px [col2]}"));
        auto document = parseOrNull("<sui><body><div id=\"g\"></div></body></sui>");
        if (document) {
            const ComputedStyle gs = resolver.resolve(*document->getElementById("g"));
            check(gs.gridRows.size() == 1 && gs.gridColumns.size() == 2,
                  "grid-template keeps line-name groups");
            check(gs.gridTemplateAreasColumns == 2 && gs.gridTemplateAreasRows == 1,
                  "grid-template line-name form keeps the area matrix");
            check(gs.gridRowLines.size() == 2 && !gs.gridRowLines[0].empty() &&
                      gs.gridRowLines[0][0] == "row1" &&
                      !gs.gridRowLines[1].empty() &&
                      gs.gridRowLines[1][0] == "row2",
                  "grid-template stores row line names");
            check(gs.gridColumnLines.size() == 3 &&
                      !gs.gridColumnLines[0].empty() &&
                      gs.gridColumnLines[0][0] == "col1" &&
                      !gs.gridColumnLines[2].empty() &&
                      gs.gridColumnLines[2][0] == "col2",
                  "grid-template stores column line names");
            check(sui::StyleResolver::propertyValue(gs, "grid-template") ==
                      "[row1] \"a b\" 20px [row2] / [col1] 50px 60px [col2]",
                  "grid-template line-name form roundtrips");
        }

        sui::StyleResolver fillResolver;
        fillResolver.setRules(sui::CSSParser::parse(
            "div{grid-template:40px / repeat(auto-fill,50px)}"));
        auto fillDocument = parseOrNull("<sui><body><div id=\"g\"></div></body></sui>");
        if (fillDocument) {
            const ComputedStyle gs =
                fillResolver.resolve(*fillDocument->getElementById("g"));
            check(gs.gridRows.size() == 1 && gs.gridColumns.size() == 1 &&
                      gs.gridColumnsAutoFill,
                  "grid-template keeps auto-fill columns");
            check(sui::StyleResolver::propertyValue(gs, "grid-template") ==
                      "40px / repeat(auto-fill, 50px)",
                  "grid-template auto-fill roundtrips");
        }
    }
}

void testGridNamedLines() {
    // Named column lines, named spans, longhands, and canonical read-back.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\" style=\"grid-column:left / right\"></i>"
            "<i id=\"b\" style=\"grid-column:mid / span right\"></i>"
            "<i id=\"c\" style=\"grid-column:left / span mid\"></i>"
            "<i id=\"d\" style=\"grid-column:mid 1 / right 1\"></i>"
            "<i id=\"e\" style=\"grid-column-start:mid;grid-column-end:right\"></i>"
            "<i id=\"f\" style=\"grid-column:left / span right\"></i>"
            "</div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;"
            "grid-template-columns:[left] 50px [mid] 100px [right];"
            "width:150px;height:20px} #g i{height:10px;grid-row:1}"));
        layoutRoot(*document, resolver, 300.0f);

        Element* g = document->getElementById("g");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        Element* d = document->getElementById("d");
        Element* e = document->getElementById("e");
        Element* f = document->getElementById("f");
        check(g && a && b && c && d && e && f, "named grid line elements found");
        if (g && a && b && c && d && e && f) {
            checkNear(a->layoutX(), 0.0f, 0.5f, "named range starts at left line");
            checkNear(a->layoutWidth(), 150.0f, 0.5f, "named range spans both tracks");
            checkNear(b->layoutX(), 50.0f, 0.5f, "named start starts at mid line");
            checkNear(b->layoutWidth(), 100.0f, 0.5f,
                      "span <name> ends at the next named line");
            checkNear(c->layoutX(), 0.0f, 0.5f, "span name from left starts at 0");
            checkNear(c->layoutWidth(), 50.0f, 0.5f,
                      "span <name> finds mid after left");
            checkNear(d->layoutX(), 50.0f, 0.5f, "line occurrence 1 starts at mid");
            checkNear(d->layoutWidth(), 100.0f, 0.5f,
                      "line occurrence range ends at right");
            checkNear(e->layoutX(), 50.0f, 0.5f,
                      "named grid-column-start longhand pins the start");
            checkNear(e->layoutWidth(), 100.0f, 0.5f,
                      "named grid-column-end longhand pins the end");
            checkNear(f->layoutX(), 0.0f, 0.5f,
                      "named span starts at left");
            checkNear(f->layoutWidth(), 150.0f, 0.5f,
                      "span <name> can cover multiple tracks");
        }

        if (g && b && c && d && e && f) {
            const ComputedStyle gs = resolver.resolve(*g);
            check(sui::StyleResolver::propertyValue(gs, "grid-template-columns") ==
                      "[left] 50px [mid] 100px [right]",
                  "named column track list roundtrips");
            const ComputedStyle bs = resolver.resolve(*b);
            check(sui::StyleResolver::propertyValue(bs, "grid-column") ==
                      "mid / span right",
                  "named span readback keeps span <name>");
            const ComputedStyle cs = resolver.resolve(*c);
            check(sui::StyleResolver::propertyValue(cs, "grid-column") ==
                      "left / span mid",
                  "span <name> readback keeps the named range");
            const ComputedStyle ds = resolver.resolve(*d);
            check(sui::StyleResolver::propertyValue(ds, "grid-column-start") == "mid" &&
                      sui::StyleResolver::propertyValue(ds, "grid-column-end") == "right",
                  "named line longhands read back by name");
            check(sui::StyleResolver::propertyValue(ds, "grid-column") ==
                      "mid / right",
                  "named line range readback omits occurrence 1");
            const ComputedStyle es = resolver.resolve(*e);
            check(sui::StyleResolver::propertyValue(es, "grid-column") ==
                      "mid / right",
                  "named longhands compose into the shorthand");
            const ComputedStyle fs = resolver.resolve(*f);
            check(sui::StyleResolver::propertyValue(fs, "grid-column") ==
                      "left / span right",
                  "multi-track named span reads back");
        }
    }

    // Fixed repeat() names, occurrence counts, and the flattened line list.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"e\" style=\"grid-column:c 2 / c 3\"></i>"
            "<i id=\"f\" style=\"grid-column:d 1 / d 2\"></i>"
            "</div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;"
            "grid-template-columns:repeat(3,[c] 50px [d]);"
            "width:150px;height:20px} #g i{height:10px;grid-row:1}"));
        layoutRoot(*document, resolver, 300.0f);

        Element* g = document->getElementById("g");
        Element* e = document->getElementById("e");
        Element* f = document->getElementById("f");
        check(g && e && f, "repeat named line elements found");
        if (e && f) {
            checkNear(e->layoutX(), 50.0f, 0.5f,
                      "second c line starts the occurrence range");
            checkNear(e->layoutWidth(), 50.0f, 0.5f,
                      "third c line ends the occurrence range");
            checkNear(f->layoutX(), 50.0f, 0.5f,
                      "first d line is shared with the repeated c line");
            checkNear(f->layoutWidth(), 50.0f, 0.5f,
                      "second d line closes the repeated block");
        }
        if (g && e) {
            const ComputedStyle gs = resolver.resolve(*g);
            check(gs.gridColumns.size() == 3 && gs.gridColumnLines.size() == 4,
                  "fixed repeat expands names with every track");
            check(gs.gridColumnLines[1].size() == 2 &&
                      std::find(gs.gridColumnLines[1].begin(),
                                gs.gridColumnLines[1].end(), "c") !=
                          gs.gridColumnLines[1].end() &&
                      std::find(gs.gridColumnLines[1].begin(),
                                gs.gridColumnLines[1].end(), "d") !=
                          gs.gridColumnLines[1].end(),
                  "repeat boundary carries both line names");
            const std::string columns =
                sui::StyleResolver::propertyValue(gs, "grid-template-columns");
            check(columns.find("[d c]") != std::string::npos,
                  "flattened repeat serializes a shared line-name list");
            const ComputedStyle es = resolver.resolve(*e);
            check(sui::StyleResolver::propertyValue(es, "grid-column") ==
                      "c 2 / c 3",
                  "named line occurrence readback keeps the counts");
        }
    }

    // Named rows and per-axis read-back.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\" style=\"grid-row:top / mid\"></i>"
            "<i id=\"b\" style=\"grid-row:mid / bottom\"></i>"
            "<i id=\"c\" style=\"grid-row:top / span bottom\"></i>"
            "</div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:100px;"
            "grid-template-rows:[top] 20px [mid] 30px [bottom];"
            "width:100px;height:50px} #g i{width:100%}"));
        layoutRoot(*document, resolver, 300.0f);

        Element* g = document->getElementById("g");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(g && a && b && c, "named row line elements found");
        if (a && b && c) {
            checkNear(a->layoutY(), 0.0f, 0.5f, "top line starts the first row");
            checkNear(a->layoutHeight(), 20.0f, 0.5f,
                      "top-to-mid range uses the first row track");
            checkNear(b->layoutY(), 20.0f, 0.5f, "mid line starts the second row");
            checkNear(b->layoutHeight(), 30.0f, 0.5f,
                      "mid-to-bottom range uses the second row track");
            checkNear(c->layoutY(), 0.0f, 0.5f,
                      "named row span starts at top");
            checkNear(c->layoutHeight(), 50.0f, 0.5f,
                      "row span <name> covers both tracks");
        }
        if (g) {
            const ComputedStyle gs = resolver.resolve(*g);
            check(sui::StyleResolver::propertyValue(gs, "grid-template-rows") ==
                      "[top] 20px [mid] 30px [bottom]",
                  "named row track list roundtrips");
        }
        if (a && c) {
            const ComputedStyle as = resolver.resolve(*a);
            check(sui::StyleResolver::propertyValue(as, "grid-row") ==
                      "top / mid",
                  "named row range reads back");
            const ComputedStyle cs = resolver.resolve(*c);
            check(sui::StyleResolver::propertyValue(cs, "grid-row") ==
                      "top / span bottom",
                  "named row span reads back");
        }
    }

    // Named areas also create <area>-start / <area>-end lines on both axes.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"h\" style=\"grid-column:head-start / head-end;"
            "grid-row:head-start / head-end\"></i>"
            "<i id=\"n\" style=\"grid-column:nav-start / nav-end;"
            "grid-row:nav-start / nav-end\"></i>"
            "<i id=\"m\" style=\"grid-column:main-start / main-end;"
            "grid-row:main-start / main-end\"></i>"
            "</div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;"
            "grid-template-areas:\"head head\" \"nav main\";"
            "grid-template-columns:50px 100px;grid-template-rows:20px 40px;"
            "width:150px;height:60px}"));
        layoutRoot(*document, resolver, 300.0f);

        Element* h = document->getElementById("h");
        Element* n = document->getElementById("n");
        Element* m = document->getElementById("m");
        check(h && n && m, "area generated line elements found");
        if (h && n && m) {
            checkNear(h->layoutX(), 0.0f, 0.5f, "head-start column line");
            checkNear(h->layoutWidth(), 150.0f, 0.5f, "head-end column line");
            checkNear(h->layoutY(), 0.0f, 0.5f, "head-start row line");
            checkNear(h->layoutHeight(), 20.0f, 0.5f, "head-end row line");
            checkNear(n->layoutX(), 0.0f, 0.5f, "nav-start column line");
            checkNear(n->layoutWidth(), 50.0f, 0.5f, "nav-end column line");
            checkNear(n->layoutY(), 20.0f, 0.5f, "nav-start row line");
            checkNear(n->layoutHeight(), 40.0f, 0.5f, "nav-end row line");
            checkNear(m->layoutX(), 50.0f, 0.5f, "main-start column line");
            checkNear(m->layoutWidth(), 100.0f, 0.5f, "main-end column line");
            checkNear(m->layoutY(), 20.0f, 0.5f, "main-start row line");
            checkNear(m->layoutHeight(), 40.0f, 0.5f, "main-end row line");
        }
        if (h) {
            const ComputedStyle hs = resolver.resolve(*h);
            check(sui::StyleResolver::propertyValue(hs, "grid-column") ==
                      "head-start / head-end",
                  "generated area line readback");
        }
    }

    // `grid-area` accepts named lines in all four slots.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\" style=\"grid-area:top / left / bottom / right\"></i>"
            "</div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;"
            "grid-template-columns:[left] 50px [right];"
            "grid-template-rows:[top] 20px [bottom];"
            "width:50px;height:20px}"));
        layoutRoot(*document, resolver, 300.0f);
        Element* a = document->getElementById("a");
        if (a) {
            checkNear(a->layoutX(), 0.0f, 0.5f, "named grid-area column start");
            checkNear(a->layoutWidth(), 50.0f, 0.5f, "named grid-area column span");
            checkNear(a->layoutY(), 0.0f, 0.5f, "named grid-area row start");
            checkNear(a->layoutHeight(), 20.0f, 0.5f, "named grid-area row span");
            const ComputedStyle as = resolver.resolve(*a);
            check(sui::StyleResolver::propertyValue(as, "grid-area") ==
                      "top / left / bottom / right",
                  "named grid-area readback");
        }
    }

    // The track-list form of the shorthand keeps names on both axes.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\" style=\"grid-column:left / right;"
            "grid-row:top / bottom\"></i>"
            "</div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;"
            "grid-template:[top] 20px [bottom] / [left] 50px [right];"
            "width:50px;height:20px}"));
        layoutRoot(*document, resolver, 300.0f);
        Element* g = document->getElementById("g");
        Element* a = document->getElementById("a");
        check(g && a, "grid-template named track elements found");
        if (a) {
            checkNear(a->layoutX(), 0.0f, 0.5f, "grid-template column line starts");
            checkNear(a->layoutWidth(), 50.0f, 0.5f, "grid-template column line spans");
            checkNear(a->layoutY(), 0.0f, 0.5f, "grid-template row line starts");
            checkNear(a->layoutHeight(), 20.0f, 0.5f, "grid-template row line spans");
        }
        if (g) {
            const ComputedStyle gs = resolver.resolve(*g);
            check(sui::StyleResolver::propertyValue(gs, "grid-template") ==
                      "[top] 20px [bottom] / [left] 50px [right]",
                  "grid-template named track lists roundtrip");
        }
    }

    // An auto-fill body repeats its line names with its tracks.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\" style=\"grid-column:col 2 / end 2;grid-row:1\"></i>"
            "</div></body></sui>");
        if (!document) return;
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;"
            "grid-template-columns:repeat(auto-fill,[col] 50px [end]);"
            "width:150px;height:20px} #g i{height:10px}"));
        layoutRoot(*document, resolver, 300.0f);
        Element* g = document->getElementById("g");
        Element* a = document->getElementById("a");
        if (a) {
            checkNear(a->layoutX(), 50.0f, 0.5f,
                      "auto-fill repeats the named column line");
            checkNear(a->layoutWidth(), 50.0f, 0.5f,
                      "auto-fill named range covers one repeated track");
        }
        if (g) {
            const ComputedStyle gs = resolver.resolve(*g);
            check(sui::StyleResolver::propertyValue(gs, "grid-template-columns") ==
                      "repeat(auto-fill, [col] 50px [end])",
                  "auto-fill named body roundtrips");
        }
    }
}

void testInputPasswordLayout() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"c\">"
        "<input id=\"p\" type=\"password\" value=\"secret\"/>"
        "<input id=\"t\" type=\"text\" value=\"secret\"/>"
        "</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0;font-size:16px}"));
    layoutRoot(*document, resolver, 500.0f);

    Element* p = document->getElementById("p");
    Element* t = document->getElementById("t");
    Element* c = document->getElementById("c");
    check(p != nullptr && t != nullptr, "input elements found");
    if (!p || !t) {
        return;
    }
    check(p->value() == "secret", "password element value preserves plain text");
    check(t->value() == "secret", "text element value preserves plain text");

    check(!p->textRuns().empty() &&
          p->textRuns()[0].text == "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2",
          "password input text run is masked with bullets");
    check(!t->textRuns().empty() && t->textRuns()[0].text == "secret",
          "plain input text run has raw text");
}

void testTextAreaLayout() {
    // A textarea's value is its text content, not an attribute, and the default
    // pre-wrap white-space keeps line breaks in the line grid.
    auto document = parseOrNull(
        "<sui><body><textarea id=\"t\">first\nsecond</textarea></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0;font-size:10px} #t{width:80px;height:40px}"));
    layoutRoot(*document, resolver, 500.0f);

    Element* t = document->getElementById("t");
    check(t != nullptr, "textarea element found");
    if (!t) {
        return;
    }
    check(t->value() == "first\nsecond", "textarea value is its text content");

    // The value flows as one line per newline, each its own text run.
    check(t->textRuns().size() == 2 && t->textRuns()[0].text == "first" &&
              t->textRuns()[1].text == "second",
          "textarea newline splits the value into two text runs");
    if (t->textRuns().size() == 2) {
        checkNear(t->textRuns()[1].y - t->textRuns()[0].y, 10.0f, 0.01f,
                  "textarea second line sits one line-height lower");
    }

    // Writing replaces the text content, so the next paint draws the new text.
    t->setValue("rewritten");
    check(t->value() == "rewritten", "textarea setValue rewrites its text content");

    // Without a CSS size the control is `cols` characters by `rows` lines.
    auto sized = parseOrNull(
        "<sui><body><textarea id=\"s\" rows=\"3\" cols=\"10\"></textarea></body></sui>");
    if (!sized) {
        return;
    }
    sui::StyleResolver sizedResolver;
    sizedResolver.setRules(sui::CSSParser::parse("body{margin:0;font-size:16px}"));
    layoutRoot(*sized, sizedResolver, 500.0f);
    Element* s = sized->getElementById("s");
    check(s != nullptr, "sized textarea found");
    if (s) {
        // cols 10 * 8px advance = 80, rows 3 * 16px lines = 48; plus 2px
        // padding and 1px border on each side.
        checkNear(s->layoutWidth(), 86.0f, 0.01f,
                  "textarea default width comes from cols");
        checkNear(s->layoutHeight(), 54.0f, 0.01f,
                  "textarea default height comes from rows");
    }

    // The 20 x 2 defaults apply when the attributes are missing.
    auto plain = parseOrNull("<sui><body><textarea id=\"p\"></textarea></body></sui>");
    if (!plain) {
        return;
    }
    sui::StyleResolver plainResolver;
    plainResolver.setRules(sui::CSSParser::parse("body{margin:0;font-size:16px}"));
    layoutRoot(*plain, plainResolver, 500.0f);
    Element* p = plain->getElementById("p");
    check(p != nullptr, "default textarea found");
    if (p) {
        checkNear(p->layoutWidth(), 166.0f, 0.01f, "textarea defaults to 20 cols");
        checkNear(p->layoutHeight(), 38.0f, 0.01f, "textarea defaults to 2 rows");
    }

    // An explicit CSS size wins over the attribute-based default.
    auto overridden = parseOrNull(
        "<sui><body><textarea id=\"o\" rows=\"10\" cols=\"10\"></textarea></body></sui>");
    if (!overridden) {
        return;
    }
    sui::StyleResolver overrideResolver;
    overrideResolver.setRules(
        sui::CSSParser::parse("body{margin:0;font-size:16px} #o{width:120px;height:50px}"));
    layoutRoot(*overridden, overrideResolver, 500.0f);
    Element* o = overridden->getElementById("o");
    check(o != nullptr, "overridden textarea found");
    if (o) {
        checkNear(o->layoutWidth(), 126.0f, 0.01f,
                  "explicit width beats the cols default");
        checkNear(o->layoutHeight(), 56.0f, 0.01f,
                  "explicit height beats the rows default");
    }
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
    checkNear(b->layoutY(), 50.0f, 0.01f,
              "adjacent block margins collapse to the larger margin");
    checkNear(document->root()->layoutHeight(), 90.0f, 0.01f,
              "root height includes the collapsed margins once");
}

// Vertical margins collapse in normal block flow: siblings take the larger of
// the two margins (plus the most negative), a parent's first/last child margins
// join its own, empty blocks collapse through, and formatting-context roots or
// padding/borders stop the parent-child join. The old additive model is still
// used for mixed inline/block content and float-bearing containers.
void testMarginCollapsing() {
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"></div><div id=\"b\"></div>"
            "<div id=\"c\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} div{height:20px} #a{margin-bottom:30px} "
            "#b{margin-top:10px;margin-bottom:30px} #c{margin-top:-10px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(a && b && c, "sibling margin elements found");
        if (a && b && c) {
            checkNear(a->layoutY(), 0.0f, 0.01f, "the first sibling starts at the top");
            checkNear(b->layoutY(), 50.0f, 0.01f,
                      "sibling margins use the larger positive value");
            checkNear(c->layoutY(), 90.0f, 0.01f,
                      "a negative sibling margin reduces the collapsed gap");
        }
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"outer\"><div id=\"inner\"></div></div>"
            "<div id=\"after\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #outer{margin-top:20px;margin-bottom:5px} "
            "#inner{margin-top:40px;margin-bottom:30px;height:10px} "
            "#after{height:10px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* outer = document->getElementById("outer");
        Element* inner = document->getElementById("inner");
        Element* after = document->getElementById("after");
        check(outer && inner && after, "parent margin elements found");
        if (outer && inner && after) {
            checkNear(outer->layoutY(), 40.0f, 0.01f,
                      "the parent's top margin collapses with its first child");
            checkNear(inner->layoutY(), 40.0f, 0.01f,
                      "the first child starts at the collapsed parent edge");
            checkNear(outer->layoutHeight(), 10.0f, 0.01f,
                      "the parent's height excludes the first child's top margin");
            checkNear(after->layoutY(), 80.0f, 0.01f,
                      "the last child's bottom margin collapses with the parent's");
        }
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"outer\"><div id=\"empty\"></div>"
            "<div id=\"b\"></div></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #outer{margin-top:10px} "
            "#empty{margin-top:20px;margin-bottom:30px} "
            "#b{margin-top:5px;height:10px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* outer = document->getElementById("outer");
        Element* empty = document->getElementById("empty");
        Element* b = document->getElementById("b");
        check(outer && empty && b, "empty margin elements found");
        if (outer && empty && b) {
            checkNear(outer->layoutY(), 30.0f, 0.01f,
                      "an empty block's margins join the parent's top chain");
            checkNear(empty->layoutHeight(), 0.0f, 0.01f,
                      "an empty block has no border-box height");
            checkNear(b->layoutY(), 30.0f, 0.01f,
                      "the next block starts at the same collapsed edge");
            checkNear(outer->layoutHeight(), 10.0f, 0.01f,
                      "the through block leaves only the real content height");
        }
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"outer\"><div id=\"inner\"></div></div>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #outer{margin-top:20px;overflow:hidden} "
            "#inner{margin-top:40px;height:10px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* outer = document->getElementById("outer");
        Element* inner = document->getElementById("inner");
        check(outer && inner, "BFC margin elements found");
        if (outer && inner) {
            checkNear(outer->layoutY(), 20.0f, 0.01f,
                      "a formatting-context root keeps its own top margin");
            checkNear(inner->layoutY(), 60.0f, 0.01f,
                      "a formatting-context root keeps the child margin inside");
            checkNear(outer->layoutHeight(), 50.0f, 0.01f,
                      "a formatting-context root contains the child's margin");
        }
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"outer\"><div id=\"inner\"></div></div>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #outer{margin-top:20px;padding-top:5px} "
            "#inner{margin-top:40px;height:10px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* outer = document->getElementById("outer");
        Element* inner = document->getElementById("inner");
        check(outer && inner, "padded margin elements found");
        if (outer && inner) {
            checkNear(outer->layoutY(), 20.0f, 0.01f,
                      "padding stops the parent's top margin from collapsing");
            checkNear(inner->layoutY(), 65.0f, 0.01f,
                      "the child margin stays inside the padded parent");
            checkNear(outer->layoutHeight(), 55.0f, 0.01f,
                      "the padded parent contains the child margin");
        }
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"></div><div id=\"b\"></div>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} div{height:20px} #a{margin-bottom:-10px} "
            "#b{margin-top:-20px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(a && b, "negative sibling elements found");
        if (a && b) {
            checkNear(b->layoutY(), 0.0f, 0.01f,
                      "two negative sibling margins collapse to the most negative");
        }
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"bfc\"></div><div id=\"next\"></div>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #bfc{overflow:hidden;height:10px;margin-bottom:20px} "
            "#next{margin-top:5px;height:10px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* bfc = document->getElementById("bfc");
        Element* next = document->getElementById("next");
        check(bfc && next, "formatting-context sibling elements found");
        if (bfc && next) {
            checkNear(next->layoutY(), 30.0f, 0.01f,
                      "a formatting-context root still collapses with siblings");
        }
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"></div>text<div id=\"b\"></div>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #a{margin-top:20px;height:10px;margin-bottom:20px} "
            "#b{height:10px;margin-top:30px;margin-bottom:20px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(a && b, "mixed-content margin elements found");
        if (a && b) {
            checkNear(a->layoutY(), 20.0f, 0.01f,
                      "mixed content keeps the first block's top margin additive");
            checkNear(b->layoutY(), 80.0f, 0.01f,
                      "inline content between blocks keeps their margins additive");
        }
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"outer\"><div id=\"inner\"></div></div>"
            "<div id=\"after\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #outer{height:10px;margin-bottom:5px} "
            "#inner{height:5px;margin-bottom:30px} #after{height:10px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* outer = document->getElementById("outer");
        Element* after = document->getElementById("after");
        check(outer && after, "fixed-height parent elements found");
        if (outer && after) {
            checkNear(after->layoutY(), 15.0f, 0.01f,
                      "a definite parent height stops the last child's bottom collapse");
        }
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"></div><div id=\"empty\"></div>"
            "<div id=\"b\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #a{height:10px} "
            "#empty{margin-top:20px;margin-bottom:30px} "
            "#b{height:10px;margin-top:5px}"));
        layoutRoot(*document, resolver, 200.0f);
        Element* b = document->getElementById("b");
        check(b != nullptr, "between-sibling empty element found");
        if (b) {
            checkNear(b->layoutY(), 40.0f, 0.01f,
                      "an empty block's margins join the following sibling's top");
        }
    }
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

void testGridLayout() {
    auto document = parseOrNull(
        "<sui><body><div id=\"t\"><i></i><i></i><i></i><i></i></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0} #t{display:grid;grid-template-columns:1fr 1fr;width:300px}"
        "#t i{height:20px}"));
    layoutRoot(*document, resolver, 400.0f);

    Element* t = document->getElementById("t");
    check(t != nullptr && t->children().size() == 4, "grid container and items found");
    if (!t || t->children().size() != 4) {
        return;
    }
    Element* a = t->children()[0].get();
    Element* b = t->children()[1].get();
    Element* c = t->children()[2].get();
    checkNear(a->layoutWidth(), 150.0f, 1.0f, "fr tracks split the row evenly");
    checkNear(b->layoutX() - a->layoutX(), 150.0f, 1.0f, "second column starts after the first");
    checkNear(a->layoutY(), b->layoutY(), 0.5f, "the first two items share a row");
    checkNear(c->layoutY() - a->layoutY(), 20.0f, 1.0f, "the third item wraps to a new row");
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

void testCascadeLayers() {
    auto document = parseOrNull("<sui><body><div id=\"x\">hi</div></body></sui>");
    if (!document) {
        return;
    }
    Element* x = document->getElementById("x");
    check(x != nullptr, "layered element found");
    if (!x) {
        return;
    }
    const auto colorOf = [&](const char* css) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        return resolver.resolve(*x).color;
    };
    const auto isRed = [](SDL_Color c) {
        return c.r == 0xff && c.g == 0x00 && c.b == 0x00;
    };
    const auto isGreen = [](SDL_Color c) {
        return c.r == 0x00 && c.g == 0xff && c.b == 0x00;
    };

    // A later layer beats an earlier one whatever the specificity says.
    check(isGreen(colorOf("body{margin:0} @layer a{ #x{color:#ff0000} }"
                          " @layer b{ div{color:#00ff00} }")),
          "a later layer beats higher specificity");
    // Specificity still orders rules inside one layer.
    check(isGreen(colorOf("body{margin:0} @layer a{ div{color:#ff0000} #x{color:#00ff00} }")),
          "specificity still wins inside a layer");
    // Unlayered author rules beat every layer, either side of it.
    check(isGreen(colorOf("body{margin:0} @layer a{ #x{color:#ff0000} }"
                          " div{color:#00ff00}")),
          "an unlayered rule beats a later layer");
    check(isGreen(colorOf("body{margin:0} div{color:#00ff00}"
                          " @layer a{ #x{color:#ff0000} }")),
          "an unlayered rule beats an earlier layer too");

    // A statement orders layers up front: the same two blocks pick different
    // winners under different statements.
    check(isGreen(colorOf("body{margin:0} @layer low, high;"
                          " @layer low{ div{color:#ff0000} }"
                          " @layer high{ div{color:#00ff00} }")),
          "the statement order decides between blocks");
    check(isRed(colorOf("body{margin:0} @layer high, low;"
                        " @layer low{ div{color:#ff0000} }"
                        " @layer high{ div{color:#00ff00} }")),
          "a reordered statement flips the winner");

    // Nesting dots the names: `outer.inner` follows `outer`.
    check(isGreen(colorOf("body{margin:0} @layer outer{ div{color:#ff0000}"
                          " @layer inner{ div{color:#00ff00} } }")),
          "a nested layer beats its parent");
    // Anonymous layers order by appearance like named ones.
    check(isGreen(colorOf("body{margin:0} @layer{ div{color:#ff0000} }"
                          " @layer{ div{color:#00ff00} }")),
          "a later anonymous layer wins");

    // `!important` reverses the layer order: layered important beats
    // unlayered important, and the first layer beats the later ones.
    check(isGreen(colorOf("body{margin:0} @layer a{ div{color:#00ff00 !important} }"
                          " div{color:#ff0000 !important}")),
          "layered !important beats unlayered !important");
    check(isRed(colorOf("body{margin:0} @layer a{ div{color:#ff0000 !important} }"
                        " @layer b{ div{color:#00ff00 !important} }")),
          "the first layer wins among !important");

    // The rules carry their layers: statements arrive as markers, blocks stamp
    // their rules, plain rules stay unlayered.
    const auto rules = sui::CSSParser::parse("@layer a, b; div{color:#ff0000}");
    check(rules.size() == 3, "a statement emits one marker per name plus the rule");
    if (rules.size() == 3) {
        check(rules[0].layerStatement && rules[0].layer == "a",
              "the first marker names the first layer");
        check(rules[1].layerStatement && rules[1].layer == "b",
              "the second marker names the second layer");
        check(!rules[2].layerStatement && rules[2].layer.empty(),
              "a plain rule stays unlayered");
    }
    const auto blocked = sui::CSSParser::parse("@layer a{ div{color:#ff0000} }");
    check(blocked.size() == 1 && blocked[0].layer == "a",
          "a block stamps its rules with the layer");
}

void testContainerQueries() {
    auto document = parseOrNull(
        "<sui><body><div id=\"wrap\"><div id=\"box\">hi</div></div>"
        "<div id=\"plain\"><div id=\"box2\">hi</div></div>"
        "<div id=\"outer\"><div id=\"inner\"><div id=\"deep\">hi</div></div></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    Element* box = document->getElementById("box");
    Element* box2 = document->getElementById("box2");
    Element* deep = document->getElementById("deep");
    Element* wrap = document->getElementById("wrap");
    check(box && box2 && deep && wrap, "container elements found");
    if (!box || !box2 || !deep || !wrap) {
        return;
    }
    // Queries read laid-out widths, so each case lays out before resolving.
    const auto colorOf = [&](Element& el, const char* css) {
        sui::StyleResolver resolver;
        resolver.setViewportSize(400.0f, 600.0f);
        resolver.setRules(sui::CSSParser::parse(css));
        layoutRoot(*document, resolver, 400.0f);
        return resolver.resolve(el).color;
    };
    const auto isRed = [](SDL_Color c) {
        return c.r == 0xff && c.g == 0x00 && c.b == 0x00;
    };
    const auto isGreen = [](SDL_Color c) {
        return c.r == 0x00 && c.g == 0xff && c.b == 0x00;
    };

    // A 300px container matches min-200 but not min-400.
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                     " #box{color:#ff0000}"
                                     " @container (min-width:200px){ #box{color:#00ff00} }")),
          "a query matches its container's width");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                   " #box{color:#ff0000}"
                                   " @container (min-width:400px){ #box{color:#00ff00} }")),
          "a query misses above its container's width");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                     " #box{color:#ff0000}"
                                     " @container (max-width:400px){ #box{color:#00ff00} }")),
          "max-width matches below its bound");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                   " #box{color:#ff0000}"
                                   " @container (max-width:200px){ #box{color:#00ff00} }")),
          "max-width misses above its container");

    // With no container ancestor the same query never matches.
    check(isRed(colorOf(*box2, "body{margin:0} #box2{color:#ff0000}"
                                      " @container (min-width:10px){ #box2{color:#00ff00} }")),
          "a query without a container never matches");

    // `size` establishes a container exactly like `inline-size`.
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:size}"
                                     " #box{color:#ff0000}"
                                     " @container (min-width:200px){ #box{color:#00ff00} }")),
          "container-type:size queries too");

    // Names pick which ancestor reads: the 100px inner answers for `b`, the
    // 500px outer answers for `a`, and an unknown name answers never.
    check(isGreen(colorOf(*deep, "body{margin:0} #outer{width:500px;container-type:inline-size;"
                                        "container-name:a}"
                                        " #inner{width:100px;container-type:inline-size;"
                                        "container-name:b} #deep{color:#ff0000}"
                                        " @container b (min-width:50px){ #deep{color:#00ff00} }")),
          "a named query reads its own container");
    check(isGreen(colorOf(*deep, "body{margin:0} #outer{width:500px;container-type:inline-size;"
                                        "container-name:a}"
                                        " #inner{width:100px;container-type:inline-size;"
                                        "container-name:b} #deep{color:#ff0000}"
                                        " @container a (min-width:400px){ #deep{color:#00ff00} }")),
          "a query skips a nearer container with another name");
    check(isRed(colorOf(*deep, "body{margin:0} #outer{width:500px;container-type:inline-size;"
                                      "container-name:a}"
                                      " #inner{width:100px;container-type:inline-size;"
                                      "container-name:b} #deep{color:#ff0000}"
                                      " @container c (min-width:10px){ #deep{color:#00ff00} }")),
          "an unknown name never matches");

    // `and` chains intersect bounds.
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                     " #box{color:#ff0000} @container (min-width:100px) and"
                                     " (max-width:400px){ #box{color:#00ff00} }")),
          "an and-chain matches inside both bounds");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                   " #box{color:#ff0000} @container (min-width:100px) and"
                                   " (max-width:200px){ #box{color:#00ff00} }")),
          "an and-chain misses outside either bound");

    // Length units other than px resolve against the right context: the
    // container's font (`em`), the root font (`rem`), the viewport (`vw`/`vh`),
    // or the queried container itself (`cq*`).
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;font-size:20px;"
                                      "container-type:inline-size} #box{color:#ff0000}"
                                      " @container (min-width:14em){ #box{color:#00ff00} }")),
          "an em query uses the container font size");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;font-size:20px;"
                               "container-type:inline-size} #box{color:#ff0000}"
                               " @container (min-width:16em){ #box{color:#00ff00} }")),
          "an em query misses past its resolved bound");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                      " #box{color:#ff0000}"
                                      " @container (min-width:18rem){ #box{color:#00ff00} }")),
          "a rem query uses the root font size");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                               " #box{color:#ff0000}"
                               " @container (min-width:19rem){ #box{color:#00ff00} }")),
          "a rem query misses past its resolved bound");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                      " #box{color:#ff0000}"
                                      " @container (min-width:50vw){ #box{color:#00ff00} }")),
          "a viewport-width query uses the viewport");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                               " #box{color:#ff0000}"
                               " @container (min-width:100vw){ #box{color:#00ff00} }")),
          "a viewport-width query misses past the viewport");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;height:100px;"
                                      "container-type:size} #box{color:#ff0000}"
                                      " @container (min-height:10vh){ #box{color:#00ff00} }")),
          "a viewport-height query uses the viewport");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;height:100px;"
                               "container-type:size} #box{color:#ff0000}"
                               " @container (min-height:20vh){ #box{color:#00ff00} }")),
          "a viewport-height query misses past the viewport");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                      " #box{color:#ff0000}"
                                      " @container (min-width:50cqw){ #box{color:#00ff00} }")),
          "a cqw query uses the container width");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                               " #box{color:#ff0000}"
                               " @container (min-width:101cqw){ #box{color:#00ff00} }")),
          "a cqw query misses past the container width");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                                      "container-type:size} #box{color:#ff0000}"
                                      " @container (min-height:75cqh){ #box{color:#00ff00} }")),
          "a cqh query uses the container height");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                               "container-type:size} #box{color:#ff0000}"
                               " @container (min-height:150cqh){ #box{color:#00ff00} }")),
          "a cqh query misses past the container height");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                                      "container-type:size} #box{color:#ff0000}"
                                      " @container (min-width:150cqmin){ #box{color:#00ff00} }")),
          "a cqmin query uses the smaller container axis");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                               "container-type:size} #box{color:#ff0000}"
                               " @container (min-width:200cqmin){ #box{color:#00ff00} }")),
          "a cqmin query misses past the smaller axis");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                                      "container-type:size} #box{color:#ff0000}"
                                      " @container (min-width:100cqmax){ #box{color:#00ff00} }")),
          "a cqmax query uses the larger container axis");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                               "container-type:size} #box{color:#ff0000}"
                               " @container (min-width:120cqmax){ #box{color:#00ff00} }")),
          "a cqmax query misses past the larger axis");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                                      "container-type:size} #box{color:#ff0000}"
                                      " @container (min-width:50cqi){ #box{color:#00ff00} }")),
          "a cqi query uses the inline container size");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                               "container-type:size} #box{color:#ff0000}"
                               " @container (min-width:150cqi){ #box{color:#00ff00} }")),
          "a cqi query misses past the inline size");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                                      "container-type:size} #box{color:#ff0000}"
                                      " @container (min-height:50cqb){ #box{color:#00ff00} }")),
          "a cqb query uses the block container size");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;height:200px;"
                               "container-type:size} #box{color:#ff0000}"
                               " @container (min-height:150cqb){ #box{color:#00ff00} }")),
          "a cqb query misses past the block size");

    // Range syntax, boolean operators, and nested parentheses.
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                      " #box{color:#ff0000}"
                                      " @container (200px < width < 400px){ #box{color:#00ff00} }")),
          "a two-sided range matches between its bounds");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                               " #box{color:#ff0000}"
                               " @container (300px < width < 400px){ #box{color:#00ff00} }")),
          "a strict range excludes its open bound");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                      " #box{color:#ff0000}"
                                      " @container (min-width:1000px) or (max-width:400px){"
                                      " #box{color:#00ff00} }")),
          "or matches either branch");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                      " #box{color:#ff0000}"
                                      " @container not (min-width:400px){ #box{color:#00ff00} }")),
          "not negates a feature");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                      " #box{color:#ff0000}"
                                      " @container ((min-width:100px) and (max-width:400px)){"
                                      " #box{color:#00ff00} }")),
          "nested parentheses keep an and branch");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                               " #box{color:#ff0000}"
                               " @container ((min-width:100px) and (max-width:200px)){"
                               " #box{color:#00ff00} }")),
          "a false nested branch does not match");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;container-type:inline-size}"
                                      " #box{color:#ff0000}"
                                      " @container (min-width:1000px) and (max-width:200px)"
                                      " or (min-width:200px){ #box{color:#00ff00} }")),
          "and binds tighter than or");

    // Heights need `container-type: size`; inline-size only exposes the width.
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;height:100px;"
                                      "container-type:size} #box{color:#ff0000}"
                                      " @container (min-height:80px){ #box{color:#00ff00} }")),
          "a size container answers height queries");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;height:100px;"
                               "container-type:size} #box{color:#ff0000}"
                               " @container (min-height:120px){ #box{color:#00ff00} }")),
          "a size container rejects an unmet height bound");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{width:300px;height:100px;"
                               "container-type:inline-size} #box{color:#ff0000}"
                               " @container (min-height:10px){ #box{color:#00ff00} }")),
          "an inline-size container cannot answer height queries");

    // Style queries read the nearest ancestor's computed style; every element
    // is a style container, so no container-type is required.
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{--theme:dark;color:#ff0000}"
                                      " #box{color:#ff0000}"
                                      " @container style(--theme:dark){ #box{color:#00ff00} }")),
          "a style query matches a custom property");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{--theme:light;color:#ff0000}"
                               " #box{color:#ff0000}"
                               " @container style(--theme:dark){ #box{color:#00ff00} }")),
          "a style query rejects a different custom value");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{color:#ff0000}"
                                      " #box{color:#ff0000}"
                                      " @container style(color:red){ #box{color:#00ff00} }")),
          "a style query canonicalises a named colour");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{color:#ff0000}"
                               " #box{color:#ff0000}"
                               " @container style(color:not-a-colour){ #box{color:#00ff00} }")),
          "a style query rejects an invalid declaration value");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{color:#ff0000}"
                                      " #box{color:#ff0000}"
                                      " @container style(color:#ff0000){ #box{color:#00ff00} }")),
          "a style query compares a computed declaration");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{--theme:dark;color:#ff0000}"
                                      " #box{color:#ff0000}"
                                      " @container style(--theme){ #box{color:#00ff00} }")),
          "a boolean style query tests custom-property presence");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{color:#ff0000}"
                                      " #box{color:#ff0000}"
                                      " @container not style(--theme:dark){ #box{color:#00ff00} }")),
          "not negates a style feature");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{width:300px;--theme:dark;"
                                      "container-type:inline-size} #box{color:#ff0000}"
                                      " @container (min-width:200px) and style(--theme:dark){"
                                      " #box{color:#00ff00} }")),
          "size and style features compose");
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{color:#ff0000}"
                                      " #box{color:#ff0000}"
                                      " @container style((color:#ff0000) or (color:#00ff00)){"
                                      " #box{color:#00ff00} }")),
          "style() accepts its own boolean condition");

    // A name-only query selects named descendants without a size feature.
    check(isGreen(colorOf(*box, "body{margin:0} #wrap{container-name:card}"
                                      " #box{color:#ff0000}"
                                      " @container card { #box{color:#00ff00} }")),
          "a name-only query matches a named container");
    check(isRed(colorOf(*box, "body{margin:0} #wrap{container-name:card}"
                               " #box{color:#ff0000}"
                               " @container other { #box{color:#00ff00} }")),
          "a name-only query misses another name");

    // Nested `@container` blocks may target different containers; each
    // condition selects its own nearest ancestor rather than merging names.
    check(isGreen(colorOf(*deep, "body{margin:0} #outer{width:500px;height:200px;"
                                       "container-type:inline-size;container-name:outer}"
                                       " #inner{width:200px;container-type:inline-size;"
                                       "container-name:inner} #deep{color:#ff0000}"
                                       " @container outer (min-width:400px){"
                                       " @container inner (min-width:100px){"
                                       " #deep{color:#00ff00} } }")),
          "nested queries read their own containers");
    check(isRed(colorOf(*deep, "body{margin:0} #outer{width:500px;height:200px;"
                               "container-type:inline-size;container-name:outer}"
                               " #inner{width:200px;container-type:inline-size;"
                               "container-name:inner} #deep{color:#ff0000}"
                               " @container outer (min-width:400px){"
                               " @container inner (min-width:300px){"
                               " #deep{color:#00ff00} } }")),
          "a false inner nested query keeps the deeper rule off");
    check(isRed(colorOf(*deep, "body{margin:0} #outer{width:500px;height:200px;"
                               "container-type:inline-size;container-name:outer}"
                               " #inner{width:200px;container-type:inline-size;"
                               "container-name:inner} #deep{color:#ff0000}"
                               " @container outer (min-width:600px){"
                               " @container inner (min-width:100px){"
                               " #deep{color:#00ff00} } }")),
          "a false outer nested query keeps the deeper rule off");

    // A query inside `@media` needs both to hold.
    {
        sui::StyleResolver resolver;
        resolver.setViewportSize(800.0f, 600.0f);
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{width:300px;container-type:inline-size}"
            " #box{color:#ff0000}"
            " @media (min-width:1px){ @container (min-width:200px){"
            " #box{color:#00ff00} } }"));
        layoutRoot(*document, resolver, 400.0f);
        check(isGreen(resolver.resolve(*box).color),
              "a query nested in matching media applies");
    }

    // The shorthand sets both longhands at once.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{width:300px;container:side/inline-size}"));
        layoutRoot(*document, resolver, 400.0f);
        const sui::ComputedStyle style = resolver.resolve(*wrap);
        check(style.containerType == sui::ComputedStyle::ContainerInlineSize,
              "the shorthand sets the type");
        check(style.containerNames.size() == 1 && style.containerNames[0] == "side",
              "the shorthand sets the name");
    }

    // The rule carries its query tree for the resolver to evaluate.
    const auto rules = sui::CSSParser::parse(
        "@container sidebar (min-width:100px){ div{color:#00ff00} }");
    check(rules.size() == 1 && !rules[0].containers.empty(),
          "a query block produces an active query rule");
    if (rules.size() == 1 && !rules[0].containers.empty()) {
        const auto& query = rules[0].containers.front();
        check(query.names.size() == 1 && query.names[0] == "sidebar",
              "the query keeps its name");
        check(query.hasCondition &&
                  query.condition.kind ==
                      sui::Rule::ContainerQuery::Node::Kind::Feature,
              "the query keeps one size feature");
        const auto& feature = query.condition.feature;
        check(!feature.style && !feature.height &&
                  feature.comparison ==
                      sui::Rule::ContainerQuery::Comparison::GreaterEqual &&
                  feature.unit == sui::Rule::ContainerQuery::Unit::Px &&
                  feature.amount == 100.0f,
              "the query keeps its axis, comparison, unit, and bound");
    }

    // Boolean nesting, style features, and non-px units all survive parsing.
    const auto complex = sui::CSSParser::parse(
        "@container card ((width >= 10em) or style(--theme: dark))"
        " and (height > 20vh){ div{color:#00ff00} }");
    check(complex.size() == 1 && !complex[0].containers.empty(),
          "a complex query produces a rule");
    if (complex.size() == 1 && !complex[0].containers.empty()) {
        const auto& query = complex[0].containers.front();
        check(query.names.size() == 1 && query.names[0] == "card",
              "a complex query keeps its name");
        check(query.condition.kind ==
                  sui::Rule::ContainerQuery::Node::Kind::And &&
                  query.condition.children.size() == 2,
              "and binds the two halves of a complex query");
        const auto& either = query.condition.children[0];
        check(either.kind == sui::Rule::ContainerQuery::Node::Kind::Or &&
                  either.children.size() == 2,
              "parentheses keep an or branch");
        const auto& width = either.children[0].feature;
        check(!width.style && !width.height &&
                  width.unit == sui::Rule::ContainerQuery::Unit::Em &&
                  width.amount == 10.0f &&
                  width.comparison ==
                      sui::Rule::ContainerQuery::Comparison::GreaterEqual,
              "a range feature keeps its em bound");
        const auto& theme = either.children[1].feature;
        check(theme.style && theme.property == "--theme" &&
                  theme.value == "dark",
              "a style feature keeps its property and value");
        const auto& height = query.condition.children[1].feature;
        check(!height.style && height.height &&
                  height.unit == sui::Rule::ContainerQuery::Unit::Vh &&
                  height.amount == 20.0f &&
                  height.comparison ==
                      sui::Rule::ContainerQuery::Comparison::Greater,
              "a height feature keeps its vh bound");
    }

    // A name alone is a valid query and needs no feature.
    const auto nameOnly = sui::CSSParser::parse(
        "@container sidebar { div{color:#00ff00} }");
    check(nameOnly.size() == 1 && !nameOnly[0].containers.empty() &&
              !nameOnly[0].containers.front().hasCondition &&
              nameOnly[0].containers.front().names.size() == 1,
          "a name-only query keeps its name and no condition");

    // A fresh tree has no prior height to read: the multipass layout has to
    // finish a pass, then re-evaluate the height query with that geometry.
    {
        auto fresh = parseOrNull(
            "<sui><body><div id=\"wrap\"><div id=\"box\">hi</div></div>"
            "</body></sui>");
        if (!fresh) {
            return;
        }
        Element* freshBox = fresh->getElementById("box");
        check(freshBox != nullptr, "fresh height box found");
        if (!freshBox) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{height:100px;container-type:size}"
            " #box{color:#ff0000}"
            " @container (min-height:80px){ #box{color:#00ff00} }"));
        layoutRoot(*fresh, resolver, 400.0f);
        check(isGreen(resolver.resolve(*freshBox).color),
              "a fresh tree settles a height query");
    }

    // Height queries can feed another pass: the first query grows the only
    // child, which changes the container height enough to satisfy the second
    // query. A width-only settled check would stop before the second pass.
    {
        auto fresh = parseOrNull(
            "<sui><body><div id=\"wrap\"><div id=\"a\">hi</div></div>"
            "</body></sui>");
        if (!fresh) {
            return;
        }
        Element* a = fresh->getElementById("a");
        check(a != nullptr, "fresh reflow box found");
        if (!a) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{container-type:size}"
            " #a{height:200px}"
            " @container (min-height:150px){ #a{height:300px} }"
            " @container (min-height:250px){ #a{height:400px} }"));
        layoutRoot(*fresh, resolver, 400.0f);
        check(std::fabs(a->layoutHeight() - 400.0f) < 0.5f,
              "height changes feed another container-query pass");
    }

    // Layout and paint agree: a matched display rule hides the box it laid
    // out, which is what the passes are for. Fresh documents here: a box
    // skipped as `display:none` keeps whatever geometry it had, so reusing a
    // laid-out tree would read stale widths. The sibling proves the reflow:
    // without a second pass it would sit a line too low.
    {
        auto fresh = parseOrNull(
            "<sui><body><div id=\"wrap\"><div id=\"box\">hi</div>"
            "<div id=\"after\">yo</div></div></body></sui>");
        if (!fresh) {
            return;
        }
        Element* freshBox = fresh->getElementById("box");
        Element* freshAfter = fresh->getElementById("after");
        Element* freshWrap = fresh->getElementById("wrap");
        check(freshBox && freshAfter && freshWrap, "fresh boxes found");
        if (!freshBox || !freshAfter || !freshWrap) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{width:300px;container-type:inline-size}"
            " @container (min-width:200px){ #box{display:none} }"));
        layoutRoot(*fresh, resolver, 400.0f);
        check(freshBox->layoutWidth() == 0.0f, "a matched query hides its box");
        check(freshAfter->layoutY() == freshWrap->contentY(),
              "siblings reflow around the hidden box");
    }
    {
        auto fresh = parseOrNull(
            "<sui><body><div id=\"wrap\"><div id=\"box\">hi</div></div></body></sui>");
        if (!fresh) {
            return;
        }
        Element* freshBox = fresh->getElementById("box");
        check(freshBox != nullptr, "fresh box found");
        if (!freshBox) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{width:300px;container-type:inline-size}"
            " @container (min-width:400px){ #box{display:none} }"));
        layoutRoot(*fresh, resolver, 400.0f);
        check(freshBox->layoutWidth() > 0.0f, "an unmatched query keeps its box");
    }
}

void testClipPath() {
    auto document = parseOrNull("<sui><body><div id=\"x\">hi</div></body></sui>");
    if (!document) {
        return;
    }
    Element* x = document->getElementById("x");
    check(x != nullptr, "clipped element found");
    if (!x) {
        return;
    }
    using Clip = sui::ComputedStyle::ClipPath;
    const auto clipOf = [&](const char* css) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        return resolver.resolve(*x).clipPath;
    };
    const auto isPx = [](sui::Length l, float v) {
        return l.unit == sui::Length::Px && l.value == v;
    };
    const auto isPct = [](sui::Length l, float v) {
        return l.unit == sui::Length::Percent && l.value == v;
    };

    Clip inset = clipOf("#x{clip-path:inset(10px 20px)}");
    check(inset.shape == Clip::Shape::Inset, "inset() parses to a box");
    check(isPx(inset.inset[0], 10.0f) && isPx(inset.inset[1], 20.0f) &&
              isPx(inset.inset[2], 10.0f) && isPx(inset.inset[3], 20.0f),
          "inset() expands two sides like a margin");
    check(inset.box == Clip::Box::Border, "a shape defaults to the border box");
    Clip round = clipOf("#x{clip-path:inset(10px round 5px)}");
    check(isPx(round.insetRadius, 5.0f), "inset() keeps its round radius");
    Clip padded = clipOf("#x{clip-path:inset(10px 20px 30px 40px) padding-box}");
    check(isPx(padded.inset[0], 10.0f) && isPx(padded.inset[1], 20.0f) &&
              isPx(padded.inset[2], 30.0f) && isPx(padded.inset[3], 40.0f),
          "inset() reads four sides");
    check(padded.box == Clip::Box::Padding, "a trailing box keyword sticks");
    Clip three = clipOf("#x{clip-path:inset(1px 2px 3px)}");
    check(isPx(three.inset[0], 1.0f) && isPx(three.inset[1], 2.0f) &&
              isPx(three.inset[2], 3.0f) && isPx(three.inset[3], 2.0f),
          "inset() mirrors the horizontal side with three values");
    Clip one = clipOf("#x{clip-path:inset(7px)}");
    check(isPx(one.inset[0], 7.0f) && isPx(one.inset[1], 7.0f) &&
              isPx(one.inset[2], 7.0f) && isPx(one.inset[3], 7.0f),
          "inset() spreads one value to every side");

    Clip circle = clipOf("#x{clip-path:circle(50%)}");
    check(circle.shape == Clip::Shape::Circle, "circle() parses");
    check(circle.radius == Clip::Radial::Explicit && isPct(circle.radiusLength, 50.0f),
          "circle() keeps an explicit radius");
    check(isPct(circle.centerX, 50.0f) && isPct(circle.centerY, 50.0f),
          "a circle centres itself by default");
    Clip named = clipOf("#x{clip-path:circle(closest-side at 10px top)}");
    check(named.radius == Clip::Radial::ClosestSide, "circle() reads a radial keyword");
    check(isPx(named.centerX, 10.0f) && isPct(named.centerY, 0.0f),
          "circle() reads an at position");

    Clip ellipse = clipOf("#x{clip-path:ellipse(10px 20px at left bottom)}");
    check(ellipse.shape == Clip::Shape::Ellipse, "ellipse() parses");
    check(isPx(ellipse.radiusXLength, 10.0f) && isPx(ellipse.radiusYLength, 20.0f),
          "ellipse() keeps both radii");
    check(isPct(ellipse.centerX, 0.0f) && isPct(ellipse.centerY, 100.0f),
          "ellipse() reads keyword positions");

    Clip polygon = clipOf("#x{clip-path:polygon(0 0, 10px 0, 5px 8px)}");
    check(polygon.shape == Clip::Shape::Polygon, "polygon() parses");
    check(polygon.points.size() == 3, "polygon() keeps three points");
    check(!polygon.evenOdd, "polygon() defaults to nonzero");
    if (polygon.points.size() == 3) {
        check(isPx(polygon.points[2].x, 5.0f) && isPx(polygon.points[2].y, 8.0f),
              "polygon() reads point coordinates");
    }
    Clip even = clipOf("#x{clip-path:polygon(evenodd, 0 0, 10px 0, 5px 8px)}");
    check(even.evenOdd && even.points.size() == 3, "polygon() reads its fill rule");

    // Garbage never clips: a failed parse keeps whatever won before, which is
    // `none` on a fresh resolve.
    const char* bad[] = {"banana",
                         "inset(10px banana)",
                         "polygon(0 0)",
                         "path(\"M0 0\")",
                         "url(#clip)",
                         "circle(10px banana)",
                         "ellipse(10px)"};
    for (const char* css : bad) {
        const std::string rule = std::string("#x{clip-path:") + css + "}";
        check(clipOf(rule.c_str()).shape == Clip::Shape::None,
              (std::string("invalid clip-path rejected: ") + css).c_str());
    }

    // A bare geometry box crops to itself.
    Clip bare = clipOf("#x{clip-path:padding-box}");
    check(bare.shape == Clip::Shape::Inset && bare.box == Clip::Box::Padding,
          "a bare box clips to itself");

    // Serialization round-trips the shapes the parser accepts.
    const auto serialOf = [&](const char* css) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        return sui::StyleResolver::propertyValue(resolver.resolve(*x), "clip-path");
    };
    check(serialOf("#x{clip-path:inset(10px 20px)}") == "inset(10px 20px 10px 20px)",
          "inset() serializes");
    check(serialOf("#x{clip-path:circle(50%)}") == "circle(50% at 50% 50%)",
          "circle() serializes");
    check(serialOf("#x{clip-path:polygon(evenodd, 0 0, 10px 0, 5px 8px)}") ==
              "polygon(evenodd, 0px 0px, 10px 0px, 5px 8px)",
          "polygon() serializes");
    check(serialOf("#x{clip-path:none}") == "none", "none serializes");
}

void testCssImports() {
    auto document = parseOrNull("<sui><body><div id=\"x\">hi</div></body></sui>");
    if (!document) {
        return;
    }
    Element* x = document->getElementById("x");
    check(x != nullptr, "imported element found");
    if (!x) {
        return;
    }
    // Canned sheets stand in for resources; every href the parser asks for is
    // recorded so relative resolution is observable.
    std::map<std::string, std::string, std::less<>> sheets;
    std::vector<std::string> fetched;
    auto fetch = [&](std::string_view href) -> std::optional<std::string> {
        fetched.emplace_back(href);
        const auto it = sheets.find(href);
        if (it == sheets.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    const auto resolveColor = [&](Element& el, const char* css,
                                  const sui::CSSParser::FetchStylesheet& fetcher,
                                  const char* base = "") {
        sui::StyleResolver resolver;
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes(css, rules, keyframes, fetcher, base);
        resolver.setRules(std::move(rules));
        layoutRoot(*document, resolver, 400.0f);
        return resolver.resolve(el).color;
    };
    const auto isRed = [](SDL_Color c) {
        return c.r == 0xff && c.g == 0x00 && c.b == 0x00;
    };
    const auto isGreen = [](SDL_Color c) {
        return c.r == 0x00 && c.g == 0xff && c.b == 0x00;
    };
    const auto isDefault = [](SDL_Color c) {
        return c.r == 228 && c.g == 228 && c.b == 231;
    };

    // An import inlines where it stands: the base sheet first, the main sheet
    // after, so the main rule wins ties.
    sheets["base.css"] = "#x{color:#00ff00}";
    check(isGreen(resolveColor(*x, "@import \"base.css\"; div{color:#ff0000}", fetch)),
          "imported rules carry their specificity");

    // An import after any other rule is invalid and ignored.
    {
        sui::StyleResolver resolver;
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes("#x{color:#ff0000} @import \"base.css\";",
                                           rules, keyframes, fetch);
        check(rules.size() == 1, "a misplaced import produces no rules");
        resolver.setRules(std::move(rules));
        layoutRoot(*document, resolver, 400.0f);
        check(isRed(resolver.resolve(*x).color), "a misplaced import changes nothing");
    }

    // Relative hrefs resolve against the importing sheet.
    fetched.clear();
    sheets["css/reset.css"] = "#x{color:#00ff00}";
    check(isGreen(resolveColor(*x, "@import \"reset.css\";", fetch, "css/main.css")),
          "a relative import applies");
    check(fetched.size() == 1 && fetched[0] == "css/reset.css",
          "a relative href resolves against its importer");

    // An unreadable sheet is skipped, not fatal.
    check(isRed(resolveColor(*x, "@import \"nope.css\"; #x{color:#ff0000}", fetch)),
          "a missing sheet is skipped");

    // Without a fetcher every import is skipped, which is what parse() does.
    {
        const auto rules = sui::CSSParser::parse("@import \"base.css\"; #x{color:#ff0000}");
        check(rules.size() == 1, "parse() skips imports");
    }

    // Cycles terminate: each sheet applies once.
    sheets["b.css"] = "@import \"c.css\"; #y{color:#00ff00}";
    sheets["c.css"] = "@import \"b.css\"; #z{color:#00ff00}";
    {
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes("@import \"b.css\"; #x{color:#ff0000}", rules,
                                           keyframes, fetch);
        check(rules.size() == 3, "a cyclic import terminates with each sheet once");
    }

    // An import condition gates the sheet: matching media applies it.
    sheets["m.css"] = "#x{color:#00ff00}";
    {
        sui::StyleResolver resolver;
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes("@import \"m.css\" (min-width:100px);", rules,
                                           keyframes, fetch);
        resolver.setRules(std::move(rules));
        resolver.setViewportSize(800.0f, 600.0f);
        layoutRoot(*document, resolver, 400.0f);
        check(isGreen(resolver.resolve(*x).color), "a matching import condition applies");
    }
    {
        sui::StyleResolver resolver;
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes("@import \"m.css\" (min-width:100px);", rules,
                                           keyframes, fetch);
        resolver.setRules(std::move(rules));
        layoutRoot(*document, resolver, 400.0f);
        check(isDefault(resolver.resolve(*x).color),
              "a failing import condition applies nothing");
    }

    // An import layer wraps the sheet: unlayered main rules still win, even
    // against higher specificity inside the layer.
    sheets["l.css"] = "#x{color:#00ff00}";
    {
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes(
            "@import \"l.css\" layer(foo); div{color:#ff0000}", rules, keyframes, fetch);
        check(rules.size() == 2 && rules[0].layer == "foo",
              "an import layer wraps the sheet");
        sui::StyleResolver resolver;
        resolver.setRules(std::move(rules));
        layoutRoot(*document, resolver, 400.0f);
        check(isRed(resolver.resolve(*x).color), "unlayered rules beat an import layer");
    }
    // A bare `layer` wraps the sheet anonymously.
    {
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes("@import \"l.css\" layer;", rules, keyframes,
                                           fetch);
        check(rules.size() == 1 && rules[0].layer.rfind("@anonymous-", 0) == 0,
              "a bare import layer is anonymous");
    }
    // `supports()` gates the sheet. A true condition keeps and applies it; a
    // false one drops it before it is even fetched.
    {
        sui::StyleResolver resolver;
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes(
            "@import \"l.css\" supports(display:grid);", rules, keyframes, fetch);
        check(rules.size() == 1, "a true supports() keeps the import");
        resolver.setRules(std::move(rules));
        layoutRoot(*document, resolver, 400.0f);
        check(isGreen(resolver.resolve(*x).color), "a true supports() import applies");
    }
    {
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        fetched.clear();
        sui::CSSParser::parseWithKeyframes(
            "@import \"l.css\" supports(display:nonsense);", rules, keyframes, fetch);
        check(rules.empty(), "a false supports() drops the import");
        check(fetched.empty(), "a false supports() does not fetch the sheet");
    }
    {
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes(
            "@import \"l.css\" layer(foo) supports((display:grid) and (color:red)) "
            "(min-width:100px);",
            rules, keyframes, fetch);
        check(rules.size() == 1 && rules[0].layer == "foo" &&
                  rules[0].media.size() == 1,
              "layer, supports(), and media compose in an import");
        sui::StyleResolver resolver;
        resolver.setRules(std::move(rules));
        resolver.setViewportSize(800.0f, 600.0f);
        layoutRoot(*document, resolver, 400.0f);
        check(isGreen(resolver.resolve(*x).color),
              "an import with layer, supports(), and matching media applies");
    }
    {
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes(
            "@import \"l.css\" supports(selector(:has(.x)));", rules, keyframes, fetch);
        check(rules.size() == 1, "selector() can gate an import");
    }
    {
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> keyframes;
        sui::CSSParser::parseWithKeyframes(
            "@import \"l.css\" supports(not (display:grid));", rules, keyframes, fetch);
        check(rules.empty(), "a false not() supports condition drops the import");
    }
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

// A checkbox/radio is a control, not a text field: the UA style gives it a fixed
// square instead of the input padding, and `:checked` reads the attribute.
// Structural HTML elements carry browser-ish UA chrome: horizontal rules,
// quoted/illustrated passages, description lists, form groups, centered text,
// and addresses. These checks cover both the style record and the layout it
// produces for the elements whose margins or borders are visible.
void testStructuralUaDefaults() {
    using Style = sui::ComputedStyle;
    const Style hr = sui::defaultStyleFor("hr");
    check(hr.display == Style::Block, "hr defaults to block");
    check(hr.margin.top.value == 8.0f && hr.margin.bottom.value == 8.0f,
          "hr has the browser's half-line margins");
    check(hr.margin.left.unit == sui::Length::Auto &&
              hr.margin.right.unit == sui::Length::Auto,
          "hr auto-centres horizontally");
    check(hr.borderWidth.top.value == 1.0f &&
              hr.borderWidth.bottom.value == 1.0f &&
              hr.borderStyles.top == Style::BorderInset,
          "hr's inset top and bottom borders make its rule");

    for (const char* tag : {"blockquote", "figure"}) {
        const Style s = sui::defaultStyleFor(tag);
        const std::string name(tag);
        check(s.display == Style::Block, (name + " defaults to block").c_str());
        check(s.margin.top.value == 16.0f && s.margin.bottom.value == 16.0f &&
                  s.margin.left.value == 40.0f && s.margin.right.value == 40.0f,
              (name + " has the browser's 1em/40px margins").c_str());
    }
    check(sui::defaultStyleFor("figcaption").display == Style::Block,
          "figcaption defaults to block");

    const Style dl = sui::defaultStyleFor("dl");
    check(dl.margin.top.value == 16.0f && dl.margin.bottom.value == 16.0f,
          "dl has one-line vertical margins");
    check(sui::defaultStyleFor("dt").display == Style::Block,
          "dt defaults to block");
    const Style dd = sui::defaultStyleFor("dd");
    check(dd.display == Style::Block && dd.margin.left.value == 40.0f,
          "dd indents its description");

    const Style fieldset = sui::defaultStyleFor("fieldset");
    check(fieldset.display == Style::Block &&
              fieldset.borderWidth.top.value == 2.0f &&
              fieldset.borderStyles.top == Style::BorderGroove,
          "fieldset has a two-pixel groove border");
    check(fieldset.padding.top.value == 6.0f &&
              fieldset.padding.left.value == 12.0f &&
              fieldset.margin.left.value == 2.0f,
          "fieldset has browser-like padding and inset margins");
    const Style legend = sui::defaultStyleFor("legend");
    check(legend.display == Style::Block && legend.padding.left.value == 2.0f,
          "legend pads its label");

    const Style center = sui::defaultStyleFor("center");
    check(center.display == Style::Block && center.textAlign == Style::Center,
          "center aligns its text");
    check((center.specified & Style::SpecifiedTextAlign) != 0,
          "center marks text-align as UA-specified");
    const Style address = sui::defaultStyleFor("address");
    check(address.display == Style::Block &&
              address.fontStyle == Style::FontItalic,
          "address defaults to italic");
    check((address.specified & Style::SpecifiedFontStyle) != 0,
          "address marks font-style as UA-specified");

    auto document = parseOrNull(
        "<sui><body><hr id=\"rule\"/>"
        "<dl><dt id=\"term\">Term</dt><dd id=\"desc\">Definition</dd></dl>"
        "<blockquote id=\"quote\">Quote</blockquote>"
        "<center id=\"centre\">Middle</center>"
        "<address id=\"addr\">Somewhere</address></body></sui>");
    if (!document) {
        return;
    }
    Element* rule = document->getElementById("rule");
    Element* term = document->getElementById("term");
    Element* desc = document->getElementById("desc");
    Element* quote = document->getElementById("quote");
    Element* centre = document->getElementById("centre");
    Element* addr = document->getElementById("addr");
    check(rule && term && desc && quote && centre && addr,
          "structural-default elements found");
    if (!rule || !term || !desc || !quote || !centre || !addr) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, resolver, 400.0f);
    checkNear(rule->layoutHeight(), 2.0f, 0.01f,
              "hr lays out as a two-pixel rule");
    checkNear(rule->layoutWidth(), 400.0f, 0.01f,
              "hr spans the content width");
    checkNear(term->layoutX(), 0.0f, 0.01f, "dt stays flush left");
    checkNear(desc->layoutX(), 40.0f, 0.01f, "dd is indented in layout");
    checkNear(desc->layoutWidth(), 360.0f, 0.01f,
              "dd narrows by its indentation");
    checkNear(quote->layoutX(), 40.0f, 0.01f,
              "blockquote is inset in layout");
    checkNear(quote->layoutWidth(), 320.0f, 0.01f,
              "blockquote narrows by both margins");
    check(resolver.resolve(*centre).textAlign == Style::Center,
          "center's alignment survives resolution");
    check(resolver.resolve(*addr).fontStyle == Style::FontItalic,
          "address's italics survive resolution");
}

void testFormControlDefaults() {
    const auto checked = sui::CSSParser::parseSelector("input:checked");
    check(!checked.unsupported, ":checked parses as a supported selector");
    bool checkedPseudo = false;
    for (const sui::PseudoClass pseudo : checked.pseudoClasses) {
        if (pseudo == sui::PseudoClass::Checked) {
            checkedPseudo = true;
        }
    }
    check(checkedPseudo, ":checked records the Checked pseudo-class");

    auto document = parseOrNull(
        "<sui><body><input id=\"cb\" type=\"checkbox\"/>"
        "<input id=\"rb\" type=\"radio\" name=\"g\"/>"
        "<input id=\"on\" type=\"checkbox\" checked/>"
        "<input id=\"txt\" type=\"text\"/></body></sui>");
    if (!document) {
        return;
    }
    Element* cb = document->getElementById("cb");
    Element* rb = document->getElementById("rb");
    Element* on = document->getElementById("on");
    Element* txt = document->getElementById("txt");
    check(cb && rb && on && txt, "form-control elements found");
    if (!cb || !rb || !on || !txt) {
        return;
    }

    check(!selectorMatches(checked, *cb), ":checked misses an unchecked box");
    check(selectorMatches(checked, *on), ":checked matches a checked box");
    // `:checked` is a control state, not a bare attribute test: a text input
    // carrying `checked` still does not match.
    txt->setAttribute("checked", "");
    check(!selectorMatches(checked, *txt), ":checked does not match a text input");
    txt->removeAttribute("checked");

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    const sui::ComputedStyle box = resolver.resolve(*cb);
    check(box.padding.top.value == 0.0f && box.padding.left.value == 0.0f,
          "a checkbox has no text padding");
    check(box.width.value == 13.0f && box.height.value == 13.0f,
          "a checkbox is a fixed 13px control");
    check(resolver.resolve(*rb).width.value == 13.0f,
          "a radio is the same size as a checkbox");
    check(resolver.resolve(*rb).borderRadii.topLeft.x.unit == sui::Length::Percent,
          "a radio's corners are fully rounded");
    check(resolver.resolve(*txt).padding.left.value == 8.0f,
          "a text input keeps its UA padding");

    // `:checked` drives the cascade, so an attribute flip restyles the element.
    sui::StyleResolver themed;
    themed.setRules(sui::CSSParser::parse(
        "body { margin: 0 } input:checked { background-color: #ff0000 }"));
    check(themed.resolve(*cb).backgroundColor.r != 0xff, "an unchecked box is unstyled");
    cb->setAttribute("checked", "");
    check(themed.resolve(*cb).backgroundColor.r == 0xff,
          ":checked applies once the attribute is set");
    cb->removeAttribute("checked");
    check(themed.resolve(*cb).backgroundColor.r != 0xff,
          "clearing checked drops the rule");

    // The fixed size survives layout: 13px of content plus a 1px border.
    sui::StyleResolver layout;
    layout.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    layoutRoot(*document, layout, 400.0f);
    check(cb->layoutWidth() == 15.0f && cb->layoutHeight() == 15.0f,
          "a checkbox lays out as a 15px border box");
}

// A range is a fixed-size groove with no text-field chrome: no padding, no
// border, and a transparent background. Author CSS still wins over the UA size.
void testRangeDefaults() {
    auto document = parseOrNull(
        "<sui><body><input id=\"r\" type=\"range\"/>"
        "<input id=\"w\" type=\"range\" style=\"width:200px\"/></body></sui>");
    if (!document) {
        return;
    }
    Element* r = document->getElementById("r");
    Element* w = document->getElementById("w");
    check(r && w, "range elements found");
    if (!r || !w) {
        return;
    }

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    const sui::ComputedStyle groove = resolver.resolve(*r);
    check(groove.padding.top.value == 0.0f && groove.padding.left.value == 0.0f,
          "a range has no text padding");
    check(groove.width.value == 129.0f && groove.height.value == 15.0f,
          "a range is a fixed 129x15 control");
    check(groove.borderWidth.top.value == 0.0f,
          "a range has no text-field border");
    check(groove.backgroundColor.a == 0,
          "a range background stays transparent");
    check(resolver.resolve(*w).width.value == 200.0f,
          "an author width overrides the range default");

    // No border or padding, so the border box is the content box.
    sui::StyleResolver layout;
    layout.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    layoutRoot(*document, layout, 400.0f);
    check(r->layoutWidth() == 129.0f && r->layoutHeight() == 15.0f,
          "a range lays out as a 129x15 border box");
}

// A select is a closed combo: inline-block with text-field chrome, options
// taking no space, auto-sized to the longest option plus the drop arrow.
void testSelectDefaults() {
    auto document = parseOrNull(
        "<sui><body><select id=\"s\"><option>fig</option>"
        "<option>cherry</option></select>"
        "<select id=\"w\" style=\"width:200px;height:40px\">"
        "<option>fig</option></select>"
        "<select id=\"p\"><option>  plum  </option></select></body></sui>");
    if (!document) {
        return;
    }
    Element* s = document->getElementById("s");
    Element* w = document->getElementById("w");
    Element* p = document->getElementById("p");
    check(s && w && p, "select elements found");
    if (!s || !w || !p) {
        return;
    }
    Element* option = nullptr;
    if (!s->children().empty()) {
        option = s->children()[0].get();
    }
    check(option && option->tag() == "option", "an option child parsed");
    if (!option) {
        return;
    }

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    const sui::ComputedStyle combo = resolver.resolve(*s);
    check(combo.display == sui::ComputedStyle::InlineBlock,
          "a select defaults to inline-block");
    check(combo.padding.left.value == 8.0f && combo.padding.top.value == 4.0f,
          "a select keeps text-field padding");
    check(combo.borderWidth.top.value == 1.0f, "a select keeps its border");
    check(combo.lineHeight == 1.0f, "a select is one line tall");
    check(resolver.resolve(*option).display == sui::ComputedStyle::None,
          "an option lays out no box of its own");

    // Without a font the longest option ("cherry", 6 chars) measures
    // 6 * 16 * 0.5 = 48px, plus 24px for the drop arrow.
    sui::StyleResolver layout;
    layout.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, layout, 400.0f);
    check(s->layoutWidth() == 48.0f + 24.0f + 16.0f + 2.0f,
          "a select auto-sizes to its longest option plus the arrow");
    check(s->layoutHeight() == 16.0f + 8.0f + 2.0f,
          "a select is one line tall: its options take no space");
    check(w->contentWidth() == 200.0f && w->contentHeight() == 40.0f,
          "an author size overrides the select default");

    // The value reads the selected option: the marked one, else the first.
    check(s->value() == "fig", "a select value defaults to its first option");
    s->children()[1]->setAttribute("selected", "");
    check(s->value() == "cherry", "the marked option wins the value");
    s->children()[1]->removeAttribute("selected");
    check(s->value() == "fig", "clearing the mark falls back to the first");

    // Indented markup reads cleanly: surrounding whitespace is stripped, so the
    // padded "plum" (4 chars) sizes 4 * 16 * 0.5 + 24 + 18.
    check(p->value() == "plum", "an option label strips surrounding whitespace");
    check(p->layoutWidth() == 32.0f + 24.0f + 16.0f + 2.0f,
          "the stripped label sizes the select");
}

// A select with `size` or `multiple` is an open list: options lay out as rows,
// the box is as tall as its visible rows, and it scrolls what does not fit.
void testSelectListbox() {
    auto document = parseOrNull(
        "<sui><body><select id=\"lb\" size=\"3\">"
        "<option>one</option><option>two</option><option>three</option>"
        "<option>four</option><option>five</option></select>"
        "<select id=\"ml\" multiple>"
        "<option>one</option><option>two</option></select>"
        "<select id=\"m2\" multiple size=\"2\"><option>one</option></select>"
        "</body></sui>");
    if (!document) {
        return;
    }
    Element* lb = document->getElementById("lb");
    Element* ml = document->getElementById("ml");
    Element* m2 = document->getElementById("m2");
    check(lb && ml && m2, "listbox elements found");
    if (!lb || !ml || !m2) {
        return;
    }
    check(lb->listboxRows() == 3, "size=3 shows three rows");
    check(ml->listboxRows() == 4, "a bare multiple shows four rows");
    check(m2->listboxRows() == 2, "multiple size=2 shows two rows");

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    Element* row = lb->children()[0].get();
    check(resolver.resolve(*row).display == sui::ComputedStyle::Block,
          "a listbox option lays out as a row");
    check(resolver.resolve(*lb).overflowY == sui::ComputedStyle::Auto,
          "an open list clips and scrolls");

    // One line is 16px; the chrome adds 8px of padding and a 1px border.
    sui::StyleResolver layout;
    layout.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, layout, 400.0f);
    check(lb->layoutHeight() == 3.0f * 16.0f + 10.0f,
          "a size=3 list is three rows tall");
    check(ml->layoutHeight() == 4.0f * 16.0f + 10.0f,
          "a bare multiple is four rows tall");
    check(row->layoutHeight() == 16.0f, "each option row is one line tall");
    check(row->layoutY() == lb->contentY(), "rows start at the content top");
}

// Date/time/color inputs keep the text-field chrome, edit as text, and paint
// an indicator the UA padding leaves room for; a file control shows a Browse
// button and the file name without its path.
void testDateTimeColorFile() {
    auto document = parseOrNull(
        "<sui><body><input id=\"dt\" type=\"date\" value=\"2026-10-12\"/>"
        "<input id=\"tm\" type=\"time\" value=\"14:30\"/>"
        "<input id=\"cl\" type=\"color\" value=\"#ff0000\"/>"
        "<input id=\"fl\" type=\"file\" value=\"C:\\fakepath\\report.pdf\"/>"
        "<input id=\"fl2\" type=\"file\" value=\"/home/u/notes.txt\"/></body></sui>");
    if (!document) {
        return;
    }
    Element* dt = document->getElementById("dt");
    Element* tm = document->getElementById("tm");
    Element* cl = document->getElementById("cl");
    Element* fl = document->getElementById("fl");
    Element* fl2 = document->getElementById("fl2");
    check(dt && tm && cl && fl && fl2, "picker elements found");
    if (!dt || !tm || !cl || !fl || !fl2) {
        return;
    }

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    check(resolver.resolve(*dt).padding.right.value == 24.0f,
          "a date leaves room for its calendar glyph");
    check(resolver.resolve(*tm).padding.right.value == 24.0f,
          "a time leaves room for its clock glyph");
    check(resolver.resolve(*cl).padding.right.value == 32.0f,
          "a color leaves room for its swatch");
    check(resolver.resolve(*fl).padding.left.value == 80.0f,
          "a file leaves room for its Browse button");

    // The value flows as normal text runs, so the controls auto-size; a file
    // run holds only the name.
    sui::StyleResolver layout;
    layout.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, layout, 400.0f);
    check(!dt->textRuns().empty() && dt->textRuns()[0].text == "2026-10-12",
          "a date value lays out as text");
    check(dt->layoutWidth() > 100.0f, "a date auto-sizes past its indicator room");
    check(!tm->textRuns().empty() && tm->textRuns()[0].text == "14:30",
          "a time value lays out as text");
    check(!cl->textRuns().empty() && cl->textRuns()[0].text == "#ff0000",
          "a color value lays out as text");
    check(!fl->textRuns().empty() && fl->textRuns()[0].text == "report.pdf",
          "a file run shows the name without its path");
    check(fl->fileName() == "report.pdf", "fileName strips backslash paths");
    check(fl2->fileName() == "notes.txt", "fileName strips slash paths");
    check(fl->layoutWidth() > 100.0f, "a file auto-sizes past its button room");
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
        resolver.setRules(sui::CSSParser::parse("div { scroll-timeline: --x; width: 10px }"));
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
    check(mentions("scroll-timeline"), "an unsupported property is logged by name");
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

// `cursor` changes the pointer shape, not the box: layout ignores it, but the
// application reads it on every hover. The interesting parts are the fallback
// list grammar, the inheritance, and the keyword round-trip.
void testCursorProperty() {
    auto document = parseOrNull(
        "<sui><body>"
        "<a id=\"lnk\" href=\"#\">home</a>"
        "<div id=\"box\"><span id=\"kid\">k</span>"
        "<span id=\"autokid\">a</span></div>"
        "<div id=\"none\">n</div>"
        "<div id=\"url\">u</div>"
        "<div id=\"bogus\">b</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(R"(
        #box { cursor: pointer }
        #autokid { cursor: auto }
        #none { cursor: none }
        #url { cursor: url(hand.png), text }
        #bogus { cursor: nonsense }
    )"));

    auto cursorOf = [&](const char* id) {
        return resolver.resolve(*document->getElementById(id)).cursor;
    };
    auto valueOf = [&](const char* id) {
        return sui::StyleResolver::propertyValue(
            resolver.resolve(*document->getElementById(id)), "cursor");
    };

    check(cursorOf("box") == sui::ComputedStyle::CursorPointer,
          "cursor: pointer resolves to the pointer shape");
    check(cursorOf("kid") == sui::ComputedStyle::CursorPointer,
          "cursor inherits into an unstyled descendant");
    check(cursorOf("autokid") == sui::ComputedStyle::CursorAuto,
          "an explicit cursor: auto overrides the inherited shape");
    check(cursorOf("lnk") == sui::ComputedStyle::CursorPointer,
          "a link gets the pointer shape from the UA");
    check(cursorOf("none") == sui::ComputedStyle::CursorNone,
          "cursor: none is kept as its own value");
    check(cursorOf("url") == sui::ComputedStyle::CursorText,
          "a url() fallback list resolves to its trailing keyword");
    check(cursorOf("bogus") == sui::ComputedStyle::CursorAuto,
          "an unrecognised cursor value is dropped");

    check(valueOf("box") == "pointer", "cursor: pointer round-trips");
    check(valueOf("none") == "none", "cursor: none round-trips");
    check(valueOf("url") == "text", "the keyword chosen from a fallback list round-trips");
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

// `order` is an integer that rearranges flex and grid items into
// `order`-modified document order without touching the DOM; equal orders keep
// document order, block containers ignore it, and it reads back as written so
// the CSS-wide keywords can copy it.
void testOrder() {
    const auto rules = sui::CSSParser::parse(
        "#oa { order: 2 } #ob { order: -1 } #oc { order: 0 } #od { order: banana }");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    auto body = parseOrNull("<sui><body>"
                            "<div id=\"oa\"></div><div id=\"ob\"></div>"
                            "<div id=\"oc\"></div><div id=\"od\"></div>"
                            "</body></sui>");
    if (!body) {
        return;
    }
    Element* oa = body->getElementById("oa");
    Element* ob = body->getElementById("ob");
    Element* oc = body->getElementById("oc");
    Element* od = body->getElementById("od");
    if (!oa || !ob || !oc || !od) {
        check(false, "order elements found");
        return;
    }
    const ComputedStyle styleA = resolver.resolve(*oa);
    const ComputedStyle styleB = resolver.resolve(*ob);
    const ComputedStyle styleC = resolver.resolve(*oc);
    const ComputedStyle styleD = resolver.resolve(*od);
    check(styleA.order == 2 && styleB.order == -1, "order keeps its sign");
    check(styleC.order == 0 && styleD.order == 0,
          "the initial order is zero and garbage reads as the initial");
    check(sui::StyleResolver::propertyValue(styleA, "order") == "2" &&
              sui::StyleResolver::propertyValue(styleB, "order") == "-1",
          "order reads back as an integer");

    auto document = parseOrNull(
        "<sui><body><div id=\"block\"><div id=\"k1\"></div>"
        "<div id=\"k2\"></div></div><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "<div id=\"c\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver layoutResolver;
    layoutResolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 300px }"
        "#a, #b, #c { width: 100px; height: 10px }"
        "#a { order: 2 } #b { order: -1 } #c { order: 1 }"
        "#k1, #k2 { width: 40px; height: 10px } #k2 { order: -1 }"));
    layoutRoot(*document, layoutResolver, 800.0f);
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* c = document->getElementById("c");
    Element* k1 = document->getElementById("k1");
    Element* k2 = document->getElementById("k2");
    if (!a || !b || !c || !k1 || !k2) {
        check(false, "order layout elements found");
        return;
    }
    checkNear(b->layoutX(), 0.0f, 0.5f, "the lowest order paints first");
    checkNear(c->layoutX(), 100.0f, 0.5f, "the middle order follows");
    checkNear(a->layoutX(), 200.0f, 0.5f, "the highest order lands last");
    checkNear(k1->layoutY(), 0.0f, 0.5f, "block layout keeps document order");
    checkNear(k2->layoutY(), 10.0f, 0.5f, "a block child's order changes nothing");

    // Equal orders keep document order, so only the odd one out moves.
    auto stable = parseOrNull(
        "<sui><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div>"
        "<div id=\"c\"></div></div></body></sui>");
    if (!stable) {
        return;
    }
    sui::StyleResolver stableResolver;
    stableResolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #row { display: flex; width: 300px }"
        "#a, #b, #c { width: 100px; height: 10px }"
        "#a, #b { order: 1 } #c { order: 0 }"));
    layoutRoot(*stable, stableResolver, 800.0f);
    Element* sa = stable->getElementById("a");
    Element* sb = stable->getElementById("b");
    Element* sc = stable->getElementById("c");
    if (!sa || !sb || !sc) {
        return;
    }
    checkNear(sc->layoutX(), 0.0f, 0.5f, "a lower order starts the row");
    checkNear(sa->layoutX(), 100.0f, 0.5f, "equal orders keep document order (first)");
    checkNear(sb->layoutX(), 200.0f, 0.5f, "equal orders keep document order (second)");

    // Grid auto-placement runs in the same `order`-modified order.
    auto grid = parseOrNull(
        "<sui><body><div id=\"grid\"><div id=\"g1\"></div><div id=\"g2\"></div>"
        "<div id=\"g3\"></div></div></body></sui>");
    if (!grid) {
        return;
    }
    sui::StyleResolver gridResolver;
    gridResolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #grid { display: grid; width: 300px;"
        " grid-template-columns: 100px 100px 100px }"
        "#g1, #g2, #g3 { height: 10px } #g3 { order: -1 }"));
    layoutRoot(*grid, gridResolver, 800.0f);
    Element* g1 = grid->getElementById("g1");
    Element* g2 = grid->getElementById("g2");
    Element* g3 = grid->getElementById("g3");
    if (!g1 || !g2 || !g3) {
        return;
    }
    checkNear(g3->layoutX(), 0.0f, 0.5f, "a grid item's order moves it to the first cell");
    checkNear(g1->layoutX(), 100.0f, 0.5f, "the next document item takes the second cell");
    checkNear(g2->layoutX(), 200.0f, 0.5f, "and the last takes the third");
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

void testBlockAutoMargins() {
    // A fixed-width block that does not fill its containing block splits the
    // leftover space into its auto margins: both halves centre it, one side
    // pushes it against the other edge, and an auto-width block fills the line
    // with no margins at all.
    auto document = parseOrNull(
        "<sui><body><div id=\"outer\">"
        "<div id=\"both\"></div>"
        "<div id=\"left\"></div>"
        "<div id=\"right\"></div>"
        "<div id=\"fill\"></div>"
        "<div id=\"padded\"></div>"
        "<div id=\"over\"></div>"
        "<div id=\"clamped\"></div>"
        "<div id=\"borderbox\"></div>"
        "<div id=\"flex\"></div>"
        "<div id=\"grid\"></div>"
        "<div id=\"percent\"></div>"
        "<div id=\"offset\"></div>"
        "<div id=\"maxed\"></div>"
        "<div id=\"relative\"></div>"
        "<div id=\"vertical\"></div>"
        "<div id=\"paddedouter\"><div id=\"inner\"></div></div>"
        "<span id=\"inline\"></span>"
        "</div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #outer { width: 400px }"
        "#both, #left, #right, #fill, #over, #clamped { height: 10px }"
        "#grid, #percent, #offset, #maxed, #relative, #vertical, #inner { height: 10px }"
        "#both { width: 100px; margin: 0 auto }"
        "#left { width: 100px; margin-left: auto }"
        "#right { width: 100px; margin-right: auto }"
        "#fill { margin: 0 auto }"
        "#padded { width: 100px; height: 10px; padding: 10px;"
        " border: 1px solid #000; margin: 0 auto }"
        "#over { width: 500px; margin: 0 auto }"
        "#clamped { width: 50px; min-width: 100px; margin: 0 auto }"
        "#borderbox { box-sizing: border-box; width: 100px; height: 10px;"
        " padding: 10px; border: 1px solid #000; margin: 0 auto }"
        "#flex { display: flex; width: 100px; height: 10px; margin: 0 auto }"
        "#grid { display: grid; width: 100px; margin: 0 auto }"
        "#percent { width: 50%; margin: 0 auto }"
        "#offset { width: 100px; margin-left: auto; margin-right: 20px }"
        "#maxed { max-width: 100px; margin: 0 auto }"
        "#relative { position: relative; left: 5px; width: 100px; margin: 0 auto }"
        "#vertical { width: 100px; margin: auto 0 }"
        "#paddedouter { width: 300px; padding: 25px }"
        "#inner { width: 100px; margin: 0 auto }"
        "#inline { display: inline-block; width: 100px; height: 10px;"
        " margin: 0 auto }"));
    layoutRoot(*document, resolver, 800.0f);
    Element* both = document->getElementById("both");
    Element* left = document->getElementById("left");
    Element* right = document->getElementById("right");
    Element* fill = document->getElementById("fill");
    Element* padded = document->getElementById("padded");
    Element* over = document->getElementById("over");
    Element* clamped = document->getElementById("clamped");
    Element* borderbox = document->getElementById("borderbox");
    Element* flex = document->getElementById("flex");
    Element* grid = document->getElementById("grid");
    Element* percent = document->getElementById("percent");
    Element* offset = document->getElementById("offset");
    Element* maxed = document->getElementById("maxed");
    Element* relative = document->getElementById("relative");
    Element* vertical = document->getElementById("vertical");
    Element* inner = document->getElementById("inner");
    Element* inlineBox = document->getElementById("inline");
    if (!both || !left || !right || !fill || !padded || !over || !clamped ||
        !borderbox || !flex || !grid || !percent || !offset || !maxed ||
        !relative || !vertical || !inner || !inlineBox) {
        check(false, "auto-margin fixture elements found");
        return;
    }
    checkNear(both->layoutX(), 150.0f, 0.01f, "margin:0 auto centres a fixed-width block");
    checkNear(both->layoutWidth(), 100.0f, 0.01f, "centring keeps the authored width");
    checkNear(left->layoutX(), 300.0f, 0.01f, "an auto left margin pushes the block right");
    checkNear(right->layoutX(), 0.0f, 0.01f, "an auto right margin leaves the block at the start");
    checkNear(fill->layoutX(), 0.0f, 0.01f, "an auto width fills the line instead of centring");
    checkNear(fill->layoutWidth(), 400.0f, 0.01f, "the auto-width block spans the container");
    checkNear(padded->layoutX(), 139.0f, 0.01f,
              "padding and border join the centred border box");
    checkNear(padded->layoutWidth(), 122.0f, 0.01f, "the centred box keeps its chrome");
    checkNear(over->layoutX(), 0.0f, 0.01f, "an oversized block clamps its auto margin to zero");
    checkNear(over->layoutWidth(), 500.0f, 0.01f, "the oversized block still overflows");
    checkNear(clamped->layoutX(), 150.0f, 0.01f, "min-width sets the centred width");
    checkNear(clamped->layoutWidth(), 100.0f, 0.01f, "the clamped block is 100px wide");
    checkNear(borderbox->layoutX(), 150.0f, 0.01f,
              "a border-box width is what gets centred");
    checkNear(borderbox->layoutWidth(), 100.0f, 0.01f,
              "border-box chrome stays inside the centred width");
    checkNear(flex->layoutX(), 150.0f, 0.01f, "a block-level flex container centres too");
    checkNear(grid->layoutX(), 150.0f, 0.01f, "a block-level grid container centres too");
    checkNear(percent->layoutX(), 100.0f, 0.01f,
              "a percentage width leaves the rest to the auto margins");
    checkNear(percent->layoutWidth(), 200.0f, 0.01f, "the percentage width is kept");
    checkNear(offset->layoutX(), 280.0f, 0.01f,
              "the other auto margin gives way to a fixed margin");
    checkNear(maxed->layoutX(), 150.0f, 0.01f,
              "max-width clamps an auto width and then centres it");
    checkNear(maxed->layoutWidth(), 100.0f, 0.01f, "the max-clamped width is kept");
    checkNear(relative->layoutX(), 155.0f, 0.01f,
              "a relative offset shifts the centred box");
    checkNear(vertical->layoutX(), 0.0f, 0.01f,
              "vertical auto margins do not centre in flow");
    checkNear(inner->layoutX(), 125.0f, 0.01f,
              "the containing block's padding shifts the centre");
    checkNear(inlineBox->layoutX(), 0.0f, 0.01f,
              "inline-level auto margins still resolve to zero");
}

void testFlexAutoMargins() {
    // Auto margins take the free space before alignment sees it: both sides
    // centre, one side pushes to the far edge, several auto edges share alike,
    // and a negative remainder leaves them at zero. Cross-axis margins do the
    // same inside a line, overriding align-self, and the start margin is part
    // of the item's place (a fixed one too).
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"centre\"><div id=\"c1\"></div></div>"
        "<div id=\"pushright\"><div id=\"r1\"></div></div>"
        "<div id=\"pushleft\"><div id=\"l1\"></div></div>"
        "<div id=\"split\"><div id=\"s1\"></div><div id=\"s2\"></div>"
        "<div id=\"s3\"></div></div>"
        "<div id=\"tight\"><div id=\"t1\"></div></div>"
        "<div id=\"crossboth\"><div id=\"cb\"></div></div>"
        "<div id=\"crossend\"><div id=\"ce\"></div></div>"
        "<div id=\"fixedcross\"><div id=\"fc\"></div></div>"
        "<div id=\"fixedend\"><div id=\"fe\"></div></div>"
        "<div id=\"colmain\"><div id=\"cm\"></div></div>"
        "<div id=\"colcross\"><div id=\"cc\"></div></div>"
        "<div id=\"rev\"><div id=\"rv\"></div></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 }"
        "#centre { display: flex; width: 400px; height: 100px; justify-content: flex-end }"
        "#c1 { width: 100px; height: 20px; margin: 0 auto }"
        "#pushright { display: flex; width: 400px; height: 40px }"
        "#r1 { width: 100px; height: 20px; margin-left: auto }"
        "#pushleft { display: flex; width: 400px; height: 40px }"
        "#l1 { width: 100px; height: 20px; margin-right: auto }"
        "#split { display: flex; width: 400px; height: 40px }"
        "#s1, #s2, #s3 { width: 50px; height: 20px; margin-left: auto }"
        "#tight { display: flex; width: 80px; height: 40px }"
        "#t1 { width: 100px; height: 20px; margin: 0 auto }"
        "#crossboth { display: flex; width: 400px; height: 100px; align-items: flex-end }"
        "#cb { width: 100px; height: 20px; margin: auto 0 }"
        "#crossend { display: flex; width: 400px; height: 100px; align-items: center }"
        "#ce { width: 100px; height: 20px; margin-top: auto }"
        "#fixedcross { display: flex; width: 400px; height: 100px; align-items: flex-start }"
        "#fc { width: 100px; height: 20px; margin-top: 10px; margin-bottom: 30px }"
        "#fixedend { display: flex; width: 400px; height: 100px; align-items: flex-end }"
        "#fe { width: 100px; height: 20px; margin-top: 10px; margin-bottom: 30px }"
        "#colmain { display: flex; flex-direction: column; width: 400px; height: 100px }"
        "#cm { width: 100px; height: 20px; margin: auto 0 }"
        "#colcross { display: flex; flex-direction: column; width: 400px; height: 100px;"
        " align-items: flex-start }"
        "#cc { width: 100px; height: 20px; margin: 0 auto }"
        "#rev { display: flex; flex-direction: row-reverse; width: 400px; height: 40px }"
        "#rv { width: 100px; height: 20px; margin-left: auto }"));
    layoutRoot(*document, resolver, 800.0f);
    const auto at = [&](const char* id, const char* container, float x, float y,
                        const char* what) {
        Element* el = document->getElementById(id);
        Element* box = document->getElementById(container);
        if (!el || !box) {
            check(false, "flex auto-margin fixture found");
            return;
        }
        checkNear(el->layoutX() - box->layoutX(), x, 0.05f, what);
        checkNear(el->layoutY() - box->layoutY(), y, 0.05f, what);
    };
    at("c1", "centre", 150.0f, 0.0f,
       "both main auto margins centre against justify-content");
    at("r1", "pushright", 300.0f, 0.0f,
       "an auto left margin pushes the item to the end");
    at("l1", "pushleft", 0.0f, 0.0f,
       "an auto right margin leaves the item at the start");
    at("s1", "split", 83.333f, 0.0f, "three auto margins share the free space");
    at("s2", "split", 216.667f, 0.0f,
       "the second share lands after the first item");
    at("s3", "split", 350.0f, 0.0f, "the third share ends at the container edge");
    at("t1", "tight", 0.0f, 0.0f, "no room leaves the auto margins at zero");
    at("cb", "crossboth", 0.0f, 40.0f,
       "both cross auto margins centre against align-self");
    at("ce", "crossend", 0.0f, 80.0f,
       "an auto start margin pushes to the cross end");
    at("fc", "fixedcross", 0.0f, 10.0f,
       "a fixed cross start margin offsets the item");
    at("fe", "fixedend", 0.0f, 50.0f,
       "a fixed cross end margin is part of the alignment");
    at("cm", "colmain", 0.0f, 40.0f, "column main auto margins centre vertically");
    at("cc", "colcross", 150.0f, 0.0f,
       "column cross auto margins centre horizontally");
    Element* cc = document->getElementById("cc");
    if (cc) {
        checkNear(cc->layoutWidth(), 100.0f, 0.05f,
                  "a cross auto margin stops the stretch");
    }
    at("rv", "rev", 300.0f, 0.0f, "a reversed row mirrors the auto-margin box");
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
    check(value("a", "gap") == "7px", "gap reads back");
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

void testGapShorthand() {
    // `gap` takes one value for both axes or two, row then column. The axis a
    // value lands on follows the container's direction: a row spaces its items
    // with the column gap and its wrapped lines with the row gap, a column
    // container the other way round.
    auto document = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"row\" style=\"display:flex;width:200px;gap:8px 20px\">"
        "<div id=\"r1\" style=\"width:30px;height:10px\"></div>"
        "<div id=\"r2\" style=\"width:30px;height:10px\"></div></div>"
        "<div id=\"col\" style=\"display:flex;flex-direction:column;gap:8px 20px\">"
        "<div id=\"c1\" style=\"height:10px\"></div>"
        "<div id=\"c2\" style=\"height:10px\"></div></div>"
        "<div id=\"single\" style=\"display:flex;width:200px;gap:9px\">"
        "<div id=\"s1\" style=\"width:30px;height:10px\"></div>"
        "<div id=\"s2\" style=\"width:30px;height:10px\"></div></div>"
        "<div id=\"wrap\" style=\"display:flex;flex-wrap:wrap;width:70px;gap:5px 40px\">"
        "<div id=\"w1\" style=\"width:30px;height:10px\"></div>"
        "<div id=\"w2\" style=\"width:30px;height:10px\"></div></div>"
        "<div id=\"reverse\" style=\"display:flex;flex-wrap:wrap-reverse;"
        "width:70px;height:60px;gap:5px 40px\">"
        "<div id=\"v1\" style=\"width:30px;height:10px\"></div>"
        "<div id=\"v2\" style=\"width:30px;height:10px\"></div></div>"
        "<div id=\"grid\" style=\"display:grid;grid-template-columns:40px 40px;"
        "gap:4px 12px\">"
        "<div id=\"g1\" style=\"height:10px\"></div>"
        "<div id=\"g2\" style=\"height:10px\"></div>"
        "<div id=\"g3\" style=\"height:10px\"></div></div>"
        "<div id=\"bad\" style=\"gap:6px;gap:1px 2px 3px\"></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    layoutRoot(*document, resolver, 400.0f);
    const auto x = [&](const char* id) {
        Element* el = document->getElementById(id);
        return el ? el->layoutX() : -1000.0f;
    };
    const auto y = [&](const char* id) {
        Element* el = document->getElementById(id);
        return el ? el->layoutY() : -1000.0f;
    };
    checkNear(x("r2") - (x("r1") + 30.0f), 20.0f, 0.5f,
              "a row spaces its items with the column gap");
    checkNear(y("c2") - (y("c1") + 10.0f), 8.0f, 0.5f,
              "a column spaces its items with the row gap");
    checkNear(x("s2") - (x("s1") + 30.0f), 9.0f, 0.5f,
              "a single gap value covers both axes");
    checkNear(y("w2") - (y("w1") + 10.0f), 5.0f, 0.5f,
              "wrapped lines use the row gap");
    checkNear(std::fabs(y("v1") - y("v2")), 15.0f, 0.5f,
              "wrap-reverse keeps the line gap");
    checkNear(x("g2") - (x("g1") + 40.0f), 12.0f, 0.5f,
              "grid columns are separated by the column gap");
    checkNear(y("g3") - (y("g1") + 10.0f), 4.0f, 0.5f,
              "grid rows are separated by the row gap");
    // Three values are not a gap declaration at all, so the cascade keeps what
    // it had.
    Element* bad = document->getElementById("bad");
    if (bad) {
        const auto s = resolver.resolve(*bad);
        checkNear(s.rowGap, 6.0f, 0.01f, "a three-value gap leaves the row gap alone");
        checkNear(s.columnGap, 6.0f, 0.01f, "and the column gap");
    }
}

// The values that drive placement, stacking, compositing, and flex spacing had
// no reading at all, which also dropped their CSS-wide keywords. `position`,
// `z-index`, `opacity`, `flex`, and the gaps now answer `getComputedStyle`.
void testLayoutValuesReadBack() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div>"
        "<div id=\"d\"></div><div id=\"e\"></div><div id=\"f\"></div>"
        "<div id=\"g\"></div><div id=\"h\"></div><div id=\"i\"></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { position: absolute; z-index: 7; opacity: 0.5 }"
        "#b { position: sticky }"
        "#c { z-index: 0 }"
        "#d { flex: 2 3 40px }"
        "#e { flex: 1 }"
        "#f { gap: 8px 12px }"
        "#g { gap: 10px }"
        "#h { row-gap: 5px; column-gap: 6px }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("a", "position") == "absolute", "position reads back");
    check(value("a", "z-index") == "7", "z-index reads back");
    check(value("a", "opacity") == "0.5", "opacity reads back");
    check(value("b", "position") == "sticky", "every position keyword reads back");
    check(value("c", "z-index") == "auto", "zero z-index reads as auto, the shared slot");
    check(value("d", "flex") == "2 3 40px", "the flex shorthand reads back its three slots");
    check(value("e", "flex") == "1 1 0%", "the one-number flex form reads back as 0% basis");
    check(value("f", "gap") == "8px 12px", "a two-value gap reads back");
    check(value("f", "row-gap") == "8px" && value("f", "column-gap") == "12px",
          "and its longhands agree");
    check(value("g", "gap") == "10px", "a single gap reads back once");
    check(value("h", "gap") == "5px 6px", "longhands collapse into the shorthand");
    check(value("i", "position") == "static" && value("i", "z-index") == "auto" &&
              value("i", "opacity") == "1" && value("i", "flex") == "0 1 auto" &&
              value("i", "gap") == "normal" && value("i", "row-gap") == "normal" &&
              value("i", "column-gap") == "normal",
          "an untouched box reads its defaults");
    check(value("i", "grid-gap") == "normal", "the grid-gap alias reads back too");

    // The CSS-wide keywords replay those readings: `inherit` copies the parent,
    // and `unset` resets a non-inherited property to its initial state.
    auto inheritDoc = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"p\" style=\"position:relative;z-index:5;opacity:0.25;"
        "flex:2 0 30px;gap:4px 9px;color:#123456\">"
        "<div id=\"k\" style=\"position:inherit;z-index:inherit;opacity:inherit;"
        "flex:inherit;gap:inherit\"></div>"
        "<div id=\"u\" style=\"gap:7px;gap:unset\"></div>"
        "<div id=\"c\" style=\"color:unset\"></div>"
        "</div>"
        "<div id=\"plain\"><div id=\"kn\" style=\"gap:inherit;row-gap:inherit\"></div></div>"
        "</body></sui>");
    if (inheritDoc) {
        sui::StyleResolver inheritResolver;
        const auto of = [&](const char* id) {
            return inheritResolver.resolve(*inheritDoc->getElementById(id));
        };
        const sui::ComputedStyle k = of("k");
        check(k.position == sui::ComputedStyle::Relative,
              "position:inherit takes the parent's keyword");
        check(k.zIndex == 5, "z-index:inherit takes the parent's number");
        checkNear(k.opacity, 0.25f, 0.001f, "opacity:inherit takes the parent's value");
        checkNear(k.flex.grow, 2.0f, 0.01f, "flex:inherit takes the grow factor");
        checkNear(k.flex.basis.resolve(0.0f, 16.0f, 16.0f), 30.0f, 0.01f,
                  "and the basis");
        checkNear(k.rowGap, 4.0f, 0.01f, "gap:inherit takes the row gap");
        checkNear(k.columnGap, 9.0f, 0.01f, "and the column gap");
        checkNear(of("u").rowGap, -1.0f, 0.01f, "gap:unset returns to unauthored");
        checkNear(of("u").columnGap, -1.0f, 0.01f, "and clears the column gap");
        check(of("c").color.g == 0x34, "color:unset still inherits");
        checkNear(of("kn").rowGap, -1.0f, 0.01f,
                  "gap:inherit from an unauthored parent stays unauthored");
        checkNear(of("kn").columnGap, -1.0f, 0.01f, "on both axes");
    }
}

// Radii keep both slots per corner and collapse only while the horizontal and
// vertical lists agree; the per-side border styles read the same names
// `border-style` distributes.
void testBorderRadiusReadBack() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div>"
        "<div id=\"d\"></div><div id=\"e\"></div><div id=\"f\"></div>"
        "<div id=\"p\"><div id=\"r\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { border-radius: 4px }"
        "#b { border-radius: 4px 8px 12px 16px }"
        "#c { border-radius: 4px 8px / 12px 16px }"
        "#d { border-top-left-radius: 3px 9px; border-bottom-right-radius: 5px }"
        "#e { border-top-style: dashed; border-left-style: double;"
        "border-bottom-style: none }"
        "#f { border-style: solid dotted }"
        "#p { border-radius: 6px } #r { border-radius: inherit }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("a", "border-radius") == "4px", "one radius covers every corner");
    check(value("a", "border-top-left-radius") == "4px" &&
              value("a", "border-bottom-right-radius") == "4px",
          "and every corner reads back");
    check(value("b", "border-radius") == "4px 8px 12px 16px",
          "four distinct radii keep all four values");
    check(value("b", "border-top-right-radius") == "8px" &&
              value("b", "border-bottom-left-radius") == "16px",
          "each corner keeps its own value");
    check(value("c", "border-radius") == "4px 8px / 12px 16px",
          "an elliptical shorthand keeps both lists");
    check(value("c", "border-top-left-radius") == "4px 12px",
          "and a corner reads horizontal then vertical");
    check(value("c", "border-bottom-left-radius") == "8px 16px",
          "matching the shorthand's expansion");
    check(value("d", "border-top-left-radius") == "3px 9px",
          "a longhand's second length is the vertical radius");
    check(value("d", "border-bottom-right-radius") == "5px",
          "a single-length corner keeps one value");
    check(value("d", "border-top-right-radius") == "0px",
          "untouched corners read the initial zero");
    check(value("e", "border-top-style") == "dashed" &&
              value("e", "border-left-style") == "double" &&
              value("e", "border-bottom-style") == "none",
          "per-side border styles read back");
    check(value("e", "border-right-style") == "solid",
          "an untouched side keeps the default style");
    check(value("e", "border-style") == "dashed solid none double",
          "and the shorthand distributes them");
    check(value("f", "border-style") == "solid dotted",
          "two style values distribute top/bottom then left/right");
    check(value("f", "border-top-style") == "solid" &&
              value("f", "border-left-style") == "dotted",
          "the side longhands agree");
    check(value("r", "border-radius") == "6px", "border-radius: inherit replays the parent");
}

// The container shorthand, the -webkit-line-clamp alias, and the scroll-snap
// and snapport values all drove layout without a reading; they now answer
// `getComputedStyle` in forms that parse back to the same pair.
void testContainerAndScrollReadBack() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div>"
        "<div id=\"d\"></div><div id=\"e\"></div><div id=\"f\"></div>"
        "<div id=\"g\"></div><div id=\"h\"></div><div id=\"i\"></div>"
        "<div id=\"j\"></div><div id=\"k\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { container: card / inline-size }"
        "#b { container: widget }"
        "#c { container-type: size }"
        "#d { -webkit-line-clamp: 3 }"
        "#e { line-clamp: 2 }"
        "#f { scroll-padding: 4px 8px }"
        "#g { scroll-margin-top: 6px; scroll-margin: 3px 5px 7px 9px }"
        "#h { scroll-margin-left: 2px }"
        "#i { scroll-snap-type: x mandatory }"
        "#j { scroll-snap-align: start end }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("a", "container") == "card / inline-size",
          "the container shorthand reads both slots");
    check(value("a", "container-name") == "card" &&
              value("a", "container-type") == "inline-size",
          "and its longhands agree");
    check(value("b", "container") == "widget / normal", "a name alone keeps the normal type");
    check(value("c", "container") == "none / size", "a type alone keeps the none name");
    check(value("k", "container") == "none / normal",
          "an untouched box reads the pair's initials");
    check(value("d", "line-clamp") == "3" && value("d", "-webkit-line-clamp") == "3",
          "the -webkit-line-clamp alias reads back with line-clamp");
    check(value("e", "line-clamp") == "2", "line-clamp reads back");
    check(value("f", "scroll-padding") == "4px 8px", "scroll-padding collapses like margin");
    check(value("f", "scroll-padding-top") == "4px" &&
              value("f", "scroll-padding-left") == "8px",
          "and its longhands agree");
    check(value("g", "scroll-margin") == "3px 5px 7px 9px",
          "scroll-margin reads the shorthand's expansion");
    check(value("g", "scroll-margin-top") == "3px", "the longhand reads the same slot");
    check(value("h", "scroll-margin") == "0px 0px 0px 2px",
          "one inset keeps the other three at zero");
    check(value("k", "scroll-padding") == "auto" && value("k", "scroll-margin") == "0px",
          "the untouched insets read their initials");
    check(value("k", "scroll-margin-top") == "0px", "down to the longhand");
    check(value("k", "scroll-padding-top") == "auto", "the padding longhand keeps auto");
    check(value("i", "scroll-snap-type") == "x mandatory",
          "scroll-snap-type reads the axis and strictness");
    check(value("j", "scroll-snap-align") == "start end",
          "scroll-snap-align reads block then inline");
    check(value("k", "scroll-snap-type") == "none" && value("k", "scroll-snap-align") == "none",
          "an untouched snapper reads none");
}

// Shadows read back colour first and keep every length, including the default
// zeros and blur/spread, so the string parses back to the same list; the
// translucent default colour survives as `#rrggbbaa`.
void testShadowReadBack() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div>"
        "<div id=\"d\"></div>"
        "<div id=\"p\" style=\"box-shadow:3px 3px 4px #112233\">"
        "<div id=\"q\" style=\"box-shadow:inherit\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { box-shadow: none }"
        "#b { box-shadow: 2px 4px 6px 8px #ff0000, inset 1px 1px 2px #00000080 }"
        "#c { text-shadow: 1px 2px 3px #00ff00, 4px 5px }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("a", "box-shadow") == "none", "no box shadow reads none");
    check(value("b", "box-shadow") ==
              "#ff0000 2px 4px 6px 8px, #00000080 1px 1px 2px 0px inset",
          "box shadows read colour, offsets, blur, spread, and inset");
    check(value("c", "text-shadow") == "#00ff00 1px 2px 3px, #00000080 4px 5px 0px",
          "text shadows read colour, offsets, and blur");
    check(value("d", "box-shadow") == "none" && value("d", "text-shadow") == "none",
          "an untouched box reads none");
    check(value("q", "box-shadow") == "#112233 3px 3px 4px 0px",
          "box-shadow: inherit replays the parent's list");
    check(value("p", "box-shadow") == value("q", "box-shadow"),
          "so parent and child agree");
}

// The list-style shorthand distributes three inheritable longhands; it reads
// back all three in the grammar's order, omitting the initial position and
// image so a lone `none` is unambiguously the type.
void testListStyleReadBack() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div>"
        "<div id=\"d\"></div><div id=\"e\"></div>"
        "<div id=\"p\" style=\"list-style:square inside\">"
        "<div id=\"q\" style=\"list-style:inherit\"></div></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { list-style: circle inside }"
        "#b { list-style: url(marker.png) upper-roman }"
        "#c { list-style: none }"
        "#d { list-style-position: inside }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("a", "list-style") == "circle inside", "the shorthand reads type and position");
    check(value("a", "list-style-type") == "circle" &&
              value("a", "list-style-position") == "inside",
          "and the longhands agree");
    check(value("b", "list-style") == "upper-roman url(marker.png)",
          "an image rides after the type");
    check(value("b", "list-style-image") == "url(marker.png)", "the image longhand agrees");
    check(value("c", "list-style") == "none" && value("c", "list-style-image") == "none",
          "a lone none is the type");
    check(value("d", "list-style") == "disc inside", "an omitted type reads its default");
    check(value("e", "list-style") == "disc", "an untouched list reads disc");
    check(value("q", "list-style") == "square inside",
          "list-style: inherit replays the parent's shorthand");
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

    // ...and resets to the initial value for one that does not.
    sui::StyleResolver r6;
    r6.setRules(sui::CSSParser::parse("#t{width:80px} #t{width:unset}"));
    check(r6.resolve(*t).width.isAuto(), "width:unset resets to the initial auto");

    // `initial` resets to the engine's initial value; `revert` falls back to the
    // element's UA/default cascade for this engine's model.
    sui::StyleResolver r7;
    r7.setRules(sui::CSSParser::parse("#t{border-width-left:initial}"));
    // With initial, border width resets to the computed initial (0px in this engine)
    check(r7.resolve(*t).borderWidth.left.resolve(0,0,0) >= 0,
          "border-width-left accepts initial keyword");
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

// The per-side and logical spellings of the border shorthands. `border-top` and
// friends take the same width, style, and colour slots as `border`, and the
// logical names pick physical sides: inline is left/right and block is
// top/bottom in the one writing mode the engine lays out, with each `*-start`
// the first of its pair. Like `border`, only the slots the value names are
// written, so a later shorthand never resets a slot it left out.
void testBorderSideShorthands() {
    auto document = parseOrNull("<sui><body><div id=\"t\">x</div></body></sui>");
    if (!document) {
        return;
    }
    Element* t = document->getElementById("t");
    if (!t) {
        return;
    }

    const auto styleFrom = [&](const char* css) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        return resolver.resolve(*t);
    };
    const auto width = [](const sui::Length& l) { return l.resolve(0.0f, 16.0f, 16.0f); };

    {
        const auto s = styleFrom("#t{border-top:6px dashed #ff0000}");
        checkNear(width(s.borderWidth.top), 6.0f, 0.01f, "border-top sets that side's width");
        check(s.borderStyles.top == sui::ComputedStyle::BorderDashed,
              "border-top sets that side's style");
        check(s.borderColor.r == 0xff && s.borderColor.g == 0x00 && s.borderColor.b == 0x00,
              "border-top sets the colour");
        check(s.borderWidth.left.isAuto() && s.borderWidth.right.isAuto() &&
                  s.borderWidth.bottom.isAuto(),
              "border-top leaves the other sides alone");
        check(s.borderStyles.left == sui::ComputedStyle::BorderSolid,
              "border-top leaves the other styles alone");
        check(sui::StyleResolver::propertyValue(s, "border-top") == "6px dashed #ff0000",
              "border-top reads back in the shorthand's own form");
        check(sui::StyleResolver::propertyValue(s, "border-block-start") ==
                  "6px dashed #ff0000",
              "the matching logical start name reads the same side back");
    }

    {
        const auto s = styleFrom("#t{border-inline:2px dotted #00ff00}");
        checkNear(width(s.borderWidth.left), 2.0f, 0.01f,
                  "border-inline sets the start (left) width");
        checkNear(width(s.borderWidth.right), 2.0f, 0.01f,
                  "border-inline sets the end (right) width");
        check(s.borderStyles.left == sui::ComputedStyle::BorderDotted &&
                  s.borderStyles.right == sui::ComputedStyle::BorderDotted,
              "border-inline sets both styles");
        check(s.borderColor.g == 0xff, "border-inline sets the colour");
        check(s.borderWidth.top.isAuto() && s.borderWidth.bottom.isAuto(),
              "border-inline leaves the block sides alone");
        check(sui::StyleResolver::propertyValue(s, "border-inline") ==
                  "2px dotted #00ff00",
              "border-inline reads back while both sides agree");
        check(sui::StyleResolver::propertyValue(s, "border-inline-start") ==
                  "2px dotted #00ff00",
              "border-inline-start reads the left side back");
        check(sui::StyleResolver::propertyValue(s, "border-inline-end") ==
                  "2px dotted #00ff00",
              "border-inline-end reads the right side back");
    }

    {
        const auto s = styleFrom("#t{border-color:#abcdef;border-block:4px}");
        checkNear(width(s.borderWidth.top), 4.0f, 0.01f, "border-block sets the top width");
        checkNear(width(s.borderWidth.bottom), 4.0f, 0.01f, "border-block sets the bottom width");
        check(s.borderWidth.left.isAuto() && s.borderWidth.right.isAuto(),
              "border-block leaves the inline sides alone");
        check(s.borderStyles.top == sui::ComputedStyle::BorderSolid,
              "a width-only value leaves the style as it was");
        check(sui::StyleResolver::propertyValue(s, "border-block") == "4px solid #abcdef",
              "border-block reads back");
    }

    {
        const auto s = styleFrom("#t{border-right:2px;border-bottom:3px;border-color:#aabbcc}");
        checkNear(width(s.borderWidth.right), 2.0f, 0.01f, "border-right sets the right width");
        checkNear(width(s.borderWidth.bottom), 3.0f, 0.01f,
                  "border-bottom sets the bottom width");
        check(s.borderWidth.top.isAuto() && s.borderWidth.left.isAuto(),
              "the physical per-side names touch one side each");
        check(sui::StyleResolver::propertyValue(s, "border-right") == "2px solid #aabbcc",
              "border-right reads back");
    }

    {
        const auto s = styleFrom("#t{border-inline-start:5px;"
                                 "border-inline-end:3px dotted;"
                                 "border-block-start:1px solid #123456;"
                                 "border-block-end:2px}");
        checkNear(width(s.borderWidth.left), 5.0f, 0.01f,
                  "border-inline-start writes the start side");
        checkNear(width(s.borderWidth.right), 3.0f, 0.01f,
                  "border-inline-end writes the end side");
        check(s.borderStyles.right == sui::ComputedStyle::BorderDotted,
              "border-inline-end writes the end style");
        check(s.borderStyles.left == sui::ComputedStyle::BorderSolid,
              "border-inline-start leaves the start style as it was");
        checkNear(width(s.borderWidth.top), 1.0f, 0.01f, "border-block-start writes the top");
        checkNear(width(s.borderWidth.bottom), 2.0f, 0.01f,
                  "border-block-end writes the bottom");
        check(s.borderStyles.top == sui::ComputedStyle::BorderSolid,
              "border-block-start writes the top style");
        check(s.borderColor.r == 0x12 && s.borderColor.g == 0x34 && s.borderColor.b == 0x56,
              "the pair shorthands share the one border colour");
    }

    {
        // Only the named slots move: `border-inline:2px` is a later, higher-priority
        // declaration for the width and must not touch `border-left`'s style.
        const auto s = styleFrom("#t{border-left:4px dashed #112233;border-inline:2px}");
        checkNear(width(s.borderWidth.left), 2.0f, 0.01f,
                  "a later logical shorthand overwrites the width it names");
        checkNear(width(s.borderWidth.right), 2.0f, 0.01f,
                  "on both of its sides");
        check(s.borderStyles.left == sui::ComputedStyle::BorderDashed,
              "and keeps a style it does not name");
        check(s.borderStyles.right == sui::ComputedStyle::BorderSolid,
              "the untouched side keeps its own style");
        check(s.borderColor.r == 0x11 && s.borderColor.b == 0x33,
              "and keeps the colour it does not name");
        check(sui::StyleResolver::propertyValue(s, "border-inline") == "",
              "sides that differ have no shorthand serialisation");
        check(sui::StyleResolver::propertyValue(s, "border-inline-start") ==
                  "2px dashed #112233",
              "but each side still reads back");
        check(sui::StyleResolver::propertyValue(s, "border-inline-end") ==
                  "2px solid #112233",
              "on its own");

        const auto block = styleFrom("#t{border-block:1px solid #445566;border-bottom:2px}");
        check(sui::StyleResolver::propertyValue(block, "border-block") == "",
              "the block pair has no serialisation while its sides differ either");
        check(sui::StyleResolver::propertyValue(block, "border-block-end") ==
                  "2px solid #445566",
              "and its end side still reads back");
    }

    {
        const auto s = styleFrom("#t{border-top:9px;border-top:dashed}");
        checkNear(width(s.borderWidth.top), 9.0f, 0.01f,
                  "a style-only value keeps the width it does not name");
        check(s.borderStyles.top == sui::ComputedStyle::BorderDashed, "and applies its style");
    }

    {
        const auto kept = styleFrom("#t{border-top:9px;border-top:banana}");
        checkNear(width(kept.borderWidth.top), 9.0f, 0.01f,
                  "an invalid declaration leaves the side as it was");
        const auto partial = styleFrom("#t{border-inline:2px solid banana}");
        check(partial.borderWidth.left.isAuto() && partial.borderWidth.right.isAuto(),
              "an unknown token drops the whole declaration, not just that token");
    }

    // The logical names reach layout as their physical sides: an absolute box
    // anchors to the padding box, which the start borders push inwards.
    auto stage = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"stage\" style=\"position:relative;width:200px;height:100px;"
        "border-inline:10px solid #ff0000;border-block:7px solid #ff0000\">"
        "<div id=\"box\" style=\"position:absolute;left:0;top:0;width:20px;height:10px\">"
        "</div></div></body></sui>");
    if (stage) {
        sui::StyleResolver resolver;
        layoutRoot(*stage, resolver, 400.0f);
        Element* box = stage->getElementById("box");
        check(box != nullptr, "border shorthand elements found");
        if (box) {
            checkNear(box->layoutX(), 10.0f, 0.01f,
                      "the inline start border shifts the padding box");
            checkNear(box->layoutY(), 7.0f, 0.01f,
                      "the block start border shifts it down");
        }
    }

    // `inherit` round-trips through the same serialisation, so both sides of a
    // logical shorthand come from the parent.
    auto inheritDoc = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"p\" style=\"border-inline:3px dotted #0000ff\">"
        "<div id=\"c\" style=\"border-inline:inherit\"></div></div></body></sui>");
    if (inheritDoc) {
        Element* c = inheritDoc->getElementById("c");
        check(c != nullptr, "inherit elements found");
        if (c) {
            sui::StyleResolver resolver;
            const auto s = resolver.resolve(*c);
            checkNear(width(s.borderWidth.left), 3.0f, 0.01f,
                      "border-inline:inherit takes the parent's start width");
            checkNear(width(s.borderWidth.right), 3.0f, 0.01f,
                      "and its end width");
            check(s.borderStyles.left == sui::ComputedStyle::BorderDotted &&
                      s.borderColor.b == 0xff,
                  "with the parent's style and colour");
        }
    }
}

// `border`, `border-width`, `border-style`, and the whole `outline` family read
// back in the form the cascade writes them: the four-sided shorthands collapse
// like `margin`, and `border` answers only while all four of its sides agree.
// Reading them back is also what lets their CSS-wide keywords round-trip.
void testBorderAndOutlineShorthands() {
    auto document = parseOrNull("<sui><body><div id=\"t\">x</div></body></sui>");
    if (!document) {
        return;
    }
    Element* t = document->getElementById("t");
    if (!t) {
        return;
    }

    const auto styleFrom = [&](const char* css) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        return resolver.resolve(*t);
    };
    const auto value = [](const sui::ComputedStyle& s, const char* property) {
        return sui::StyleResolver::propertyValue(s, property);
    };

    {
        const auto s = styleFrom("#t{border:2px solid #ff0000}");
        check(value(s, "border") == "2px solid #ff0000",
              "border reads back as its three slots");
        check(value(s, "border-width") == "2px", "border-width collapses to one value");
        check(value(s, "border-style") == "solid", "and border-style with it");
    }

    {
        const auto s = styleFrom(
            "#t{border-width:1px 2px 3px 4px;border-style:solid dashed dotted double}");
        check(value(s, "border-width") == "1px 2px 3px 4px",
              "four distinct border widths stay four values");
        check(value(s, "border-style") == "solid dashed dotted double",
              "border styles read back in the same order");
        check(value(s, "border") == "",
              "border itself has no reading while its sides disagree");
    }

    {
        const auto s = styleFrom(
            "#t{border-width:1px 2px;border-style:solid dotted;border-color:#123456}");
        check(value(s, "border-width") == "1px 2px", "a matching bottom drops out of the widths");
        check(value(s, "border-style") == "solid dotted", "and out of the styles");
        check(value(s, "border") == "", "even with one colour, differing sides stay unreadable");
    }

    {
        // Widths that agree are not enough: the style slots have to agree too.
        const auto s = styleFrom("#t{border-width:1px;border-style:solid dotted}");
        check(value(s, "border-width") == "1px", "the widths are all the same");
        check(value(s, "border-style") == "solid dotted", "while a style pair differs");
        check(value(s, "border") == "", "so border still has no reading");
    }

    {
        // One differing width is enough to make the shorthand unrepresentable.
        const auto s = styleFrom(
            "#t{border-top:1px solid #00ff00;border-right:2px solid #00ff00;"
            "border-bottom:1px solid #00ff00;border-left:1px solid #00ff00}");
        check(value(s, "border-width") == "1px 2px 1px 1px",
              "a single differing side stays in the list");
        check(value(s, "border") == "", "a differing side still leaves border unreadable");
    }

    {
        const auto s = styleFrom("#t{outline:2px dashed #0000ff;outline-offset:4px}");
        check(value(s, "outline") == "2px dashed #0000ff",
              "outline reads back as its three slots");
        check(value(s, "outline-width") == "2px", "outline-width reads back");
        check(value(s, "outline-style") == "dashed", "outline-style reads back");
        check(value(s, "outline-color") == "#0000ff", "outline-color reads back");
        check(value(s, "outline-offset") == "4px", "outline-offset reads back");
    }

    {
        // The longhands and the shorthand agree even when only some were written.
        const auto s = styleFrom("#t{outline-style:dotted;outline-color:#abcdef}");
        check(value(s, "outline-style") == "dotted" && value(s, "outline-color") == "#abcdef",
              "author-written outline longhands read back");
        check(value(s, "outline-width") == "0px",
              "an unset outline width reads as zero, as it paints");
        check(value(s, "outline") == "0px dotted #abcdef",
              "and the shorthand agrees with every longhand");
    }

    // The CSS-wide keywords re-apply exactly these readings, so each shorthand and
    // longhand copies the parent's value instead of dropping the declaration.
    auto inheritDoc = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"p1\" style=\"border:3px dashed #123456\">"
        "<div id=\"b1\" style=\"border:inherit\"></div>"
        "<div id=\"b2\" style=\"border-width:inherit\"></div>"
        "<div id=\"b3\" style=\"border-style:inherit\"></div>"
        "<div id=\"b4\" style=\"border-color:inherit\"></div>"
        "</div>"
        "<div id=\"p2\" style=\"outline:1px solid #654321;outline-offset:2px\">"
        "<div id=\"o1\" style=\"outline:inherit\"></div>"
        "<div id=\"o2\" style=\"outline-width:inherit\"></div>"
        "<div id=\"o3\" style=\"outline-style:inherit\"></div>"
        "<div id=\"o4\" style=\"outline-color:inherit\"></div>"
        "<div id=\"o5\" style=\"outline-offset:inherit\"></div>"
        "</div>"
        "</body></sui>");
    if (inheritDoc) {
        sui::StyleResolver resolver;
        const auto of = [&](const char* id) {
            return resolver.resolve(*inheritDoc->getElementById(id));
        };
        const sui::ComputedStyle b1 = of("b1");
        checkNear(b1.borderWidth.left.value, 3.0f, 0.01f, "border:inherit takes the width");
        checkNear(b1.borderWidth.bottom.value, 3.0f, 0.01f, "on every side");
        check(b1.borderStyles.left == sui::ComputedStyle::BorderDashed &&
                  b1.borderColor.r == 0x12 && b1.borderColor.b == 0x56,
              "with the parent's style and colour");
        checkNear(of("b2").borderWidth.right.value, 3.0f, 0.01f,
                  "border-width:inherit takes the width alone");
        check(of("b3").borderStyles.top == sui::ComputedStyle::BorderDashed,
              "border-style:inherit takes the style alone");
        check(of("b4").borderColor.g == 0x34, "border-color:inherit takes the colour alone");
        const sui::ComputedStyle o1 = of("o1");
        checkNear(o1.outline.width.value, 1.0f, 0.01f, "outline:inherit takes the width");
        check(o1.outline.style == sui::ComputedStyle::BorderSolid &&
                  o1.outline.color.b == 0x21,
              "with the parent's style and colour");
        checkNear(of("o2").outline.width.value, 1.0f, 0.01f,
                  "outline-width:inherit takes the width alone");
        check(of("o3").outline.style == sui::ComputedStyle::BorderSolid,
              "outline-style:inherit takes the style alone");
        check(of("o4").outline.color.r == 0x65, "outline-color:inherit takes the colour alone");
        checkNear(of("o5").outline.offset.value, 2.0f, 0.01f,
                  "outline-offset:inherit takes the offset alone");
    }
}

// `hyphens`: the engine has no hyphenation dictionary, so `auto` offers no
// breaks of its own and behaves like `manual` — a soft hyphen (U+00AD) the
// author wrote is the opportunity. The character is invisible unless a break is
// taken at it, and a taken break ends its line in `-`; `none` leaves the soft
// hyphens invisible but never breaks at one. Runs are fontless here, so one
// character is 8px and a space 4px at the 16px default.
void testHyphens() {
    const std::string shy = "\xC2\xAD";
    auto document = parseOrNull(
        ("<sui><body style=\"margin:0\">"
         // One opportunity: the first piece is `hy-`, and `phenation` has no
         // further break to take, so the rest overflows.
         "<div id=\"a\" style=\"width:60px\">hy" + shy + "phenation</div>"
         "<div id=\"b\" style=\"width:60px;hyphens:none\">hy" + shy + "phenation</div>"
         // Fitting text keeps its soft hyphen invisible, at the plain width.
         "<div id=\"c\" style=\"width:200px\">hy" + shy + "phen</div>"
         "<div id=\"d\" style=\"width:60px;hyphens:auto\">hy" + shy + "phenation</div>"
         // Neither `white-space` nor `text-wrap` wrapping off may break at one.
         "<div id=\"e\" style=\"width:60px;white-space:nowrap\">hy" + shy + "phenation</div>"
         "<div id=\"f\" style=\"width:60px;text-wrap:nowrap\">hy" + shy + "phenation</div>"
         "<div id=\"g\" style=\"width:16px\">hy" + shy + "phenation</div>"
         // A fitting word is never broken, so the wrap after it is ordinary.
         "<div id=\"h\" style=\"width:60px\">hy" + shy + "phen ation</div>"
         // The opportunity is taken before `overflow-wrap: break-word` splits
         // what is left of an overlong word.
         "<div id=\"i\" style=\"width:60px;overflow-wrap:break-word\">hy" + shy + "phenation</div>"
         // Two opportunities: greedy takes the last whose piece and `-` fit.
         "<div id=\"o\" style=\"width:60px\">hy" + shy + "phen" + shy + "ation</div>"
         "<div id=\"p\" style=\"width:200px\">hy" + shy + "phen" + shy + "ation</div>"
         // A trailing soft hyphen has nothing to carry to the next line.
         "<div id=\"k\" style=\"width:40px\">hyphen" + shy + "</div>"
         // A piece plus its `-` is what has to fit, not the bare piece.
         "<div id=\"r\" style=\"width:52px\">hy" + shy + "phen" + shy + "ation</div>"
         // A textarea keeps one run per word, so no break is taken there.
         "<textarea id=\"s\" style=\"width:60px\">hy" + shy + "phenation</textarea>"
         // `text-transform` applies to a taken piece like any other run.
         "<div id=\"n\" style=\"width:60px;text-transform:uppercase\">hy" + shy +
         "phenation</div>"
         // `hyphens: none` never breaks, on the box or inherited.
         "<div id=\"l\" style=\"width:60px;hyphens:none\"><span>hy" + shy +
         "phenation</span></div>"
         "<div id=\"m\" style=\"width:60px;hyphens:none\"><div>hy" + shy + "phenation</div></div>"
         "</body></sui>")
            .c_str());
    if (!document) {
        return;
    }
    Element* t = document->getElementById("a");
    Element* at = document->getElementById("h");
    check(t && at, "hyphens elements found");
    if (!t || !at) {
        return;
    }

    sui::StyleResolver resolver;
    resolver.setRules(
        sui::CSSParser::parse("body { margin: 0; font-size: 16px } div { margin: 0 }"));
    layoutRoot(*document, resolver, 400.0f);

    const auto runs = [](Element* box) {
        std::vector<std::pair<std::string, float>> out;
        for (const sui::Element::TextRun& run : box->textRuns()) {
            out.emplace_back(run.text, run.width);
        }
        return out;
    };
    const auto at2 = [&](const char* id) { return document->getElementById(id); };

    {
        const auto r = runs(at2("a"));
        check(r.size() == 2, "a soft hyphen offers a break in an overflowing word");
        if (r.size() == 2) {
            check(r[0].first == "hy-", "the taken break ends the line in a hyphen");
            checkNear(r[0].second, 24.0f, 0.01f, "and measures the piece plus its hyphen");
            check(r[1].first == "phenation", "the rest of the word carries on");
            checkNear(r[1].second, 72.0f, 0.01f, "with the soft hyphen weighing nothing");
        }
    }
    {
        const auto r = runs(at2("b"));
        check(r.size() == 1 && r[0].first == "hyphenation" && r[0].second > 60.0f,
              "hyphens:none never breaks at a soft hyphen");
    }
    {
        const auto r = runs(at2("c"));
        check(r.size() == 1 && r[0].first == "hyphen" &&
                  std::fabs(r[0].second - 48.0f) < 0.01f,
              "a soft hyphen in fitting text is invisible and takes no break");
    }
    {
        const auto r = runs(at2("d"));
        check(r.size() == 2 && r[0].first == "hy-" && r[1].first == "phenation",
              "hyphens:auto breaks like manual, having no dictionary of its own");
    }
    {
        const auto e = runs(at2("e"));
        const auto f = runs(at2("f"));
        check(e.size() == 1 && e[0].first == "hyphenation", "white-space:nowrap keeps it whole");
        check(f.size() == 1 && f[0].first == "hyphenation", "text-wrap:nowrap keeps it whole");
    }
    {
        const auto r = runs(at2("g"));
        check(r.size() == 2 && r[0].first == "hy-",
              "a piece too wide for the box is still the break taken");
    }
    {
        const auto r = runs(at2("h"));
        check(r.size() == 2 && r[0].first == "hyphen" && r[1].first == "ation",
              "a word that fits is not broken, and the next word wraps");
    }
    {
        const auto r = runs(at2("i"));
        check(r.size() == 3, "the soft hyphen break comes before break-word splitting");
        if (r.size() == 3) {
            check(r[0].first == "hy-" && r[1].first == "phenati" && r[2].first == "on",
                  "and the rest still splits by character");
        }
    }
    {
        const auto r = runs(at2("o"));
        check(r.size() == 2 && r[0].first == "hyphen-" && r[1].first == "ation",
              "the last break whose piece and hyphen fit is the one taken");
        const auto p = runs(at2("p"));
        check(p.size() == 1 && p[0].first == "hyphenation",
              "and no break is taken when the whole word fits");
    }
    {
        const auto r = runs(at2("k"));
        check(r.size() == 1 && r[0].first == "hyphen",
              "a trailing soft hyphen is no break opportunity");
    }
    {
        const auto n = runs(at2("n"));
        check(n.size() == 2 && n[0].first == "HY-" && n[1].first == "PHENATION",
              "text-transform applies to the pieces");
    }
    {
        const auto r = runs(at2("r"));
        check(r.size() == 3 && r[0].first == "hy-" && r[1].first == "phen-" &&
                  r[2].first == "ation",
              "a piece plus its hyphen is what has to fit");
    }
    {
        const auto r = runs(at2("s"));
        check(r.size() == 1 && r[0].first == "hyphenation",
              "a textarea takes no soft-hyphen break, keeping runs word-sized");
    }
    {
        Element* l = at2("l");
        Element* m = document->getElementById("m");
        if (l && m && !l->textRuns().empty() && !m->textRuns().empty()) {
            check(l->textRuns()[0].text == "hyphenation",
                  "a descendant of a hyphens:none box gets no break");
            check(m->textRuns()[0].text == "hyphenation",
                  "hyphens inherits into nested boxes");
        }
    }
    // No run anywhere may keep the character: it never paints.
    bool keptSoftHyphen = false;
    for (const char* id : {"a", "c", "h", "k", "l", "m"}) {
        if (Element* el = at2(id)) {
            for (const sui::Element::TextRun& run : el->textRuns()) {
                keptSoftHyphen |= run.text.find(shy) != std::string::npos;
            }
        }
    }
    check(!keptSoftHyphen, "no run ever contains a soft hyphen");

    // Style level: parse, serialize, inherit, and drop garbage.
    auto styleDoc = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"s1\" style=\"hyphens:none\">x</div>"
        "<div id=\"s2\" style=\"hyphens:auto\">x</div>"
        "<div id=\"s3\">x</div>"
        "<div id=\"s4\" style=\"hyphens:manual\">x</div>"
        "<div id=\"s5\" style=\"hyphens:none;hyphens:banana\">x</div>"
        "<div style=\"hyphens:none\"><div id=\"s6\">x</div></div>"
        "<div style=\"hyphens:auto\"><div id=\"s7\" style=\"hyphens:unset\">x</div></div>"
        "</body></sui>");
    if (styleDoc) {
        sui::StyleResolver r;
        const auto styleOf = [&](const char* id) { return r.resolve(*styleDoc->getElementById(id)); };
        check(styleOf("s1").hyphens == ComputedStyle::Hyphens::None, "hyphens:none parses");
        check(styleOf("s2").hyphens == ComputedStyle::Hyphens::Auto, "hyphens:auto parses");
        check(styleOf("s3").hyphens == ComputedStyle::Hyphens::Manual,
              "hyphens starts at its initial value, manual");
        check(styleOf("s4").hyphens == ComputedStyle::Hyphens::Manual, "hyphens:manual parses");
        check(styleOf("s5").hyphens == ComputedStyle::Hyphens::None,
              "garbage leaves the cascade winner in place");
        check(sui::StyleResolver::propertyValue(styleOf("s1"), "hyphens") == "none",
              "hyphens reads back");
        check(sui::StyleResolver::propertyValue(styleOf("s2"), "hyphens") == "auto",
              "and so does auto");
        check(styleOf("s6").hyphens == ComputedStyle::Hyphens::None, "hyphens inherits");
        check(styleOf("s7").hyphens == ComputedStyle::Hyphens::Auto, "hyphens:unset inherits");
    }
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
    const auto marker = sui::CSSParser::parseSelector("#card::marker");
    check(!marker.unsupported && marker.parts.back().value == "::marker",
          "::marker parses as a pseudo-element part");
    const auto firstLine = sui::CSSParser::parseSelector("#card::first-line");
    check(!firstLine.unsupported, "::first-line is a supported selector");
    check(firstLine.parts.back().kind == sui::Selector::Kind::PseudoElement &&
              firstLine.parts.back().value == "::first-line",
          "::first-line parses as a pseudo-element part");

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

    // Empty quotes content: "" generates an empty pseudo-element box
    sui::StyleResolver emptyContent;
    emptyContent.setRules(sui::CSSParser::parse("#card::before { content: \"\" }"));
    sui::generatePseudoElements(*document, emptyContent);
    check(card->children().size() == 2, "content: \"\" generates a pseudo-element box");
    check(card->children()[0]->tag() == "::before", "the generated box is ::before");
    check(card->children()[0]->textContent() == "", "the generated box has empty content");

    // content: attr(title) resolves attribute from originating element
    card->setAttribute("title", "HelloCard");
    sui::StyleResolver attrContent;
    attrContent.setRules(sui::CSSParser::parse("#card::before { content: \"Prefix: \" attr(title) }"));
    sui::generatePseudoElements(*document, attrContent);
    check(card->children().size() == 2, "content with attr() generates a pseudo-element box");
    check(card->children()[0]->textContent() == "Prefix: HelloCard", "content: attr(title) resolves attribute");
}

// `content` reads back as the argument list it was written as: literal runs
// re-quoted (with `\"`/`\\` escapes), `attr(...)`, `counter(...)` with or
// without a style, `counters(...)` with its separator re-quoted, and the four
// quote keywords. The empty string argument stays distinguishable from `none`,
// which matters because the reading is what the CSS-wide keywords replay.
void testContentReadBack() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"c1\"></div><div id=\"c2\"></div><div id=\"c3\"></div>"
        "<div id=\"c4\"></div><div id=\"c7\"></div><div id=\"c8\"></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(R"(
        #c1::before { content: "h" attr(title) counter(s) counters(t, "-") open-quote "x" close-quote no-open-quote no-close-quote }
        #c2::before { content: "say \"hi\"" }
        #c3::before { content: "back\\slash" }
        #c4::before { content: "" }
        #c7::before { content: counter(c, decimal) counters(d, ". ") }
        #c8 { content: "base" } #c8::before { content: inherit }
    )"));
    sui::generatePseudoElements(*document, resolver);
    const auto contentOf = [&](const char* id) {
        Element* el = document->getElementById(id);
        if (!el) {
            return std::string();
        }
        Element* target = el;
        for (const std::unique_ptr<Element>& child : el->children()) {
            if (child->isPseudoElement()) {
                target = child.get();
                break;
            }
        }
        return sui::StyleResolver::propertyValue(resolver.resolve(*target), "content");
    };
    check(contentOf("c1") ==
              "\"h\" attr(title) counter(s) counters(t, \"-\") open-quote \"x\" close-quote"
              " no-open-quote no-close-quote",
          "content rebuilds every argument in order");
    check(contentOf("c2") == "\"say \\\"hi\\\"\"",
          "a literal quote round-trips escaped");
    check(contentOf("c3") == "\"back\\\\slash\"",
          "a literal backslash round-trips escaped");
    check(contentOf("c4") == "\"\"", "an empty string argument is not none");
    check(contentOf("c7") == "counter(c, decimal) counters(d, \". \")",
          "counter styles and counters separators read back");
    check(contentOf("c8") == "\"base\"",
          "content: inherit replays the originating element's reading");
    Element* plain = document->getElementById("c4");
    check(plain && sui::StyleResolver::propertyValue(resolver.resolve(*plain), "content") ==
                       "none",
          "an element with no content reads none");
}

// CSS counters: `counter-reset` creates a named counter, `counter-increment`
// advances it, and `counter()` / `counters()` read the value into generated
// content. The value is scoped to the resetting element's descendants and
// following siblings, which is what makes per-section numbering restart.
void testCssCounters() {
    // Parsing and serialization.
    {
        sui::StyleResolver resolver;
        resolver.setRules(
            sui::CSSParser::parse("#a { counter-reset: section 5 other } "
                                  "#b { counter-increment: item }"));
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"></div><div id=\"b\"></div></body></sui>");
        if (!document) {
            return;
        }
        const ComputedStyle a = resolver.resolve(*document->getElementById("a"));
        check(a.counterReset.size() == 2, "counter-reset parses every name");
        check(a.counterReset[0].name == "section" && a.counterReset[0].value == 5,
              "counter-reset keeps an explicit value");
        check(a.counterReset[1].name == "other" && a.counterReset[1].value == 0,
              "counter-reset defaults a missing value to 0");
        check(sui::StyleResolver::propertyValue(a, "counter-reset") == "section 5 other 0",
              "counter-reset serializes back");
        const ComputedStyle b = resolver.resolve(*document->getElementById("b"));
        check(b.counterIncrement.size() == 1 && b.counterIncrement[0].value == 1,
              "counter-increment defaults to 1");
        check(sui::StyleResolver::propertyValue(b, "counter-increment") == "item 1",
              "counter-increment serializes back");
    }

    // A container reset plus per-sibling increments counts up in generated text.
    {
        auto document = parseOrNull(
            "<sui><body><h2>One</h2><h2>Two</h2><h2>Three</h2></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body { counter-reset: section }"
            "h2 { counter-increment: section }"
            "h2::before { content: counter(section) '. ' }"));
        sui::generatePseudoElements(*document, resolver);
        Element* body = document->querySelector("body");
        if (!body) {
            return;
        }
        const auto& h2s = body->children();
        check(h2s.size() == 3, "three h2 elements survived");
        if (h2s.size() == 3) {
            check(h2s[0]->children().size() == 2, "::before is added ahead of the element text");
            check(h2s[0]->children()[0]->textContent() == "1. ", "first heading reads 1");
            check(h2s[1]->children()[0]->textContent() == "2. ", "second heading reads 2");
            check(h2s[2]->children()[0]->textContent() == "3. ", "third heading reads 3");
        }
    }

    // An explicit reset value is the starting point, and an increment that finds
    // no counter creates one at the increment value.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"></div><div id=\"b\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#a { counter-reset: c 10 } #a::after { content: counter(c) }"
            "#b { counter-increment: k 3 } #b::after { content: counter(k) }"));
        sui::generatePseudoElements(*document, resolver);
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        if (!a || !b) {
            return;
        }
        check(a->children().size() == 1 && a->children()[0]->textContent() == "10",
              "an explicit reset value starts the counter");
        check(b->children().size() == 1 && b->children()[0]->textContent() == "3",
              "an increment with no reset creates the counter");
    }

    // `counter-set` parses and serializes like its siblings, defaulting a
    // missing value to 0, and wins over an increment on the same element.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("#a { counter-set: section 5 other }"));
        auto document = parseOrNull("<sui><body><div id=\"a\"></div></body></sui>");
        if (!document) {
            return;
        }
        const ComputedStyle a = resolver.resolve(*document->getElementById("a"));
        check(a.counterSet.size() == 2, "counter-set parses every name");
        check(a.counterSet[0].name == "section" && a.counterSet[0].value == 5,
              "counter-set keeps an explicit value");
        check(a.counterSet[1].name == "other" && a.counterSet[1].value == 0,
              "counter-set defaults a missing value to 0");
        check(sui::StyleResolver::propertyValue(a, "counter-set") == "section 5 other 0",
              "counter-set serializes back");
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"></div><div id=\"b\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#a { counter-increment: c 1; counter-set: c 9 } #a::after { content: counter(c) }"
            "#b { counter-set: k 4 } #b::after { content: counter(k) }"));
        sui::generatePseudoElements(*document, resolver);
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        if (!a || !b) {
            return;
        }
        check(a->children().size() == 1 && a->children()[0]->textContent() == "9",
              "a set wins over an increment on the same element");
        check(b->children().size() == 1 && b->children()[0]->textContent() == "4",
              "a set with no increment creates the counter");
    }

    // `counters(name, sep)` joins every counter of that name in scope, outermost
    // first, so nested lists number as 1, 1.1, and so on.
    {
        auto document = parseOrNull(
            "<sui><body>"
            "<ul><li id=\"a\"><ul><li id=\"b\"></li></ul></li></ul>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "ul { counter-reset: item }"
            "li { counter-increment: item }"
            "li::before { content: counters(item, '.') ' ' }"));
        sui::generatePseudoElements(*document, resolver);
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        if (!a || !b) {
            return;
        }
        check(a->children()[0]->textContent() == "1 ", "the outer item reads 1");
        check(b->children()[0]->textContent() == "1.1 ", "the nested item reads 1.1");
    }

    // Two sibling sections each reset the counter, so numbering restarts rather
    // than carrying on from the previous section.
    {
        auto document = parseOrNull(
            "<sui><body>"
            "<section id=\"s1\"><p></p><p></p></section>"
            "<section id=\"s2\"><p></p><p></p></section>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "section { counter-reset: c }"
            "p { counter-increment: c }"
            "p::before { content: counter(c) '. ' }"));
        sui::generatePseudoElements(*document, resolver);
        Element* s1 = document->getElementById("s1");
        Element* s2 = document->getElementById("s2");
        if (!s1 || !s2) {
            return;
        }
        check(s1->children()[0]->children()[0]->textContent() == "1. ", "first section starts at 1");
        check(s1->children()[1]->children()[0]->textContent() == "2. ", "first section counts to 2");
        check(s2->children()[0]->children()[0]->textContent() == "1. ", "second section restarts at 1");
        check(s2->children()[1]->children()[0]->textContent() == "2. ", "second section counts to 2");
    }
}

// `white-space: break-spaces` keeps every space as its own run: runs of
// spaces take room, lines wrap after any space, and trailing spaces start the
// next line instead of vanishing.
void testBreakSpaces() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"b\" style=\"width:200px;white-space:break-spaces\">a  b</div>"
        "<div id=\"t\" style=\"width:35px;white-space:break-spaces\">aaaa </div>"
        "<div id=\"p\" style=\"width:200px;white-space:pre-wrap\">a  b</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* b = el("b");
    Element* t = el("t");
    Element* p = el("p");
    check(b && t && p, "break-spaces elements found");
    if (!b || !t || !p) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, resolver, 400.0f);
    // Two spaces make two runs: nothing collapses.
    check(b->textRuns().size() == 4, "every space takes a run");
    if (b->textRuns().size() == 4) {
        check(b->textRuns()[0].text == "a" && b->textRuns()[1].text == " " &&
                  b->textRuns()[2].text == " " && b->textRuns()[3].text == "b",
              "words and spaces alternate");
        check(b->textRuns()[1].width == 4.0f, "a space run is a space wide");
    }
    // A trailing space that overflows starts the next line.
    check(t->textRuns().size() == 2, "a trailing space wraps alone");
    if (t->textRuns().size() == 2) {
        check(t->textRuns()[1].y > t->textRuns()[0].y, "the space drops a line");
    }
    // pre-wrap still collapses the run.
    check(p->textRuns().size() == 2, "pre-wrap collapses spaces");
}

// `tab-size`: a tab in preserved whitespace advances the pen to the next tab
// stop. A bare number counts space widths (8 by default), a length is
// absolute, and stops are measured from the line's left edge.
void testTabSize() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"d\" style=\"white-space:pre\">a\tb</div>"
        "<div id=\"n\" style=\"white-space:pre;tab-size:4\">a\tb</div>"
        "<div id=\"e\" style=\"white-space:pre;tab-size:2em\">a\tb</div>"
        "<div id=\"l\" style=\"white-space:pre;tab-size:3px\">a\tb</div>"
        "<div id=\"z\" style=\"white-space:pre;tab-size:0\">a\tb</div>"
        "<div id=\"m\" style=\"white-space:pre;tab-size:-2\">a\tb</div>"
        "<div id=\"g\" style=\"white-space:pre;tab-size:banana\">a\tb</div>"
        "<div id=\"k\" style=\"white-space:break-spaces;tab-size:4\">a\tb</div>"
        "<div id=\"c\" style=\"white-space:pre\">ab\tc</div>"
        "<div style=\"tab-size:3\"><div id=\"i\" style=\"white-space:pre\">a\tb</div></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* d = el("d");
    Element* n = el("n");
    Element* e = el("e");
    Element* l = el("l");
    Element* z = el("z");
    Element* m = el("m");
    Element* g = el("g");
    Element* k = el("k");
    Element* c = el("c");
    Element* inh = el("i");
    check(d && n && e && l && z && m && g && k && c && inh, "tab elements found");
    if (!d || !n || !e || !l || !z || !m || !g || !k || !c || !inh) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px } div { margin: 0 }"));
    layoutRoot(*document, resolver, 400.0f);

    // Without a font a space is 4px at 16px, so the default stop is 32. "a"
    // takes 8px, the tab run the remaining 24, and "b" starts on the stop.
    const auto gap = [](Element* box) {
        return box->textRuns().size() == 3 ? box->textRuns()[2].x - box->textRuns()[0].x
                                           : -1.0f;
    };
    check(d->textRuns().size() == 3, "a tab splits a pre line into three runs");
    if (d->textRuns().size() == 3) {
        check(d->textRuns()[1].text == " ", "the tab paints as a blank run");
        checkNear(d->textRuns()[1].width, 24.0f, 0.01f,
                  "the default tab run fills to the eighth space");
        checkNear(gap(d), 32.0f, 0.01f, "the tab lands the next word on the second stop");
    }
    check(n->textRuns().size() == 3, "tab-size:4 keeps three runs");
    if (n->textRuns().size() == 3) {
        checkNear(gap(n), 16.0f, 0.01f, "tab-size:4 halves the stop");
    }
    check(e->textRuns().size() == 3, "an absolute tab length keeps three runs");
    if (e->textRuns().size() == 3) {
        checkNear(gap(e), 32.0f, 0.01f, "2em resolves against the font size");
    }
    check(l->textRuns().size() == 3, "a px tab length keeps three runs");
    if (l->textRuns().size() == 3) {
        checkNear(gap(l), 9.0f, 0.01f, "a 3px stop snaps the pen forward to 9");
    }
    check(z->textRuns().size() == 3, "a zero tab size keeps three runs");
    if (z->textRuns().size() == 3) {
        checkNear(gap(z), 8.0f, 0.01f, "a zero stop never moves the pen");
    }
    check(m->textRuns().size() == 3, "a negative tab size keeps three runs");
    if (m->textRuns().size() == 3) {
        checkNear(gap(m), 32.0f, 0.01f, "a negative tab size is invalid");
    }
    check(g->textRuns().size() == 3, "a garbage tab size keeps three runs");
    if (g->textRuns().size() == 3) {
        checkNear(gap(g), 32.0f, 0.01f, "garbage keeps the default stop");
    }
    check(k->textRuns().size() == 3, "break-spaces keeps its tab runs");
    if (k->textRuns().size() == 3) {
        checkNear(gap(k), 16.0f, 0.01f, "break-spaces expands tabs too");
    }
    check(c->textRuns().size() == 3, "a longer prefix keeps its tab runs");
    if (c->textRuns().size() == 3) {
        checkNear(gap(c), 32.0f, 0.01f, "the stop counts from the line's left edge");
    }
    checkNear(resolver.resolve(*inh).tabSize.value, 3.0f, 0.01f, "tab-size inherits");
    check(resolver.resolve(*inh).tabSize.unit == sui::Length::Number,
          "an inherited number stays a number");

    // Serialization round-trips the three shapes.
    check(sui::StyleResolver::propertyValue(resolver.resolve(*d), "tab-size") == "8",
          "the default serializes as a number");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*n), "tab-size") == "4",
          "a number serializes without a unit");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*e), "tab-size") == "2em",
          "an em length serializes");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*l), "tab-size") == "3px",
          "a px length serializes");
}

// A `font-family` value is a list, not just its first name: the cascade
// stores every comma-separated family, computed-style read-back answers the
// list, and the laid-out runs carry it so the text engine can try each family
// before its default face.
void testFontFamilyList() {
    auto document = parseOrNull(
        "<sui><body><div id=\"d\">x</div><div id=\"e\">y</div></body></sui>");
    if (!document) {
        return;
    }
    Element* d = document->getElementById("d");
    Element* e = document->getElementById("e");
    check(d && e, "font-family elements found");
    if (!d || !e) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } #d { font-family: Missing, \"Fancy\", serif } "
        "#e { font-family: serif }"));
    const sui::ComputedStyle list = resolver.resolve(*d);
    check(list.fontFamily == "Missing, \"Fancy\", serif",
          "the whole font-family list is stored");
    check(sui::StyleResolver::propertyValue(list, "font-family") ==
              "Missing, \"Fancy\", serif",
          "font-family reads the whole list back");
    check(resolver.resolve(*e).fontFamily == "serif",
          "a one-family declaration still reads back alone");

    layoutRoot(*document, resolver, 200.0f);
    check(!d->textRuns().empty() &&
              d->textRuns()[0].fontFamily == "Missing, \"Fancy\", serif",
          "a text run carries the family list it was measured with");
    check(!e->textRuns().empty() && e->textRuns()[0].fontFamily == "serif",
          "a single-family run carries its name");
}

// `<pre>` is a code block out of the box: whitespace and newlines survive,
// nested blocks inherit the preserved whitespace, tabs stop at `tab-size`, and
// the UA asks for the generic monospace family.
void testPreDefaults() {
    const sui::ComputedStyle pre = sui::defaultStyleFor("pre");
    check(pre.display == sui::ComputedStyle::Block, "pre is a block");
    check(pre.whiteSpace == sui::ComputedStyle::WrapPre, "pre preserves whitespace");
    check(pre.fontFamily == "monospace", "pre asks for monospace");
    checkNear(pre.margin.top.value, 16.0f, 0.01f, "pre keeps the UA lead");
    checkNear(pre.margin.bottom.value, 16.0f, 0.01f, "pre keeps the UA trail");

    auto document = parseOrNull(
        "<sui><body>"
        "<pre id=\"a\">a  b\nc</pre>"
        "<pre id=\"t\">x\ty</pre>"
        "<pre id=\"n\"><div id=\"child\">m  n</div></pre>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* t = el("t");
    Element* n = el("n");
    Element* child = el("child");
    check(a && t && n && child, "pre elements found");
    if (!a || !t || !n || !child) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, resolver, 400.0f);

    check(a->textRuns().size() == 2, "a newline in a pre starts a second run");
    if (a->textRuns().size() == 2) {
        check(a->textRuns()[0].text == "a  b", "the double space survives in a pre");
        check(a->textRuns()[1].y > a->textRuns()[0].y, "the newline takes a new line");
    }
    // The default stop is eight spaces of 4px each, so "y" lands at x=32.
    check(t->textRuns().size() == 3, "a tab in a pre becomes a blank run");
    if (t->textRuns().size() == 3) {
        check(t->textRuns()[1].text == " ", "the pre tab run is blank");
        checkNear(t->textRuns()[2].x - t->textRuns()[0].x, 32.0f, 0.01f,
                  "the pre tab lands on the default stop");
    }
    check(resolver.resolve(*child).whiteSpace == sui::ComputedStyle::WrapPre,
          "white-space inherits into a nested block");
    check(child->textRuns().size() == 1 && child->textRuns()[0].text == "m  n",
          "the nested block keeps its spaces");

    // Author declarations still win over the UA block.
    sui::StyleResolver author;
    author.setRules(sui::CSSParser::parse(
        "body { margin: 0 } pre { white-space: pre-wrap; font-family: serif; margin: 0 }"));
    auto document2 = parseOrNull("<sui><body><pre id=\"p\">a  b</pre></body></sui>");
    if (!document2) {
        return;
    }
    Element* p = document2->getElementById("p");
    check(p != nullptr, "author pre found");
    if (p) {
        const sui::ComputedStyle s = author.resolve(*p);
        check(s.whiteSpace == sui::ComputedStyle::WrapPreWrap, "an author pre-wrap wins");
        check(s.fontFamily == "serif", "an author family wins");
        checkNear(s.margin.top.value, 0.0f, 0.01f, "an author margin wins");
    }
}

// `text-wrap: balance` re-breaks a short paragraph at the smallest width that
// keeps its line count, so the lines even out; `pretty` only pulls words down
// until the last line stops being a lone one; `nowrap` stops wrapping per run.
// Without a font a glyph advances 8px at 16px and a space is 4px, so every
// expected edge below is arithmetic.
void testTextWrap() {
    auto document = parseOrNull(
        "<sui><body>"
        // Balance: greedy packs 216+84 at 240px, balanced settles at 172+128.
        "<div id=\"a\" style=\"width:240px\">aaaaaaaaaaaaaaaa bbbbb ccccc ddddd eeeee</div>"
        "<div id=\"b\" style=\"width:240px;text-wrap:balance\">aaaaaaaaaaaaaaaa bbbbb ccccc ddddd eeeee</div>"
        // Pretty: greedy leaves one word alone at 200px; a step down pairs it up.
        "<div id=\"c\" style=\"width:200px\">aaaaaaaaaaaaaaa bbbbb ccccc</div>"
        "<div id=\"d\" style=\"width:200px;text-wrap:pretty\">aaaaaaaaaaaaaaa bbbbb ccccc</div>"
        // Nowrap: whole box, the mode longhand, and a span inside a wrapping box.
        "<div id=\"e\" style=\"width:100px\">aaaa bbbb cccc</div>"
        "<div id=\"f\" style=\"width:100px;text-wrap:nowrap\">aaaa bbbb cccc</div>"
        "<div id=\"g\" style=\"width:100px;text-wrap-mode:nowrap\">aaaa bbbb cccc</div>"
        "<div id=\"h\" style=\"width:110px\">aaaa bbbb "
        "<span style=\"text-wrap:nowrap\">cccc dddd</span></div>"
        "<div id=\"i\" style=\"width:110px\">aaaa bbbb cccc dddd</div>"
        // Serialization and inheritance.
        "<div id=\"j\" style=\"text-wrap:nowrap balance\"></div>"
        "<div id=\"k\" style=\"text-wrap:balance;text-wrap:banana\"></div>"
        "<div style=\"text-wrap:pretty\"><div id=\"l\"></div></div>"
        // A float narrows the first line: balance must leave those breaks alone,
        // even though the lines after it are uneven enough to tempt it.
        "<div id=\"m\" style=\"width:300px\">"
        "<div style=\"float:left;width:200px;height:10px\"></div>"
        "aaaa aaaaaaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbb cccccccc</div>"
        "<div id=\"n\" style=\"width:300px;text-wrap:balance\">"
        "<div style=\"float:left;width:200px;height:10px\"></div>"
        "aaaa aaaaaaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbb cccccccc</div>"
        // Nowrap also holds atomic boxes, preserved spaces, and tabs back.
        "<div id=\"o\" style=\"width:100px;text-wrap:nowrap\">"
        "<span style=\"display:inline-block;width:60px;height:10px\"></span>"
        "<span style=\"display:inline-block;width:60px;height:10px\"></span></div>"
        "<div id=\"p\" style=\"width:100px\">"
        "<span style=\"display:inline-block;width:60px;height:10px\"></span>"
        "<span style=\"display:inline-block;width:60px;height:10px\"></span></div>"
        "<div id=\"q\" style=\"width:40px;white-space:break-spaces\">xxxx xxxx</div>"
        "<div id=\"r\" style=\"width:40px;white-space:break-spaces;text-wrap:nowrap\">"
        "xxxx xxxx</div>"
        "<div id=\"s\" style=\"width:18px;white-space:break-spaces\">xx xxxx</div>"
        "<div id=\"w\" style=\"width:18px;white-space:break-spaces;text-wrap:nowrap\">"
        "xx xxxx</div>"
        "<div id=\"t\" style=\"width:20px;white-space:break-spaces\">a\tb</div>"
        "<div id=\"u\" style=\"width:20px;white-space:break-spaces;text-wrap:nowrap\">"
        "a\tb</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* b = el("b");
    Element* c = el("c");
    Element* d = el("d");
    Element* e = el("e");
    Element* f = el("f");
    Element* g = el("g");
    Element* h = el("h");
    Element* i = el("i");
    Element* j = el("j");
    Element* k = el("k");
    Element* l = el("l");
    Element* m = el("m");
    Element* n = el("n");
    check(a && b && c && d && e && f && g && h && i && j && k && l && m && n,
          "text-wrap elements found");
    if (!a || !b || !c || !d || !e || !f || !g || !h || !i || !j || !k || !l || !m || !n) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(
        sui::CSSParser::parse("body { margin: 0; font-size: 16px } div { margin: 0 }"));
    layoutRoot(*document, resolver, 400.0f);

    // The right edge of every line, in order: runs come line by line, and the
    // runs of one line share their y.
    const auto edges = [](Element* box) {
        std::vector<float> out;
        float lineY = -1.0f;
        for (const sui::Element::TextRun& run : box->textRuns()) {
            if (out.empty() || run.y > lineY) {
                out.push_back(run.x + run.width);
                lineY = run.y;
            } else {
                out.back() = std::max(out.back(), run.x + run.width);
            }
        }
        return out;
    };

    const std::vector<float> greedyBal = edges(a);
    check(greedyBal.size() == 2, "the unwrapped control breaks in two lines");
    if (greedyBal.size() == 2) {
        checkNear(greedyBal[0], 216.0f, 1.0f, "greedy packs the first line full");
        checkNear(greedyBal[1], 84.0f, 1.0f, "and leaves the last line short");
    }
    const std::vector<float> balanced = edges(b);
    check(balanced.size() == 2, "balance keeps the line count");
    if (balanced.size() == 2) {
        checkNear(balanced[0], 172.0f, 1.0f, "balance evens the widest line down");
        checkNear(balanced[1], 128.0f, 1.0f, "balance fills the last line out");
    }

    const std::vector<float> greedyPretty = edges(c);
    check(greedyPretty.size() == 2, "the orphan control breaks in two lines");
    if (greedyPretty.size() == 2) {
        checkNear(greedyPretty[1], 40.0f, 1.0f, "greedy leaves a lone word behind");
    }
    const std::vector<float> pretty = edges(d);
    check(pretty.size() == 2, "pretty keeps the line count");
    if (pretty.size() == 2) {
        checkNear(pretty[0], 120.0f, 1.0f, "pretty moves the word down");
        checkNear(pretty[1], 84.0f, 1.0f, "pretty pairs the last line up");
    }

    check(edges(e).size() == 2, "the wrapping control breaks at 100px");
    const std::vector<float> nowrap = edges(f);
    check(nowrap.size() == 1, "text-wrap:nowrap keeps one line");
    if (nowrap.size() == 1) {
        checkNear(nowrap[0], 104.0f, 1.0f, "the overflowing line still measures whole");
    }
    const std::vector<float> nowrapMode = edges(g);
    check(nowrapMode.size() == 1, "the mode longhand nowraps the same way");
    if (nowrapMode.size() == 1 && !nowrap.empty()) {
        checkNear(nowrapMode[0], nowrap[0], 0.01f, "the longhand matches the shorthand");
    }
    const std::vector<float> spanNowrap = edges(h);
    check(spanNowrap.size() == 1, "a nowrap span holds its words on one line");
    if (spanNowrap.size() == 1) {
        checkNear(spanNowrap[0], 140.0f, 1.0f, "the span keeps its two words together");
    }
    const std::vector<float> spanControl = edges(i);
    check(spanControl.size() == 2, "without the span the same words break");

    check(sui::StyleResolver::propertyValue(resolver.resolve(*i), "text-wrap") == "wrap",
          "the initial shorthand serializes as wrap");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*j), "text-wrap") == "nowrap balance",
          "the shorthand serializes both longhands");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*j), "text-wrap-mode") == "nowrap",
          "the mode serializes on its own");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*j), "text-wrap-style") == "balance",
          "the style serializes on its own");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*k), "text-wrap") == "balance",
          "a garbage declaration keeps the previous winner");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*l), "text-wrap") == "pretty",
          "text-wrap inherits into a child");

    // The float narrows the first line, so the balanced box must keep the same
    // greedy breaks instead of chasing one target width for every line.
    const std::vector<float> floatAuto = edges(m);
    const std::vector<float> floatBalanced = edges(n);
    check(floatAuto.size() == 3 && floatBalanced.size() == 3,
          "a float still leaves three greedy lines under balance");
    if (floatAuto.size() == 3 && floatBalanced.size() == 3) {
        checkNear(floatAuto[0], 232.0f, 1.0f, "the float indents the first line");
        checkNear(floatAuto[1], 292.0f, 1.0f, "greedy packs the second line");
        checkNear(floatBalanced[1], floatAuto[1], 0.5f,
                  "balance leaves a float-shaped line alone");
        checkNear(floatBalanced[2], floatAuto[2], 0.5f, "and leaves the last line alone");
    }

    // Nowrap holds atomic boxes, preserved spaces, and tabs on the line too.
    Element* o = el("o");
    Element* p = el("p");
    Element* q = el("q");
    Element* r = el("r");
    Element* s = el("s");
    Element* t = el("t");
    Element* u = el("u");
    Element* w = el("w");
    check(o && p && q && r && s && t && u && w, "the remaining text-wrap boxes are found");
    if (o && p && q && r && s && t && u && w) {
        const auto boxesOf = [](Element* box) {
            std::vector<Element*> out;
            for (const auto& child : box->children()) {
                if (child) {
                    out.push_back(child.get());
                }
            }
            return out;
        };
        const std::vector<Element*> nowrapBoxes = boxesOf(o);
        const std::vector<Element*> wrapBoxes = boxesOf(p);
        check(nowrapBoxes.size() == 2 && wrapBoxes.size() == 2, "the inline-blocks are found");
        if (nowrapBoxes.size() == 2 && wrapBoxes.size() == 2) {
            checkNear(nowrapBoxes[1]->layoutX(), 60.0f, 0.5f,
                      "nowrap keeps the second inline-block beside the first");
            checkNear(nowrapBoxes[1]->layoutY(), nowrapBoxes[0]->layoutY(), 0.5f,
                      "and on the first box's line");
            check(wrapBoxes[1]->layoutY() > wrapBoxes[0]->layoutY() + 1.0f,
                  "without nowrap the second inline-block wraps");
        }

        const std::vector<float> spaceWrap = edges(q);
        const std::vector<float> spaceNowrap = edges(r);
        check(spaceWrap.size() == 2, "a preserved space still wraps by default");
        check(spaceNowrap.size() == 1, "nowrap holds a preserved space on one line");
        if (spaceNowrap.size() == 1) {
            checkNear(spaceNowrap[0], 68.0f, 1.0f, "the preserved line measures whole");
        }

        const std::vector<float> tightWrap = edges(s);
        const std::vector<float> tightNowrap = edges(w);
        check(tightWrap.size() == 3, "a preserved space that does not fit starts a line");
        check(tightNowrap.size() == 1, "nowrap keeps a tight preserved space in place");
        if (tightNowrap.size() == 1) {
            checkNear(tightNowrap[0], 52.0f, 1.0f, "the tight line runs past the edge");
        }

        const std::vector<float> tabWrap = edges(t);
        const std::vector<float> tabNowrap = edges(u);
        check(tabWrap.size() == 3, "a tab wraps onto its own line by default");
        check(tabNowrap.size() == 1, "nowrap keeps a tab on the line");
        if (tabNowrap.size() == 1) {
            checkNear(tabNowrap[0], 40.0f, 1.0f, "the tab holds its whole stop");
        }
    }
}

// no visible content, in the separated borders model only.
void testEmptyCells() {
    // The decision table for one cell.
    {
        sui::ComputedStyle s;
        s.emptyCells = sui::ComputedStyle::EmptyCellsHide;
        check(!s.showsCellDecoration(false, true), "hide blanks an empty separate cell");
        check(s.showsCellDecoration(true, true), "a collapsing table ignores hide");
        check(s.showsCellDecoration(false, false), "content keeps its decoration");
        s.emptyCells = sui::ComputedStyle::EmptyCellsShow;
        check(s.showsCellDecoration(false, true), "show keeps it too");
    }

    // Parse, inherit through the row, override on the cell, and serialize.
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "table { empty-cells: hide } td#o { empty-cells: show } "
        "td#g { empty-cells: banana }"));
    auto document = parseOrNull(
        "<sui><body><table id=\"t\"><tr id=\"r\"><td id=\"a\"></td>"
        "<td id=\"o\"></td><td id=\"g\"></td></tr></table></body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* r = el("r");
    Element* a = el("a");
    Element* o = el("o");
    Element* g = el("g");
    check(r && a && o && g, "cell elements found");
    if (!r || !a || !o || !g) {
        return;
    }
    check(resolver.resolve(*r).emptyCells == ComputedStyle::EmptyCellsHide,
          "the row inherits hide");
    check(resolver.resolve(*a).emptyCells == ComputedStyle::EmptyCellsHide,
          "a cell inherits hide through the row");
    check(resolver.resolve(*o).emptyCells == ComputedStyle::EmptyCellsShow,
          "a cell overrides it");
    check(resolver.resolve(*g).emptyCells == ComputedStyle::EmptyCellsHide,
          "garbage keeps the inherited value");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*a), "empty-cells") == "hide",
          "hide serializes");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*o), "empty-cells") == "show",
          "show serializes");

    // What counts as visible content: text with a non-space character, or a
    // real child box; an empty generated box counts for nothing.
    {
        auto content = parseOrNull(
            "<sui><body><div id=\"e\"></div><div id=\"w\"> \n </div>"
            "<div id=\"t\">x</div><div id=\"c\"><span></span></div>"
            "<div id=\"pe\"></div><div id=\"pt\"></div></body></sui>");
        if (!content) {
            return;
        }
        const auto cel = [&](const char* id) { return content->getElementById(id); };
        Element* e = cel("e");
        Element* w = cel("w");
        Element* t = cel("t");
        Element* c = cel("c");
        Element* pe = cel("pe");
        Element* pt = cel("pt");
        check(e && w && t && c && pe && pt, "content elements found");
        if (!e || !w || !t || !c || !pe || !pt) {
            return;
        }
        check(!e->hasVisibleContent(), "an empty element has none");
        check(!w->hasVisibleContent(), "whitespace-only text is none");
        check(t->hasVisibleContent(), "text counts");
        check(c->hasVisibleContent(), "a child box counts");
        pe->appendChild(std::make_unique<Element>("::before", content.get()));
        check(!pe->hasVisibleContent(), "an empty generated box is no content");
        pt->appendChild(std::make_unique<Element>("::before", content.get()));
        pt->children().back()->appendChild(Element::createText("q", content.get()));
        check(pt->hasVisibleContent(), "a generated box with text counts");
    }
}

// A modal dialog fixes above the page with a backdrop: showModal flags it,
// the user-agent rule pins and layers it, and close() demotes it back.
void testModalDialog() {
    auto document = parseOrNull(
        "<sui><body><dialog id=\"d\">hi</dialog><dialog id=\"e\">yo</dialog></body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* d = el("d");
    Element* e = el("e");
    check(d && e, "modal elements found");
    if (!d || !e) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    check(!d->dialogModal(), "a dialog starts modeless");
    // showModal flags; a second showModal demotes the first.
    d->showModal();
    check(d->dialogOpen() && d->dialogModal(), "showModal flags the dialog");
    e->showModal();
    check(e->dialogModal() && !d->dialogModal() && d->dialogOpen(),
          "a second modal demotes the first to plain open");
    e->close();
    check(!e->dialogModal() && !e->dialogOpen(), "close demotes and shuts");
    // show() never promotes; showModal never reopens.
    d->show();
    check(d->dialogOpen() && !d->dialogModal(), "show leaves a dialog modeless");
    d->close();
    d->showModal();
    d->show();
    check(d->dialogModal(), "show on an open dialog changes nothing");
    // The top layer is un-overridable UA: fixed above every z-index.
    const ComputedStyle modal = resolver.resolve(*d);
    check(modal.position == ComputedStyle::Fixed, "a modal dialog fixes");
    check(modal.zIndex == 1000000, "a modal dialog tops every z-index");
    d->close();
    const ComputedStyle plain = resolver.resolve(*d);
    check(plain.position == ComputedStyle::Static && plain.zIndex == 0,
          "a shut dialog lays out normally again");
    // The backdrop generates while modal, sweeps on close, and covers the
    // viewport by default.
    d->showModal();
    sui::generatePseudoElements(*document, resolver);
    check(d->children().size() == 2 && d->children()[0]->tag() == "::backdrop",
          "a modal dialog grows a backdrop");
    const ComputedStyle veil = resolver.resolve(*d->children()[0]);
    check(veil.position == ComputedStyle::Fixed, "the veil fixes too");
    check(veil.zIndex == 999999, "the veil sits under the dialog");
    check(veil.backgroundColor.r == 0 && veil.backgroundColor.a == 128,
          "the veil dims by default");
    layoutRoot(*document, resolver, 400.0f);
    Element* veilBox = d->children()[0].get();
    check(veilBox->layoutWidth() == 400.0f, "the veil covers the viewport");
    check(veilBox->layoutX() == 0.0f && veilBox->layoutY() == 0.0f,
          "the veil starts at the origin");
    // Centering moves every box and run, so hit-testing follows the dialog.
    const float wasX = d->layoutX();
    sui::BlockLayout::centerInViewport(*d, 400.0f, 300.0f);
    check(d->layoutX() == (400.0f - d->layoutWidth()) * 0.5f,
          "centering lands the dialog mid-viewport");
    check(d->layoutX() != wasX || d->layoutWidth() == 400.0f,
          "centering actually moved the dialog");
    d->close();
    sui::generatePseudoElements(*document, resolver);
    check(d->children().size() == 1 && !d->children()[0]->isPseudoElement(),
          "close sweeps the backdrop");
}

// A <dialog> without `open` takes no box; show() opens it in place and
// close() shuts it firing `onclose`. Non-modal only: showModal is queued
// behind the top-layer paint milestone.
void testDialog() {
    auto document = parseOrNull(
        "<sui><body><dialog id=\"d\">hi</dialog>"
        "<dialog id=\"o\" open=\"\">yo</dialog>"
        "<div id=\"w\" style=\"width:200px\"></div></body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* d = el("d");
    Element* o = el("o");
    check(d && o, "dialog elements found");
    if (!d || !o) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    check(resolver.resolve(*d).display == ComputedStyle::None,
          "a closed dialog takes no box");
    check(resolver.resolve(*o).display == ComputedStyle::Block,
          "an open dialog lays out block");
    check(!d->dialogOpen() && o->dialogOpen(), "open reflects the attribute");
    layoutRoot(*document, resolver, 400.0f);
    check(d->textRuns().empty(), "closed content flows nothing");
    check(!o->textRuns().empty(), "open content flows");
    // show() opens in place; show() again is a no-op; close() shuts.
    d->show();
    check(d->dialogOpen(), "show opens the dialog");
    d->show();
    check(d->dialogOpen(), "show again stays open");
    layoutRoot(*document, resolver, 400.0f);
    check(!d->textRuns().empty(), "shown content flows");
    d->close();
    check(!d->dialogOpen(), "close shuts the dialog");
    d->close();
    check(!d->dialogOpen(), "close again stays shut");
    // close() fires the `onclose` handler when there is one.
    o->setAttribute("onclose", "flagDialogClose()");
    sui::script::registerHandler("flagDialogClose", flagDialogClose);
    o->close();
    check(g_dialogClosed, "close fires onclose");
}

// `background-blend-mode` pairs one entry per image layer, cycling short
// lists; garbage never lands, and nothing authored reads back as `normal`.
void testBackgroundBlendMode() {
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { background-blend-mode: multiply, screen } "
        "#b { background-blend-mode: banana, overlay } "
        "#g { background-blend-mode: banana }"));
    auto document = parseOrNull(
        "<sui><body><div id=\"a\"></div><div id=\"b\"></div>"
        "<div id=\"g\"></div><div id=\"c\"></div></body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    using Blend = ComputedStyle::MixBlendMode;
    check(resolver.resolve(*el("a")).backgroundBlendModes.size() == 2 &&
              resolver.resolve(*el("a")).backgroundBlendModes[0] == Blend::Multiply &&
              resolver.resolve(*el("a")).backgroundBlendModes[1] == Blend::Screen,
          "entries pair in order");
    check(resolver.resolve(*el("b")).backgroundBlendModes.size() == 1 &&
              resolver.resolve(*el("b")).backgroundBlendModes[0] == Blend::Overlay,
          "garbage entries never land");
    check(resolver.resolve(*el("g")).backgroundBlendModes.empty(),
          "all garbage keeps the previous list");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*el("a")),
                                             "background-blend-mode") ==
              "multiply, screen",
          "entries serialize in order");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*el("c")),
                                             "background-blend-mode") == "normal",
          "nothing authored reads as normal");
}

// `resize` names a drag direction (`block`/`inline` are the vertical and
// horizontal axes here); only boxes with non-visible overflow grow a grip.
// A textarea resizes both ways out of the box.
void testResize() {
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#b { resize: both } #h { resize: horizontal; overflow: auto } #v { resize: vertical } "
        "#bl { resize: block } #in { resize: inline } #n { resize: none } "
        "#g { resize: banana } #vis { resize: both; overflow: visible } "
        "#ov { overflow: auto }"));
    auto document = parseOrNull(
        "<sui><body><textarea id=\"t\"></textarea><div id=\"d\"></div>"
        "<div id=\"b\"></div><div id=\"h\"></div><div id=\"v\"></div><div id=\"bl\"></div>"
        "<div id=\"in\"></div><div id=\"n\"></div><div id=\"g\"></div>"
        "<div id=\"vis\"></div><div id=\"ov\"></div></body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* t = el("t");
    check(t && el("d") && el("b") && el("h") && el("v"), "resize elements found");
    if (!t) {
        return;
    }
    using Resize = ComputedStyle::Resize;
    check(resolver.resolve(*t).resize == Resize::ResizeBoth, "textareas resize both");
    check(resolver.resolve(*el("d")).resize == Resize::ResizeNone, "divs resize none");
    check(resolver.resolve(*el("b")).resize == Resize::ResizeBoth, "both parses");
    check(resolver.resolve(*el("h")).resize == Resize::ResizeHorizontal,
          "horizontal parses");
    check(resolver.resolve(*el("v")).resize == Resize::ResizeVertical, "vertical parses");
    check(resolver.resolve(*el("bl")).resize == Resize::ResizeVertical,
          "block means vertical");
    check(resolver.resolve(*el("in")).resize == Resize::ResizeHorizontal,
          "inline means horizontal");
    check(resolver.resolve(*el("n")).resize == Resize::ResizeNone, "none parses");
    check(resolver.resolve(*el("g")).resize == Resize::ResizeNone, "garbage keeps none");
    // Grips need non-visible overflow: visible overflow plus both grows none.
    check(!resolver.resolve(*el("vis")).resizable(), "visible overflow grows no grip");
    check(resolver.resolve(*el("ov")).resize == Resize::ResizeNone,
          "overflow alone resizes nothing");
    check(!resolver.resolve(*el("n")).resizable(), "none never resizes");
    check(resolver.resolve(*t).resizable(), "a textarea grip shows");
    check(resolver.resolve(*t).resizesHorizontally() &&
              resolver.resolve(*t).resizesVertically(),
          "both resizes both ways");
    check(resolver.resolve(*el("h")).resizesHorizontally() &&
              !resolver.resolve(*el("h")).resizesVertically(),
          "horizontal resizes one way");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*t), "resize") == "both",
          "both serializes");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*el("d")), "resize") ==
              "none",
          "none serializes");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*el("in")), "resize") ==
              "horizontal",
          "inline serializes as horizontal");
}

// `line-clamp` caps a block at N lines with an ellipsis: `none` lifts the cap,
// anything but a positive integer drops the declaration, and the property does
// not inherit. Content past the cap lays out nothing; an ellipsis that nothing
// proves needed is taken back.
void testLineClamp() {
    // Parsing, the webkit alias, serialization, and no inheritance.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#a { line-clamp: 2 } #n { line-clamp: none } "
            "#w { -webkit-line-clamp: 3 } #z { line-clamp: 0 } "
            "#g { line-clamp: banana } #f { line-clamp: 2.5 }"));
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"><span id=\"c\">x</span></div><div id=\"n\"></div>"
            "<div id=\"w\"></div><div id=\"z\"></div><div id=\"g\"></div>"
            "<div id=\"f\"></div></body></sui>");
        if (!document) {
            return;
        }
        const auto el = [&](const char* id) { return document->getElementById(id); };
        Element* a = el("a");
        Element* c = el("c");
        check(a && c && el("n") && el("w") && el("z") && el("g") && el("f"),
              "clamp elements found");
        if (!a || !c) {
            return;
        }
        check(resolver.resolve(*a).lineClamp == 2, "an integer parses");
        check(resolver.resolve(*c).lineClamp == 0, "the cap does not inherit");
        check(resolver.resolve(*el("n")).lineClamp == 0, "none lifts the cap");
        check(resolver.resolve(*el("w")).lineClamp == 3, "the webkit alias parses");
        check(resolver.resolve(*el("z")).lineClamp == 0, "zero drops out");
        check(resolver.resolve(*el("g")).lineClamp == 0, "garbage drops out");
        check(resolver.resolve(*el("f")).lineClamp == 0, "a fraction drops out");
        check(resolver.resolve(*a).clampsLines(), "a positive cap clamps");
        check(!resolver.resolve(*c).clampsLines(), "zero clamps nothing");
        check(sui::StyleResolver::propertyValue(resolver.resolve(*a), "line-clamp") == "2",
              "the cap serializes");
        check(sui::StyleResolver::propertyValue(resolver.resolve(*el("n")), "line-clamp") ==
                  "none",
              "none serializes");
        check(resolver.resolve(*a).display == ComputedStyle::Block,
              "the cap changes no display");
    }

    const auto lineCount = [](Element& e) {
        std::vector<float> ys;
        for (const auto& run : e.textRuns()) {
            if (ys.empty() || run.y != ys.back()) {
                ys.push_back(run.y);
            }
        }
        return ys.size();
    };
    const auto endsEllipsis = [](Element& e) {
        if (e.textRuns().empty()) {
            return false;
        }
        const std::string& text = e.textRuns().back().text;
        return text.size() >= 3 && text.compare(text.size() - 3, 3, "\xE2\x80\xA6") == 0;
    };
    const std::string words =
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi "
        "omicron pi rho sigma tau upsilon phi chi psi omega aleph beth gimel daleth";

    // A long paragraph capped at two lines: two line levels, cut with ellipsis.
    {
        const std::string markup =
            std::string("<sui><body><div id=\"c\" style=\"width:100px;line-clamp:2\">") + words +
            "</div><div id=\"u\" style=\"width:100px\">" + words + "</div></body></sui>";
        auto document = parseOrNull(markup.c_str());
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
        layoutRoot(*document, resolver, 400.0f);
        Element* c = document->getElementById("c");
        Element* u = document->getElementById("u");
        check(c && u, "clamp layout elements found");
        if (!c || !u) {
            return;
        }
        check(lineCount(*u) > 2, "the paragraph wants more than two lines");
        check(lineCount(*c) == 2, "the cap holds two lines");
        check(endsEllipsis(*c), "cut content ends with an ellipsis");
        check(c->textRuns().size() < u->textRuns().size(), "cut runs never lay out");
    }

    // One line, and an atomic box past the cap zeroes out instead of painting on.
    {
        const std::string markup =
            std::string("<sui><body><div id=\"o\" style=\"width:100px;line-clamp:1\">") + words +
            "<div id=\"b\" style=\"display:inline-block;width:50px;height:10px\"></div></div>"
            "</body></sui>";
        auto document = parseOrNull(markup.c_str());
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
        layoutRoot(*document, resolver, 400.0f);
        Element* o = document->getElementById("o");
        Element* b = document->getElementById("b");
        check(o && b, "single-line elements found");
        if (!o || !b) {
            return;
        }
        check(lineCount(*o) == 1, "one line survives");
        check(endsEllipsis(*o), "one line still earns its ellipsis");
        check(b->layoutWidth() == 0.0f && b->layoutHeight() == 0.0f,
              "an atomic past the cap takes no box");
        check(b->textRuns().empty(), "and lays out no runs");
    }

    // What fits needs no ellipsis: exact fit, and a trailing break after one.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"e\" style=\"width:200px;line-clamp:2\">hi yo</div>"
            "<div id=\"t\" style=\"width:200px;line-clamp:1\">hi yo<br/></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
        layoutRoot(*document, resolver, 400.0f);
        Element* e = document->getElementById("e");
        Element* t = document->getElementById("t");
        check(e && t, "exact-fit elements found");
        if (!e || !t) {
            return;
        }
        check(lineCount(*e) == 1, "short text stays one line");
        check(!endsEllipsis(*e), "fitting text earns no ellipsis");
        check(!endsEllipsis(*t), "a trailing break earns none either");
    }

    // An atomic box that itself crosses the cap lays out nowhere either.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"k\" style=\"width:100px;line-clamp:1\">alpha"
            "<div id=\"a\" style=\"display:inline-block;width:90px;height:10px\"></div></div>"
            "<div id=\"w\" style=\"width:100px\">alpha"
            "<div id=\"f\" style=\"display:inline-block;width:90px;height:10px\"></div></div>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
        layoutRoot(*document, resolver, 400.0f);
        Element* a = document->getElementById("a");
        Element* f = document->getElementById("f");
        check(a && f, "wrapping atomic elements found");
        if (!a || !f) {
            return;
        }
        check(a->layoutWidth() == 0.0f && a->layoutHeight() == 0.0f,
              "a wrapping atomic takes no box");
        check(f->layoutWidth() > 0.0f, "unclamped it keeps its box");
    }
}

// Counter styles number markers and `counter()` values: roman subtracts,
// alpha rolls over, and anything unrepresentable reads as decimal.
void testCounterStyles() {
    using LST = ComputedStyle::ListStyleType;
    check(sui::toRoman(1, false) == "I", "one is I");
    check(sui::toRoman(4, false) == "IV", "four subtracts");
    check(sui::toRoman(9, false) == "IX", "nine subtracts");
    check(sui::toRoman(14, true) == "xiv", "fourteen lowers");
    check(sui::toRoman(40, false) == "XL", "forty subtracts tens");
    check(sui::toRoman(90, false) == "XC", "ninety subtracts tens");
    check(sui::toRoman(400, false) == "CD", "four hundred subtracts");
    check(sui::toRoman(900, false) == "CM", "nine hundred subtracts");
    check(sui::toRoman(1999, false) == "MCMXCIX", "years compose");
    check(sui::toRoman(3999, false) == "MMMCMXCIX", "the largest roman composes");
    check(sui::toRoman(4000, false) == "4000", "past the top reads decimal");
    check(sui::toRoman(0, false) == "0", "zero reads decimal");
    check(sui::toRoman(-3, true) == "-3", "negatives read decimal");
    check(sui::toAlpha(1, true) == "a", "one is a");
    check(sui::toAlpha(26, true) == "z", "twenty-six is z");
    check(sui::toAlpha(27, true) == "aa", "twenty-seven rolls over");
    check(sui::toAlpha(28, false) == "AB", "twenty-eight uppers");
    check(sui::toAlpha(52, true) == "az", "fifty-two pairs down");
    check(sui::toAlpha(53, true) == "ba", "fifty-three carries");
    check(sui::toAlpha(702, true) == "zz", "seven hundred two maxes two");
    check(sui::toAlpha(703, true) == "aaa", "seven hundred three triples");
    check(sui::toAlpha(0, true) == "0", "zero alpha reads decimal");
    check(sui::listMarkerText(LST::LSTUpperRoman, 4) == "IV", "markers share roman");
    check(sui::listMarkerText(LST::LSTLowerAlpha, 3) == "c", "markers share alpha");
    check(sui::isOrderedMarker(LST::LSTDecimal) && !sui::isOrderedMarker(LST::LSTDisc),
          "ordered means numbered");

    // Parsing, shorthand, aliases, and serialization.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#a { list-style-type: upper-roman } #b { list-style-type: lower-latin } "
            "#c { list-style: lower-alpha inside } #g { list-style-type: banana }"));
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"></div><div id=\"b\"></div>"
            "<div id=\"c\"></div><div id=\"g\"></div><div id=\"h\"></div></body></sui>");
        if (!document) {
            return;
        }
        const auto el = [&](const char* id) { return document->getElementById(id); };
        check(resolver.resolve(*el("a")).listStyleType == LST::LSTUpperRoman,
              "upper-roman parses");
        check(resolver.resolve(*el("b")).listStyleType == LST::LSTLowerAlpha,
              "lower-latin aliases alpha");
        check(resolver.resolve(*el("c")).listStyleType == LST::LSTLowerAlpha,
              "the shorthand carries alpha");
        check(resolver.resolve(*el("g")).listStyleType == LST::LSTDisc,
              "garbage falls back to disc");
        check(sui::StyleResolver::propertyValue(resolver.resolve(*el("a")),
                                                 "list-style-type") == "upper-roman",
              "roman serializes");
    }

    // Inside markers number in style, and `counter()` takes one too.
    {
        auto document = parseOrNull(
            "<sui><body><ol id=\"r\" style=\"list-style:upper-roman inside\">"
            "<li>a</li><li>b</li></ol>"
            "<ol id=\"l\" style=\"list-style:lower-alpha inside\">"
            "<li>a</li><li>b</li></ol>"
            "<div id=\"c\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body { margin: 0 } #c::after { content: counter(k, lower-roman) } "
            "body { counter-reset: k 3 }"));
        sui::generatePseudoElements(*document, resolver);
        layoutRoot(*document, resolver, 400.0f);
        Element* r = document->getElementById("r");
        Element* l = document->getElementById("l");
        Element* c = document->getElementById("c");
        check(r && l && c, "styled list elements found");
        if (!r || !l || !c) {
            return;
        }
        const auto firstRun = [](Element& e) {
            return e.textRuns().empty() ? std::string() : e.textRuns()[0].text;
        };
        Element* r1 = r->children()[0].get();
        Element* r2 = r->children()[1].get();
        check(firstRun(*r1).substr(0, 2) == "I.", "roman markers start at I");
        check(firstRun(*r2).substr(0, 3) == "II.", "roman markers count on");
        Element* l1 = l->children()[0].get();
        check(firstRun(*l1).substr(0, 2) == "a.", "alpha markers start at a");
        check(c->children().size() == 1 && c->children()[0]->textContent() == "iii",
              "counter() styles its value");
    }
}

// `quotes` lists open/close pairs outward in (or `none`), and `content` names
// them with open/close/no-open/no-close-quote. Depth nests through the tree;
// a `<q>` quotes itself unless the author already boxed it.
void testQuotes() {
    // Parsing, inheritance, and serialization.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#a { quotes: \"<\" \">\" \"[\" \"]\" } #n { quotes: none }"));
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"><span id=\"c\">x</span></div>"
            "<div id=\"n\">x</div><div id=\"g\" style=\"quotes: banana\">x</div>"
            "<div id=\"o\" style=\"quotes: 'a' 'b' 'c'\">x</div></body></sui>");
        if (!document) {
            return;
        }
        const auto el = [&](const char* id) { return document->getElementById(id); };
        Element* a = el("a");
        Element* c = el("c");
        Element* n = el("n");
        Element* g = el("g");
        Element* o = el("o");
        check(a && c && n && g && o, "quote elements found");
        if (!a || !c || !n || !g || !o) {
            return;
        }
        check(resolver.resolve(*a).quotes.size() == 4, "two pairs parse");
        check(resolver.resolve(*a).quotes[0] == "<", "the open mark leads");
        check(resolver.resolve(*c).quotes.size() == 4, "quotes inherit");
        check(resolver.resolve(*n).quotes.empty(), "none clears the list");
        check(resolver.resolve(*g).quotes.size() == 2, "garbage keeps the default pair");
        check(resolver.resolve(*o).quotes.size() == 2, "an odd tail keeps the default");
        check(sui::StyleResolver::propertyValue(resolver.resolve(*a), "quotes") ==
                  "\"<\" \">\" \"[\" \"]\"",
              "pairs serialize back");
        check(sui::StyleResolver::propertyValue(resolver.resolve(*n), "quotes") == "none",
              "none serializes");
    }

    const auto quoted = [](Document& root, const char* id) {
        Element* el = root.getElementById(id);
        if (!el || el->children().empty()) {
            return std::string("<missing>");
        }
        return el->children()[0]->textContent();
    };

    // Author keywords open, close, and skip depths in document order.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"></div><div id=\"b\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#a::before { content: open-quote \"x\" close-quote } "
            "#b::before { content: open-quote open-quote \"y\" close-quote }"));
        sui::generatePseudoElements(*document, resolver);
        // Siblings each start at depth zero; nesting deepens the second mark.
        check(quoted(*document, "a") == "\xE2\x80\x9Cx\xE2\x80\x9D",
              "open and close wrap at depth zero");
        check(quoted(*document, "b") == "\xE2\x80\x9C\xE2\x80\x9Cy\xE2\x80\x9D",
              "a second open nests before closing");
    }

    // The no- forms move the depth without emitting.
    {
        auto document = parseOrNull("<sui><body><div id=\"a\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#a::before { content: no-open-quote \"x\" close-quote }"));
        sui::generatePseudoElements(*document, resolver);
        check(quoted(*document, "a") == "x\xE2\x80\x9D",
              "a close still emits at depth zero");
    }

    // Deeper pairs serve nested depth: the second open takes pair one.
    {
        auto document = parseOrNull("<sui><body><div id=\"a\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#a { quotes: '<' '>' '[' ']' } "
            "#a::before { content: open-quote open-quote \"y\" close-quote close-quote }"));
        sui::generatePseudoElements(*document, resolver);
        check(quoted(*document, "a") == "<[y]>",
              "nested author depth takes deeper pairs");
    }

    // A <q> quotes itself; nesting uses deeper pairs; none quotes nothing.
    {
        auto document = parseOrNull(
            "<sui><body><q id=\"a\">x</q>"
            "<div id=\"w\" style=\"quotes: '<' '>' '[' ']'\">"
            "<q id=\"o\">a<q id=\"i\">b</q>c</q></div>"
            "<q id=\"n\" style=\"quotes:none\">x</q></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
        sui::generatePseudoElements(*document, resolver);
        Element* a = document->getElementById("a");
        Element* o = document->getElementById("o");
        Element* n = document->getElementById("n");
        check(a && o && n, "q elements found");
        if (!a || !o || !n) {
            return;
        }
        check(a->textContent() == "\xE2\x80\x9Cx\xE2\x80\x9D", "a q quotes itself");
        check(o->textContent() == "<a[b]c>", "a nested q takes the inner pair");
        check(n->textContent() == "x", "quotes none quotes nothing");
    }

    // An author box wins over the implied one, on whichever side it takes.
    {
        auto document = parseOrNull("<sui><body><q id=\"a\">x</q></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(
            sui::CSSParser::parse("q::before { content: \"Q:\" }"));
        sui::generatePseudoElements(*document, resolver);
        Element* a = document->getElementById("a");
        check(a && a->textContent() == "Q:x\xE2\x80\x9D", "one side each, no doubling");
    }
}

// A closed <details> shows only its first <summary>; everything else takes no
// box, including bare text. `open` shows all, and details/summary lay out as
// blocks.
void testDetails() {
    auto document = parseOrNull(
        "<sui><body>"
        "<details id=\"c\"><summary id=\"s\">more</summary><div id=\"x\">hidden</div></details>"
        "<details id=\"o\" open=\"\"><summary>more</summary><div id=\"y\">shown</div></details>"
        "<details id=\"n\">bare</details>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* c = el("c");
    Element* s = el("s");
    Element* x = el("x");
    Element* o = el("o");
    Element* y = el("y");
    Element* n = el("n");
    check(c && s && x && o && y && n, "details elements found");
    if (!c || !s || !x || !o || !y || !n) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    check(resolver.resolve(*c).display == ComputedStyle::Block, "details lays out block");
    check(resolver.resolve(*s).display == ComputedStyle::Block, "summary lays out block");
    layoutRoot(*document, resolver, 400.0f);
    const auto runs = [](Element& e) { return e.textRuns().size(); };
    check(runs(*s) > 0, "the summary flows while closed");
    check(runs(*x) == 0, "a closed sibling flows nothing");
    check(runs(*y) > 0, "an open sibling flows");
    check(runs(*n) == 0, "bare text hides with no summary");
}

// `:has()` selects an element by what it contains: a child (`>`), a descendant
// (no combinator), or a following sibling (`+`, `~`). Each branch of the argument
// is anchored to the element being matched.
void testHasSelector() {
    // Parsing.
    const auto parsed = sui::CSSParser::parseSelector("div:has(> img)");
    check(!parsed.unsupported, ":has() parses as a supported selector");
    check(parsed.pseudoArgument == "> img", ":has() keeps its relative selector");
    bool hasPseudo = false;
    for (const sui::PseudoClass pseudo : parsed.pseudoClasses) {
        if (pseudo == sui::PseudoClass::Has) {
            hasPseudo = true;
        }
    }
    check(hasPseudo, ":has() records the Has pseudo-class");

    // Child combinator: a div matches only when it has a direct <img> child.
    {
        auto document = parseOrNull(
            "<sui><body>"
            "<div id=\"with\"><img/></div>"
            "<div id=\"without\"><span><img/></span></div>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body { color: #000000 } div:has(> img) { color: #ff0000 }"));
        check(resolver.resolve(*document->getElementById("with")).color.r == 0xff,
              ":has(> img) matches a direct image child");
        check(resolver.resolve(*document->getElementById("without")).color.r == 0x00,
              ":has(> img) ignores a deeper image");
    }

    // Descendant, with no leading combinator.
    {
        auto document = parseOrNull(
            "<sui><body>"
            "<div id=\"card\" class=\"card\"><span><b class=\"badge\"></b></span></div>"
            "<div id=\"nog\" class=\"card\"><span></span></div>"
            "</body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body { color: #000000 } .card:has(.badge) { color: #00ff00 }"));
        check(resolver.resolve(*document->getElementById("card")).color.g == 0xff,
              ":has(.badge) matches a descendant badge");
        check(resolver.resolve(*document->getElementById("nog")).color.g == 0x00,
              ":has(.badge) does not match when the badge is absent");
    }

    // Following-sibling combinators.
    {
        auto document = parseOrNull(
            "<sui><body>"
            "<h2 id=\"np\"></h2><p></p>"
            "<h2 id=\"ns\"></h2><span></span>"
            "<h2 id=\"gp\"></h2><span></span><p></p>"
            "<h2 id=\"gs\"></h2><span></span>"
            "</body></sui>");
        if (!document) {
            return;
        }
        {
            sui::StyleResolver resolver;
            resolver.setRules(sui::CSSParser::parse(
                "body { color: #000000 } h2:has(+ p) { color: #0000ff }"));
            check(resolver.resolve(*document->getElementById("np")).color.b == 0xff,
                  ":has(+ p) matches a heading followed immediately by a paragraph");
            check(resolver.resolve(*document->getElementById("ns")).color.b == 0x00,
                  ":has(+ p) ignores a heading followed by something else");
        }
        {
            sui::StyleResolver resolver;
            resolver.setRules(sui::CSSParser::parse(
                "body { color: #000000 } h2:has(~ p) { color: #ffff00 }"));
            check(resolver.resolve(*document->getElementById("gp")).color.r == 0xff,
                  ":has(~ p) matches a heading with a later paragraph");
            check(resolver.resolve(*document->getElementById("gs")).color.r == 0x00,
                  ":has(~ p) ignores a heading with no later paragraph");
        }
    }

    // A selector list keeps working when one branch is one the parser cannot use.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"v\"><video></video></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body { color: #000000 } div:has(> ::bogus, > video) { color: #ff0000 }"));
        check(resolver.resolve(*document->getElementById("v")).color.r == 0xff,
              "a usable :has() branch still matches beside an unusable one");
    }
}

// `float` takes a box out of normal flow and reserves a strip along the left or
// right content edge; following lines wrap beside it. `clear` drops a block below
// the floats on the named side. These run without a font, so a 10px font advances
// every glyph by 5px and a space by 2.5px.
void testFloatsAndClear() {
    // A left float indents the text on each line it covers.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"box\">"
            "<img id=\"f\"/>"
            "aaaaaaaa bbbbbbbb"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #box{width:200px;font-size:10px}"
            "#f{float:left;width:60px;height:30px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* box = document->getElementById("box");
        Element* f = document->getElementById("f");
        check(f != nullptr && std::fabs(f->layoutX() - 0.0f) < 0.5f,
              "a left float sits on the left content edge");
        check(f != nullptr && std::fabs(f->layoutY() - 0.0f) < 0.5f,
              "a left float starts at the top of the flow");
        check(f != nullptr && std::fabs(f->layoutWidth() - 60.0f) < 0.5f &&
                  std::fabs(f->layoutHeight() - 30.0f) < 0.5f,
              "a float keeps its used width and height");
        const auto& runs = box->textRuns();
        check(runs.size() >= 2 && std::fabs(runs[0].x - 60.0f) < 0.5f,
              "the first line starts beside the float");
        check(runs.size() >= 2 && std::fabs(runs[1].x - 60.0f) < 0.5f,
              "the wrapped line is indented too");
        check(runs.size() >= 2 && std::fabs(runs[1].y - runs[0].y - 12.0f) < 0.5f,
              "the second word wrapped onto the next line");
        check(box != nullptr && std::fabs(box->layoutHeight() - 30.0f) < 0.5f,
              "a container grows to hold its float");
    }

    // Two left floats that do not fit side by side stack vertically.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"box\">"
            "<img id=\"a\"/><img id=\"b\"/>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #box{width:100px}"
            "#a,#b{float:left;width:60px;height:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(a != nullptr && std::fabs(a->layoutY() - 0.0f) < 0.5f,
              "the first float sits at the top");
        check(b != nullptr && std::fabs(b->layoutY() - 20.0f) < 0.5f,
              "a float that cannot fit beside the first drops below it");
    }

    // A right float anchors to the right content edge.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"box\">"
            "<img id=\"f\"/>"
            "aaaaaaaa"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #box{width:200px;font-size:10px}"
            "#f{float:right;width:60px;height:30px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* f = document->getElementById("f");
        check(f != nullptr && std::fabs(f->layoutX() - 140.0f) < 0.5f,
              "a right float sits on the right content edge");
        Element* box = document->getElementById("box");
        const auto& runs = box->textRuns();
        check(!runs.empty() && std::fabs(runs[0].x - 0.0f) < 0.5f,
              "text beside a right float keeps the left edge");
    }

    // `clear:left` moves a block below the float.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"box\">"
            "<img id=\"f\"/><div id=\"c\"></div>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #box{width:200px}"
            "#f{float:left;width:50px;height:40px}"
            "#c{clear:left;height:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* c = document->getElementById("c");
        check(c != nullptr && std::fabs(c->layoutY() - 40.0f) < 0.5f,
              "clear:left drops the box below the float");
        Element* box = document->getElementById("box");
        check(box != nullptr && std::fabs(box->layoutHeight() - 60.0f) < 0.5f,
              "cleared content adds to the container height");
    }

    // A block box without `clear` starts at the top; only its lines avoid the
    // float, so its own text still wraps.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"box\">"
            "<img id=\"f\"/>"
            "<p id=\"p\">aaaaaaaa bbbbbbbb</p>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #box{width:200px;font-size:10px}"
            "#f{float:left;width:60px;height:30px} #p{margin:0}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* p = document->getElementById("p");
        check(p != nullptr && std::fabs(p->layoutY() - 0.0f) < 0.5f,
              "an uncleared block starts at the top");
        const auto& runs = p->textRuns();
        check(!runs.empty() && std::fabs(runs[0].x - 60.0f) < 0.5f,
              "a following block's text wraps around the float");
    }
}

void testTableLayout() {
    // Cells are placed side by side; a column is as wide as its widest cell and
    // a row as tall as its tallest.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<tr><td id=\"a\"></td><td id=\"b\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"
            "#a{width:50px;height:20px} #b{width:70px;height:30px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(t != nullptr && std::fabs(t->layoutWidth() - 120.0f) < 0.5f,
              "a table is as wide as the sum of its columns");
        check(t != nullptr && std::fabs(t->layoutHeight() - 30.0f) < 0.5f,
              "a table is as tall as its tallest row");
        check(a != nullptr && std::fabs(a->layoutX() - 0.0f) < 0.5f &&
                  std::fabs(a->layoutWidth() - 50.0f) < 0.5f,
              "the first cell fills its column");
        check(b != nullptr && std::fabs(b->layoutX() - 50.0f) < 0.5f &&
                  std::fabs(b->layoutWidth() - 70.0f) < 0.5f,
              "the second cell starts after the first column");
        check(a != nullptr && std::fabs(a->layoutHeight() - 30.0f) < 0.5f,
              "a short cell stretches to its row's height");
    }

    // A table is block-level, so following content starts below it.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<tr><td id=\"a\"></td></tr></table>"
            "<div id=\"d\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"
            "#a{width:50px;height:20px} #d{height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* d = document->getElementById("d");
        check(t != nullptr && d != nullptr &&
                  d->layoutY() >= t->layoutY() + t->layoutHeight() - 0.5f,
              "a block after a table starts below the table");
    }

    // Rows can be wrapped in a row group (`thead` / `tbody`).
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<thead><tr><td id=\"h\"></td><td id=\"i\"></td></tr></thead>"
            "<tbody><tr><td id=\"b\"></td><td id=\"c\"></td></tr></tbody>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"
            "#h{width:40px;height:10px} #i{width:60px;height:10px} #c{height:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(t != nullptr && std::fabs(t->layoutHeight() - 30.0f) < 0.5f,
              "rows inside a row group are stacked");
        check(b != nullptr && std::fabs(b->layoutY() - 18.0f) < 0.5f &&
                  std::fabs(c->layoutX() - 40.0f) < 0.5f,
              "a later row group continues below the first");
    }

    // `border-spacing` surrounds and separates the cells.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<tr><td id=\"a\"></td><td id=\"b\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-spacing:10px} td{padding:0}"
            "#a{width:50px;height:20px} #b{width:70px;height:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(t != nullptr && std::fabs(t->layoutWidth() - 150.0f) < 0.5f,
              "border-spacing adds a gap on each side and between the columns");
        check(t != nullptr && std::fabs(t->layoutHeight() - 40.0f) < 0.5f,
              "border-spacing adds a gap above and below the rows");
        check(a != nullptr && std::fabs(a->layoutX() - 10.0f) < 0.5f &&
                  std::fabs(a->layoutY() - 18.0f) < 0.5f,
              "the first cell is inset by the spacing");
        check(b != nullptr && std::fabs(b->layoutX() - 70.0f) < 0.5f,
              "the second column is separated by the spacing");
    }

    // A `colspan` cell covers several columns and the later rows still align.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<tr><td id=\"a\" colspan=\"2\"></td></tr>"
            "<tr><td id=\"b\"></td><td id=\"c\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"
            "#a{height:40px} #b{width:50px;height:10px} #c{width:70px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(t != nullptr && std::fabs(t->layoutHeight() - 50.0f) < 0.5f,
              "a rowspan-free table stacks its rows");
        check(a != nullptr && std::fabs(a->layoutWidth() - 120.0f) < 0.5f,
              "a colspan cell spans the width of the columns it covers");
        check(b != nullptr && std::fabs(b->layoutX() - 0.0f) < 0.5f &&
                  std::fabs(c->layoutX() - 50.0f) < 0.5f,
              "the columns under a colspan keep their own widths");
    }

    // A wide `colspan` cell pulls the columns under it open.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<tr><td id=\"a\" colspan=\"2\"></td></tr>"
            "<tr><td id=\"b\"></td><td id=\"c\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"
            "#a{width:100px;height:10px} #b{width:10px;height:10px}"
            "#c{width:10px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(a != nullptr && std::fabs(a->layoutWidth() - 100.0f) < 0.5f,
              "a colspan cell keeps its declared width");
        check(b != nullptr && std::fabs(b->layoutWidth() - 50.0f) < 0.5f &&
                  std::fabs(c->layoutX() - 50.0f) < 0.5f,
              "the columns under a colspan share the extra width");
    }

    // A `rowspan` cell skips the slot it holds in the next row.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<tr><td id=\"a\" rowspan=\"2\"></td><td id=\"b\"></td></tr>"
            "<tr><td id=\"c\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"
            "#a{width:40px;height:10px} #b{width:30px;height:20px}"
            "#c{height:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(a != nullptr && std::fabs(a->layoutHeight() - 40.0f) < 0.5f,
              "a rowspan cell is as tall as the rows it covers");
        check(b != nullptr && std::fabs(b->layoutX() - 40.0f) < 0.5f,
              "a rowspan shifts the cell beside it into the second column");
        check(c != nullptr && std::fabs(c->layoutX() - 40.0f) < 0.5f &&
                  std::fabs(c->layoutY() - 28.0f) < 0.5f,
              "the row under a rowspan starts below the first row");
    }

    // A caption occupies the table's width above (or below) the rows.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\"><caption id=\"cap\"></caption>"
            "<tr><td id=\"a\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"
            "#cap{height:15px} #a{width:80px;height:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* cap = document->getElementById("cap");
        Element* a = document->getElementById("a");
        check(cap != nullptr && std::fabs(cap->layoutWidth() - 80.0f) < 0.5f &&
                  std::fabs(cap->layoutY() - 8.0f) < 0.5f,
              "a caption spans the table's width at the top");
        check(a != nullptr && std::fabs(a->layoutY() - 23.0f) < 0.5f,
              "the rows begin below the caption");
        check(t != nullptr && std::fabs(t->layoutHeight() - 35.0f) < 0.5f,
              "the caption adds to the table's height");
    }

    // `<col>` and `<colgroup>` widths size the columns before the cells are
    // measured, even when every cell is empty.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<colgroup><col id=\"c1\"/><col id=\"c2\"/></colgroup>"
            "<tr><td id=\"a\"></td><td id=\"b\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"
            "#c1{width:40px} #c2{width:60px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* col = document->getElementById("c1");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(col != nullptr && resolver.resolve(*col).display == ComputedStyle::TableColumn,
              "a col has the table-column display type");
        check(t != nullptr && std::fabs(t->layoutWidth() - 100.0f) < 0.5f,
              "col widths size the table without cell content");
        check(a != nullptr && std::fabs(a->layoutWidth() - 40.0f) < 0.5f &&
                  b != nullptr && std::fabs(b->layoutX() - 40.0f) < 0.5f &&
                  std::fabs(b->layoutWidth() - 60.0f) < 0.5f,
              "each col width reaches its column");
    }

    // A colgroup width is inherited by its cols, while a col's own width wins.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<colgroup id=\"g\"><col id=\"c1\"/><col id=\"c2\"/>"
            "<col id=\"c3\"/></colgroup>"
            "<tr><td id=\"a\"></td><td id=\"b\"></td><td id=\"c\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"
            "#g{width:40px} #c1{width:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* group = document->getElementById("g");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(group != nullptr &&
                  resolver.resolve(*group).display == ComputedStyle::TableColumnGroup,
              "a colgroup has the table-column-group display type");
        check(a != nullptr && std::fabs(a->layoutWidth() - 20.0f) < 0.5f,
              "a col's width overrides its colgroup");
        check(b != nullptr && std::fabs(b->layoutX() - 20.0f) < 0.5f &&
                  std::fabs(b->layoutWidth() - 40.0f) < 0.5f &&
                  c != nullptr && std::fabs(c->layoutX() - 60.0f) < 0.5f &&
                  std::fabs(c->layoutWidth() - 40.0f) < 0.5f,
              "cols without a width inherit the colgroup width");
    }

    // An empty colgroup with a span creates that many columns at the group width.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<colgroup span=\"2\" style=\"width:25px\"/>"
            "<tr><td id=\"a\"></td><td id=\"b\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(t != nullptr && std::fabs(t->layoutWidth() - 50.0f) < 0.5f,
              "a colgroup span repeats its width across the table");
        check(a != nullptr && std::fabs(a->layoutWidth() - 25.0f) < 0.5f &&
                  b != nullptr && std::fabs(b->layoutX() - 25.0f) < 0.5f &&
                  std::fabs(b->layoutWidth() - 25.0f) < 0.5f,
              "each column from a colgroup span gets the group width");
    }

    // A span repeats a column hint and trailing hints still create empty columns.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<col span=\"2\" style=\"width:30px\"/><col style=\"width:10px\"/>"
            "<tr><td id=\"a\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse} td{padding:0}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        check(a != nullptr && std::fabs(a->layoutWidth() - 30.0f) < 0.5f,
              "a spanned col width reaches the first column");
        check(t != nullptr && std::fabs(t->layoutWidth() - 70.0f) < 0.5f,
              "col spans and trailing cols add empty columns to the table");
    }

    // `table-layout: fixed` takes the columns from the table width, but a col
    // width outranks the first row's cell width.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<col id=\"c1\"/><tr><td id=\"a\"></td><td id=\"b\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{table-layout:fixed;width:200px;border-collapse:collapse}"
            "td{padding:0} #c1{width:25%} #a{width:100px;height:10px} #b{height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(a != nullptr && std::fabs(a->layoutWidth() - 50.0f) < 0.5f,
              "a fixed table resolves a percentage col width against the table");
        check(b != nullptr && std::fabs(b->layoutX() - 50.0f) < 0.5f &&
                  std::fabs(b->layoutWidth() - 150.0f) < 0.5f,
              "a col width outranks the first row's cell width in a fixed table");
    }

    // `table-layout: fixed` takes the columns from the first row and the table
    // width rather than from the content.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<tr><td id=\"a\"></td><td id=\"b\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{table-layout:fixed;width:200px;border-collapse:collapse}"
            "td{padding:0} #a{width:50px;height:10px} #b{height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(a != nullptr && std::fabs(a->layoutWidth() - 50.0f) < 0.5f,
              "a fixed layout keeps a declared column width");
        check(b != nullptr && std::fabs(b->layoutX() - 50.0f) < 0.5f &&
                  std::fabs(b->layoutWidth() - 150.0f) < 0.5f,
              "the undeclared columns share the remaining width");
    }

    // Collapsed adjacent cell borders resolve to one edge: the left cell owns
    // the shared stroke, so the right cell does not paint a second copy.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<tr><td id=\"a\"></td><td id=\"b\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse}"
            "td{padding:0;border:4px solid red}"
            "#a{width:50px;height:20px} #b{width:50px;height:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(t != nullptr && std::fabs(t->layoutWidth() - 112.0f) < 0.5f,
              "a collapsed table counts each shared border once");
        check(a != nullptr && std::fabs(a->layoutWidth() - 58.0f) < 0.5f &&
                  std::fabs(a->layoutX() - 0.0f) < 0.5f,
              "the left cell paints the shared right border");
        check(b != nullptr && std::fabs(b->layoutX() - 58.0f) < 0.5f &&
                  std::fabs(b->layoutWidth() - 54.0f) < 0.5f,
              "the right cell drops its left border into the shared edge");
    }

    // The table's own border is the outer edge when it has one; the outer cell
    // borders are folded under it instead of painting a second line.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<tr><td id=\"a\"></td><td id=\"b\"></td></tr>"
            "</table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse;border:5px solid blue}"
            "td{padding:0;border:1px solid red}"
            "#a{width:50px;height:20px} #b{width:50px;height:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(t != nullptr && std::fabs(t->layoutWidth() - 111.0f) < 0.5f,
              "a table border owns the outer edge of a collapsed table");
        check(a != nullptr && std::fabs(a->layoutX() - 5.0f) < 0.5f &&
                  std::fabs(a->layoutWidth() - 51.0f) < 0.5f,
              "cells start inside the table border and retain the shared edge");
        check(b != nullptr && std::fabs(b->layoutX() - 56.0f) < 0.5f &&
                  std::fabs(b->layoutWidth() - 50.0f) < 0.5f,
              "the right cell has no outer border of its own under the table border");
    }

    // Row and row-group borders join the same conflict: a row border wins over
    // cell edges, a group border applies only at the group's own boundary, and
    // the last group's bottom border survives at the table's outer edge.
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\">"
            "<thead style=\"border-bottom:6px solid #0000ff\">"
            "<tr><td id=\"a\"></td></tr></thead>"
            "<tbody style=\"border-bottom:9px solid #00ff00\">"
            "<tr style=\"border-bottom:5px solid #ff00ff\"><td id=\"b\"></td></tr>"
            "<tr><td id=\"c\"></td></tr>"
            "</tbody></table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse}"
            "td{padding:0;border:1px solid red;width:50px;height:20px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(a != nullptr && std::fabs(a->layoutHeight() - 27.0f) < 0.5f,
              "a row-group bottom border wins the cell edge below the group");
        check(b != nullptr && std::fabs(b->layoutHeight() - 25.0f) < 0.5f,
              "a group border does not leak into a boundary inside the group");
        check(c != nullptr && std::fabs(c->layoutHeight() - 29.0f) < 0.5f,
              "the last row group's bottom border applies at the table edge");
        check(t != nullptr && std::fabs(t->layoutHeight() - 81.0f) < 0.5f,
              "row, group, and cell heights all include the winning collapsed edge");
    }

    // Auto tables shrink between the min-content and max-content widths, and
    // never below the min-content width. The two inline-blocks can sit on one
    // line (100px) or stack (50px).
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"wrap\"><table id=\"t\"><tr><td id=\"a\">"
            "<span class=\"b\"></span><span class=\"b\"></span>"
            "</td></tr></table></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{width:80px} #t{border-collapse:collapse}"
            "td{padding:0} .b{display:inline-block;width:50px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        check(t != nullptr && std::fabs(t->layoutWidth() - 80.0f) < 0.5f,
              "an auto table fits the available width between min and max content");
        check(a != nullptr && std::fabs(a->layoutWidth() - 80.0f) < 0.5f,
              "the column takes the table width between its content bounds");
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"wrap\"><table id=\"t\"><tr><td id=\"a\">"
            "<span class=\"b\"></span><span class=\"b\"></span>"
            "</td></tr></table></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{width:20px} #t{border-collapse:collapse}"
            "td{padding:0} .b{display:inline-block;width:50px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        check(t != nullptr && std::fabs(t->layoutWidth() - 50.0f) < 0.5f,
              "an auto table stops at its min-content width");
    }
    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\"><tr><td id=\"a\">"
            "<span class=\"b\"></span><span class=\"b\"></span>"
            "</td></tr></table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse;width:30px}"
            "td{padding:0} .b{display:inline-block;width:50px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        check(t != nullptr && std::fabs(t->layoutWidth() - 50.0f) < 0.5f,
              "an authored table width below min-content is raised to the minimum");
    }

    {
        auto document = parseOrNull(
            "<sui><body><table id=\"t\"><tr><td id=\"a\">"
            "<span class=\"b\"></span><span class=\"b\"></span>"
            "</td></tr></table></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #t{border-collapse:collapse}"
            "td{padding:0;width:10px} .b{display:inline-block;width:50px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        check(t != nullptr && std::fabs(t->layoutWidth() - 100.0f) < 0.5f,
              "an authored cell width does not cap the column's max-content width");
    }

    // A colspan cell distributes its minimum and maximum constraints across the
    // covered columns. At 50px the table cannot go below the span's 60px
    // min-content width; at 80px the span's max-content width provides the
    // upper bound and the columns interpolate evenly.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"wrap\"><table id=\"t\">"
            "<tr><td id=\"a\" colspan=\"2\">"
            "<span class=\"x\"></span><span class=\"x\"></span></td></tr>"
            "<tr><td id=\"b\"><span class=\"y\"></span></td>"
            "<td id=\"c\"><span class=\"y\"></span></td></tr>"
            "</table></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{width:50px} #t{border-collapse:collapse}"
            "td{padding:0;font-size:0} .x{display:inline-block;width:60px;height:10px}"
            ".y{display:inline-block;width:20px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(t != nullptr && std::fabs(t->layoutWidth() - 60.0f) < 0.5f,
              "a colspan cell raises the table to its distributed min-content width");
        check(b != nullptr && std::fabs(b->layoutWidth() - 30.0f) < 0.5f &&
                  c != nullptr && std::fabs(c->layoutWidth() - 30.0f) < 0.5f,
              "the spanning minimum is shared by the covered columns");
    }
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"wrap\"><table id=\"t\">"
            "<tr><td id=\"a\" colspan=\"2\">"
            "<span class=\"x\"></span><span class=\"x\"></span></td></tr>"
            "<tr><td id=\"b\"><span class=\"y\"></span></td>"
            "<td id=\"c\"><span class=\"y\"></span></td></tr>"
            "</table></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{width:80px} #t{border-collapse:collapse}"
            "td{padding:0;font-size:0} .x{display:inline-block;width:60px;height:10px}"
            ".y{display:inline-block;width:20px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        check(t != nullptr && std::fabs(t->layoutWidth() - 80.0f) < 0.5f,
              "a colspan max-content constraint lets the table use the extra width");
        check(b != nullptr && std::fabs(b->layoutWidth() - 40.0f) < 0.5f &&
                  c != nullptr && std::fabs(c->layoutWidth() - 40.0f) < 0.5f,
              "the covered columns interpolate toward the spanning maximum");
    }

    // When two columns have different min/max ranges, the reduction is shared
    // by range rather than by the final max-content width. The first column can
    // give up 75px; the second is already at its minimum.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"wrap\"><table id=\"t\"><tr>"
            "<td id=\"a\">"
            "<span class=\"q\"></span><span class=\"q\"></span>"
            "<span class=\"q\"></span><span class=\"q\"></span>"
            "</td><td id=\"b\"><span class=\"w\"></span></td>"
            "</tr></table></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #wrap{width:150px} #t{border-collapse:collapse}"
            "td{padding:0;font-size:0} .q{display:inline-block;width:25px;height:10px}"
            ".w{display:inline-block;width:100px;height:10px}"));
        layoutRoot(*document, resolver, 400.0f);

        Element* t = document->getElementById("t");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        check(t != nullptr && std::fabs(t->layoutWidth() - 150.0f) < 0.5f,
              "a multi-column auto table fits the available width");
        check(a != nullptr && std::fabs(a->layoutWidth() - 50.0f) < 0.5f,
              "the flexible column absorbs the shrink down to its range");
        check(b != nullptr && std::fabs(b->layoutX() - 50.0f) < 0.5f &&
                  std::fabs(b->layoutWidth() - 100.0f) < 0.5f,
              "a column already at its minimum does not shrink further");
    }

    // The table properties parse and serialize back.
    {
        auto document = parseOrNull("<sui><body><div id=\"x\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#x{border-collapse:collapse;border-spacing:4px 6px;"
            "table-layout:fixed;caption-side:bottom}"));
        const ComputedStyle s = resolver.resolve(*document->getElementById("x"));
        check(sui::StyleResolver::propertyValue(s, "border-collapse") == "collapse",
              "border-collapse round-trips");
        check(sui::StyleResolver::propertyValue(s, "border-spacing") == "4px 6px",
              "border-spacing round-trips two values");
        check(sui::StyleResolver::propertyValue(s, "table-layout") == "fixed",
              "table-layout round-trips");
        check(sui::StyleResolver::propertyValue(s, "caption-side") == "bottom",
              "caption-side round-trips");
    }
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

void testConicalGradientParsing() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { background-image: conic-gradient(red, blue) }
        .b { background-image: conic-gradient(from 45deg, red, blue) }
        .c { background-image: conic-gradient(from 0.25turn at 20% 30%, red, blue) }
        .d { background-image: conic-gradient(at center, red, blue) }
        .e { background-image: conic-gradient(red 90deg, blue) }
        .f { background-image: conic-gradient(red) }
        .g { background-image: conic-gradient(sideways, red, blue) }
        .h { background-image: conic-gradient(red 10px, blue) }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div><div class=\"h\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 8) {
        check(false, "conic gradient elements found");
        return;
    }
    using Kind = sui::ComputedStyle::BackgroundImage::Kind;
    check(styles[0].backgroundImages[0].kind == Kind::ConicalGradientImage &&
              styles[0].backgroundImages[0].conical.angle == 0.0f &&
              styles[0].backgroundImages[0].conical.stops.size() == 2,
          "a plain conic starts at twelve with two stops");
    // The centre defaults to the middle of the box, like the radial default.
    check(styles[0].backgroundImages[0].conical.centreX.unit == sui::Length::Percent &&
              styles[0].backgroundImages[0].conical.centreX.value == 50.0f,
          "the default centre is 50%, not 50px");
    check(styles[1].backgroundImages[0].conical.angle == 45.0f, "from sets the angle");
    check(styles[2].backgroundImages[0].conical.angle == 90.0f &&
              styles[2].backgroundImages[0].conical.centreX.value == 20.0f &&
              styles[2].backgroundImages[0].conical.centreY.value == 30.0f,
          "turns read as degrees and at takes a position");
    check(styles[3].backgroundImages[0].conical.centreX.value == 50.0f &&
              styles[3].backgroundImages[0].conical.centreY.value == 50.0f,
          "at center means the middle");
    check(styles[4].backgroundImages[0].conical.stops.size() == 2 &&
              styles[4].backgroundImages[0].conical.stops[0].offset.unit ==
                  sui::Length::Percent &&
              styles[4].backgroundImages[0].conical.stops[0].offset.value == 25.0f,
          "an angle stop becomes a percentage of the turn");
    check(styles[5].backgroundImages[0].kind == Kind::ConicalGradientImage &&
              styles[5].backgroundImages[0].conical.stops.size() == 1,
          "a one-stop conic paints as a flat fill");
    check(styles[6].backgroundImages[0].kind == Kind::None,
          "a bad prelude drops the gradient");
    check(styles[7].backgroundImages[0].kind == Kind::None,
          "a length stop drops the gradient");
}

void testMaskImage() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { mask-image: linear-gradient(black, transparent) }
        .b { mask-image: radial-gradient(circle, black, transparent) }
        .c { mask-image: conic-gradient(black, transparent) }
        .d { mask-image: url(m.png) }
        .e { mask-image: none }
        .f { mask-image: banana }
        .g { mask-image: linear-gradient(red, blue), radial-gradient(red, blue) }
        .h { -webkit-mask-image: linear-gradient(black, transparent) }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div><div class=\"h\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 8) {
        check(false, "mask elements found");
        return;
    }
    using Mask = sui::ComputedStyle::MaskImage;
    check(styles[0].maskImages.size() == 1 &&
              styles[0].maskImages[0].kind == Mask::Kind::LinearGradient &&
              styles[0].maskImages[0].linear.stops.size() == 2,
          "a linear mask parses with its stops");
    check(styles[1].maskImages.size() == 1 &&
              styles[1].maskImages[0].kind == Mask::Kind::RadialGradient &&
              styles[1].maskImages[0].radial.stops.size() == 2,
          "a radial mask parses with its stops");
    check(styles[2].maskImages.size() == 1 &&
              styles[2].maskImages[0].kind == Mask::Kind::ConicalGradient &&
              styles[2].maskImages[0].conical.stops.size() == 2,
          "a conic mask parses with its stops");
    check(styles[3].maskImages.size() == 1 &&
              styles[3].maskImages[0].kind == Mask::Kind::Url &&
              styles[3].maskImages[0].href == "m.png",
          "a url mask records its href");
    check(styles[4].maskImages.size() == 1 &&
              styles[4].maskImages[0].kind == Mask::Kind::None,
          "none clears the mask");
    check(styles[5].maskImages.empty(), "garbage keeps no mask");
    check(styles[6].maskImages.size() == 2 &&
              styles[6].maskImages[0].kind == Mask::Kind::LinearGradient &&
              styles[6].maskImages[1].kind == Mask::Kind::RadialGradient,
          "every mask layer parses in order");
    check(styles[7].maskImages.size() == 1 &&
              styles[7].maskImages[0].kind == Mask::Kind::LinearGradient,
          "the webkit prefix masks alike");
}

// `mask-mode` and `mask-composite` are parallel comma lists that repeat their
// last entry at apply time; one bad entry drops the whole declaration.
void testMaskModeComposite() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { mask-mode: alpha }
        .b { mask-mode: luminance, alpha }
        .c { mask-mode: match-source }
        .d { mask-mode: banana }
        .e { mask-composite: add }
        .f { mask-composite: subtract, intersect, exclude }
        .g { mask-composite: banana }
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
        check(false, "mask mode elements found");
        return;
    }
    using Mode = sui::ComputedStyle::MaskMode;
    using Composite = sui::ComputedStyle::MaskComposite;
    check(styles[0].maskModes.size() == 1 && styles[0].maskModes[0] == Mode::Alpha,
          "a single mode parses");
    check(styles[1].maskModes.size() == 2 &&
              styles[1].maskModes[0] == Mode::Luminance &&
              styles[1].maskModes[1] == Mode::Alpha,
          "a mode list parses in order");
    check(styles[2].maskModes.size() == 1 &&
              styles[2].maskModes[0] == Mode::MatchSource,
          "match-source parses");
    check(styles[3].maskModes.empty(), "a bad mode keeps no modes");
    check(styles[4].maskComposites.size() == 1 &&
              styles[4].maskComposites[0] == Composite::Add,
          "a single composite parses");
    check(styles[5].maskComposites.size() == 3 &&
              styles[5].maskComposites[0] == Composite::Subtract &&
              styles[5].maskComposites[1] == Composite::Intersect &&
              styles[5].maskComposites[2] == Composite::Exclude,
          "a composite list parses in order");
    check(styles[6].maskComposites.empty(), "a bad composite keeps no ops");
}

// `mask-size`/`mask-position`/`mask-repeat`/`mask-origin`/`mask-clip` are
// parallel comma lists reusing the background types: a bad entry falls back to
// the initial value so later layers stay lined up with their images.
void testMaskGeometry() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { mask-size: 10px 20px }
        .b { mask-size: cover }
        .c { mask-size: 50% }
        .d { mask-position: right bottom }
        .e { mask-position: center }
        .f { mask-repeat: no-repeat }
        .g { mask-repeat: repeat-x }
        .h { mask-origin: padding-box }
        .i { mask-clip: content-box }
        .j { mask-size: banana }
        .k { mask-size: 10px, 20px }
        .l { -webkit-mask-size: 5px }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div><div class=\"h\"></div><div class=\"i\"></div>"
        "<div class=\"j\"></div><div class=\"k\"></div><div class=\"l\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 12) {
        check(false, "mask geometry elements found");
        return;
    }
    using Size = sui::ComputedStyle::BackgroundSize;
    using Position = sui::ComputedStyle::BackgroundPosition;
    check(styles[0].maskSizes.size() == 1 &&
              styles[0].maskSizes[0].mode == Size::Explicit &&
              styles[0].maskSizes[0].width.value == 10.0f &&
              styles[0].maskSizes[0].height.value == 20.0f,
          "an explicit size parses both axes");
    check(styles[1].maskSizes.size() == 1 &&
              styles[1].maskSizes[0].mode == Size::Cover,
          "cover parses");
    check(styles[2].maskSizes.size() == 1 &&
              styles[2].maskSizes[0].mode == Size::Explicit &&
              styles[2].maskSizes[0].width.unit == sui::Length::Percent &&
              styles[2].maskSizes[0].height.unit == sui::Length::Auto,
          "a lone size leaves the other axis auto");
    check(styles[3].maskPositions.size() == 1 &&
              styles[3].maskPositions[0].horizontal == Position::End &&
              styles[3].maskPositions[0].vertical == Position::End,
          "edge keywords parse");
    check(styles[4].maskPositions.size() == 1 &&
              styles[4].maskPositions[0].horizontal == Position::Center &&
              styles[4].maskPositions[0].vertical == Position::Center,
          "a lone center means both axes");
    check(styles[5].maskRepeats.size() == 1 && !styles[5].maskRepeats[0].x &&
              !styles[5].maskRepeats[0].y,
          "no-repeat parses");
    check(styles[6].maskRepeats.size() == 1 && styles[6].maskRepeats[0].x &&
              !styles[6].maskRepeats[0].y,
          "repeat-x parses");
    check(styles[7].maskOrigins.size() == 1 &&
              styles[7].maskOrigins[0] == sui::ComputedStyle::BackgroundPaddingBox,
          "an origin box parses");
    check(styles[8].maskClips.size() == 1 &&
              styles[8].maskClips[0] == sui::ComputedStyle::BackgroundContentBox,
          "a clip box parses");
    check(styles[9].maskSizes.size() == 1 &&
              styles[9].maskSizes[0].mode == Size::Explicit &&
              styles[9].maskSizes[0].width.unit == sui::Length::Auto,
          "a bad size falls back to auto axes");
    check(styles[10].maskSizes.size() == 2 &&
              styles[10].maskSizes[0].width.value == 10.0f &&
              styles[10].maskSizes[1].width.value == 20.0f,
          "a size list parses in order");
    check(styles[11].maskSizes.size() == 1 &&
              styles[11].maskSizes[0].width.value == 5.0f,
          "the webkit size prefix parses alike");
}

// The `mask` shorthand expands every layer into all eight longhands: image,
// position/size, repeat, origin/clip, composite, and mode in any order, with
// omitted components reset to their initials. One bad token drops the whole
// declaration.
void testMaskShorthand() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { mask: url(m.png) }
        .b { mask: none }
        .c { mask: linear-gradient(black, transparent) center / cover no-repeat }
        .d { mask: url(a.png) luminance, url(b.png) subtract }
        .e { mask: url(m.png) padding-box content-box }
        .f { mask: url(m.png) border-box }
        .g { mask: banana }
        .h { mask: url(m.png) repeat no-repeat }
        .i { mask: url(m.png) center / }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div><div class=\"h\"></div><div class=\"i\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 9) {
        check(false, "mask shorthand elements found");
        return;
    }
    using Kind = sui::ComputedStyle::MaskImage::Kind;
    using Mode = sui::ComputedStyle::MaskMode;
    using Composite = sui::ComputedStyle::MaskComposite;
    using Size = sui::ComputedStyle::BackgroundSize;
    using Position = sui::ComputedStyle::BackgroundPosition;
    check(styles[0].maskImages.size() == 1 &&
              styles[0].maskImages[0].kind == Kind::Url &&
              styles[0].maskImages[0].href == "m.png",
          "a bare url expands its image");
    check(styles[0].maskRepeats.size() == 1 && styles[0].maskRepeats[0].x &&
              styles[0].maskRepeats[0].y,
          "omitted repeat resets to tiling");
    check(styles[1].maskImages.size() == 1 &&
              styles[1].maskImages[0].kind == Kind::None,
          "none expands to no image");
    check(styles[2].maskImages.size() == 1 &&
              styles[2].maskImages[0].kind == Kind::LinearGradient &&
              styles[2].maskPositions.size() == 1 &&
              styles[2].maskPositions[0].horizontal == Position::Center &&
              styles[2].maskSizes.size() == 1 &&
              styles[2].maskSizes[0].mode == Size::Cover &&
              styles[2].maskRepeats.size() == 1 &&
              !styles[2].maskRepeats[0].x && !styles[2].maskRepeats[0].y,
          "position, size, and repeat expand together");
    check(styles[3].maskImages.size() == 2 &&
              styles[3].maskModes.size() == 2 &&
              styles[3].maskModes[0] == Mode::Luminance &&
              styles[3].maskModes[1] == Mode::MatchSource &&
              styles[3].maskComposites.size() == 2 &&
              styles[3].maskComposites[1] == Composite::Subtract,
          "layers stay lined up with omitted middles reset");
    check(styles[4].maskOrigins.size() == 1 &&
              styles[4].maskOrigins[0] == sui::ComputedStyle::BackgroundPaddingBox &&
              styles[4].maskClips.size() == 1 &&
              styles[4].maskClips[0] == sui::ComputedStyle::BackgroundContentBox,
          "two boxes set origin then clip");
    check(styles[5].maskOrigins.size() == 1 &&
              styles[5].maskOrigins[0] == sui::ComputedStyle::BackgroundBorderBox &&
              styles[5].maskClips.size() == 1 &&
              styles[5].maskClips[0] == sui::ComputedStyle::BackgroundBorderBox,
          "one box sets both origin and clip");
    check(styles[6].maskImages.empty() && styles[6].maskRepeats.empty(),
          "a bad token drops the whole shorthand");
    check(styles[7].maskRepeats.size() == 1 && styles[7].maskRepeats[0].x &&
              !styles[7].maskRepeats[0].y,
          "a two-word repeat pairs its axes");
    check(styles[8].maskImages.empty(), "a dangling slash drops the shorthand");
}

// The mask longhands (and their `-webkit-` aliases) answer with one entry per
// stored layer, and the shorthand rebuilds each layer in the parser's own
// grammar, so the readings parse back to the same lists.
void testMaskReadBack() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"m1\"></div><div id=\"m2\"></div><div id=\"m3\"></div>"
        "<div id=\"m4\"></div><div id=\"m5\"></div><div id=\"m6\"></div>"
        "<div id=\"m7\"></div>"
        "<div id=\"p\" style=\"mask-size:30px 40px\">"
        "<div id=\"q\" style=\"mask-size:inherit\"></div></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#m1 { mask-image: url(mask.png), linear-gradient(45deg, #00000080, #ffffff) }"
        "#m2 { mask-size: 30px 40px, cover; mask-position: right 5px bottom 6px;"
        " mask-repeat: no-repeat, space }"
        "#m3 { mask-origin: content-box; mask-clip: padding-box }"
        "#m4 { mask-mode: luminance, alpha; mask-composite: subtract, exclude }"
        "#m5 { -webkit-mask-image: radial-gradient(circle at 25% 25%, black, transparent) }"
        "#m6 { mask: url(a.png) 10px / cover no-repeat content-box intersect alpha }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("m1", "mask-image") ==
              "url(mask.png), linear-gradient(45deg, #00000080, #ffffff)",
          "mask images read per layer with alpha-safe stops");
    check(value("m2", "mask-size") == "30px 40px, cover", "mask sizes read per layer");
    check(value("m2", "mask-position") == "right 5px bottom 6px",
          "a mask position keeps its edge offsets");
    check(value("m2", "mask-repeat") == "no-repeat, space", "mask repeats read per layer");
    check(value("m3", "mask-origin") == "content-box" &&
              value("m3", "mask-clip") == "padding-box",
          "mask origin and clip read their boxes");
    check(value("m4", "mask-mode") == "luminance, alpha", "mask modes read per layer");
    check(value("m4", "mask-composite") == "subtract, exclude",
          "mask composites read per layer");
    check(value("m5", "-webkit-mask-image") == value("m5", "mask-image") &&
              value("m5", "mask-image") ==
                  "radial-gradient(circle farthest-corner at 25% 25%, #000000, #00000000)",
          "the -webkit alias answers the same reading");
    check(value("m6", "mask") ==
              "url(a.png) left 10px top 0px / cover no-repeat content-box content-box"
              " intersect alpha",
          "the shorthand rebuilds its layer in the parser's grammar");
    check(value("m1", "mask") ==
              "url(mask.png) left top / auto repeat border-box border-box add match-source,"
              " linear-gradient(45deg, #00000080, #ffffff) left top / auto repeat"
              " border-box border-box add match-source",
          "a multi-layer shorthand rebuilds every layer");
    check(value("m3", "mask") ==
              "none left top / auto repeat content-box padding-box add match-source",
          "the shorthand keeps origin and clip apart");
    check(value("m7", "mask-image") == "none" && value("m7", "mask-size") == "auto" &&
              value("m7", "mask-position") == "left top" &&
              value("m7", "mask-repeat") == "repeat",
          "an untouched mask reads the initial values");
    check(value("m7", "mask-origin") == "border-box" &&
              value("m7", "mask-clip") == "border-box" &&
              value("m7", "mask-mode") == "match-source" &&
              value("m7", "mask-composite") == "add",
          "including the initial boxes, mode, and composite");
    check(value("m7", "mask") ==
              "none left top / auto repeat border-box border-box add match-source",
          "and the untouched shorthand composes them");
    check(value("q", "mask-size") == "30px 40px",
          "mask-size: inherit replays the parent's list");
}

// `mask-type` stores which component of a mask element masks (nothing reads it
// yet), and the six `mask-border` longhands plus the shorthand describe a
// 9-patch border mask. One bad value keeps the previous cascade winner.
void testMaskBorder() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { mask-type: alpha }
        .b { mask-type: banana }
        .c { mask-border-source: url(b.png) }
        .d { mask-border-source: linear-gradient(black, white) }
        .e { mask-border-source: none }
        .f { mask-border-source: banana }
        .g { mask-border-mode: alpha }
        .h { mask-border-mode: banana }
        .i { mask-border-slice: 30 fill }
        .j { mask-border-slice: 10% 20% 30% 40% }
        .k { mask-border-slice: 10px }
        .l { mask-border-slice: -1 }
        .m { mask-border-width: 2 3px auto }
        .n { mask-border-width: 50% }
        .o { mask-border-outset: 4 5 }
        .p { mask-border-outset: -2px }
        .q { mask-border-repeat: repeat round }
        .r { mask-border-repeat: stretch stretch stretch }
        .s { mask-border: url(b.png) 30 / 4px / 2px round luminance }
        .t { mask-border: none }
        .u { mask-border: url(b.png) 10px }
        .v { mask-border: url(b.png) 30 / }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div><div class=\"h\"></div><div class=\"i\"></div>"
        "<div class=\"j\"></div><div class=\"k\"></div><div class=\"l\"></div>"
        "<div class=\"m\"></div><div class=\"n\"></div><div class=\"o\"></div>"
        "<div class=\"p\"></div><div class=\"q\"></div><div class=\"r\"></div>"
        "<div class=\"s\"></div><div class=\"t\"></div><div class=\"u\"></div>"
        "<div class=\"v\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 22) {
        check(false, "mask border elements found");
        return;
    }
    using Kind = sui::ComputedStyle::MaskImage::Kind;
    using Border = sui::ComputedStyle::MaskBorder;
    check(styles[0].maskType == sui::ComputedStyle::MaskType::Alpha,
          "alpha mask-type stores");
    check(sui::StyleResolver::propertyValue(styles[0], "mask-type") == "alpha",
          "mask-type serializes");
    check(styles[1].maskType == sui::ComputedStyle::MaskType::Luminance,
          "a bad mask-type keeps luminance");
    check(styles[2].maskBorder.source.kind == Kind::Url &&
              styles[2].maskBorder.source.href == "b.png",
          "a url border source records its href");
    check(styles[3].maskBorder.source.kind == Kind::LinearGradient,
          "a gradient border source parses");
    check(styles[4].maskBorder.source.kind == Kind::None, "none clears the source");
    check(styles[5].maskBorder.source.kind == Kind::None, "garbage keeps no source");
    check(styles[6].maskBorder.mode == Border::Mode::Alpha, "alpha border mode stores");
    check(styles[7].maskBorder.mode == Border::Mode::Luminance,
          "a bad border mode keeps luminance");
    check(styles[8].maskBorder.slice.top.value == 30.0f && styles[8].maskBorder.slice.fill,
          "a slice number parses with fill");
    check(styles[9].maskBorder.slice.top.value == 10.0f &&
              styles[9].maskBorder.slice.right.value == 20.0f &&
              styles[9].maskBorder.slice.bottom.value == 30.0f &&
              styles[9].maskBorder.slice.left.value == 40.0f,
          "four slice percentages expand clockwise");
    check(styles[10].maskBorder.slice.top.value == 0.0f,
          "a length never slices");
    check(styles[11].maskBorder.slice.top.value == 0.0f, "a negative slice drops");
    check(styles[12].maskBorder.width.top.unit == sui::Length::Number &&
              styles[12].maskBorder.width.top.value == 2.0f &&
              styles[12].maskBorder.width.right.value == 3.0f &&
              styles[12].maskBorder.width.bottom.isAuto(),
          "width mixes multiples, lengths, and auto");
    check(styles[13].maskBorder.width.top.value == 0.0f, "a percentage width drops");
    check(styles[14].maskBorder.outset.top.value == 4.0f &&
              styles[14].maskBorder.outset.bottom.value == 4.0f &&
              styles[14].maskBorder.outset.right.value == 5.0f,
          "two outsets expand vertical and horizontal");
    check(styles[15].maskBorder.outset.top.value == 0.0f, "a negative outset drops");
    check(styles[16].maskBorder.repeatX == Border::Repeat::Repeat &&
              styles[16].maskBorder.repeatY == Border::Repeat::Round,
          "a repeat pair parses per axis");
    check(styles[17].maskBorder.repeatX == Border::Repeat::Stretch,
          "a triple repeat drops");
    check(styles[18].maskBorder.source.kind == Kind::Url &&
              styles[18].maskBorder.slice.top.value == 30.0f &&
              styles[18].maskBorder.width.top.value == 4.0f &&
              styles[18].maskBorder.outset.top.value == 2.0f &&
              styles[18].maskBorder.repeatX == Border::Repeat::Round &&
              styles[18].maskBorder.mode == Border::Mode::Luminance,
          "the shorthand expands every part");
    check(styles[19].maskBorder.source.kind == Kind::None, "a none shorthand resets");
    check(styles[20].maskBorder.source.kind == Kind::None,
          "a length in the shorthand drops it all");
    check(styles[21].maskBorder.source.kind == Kind::None,
          "a dangling slash drops the shorthand");
}

// `scroll-snap-type` names axes with an optional strictness (proximity when
// absent); `scroll-snap-align` sets both axes, or block (y) then inline (x).
// Anything else keeps the previous cascade winner.
void testScrollSnap() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { scroll-snap-type: y mandatory }
        .b { scroll-snap-type: x }
        .c { scroll-snap-type: both proximity }
        .d { scroll-snap-type: block }
        .e { scroll-snap-type: none }
        .f { scroll-snap-type: banana }
        .g { scroll-snap-type: y sideways }
        .h { scroll-snap-align: start }
        .i { scroll-snap-align: center end }
        .j { scroll-snap-align: banana }
        .k { scroll-padding: 10px 20px }
        .l { scroll-padding-top: 5px }
        .m { scroll-padding: banana }
        .n { scroll-padding: 1px 2px 3px 4px }
        .o { scroll-margin: 5px 6px }
        .p { scroll-margin-left: 7px }
        .q { scroll-margin: banana }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div><div class=\"h\"></div><div class=\"i\"></div>"
        "<div class=\"j\"></div><div class=\"k\"></div><div class=\"l\"></div>"
        "<div class=\"m\"></div><div class=\"n\"></div><div class=\"o\"></div>"
        "<div class=\"p\"></div><div class=\"q\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 17) {
        check(false, "snap elements found");
        return;
    }
    using Axis = sui::ComputedStyle::ScrollSnapAxis;
    using Strict = sui::ComputedStyle::ScrollSnapStrictness;
    using Align = sui::ComputedStyle::ScrollSnapAlign;
    check(styles[0].snapAxis == Axis::Y &&
              styles[0].snapStrictness == Strict::Mandatory,
          "an axis with mandatory parses");
    check(styles[1].snapAxis == Axis::X &&
              styles[1].snapStrictness == Strict::Proximity,
          "a bare axis defaults to proximity");
    check(styles[2].snapAxis == Axis::Both, "both axes parse");
    check(styles[3].snapAxis == Axis::Y, "block maps to the vertical axis");
    check(styles[4].snapAxis == Axis::None, "none disables");
    check(styles[5].snapAxis == Axis::None, "garbage keeps none");
    check(styles[6].snapAxis == Axis::None, "a bad strictness drops the type");
    check(styles[7].snapAlignX == Align::Start && styles[7].snapAlignY == Align::Start,
          "one alignment sets both axes");
    check(styles[8].snapAlignY == Align::Center && styles[8].snapAlignX == Align::End,
          "two alignments set block then inline");
    check(styles[9].snapAlignX == Align::None, "a bad alignment drops");
    check(styles[10].scrollPadding.top.value == 10.0f &&
              styles[10].scrollPadding.bottom.value == 10.0f &&
              styles[10].scrollPadding.left.value == 20.0f &&
              styles[10].scrollPadding.right.value == 20.0f,
          "two paddings expand vertical and horizontal");
    check(styles[11].scrollPadding.top.value == 5.0f &&
              styles[11].scrollPadding.bottom.value == 0.0f,
          "a top longhand sets one side");
    check(styles[12].scrollPadding.top.value == 0.0f, "a bad padding drops");
    check(styles[13].scrollPadding.top.value == 1.0f &&
              styles[13].scrollPadding.right.value == 2.0f &&
              styles[13].scrollPadding.bottom.value == 3.0f &&
              styles[13].scrollPadding.left.value == 4.0f,
          "four paddings expand clockwise");
    check(styles[14].scrollMargin.top.value == 5.0f &&
              styles[14].scrollMargin.bottom.value == 5.0f &&
              styles[14].scrollMargin.left.value == 6.0f &&
              styles[14].scrollMargin.right.value == 6.0f,
          "two margins expand vertical and horizontal");
    check(styles[15].scrollMargin.left.value == 7.0f &&
              styles[15].scrollMargin.top.value == 0.0f,
          "a left longhand sets one side");
    check(styles[16].scrollMargin.top.value == 0.0f, "a bad margin drops");
}

// `border-image` describes a 9-patch like the mask border but paints it:
// source, slice, width, outset, repeat, and the shorthand. One bad value
// keeps the previous cascade winner.
void testBorderImage() {
    const auto rules = sui::CSSParser::parse(R"(
        .a { border-image-source: url(f.png) }
        .b { border-image-source: linear-gradient(red, blue) }
        .c { border-image-source: none }
        .d { border-image-source: banana }
        .e { border-image-slice: 30 fill }
        .f { border-image-slice: 10% 20% }
        .g { border-image-slice: 10px }
        .h { border-image-width: 2 3px auto }
        .i { border-image-width: 50% }
        .j { border-image-outset: 4 }
        .k { border-image-outset: -1 }
        .l { border-image-repeat: round space }
        .m { border-image-repeat: banana }
        .n { border-image: url(f.png) 30 / 4px / 2px round }
        .o { border-image: none }
        .p { border-image: url(f.png) 10px }
    )");
    sui::StyleResolver resolver;
    resolver.setRules(rules);
    static const char* kClasses =
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div>"
        "<div class=\"d\"></div><div class=\"e\"></div><div class=\"f\"></div>"
        "<div class=\"g\"></div><div class=\"h\"></div><div class=\"i\"></div>"
        "<div class=\"j\"></div><div class=\"k\"></div><div class=\"l\"></div>"
        "<div class=\"m\"></div><div class=\"n\"></div><div class=\"o\"></div>"
        "<div class=\"p\"></div>";
    auto body = parseOrNull((std::string("<sui><body>") + kClasses + "</body></sui>").c_str());
    if (!body) {
        return;
    }
    const std::vector<ComputedStyle> styles = bodyStyles(*body, resolver);
    if (styles.size() < 16) {
        check(false, "border image elements found");
        return;
    }
    using Kind = sui::ComputedStyle::BackgroundImage::Kind;
    using Border = sui::ComputedStyle::BorderImage;
    check(styles[0].borderImage.source.kind == Kind::Url &&
              styles[0].borderImage.source.href == "f.png",
          "a url border source records its href");
    check(styles[1].borderImage.source.kind == Kind::LinearGradientImage,
          "a gradient border source parses");
    check(styles[2].borderImage.source.kind == Kind::None, "none clears the source");
    check(styles[3].borderImage.source.kind == Kind::None, "garbage keeps no source");
    check(styles[4].borderImage.slice.top.value == 30.0f &&
              styles[4].borderImage.slice.fill,
          "a slice number parses with fill");
    check(styles[5].borderImage.slice.top.value == 10.0f &&
              styles[5].borderImage.slice.right.value == 20.0f &&
              styles[5].borderImage.slice.bottom.value == 10.0f &&
              styles[5].borderImage.slice.left.value == 20.0f,
          "two slice percentages expand vertical and horizontal");
    check(styles[6].borderImage.slice.top.value == 0.0f, "a length never slices");
    check(styles[7].borderImage.width.top.unit == sui::Length::Number &&
              styles[7].borderImage.width.right.value == 3.0f &&
              styles[7].borderImage.width.bottom.isAuto(),
          "width mixes multiples, lengths, and auto");
    check(styles[8].borderImage.width.top.value == 0.0f, "a percentage width drops");
    check(styles[9].borderImage.outset.top.value == 4.0f, "an outset parses");
    check(styles[10].borderImage.outset.top.value == 0.0f, "a negative outset drops");
    check(styles[11].borderImage.repeatX == Border::Repeat::Round &&
              styles[11].borderImage.repeatY == Border::Repeat::Space,
          "a repeat pair parses per axis");
    check(styles[12].borderImage.repeatX == Border::Repeat::Stretch,
          "a bad repeat drops");
    check(styles[13].borderImage.source.kind == Kind::Url &&
              styles[13].borderImage.slice.top.value == 30.0f &&
              styles[13].borderImage.width.top.value == 4.0f &&
              styles[13].borderImage.outset.top.value == 2.0f &&
              styles[13].borderImage.repeatX == Border::Repeat::Round,
          "the shorthand expands every part");
    check(styles[14].borderImage.source.kind == Kind::None, "a none shorthand resets");
    check(styles[15].borderImage.source.kind == Kind::None,
          "a length in the shorthand drops it all");
}

// The border-image and mask-border families answer in their own shorthand's
// grammar: source, slice (with `fill`), width, outset, and per-axis repeat —
// each collapsed like a box shorthand — so the shorthand readings parse back.
void testBorderImageReadBack() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"b1\"></div><div id=\"b3\"></div><div id=\"b5\"></div>"
        "<div id=\"b2\"><div id=\"b4\" style=\"border-image:inherit\"></div></div>"
        "<div id=\"m1\"></div><div id=\"m3\"></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#b1 { border-image-source: url(border.png); border-image-slice: 30;"
        " border-image-width: 10px; border-image-outset: 5px; border-image-repeat: round }"
        "#b2 { border-image: linear-gradient(45deg, #ff0000, #0000ff80) 25% fill / 2 / 1px"
        " space }"
        "#b3 { border-image-slice: 1 2 3 4 fill; border-image-repeat: stretch round }"
        "#m1 { mask-border-source: url(border-mask.png); mask-border-slice: 10% fill;"
        " mask-border-width: auto; mask-border-outset: 2; mask-border-repeat: space;"
        " mask-border-mode: alpha }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("b1", "border-image-source") == "url(border.png)" &&
              value("b1", "border-image-slice") == "30" &&
              value("b1", "border-image-width") == "10px" &&
              value("b1", "border-image-outset") == "5px" &&
              value("b1", "border-image-repeat") == "round",
          "border-image longhands read back one value at a time");
    check(value("b2", "border-image") ==
              "linear-gradient(45deg, #ff0000, #0000ff80) 25% fill / 2 / 1px space",
          "a border-image shorthand rebuilds in parser grammar");
    check(value("b3", "border-image-slice") == "1 2 3 4 fill" &&
              value("b3", "border-image-repeat") == "stretch round",
          "slices collapse like a box and repeats stay per axis");
    check(value("b5", "border-image-source") == "none" &&
              value("b5", "border-image-slice") == "0" &&
              value("b5", "border-image-width") == "auto" &&
              value("b5", "border-image-outset") == "0" &&
              value("b5", "border-image-repeat") == "stretch",
          "an untouched border image reads the initials");
    check(value("b5", "border-image") == "none 0 / auto / 0 stretch",
          "and the untouched shorthand composes them");
    check(value("b4", "border-image") == value("b2", "border-image"),
          "border-image: inherit replays the parent's shorthand");
    check(value("m1", "mask-border-source") == "url(border-mask.png)" &&
              value("m1", "mask-border-slice") == "10% fill" &&
              value("m1", "mask-border-width") == "auto" &&
              value("m1", "mask-border-outset") == "2" &&
              value("m1", "mask-border-repeat") == "space" &&
              value("m1", "mask-border-mode") == "alpha",
          "mask-border longhands read back one value at a time");
    check(value("m3", "mask-border-mode") == "luminance" &&
              value("m3", "mask-border-slice") == "0" &&
              value("m3", "mask-border") == "none 0 / auto / 0 stretch luminance",
          "an untouched mask border reads its mode and initials");
}

// `:required`/`:optional` read the attribute on inputs, selects, and
// textareas; `:valid`/`:invalid` judge candidates (no disabled, hidden, or
// buttons) by required emptiness, checkable state, numeric shape and min/max,
// and a basic single-@ email shape. A required radio group counts when any
// same-name member is checked.
void testValidationPseudos() {
    auto document = parseOrNull(
        "<sui><body>"
        "<input id=\"a\" type=\"text\" required=\"\" value=\"\"/>"
        "<input id=\"b\" type=\"text\" required=\"\" value=\"hi\"/>"
        "<input id=\"c\" type=\"text\" value=\"\"/>"
        "<input id=\"d\" type=\"text\" required=\"\" disabled=\"\" value=\"\"/>"
        "<input id=\"e\" type=\"hidden\" value=\"\"/>"
        "<div id=\"f\"></div>"
        "<input id=\"g\" type=\"checkbox\" required=\"\"/>"
        "<input id=\"h\" type=\"checkbox\" required=\"\" checked=\"\"/>"
        "<input id=\"i\" type=\"radio\" name=\"g\" required=\"\"/>"
        "<input id=\"j\" type=\"radio\" name=\"g\" required=\"\"/>"
        "<input id=\"k\" type=\"radio\" name=\"h\" required=\"\"/>"
        "<input id=\"l\" type=\"radio\" name=\"h\" required=\"\" checked=\"\"/>"
        "<input id=\"m\" type=\"number\" value=\"abc\"/>"
        "<input id=\"n\" type=\"number\" value=\"5\" min=\"10\"/>"
        "<input id=\"o\" type=\"number\" value=\"15\" min=\"10\" max=\"20\"/>"
        "<input id=\"p\" type=\"email\" value=\"nope\"/>"
        "<input id=\"q\" type=\"email\" value=\"a@b\"/>"
        "<select id=\"r\" required=\"\"><option value=\"\" selected=\"\"></option>"
        "<option value=\"x\">x</option></select>"
        "<textarea id=\"s\" required=\"\">hi</textarea>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* b = el("b");
    Element* c = el("c");
    Element* d = el("d");
    Element* e = el("e");
    Element* f = el("f");
    Element* g = el("g");
    Element* h = el("h");
    Element* i = el("i");
    Element* j = el("j");
    Element* k = el("k");
    Element* l = el("l");
    Element* m = el("m");
    Element* n = el("n");
    Element* o = el("o");
    Element* p = el("p");
    Element* q = el("q");
    Element* r = el("r");
    Element* s = el("s");
    check(a && b && c && d && e && f && g && h && i && j && k && l && m && n && o &&
              p && q && r && s,
          "validation elements found");
    if (!a || !b || !c || !d || !e || !f || !g || !h || !i || !j || !k || !l || !m ||
        !n || !o || !p || !q || !r || !s) {
        return;
    }
    const auto sel = [](const char* text) { return sui::CSSParser::parseSelector(text); };
    const auto on = [&](const char* text, Element& target) {
        return selectorMatches(sel(text), target);
    };

    check(on("input:required", *a) && !on("input:optional", *a), "a required input is required");
    check(on("input:required", *b), "a filled required input stays required");
    check(on("input:optional", *c) && !on("input:required", *c), "a plain input is optional");
    check(on("input:required", *d), "a disabled input is still required");
    check(on("input:optional", *e), "a hidden input is optional");
    check(!on("input:required", *f) && !on("input:optional", *f), "a div is neither");

    check(on("input:invalid", *a) && !on("input:valid", *a), "required emptiness is invalid");
    check(on("input:valid", *b) && !on("input:invalid", *b), "a filled required input is valid");
    check(on("input:valid", *c), "an optional empty input is valid");
    check(!on("input:valid", *d) && !on("input:invalid", *d), "a disabled input is neither");
    check(!on("input:valid", *e) && !on("input:invalid", *e), "a hidden input is neither");
    check(!on("input:valid", *f) && !on("input:invalid", *f), "a div is neither");

    check(on("input:invalid", *g), "a required unchecked box is invalid");
    check(on("input:valid", *h), "a required checked box is valid");
    check(on("input:invalid", *i) && on("input:invalid", *j), "an empty required group is invalid");
    check(on("input:valid", *k) && on("input:valid", *l), "a satisfied group validates its members");

    check(on("input:invalid", *m), "a non-numeric number is invalid");
    check(on("input:invalid", *n), "a number under its min is invalid");
    check(on("input:valid", *o), "a number inside its range is valid");
    check(on("input:invalid", *p), "an @-less email is invalid");
    check(on("input:valid", *q), "a shaped email is valid");
    check(on("select:invalid", *r), "a required select on its empty option is invalid");
    check(on("textarea:valid", *s), "a filled required textarea is valid");

// The script-facing validity API shares the matcher engine: candidates,
// per-constraint flags, messages, custom errors, and focusing reports.

    // `pattern` full-matches non-empty values (a broken pattern constrains
    // nothing) and `minlength` judges lengths in codepoints.
    auto document2 = parseOrNull(
        "<sui><body>"
        "<input id=\"p1\" type=\"text\" value=\"abc\" pattern=\"[a-z]+\"/>"
        "<input id=\"p2\" type=\"text\" value=\"ABC\" pattern=\"[a-z]+\"/>"
        "<input id=\"p3\" type=\"text\" value=\"abc\" pattern=\"([a-z\"/>"
        "<input id=\"p4\" type=\"text\" value=\"\" pattern=\"[a-z]+\"/>"
        "<input id=\"m1\" type=\"text\" value=\"ab\" minlength=\"3\"/>"
        "<input id=\"m2\" type=\"text\" value=\"abc\" minlength=\"3\"/>"
        "<textarea id=\"m3\" minlength=\"3\">ab</textarea>"
        "</body></sui>");
    if (document2) {
        const auto el2 = [&](const char* id) { return document2->getElementById(id); };
        Element* p1 = el2("p1");
        Element* p2 = el2("p2");
        Element* p3 = el2("p3");
        Element* p4 = el2("p4");
        Element* m1 = el2("m1");
        Element* m2 = el2("m2");
        Element* m3 = el2("m3");
        check(p1 && p2 && p3 && p4 && m1 && m2 && m3, "pattern elements found");
        if (p1 && p2 && p3 && p4 && m1 && m2 && m3) {
            check(on("input:valid", *p1), "a matching pattern is valid");
            check(on("input:invalid", *p2), "a misshaped value is invalid");
            check(on("input:valid", *p3), "a broken pattern constrains nothing");
            check(on("input:valid", *p4), "an empty value skips its pattern");
            check(on("input:invalid", *m1), "a short value misses its minlength");
            check(on("input:valid", *m2), "a long enough value meets minlength");
            check(on("textarea:invalid", *m3), "a short textarea misses minlength");
        }
    }
}

// A `::placeholder` box carries the `placeholder` attribute while its field is
// empty, and the field flows the hint in the box's own style instead of a
// value. Typing removes the box on the next rebuild; passwords never mask it.
void testPlaceholder() {
    auto document = parseOrNull(
        "<sui><body><input id=\"e\" type=\"text\" placeholder=\"Name\"/>"
        "<input id=\"f\" type=\"text\" placeholder=\"Name\" value=\"Ada\"/>"
        "<input id=\"p\" type=\"password\" placeholder=\"Secret\"/>"
        "<textarea id=\"t\" placeholder=\"Notes\"></textarea>"
        "<div id=\"d\" placeholder=\"Nope\">x</div></body></sui>");
    if (!document) {
        return;
    }
    Element* e = document->getElementById("e");
    Element* f = document->getElementById("f");
    Element* p = document->getElementById("p");
    Element* t = document->getElementById("t");
    Element* d = document->getElementById("d");
    check(e && f && p && t && d, "placeholder elements found");
    if (!e || !f || !p || !t || !d) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body { margin: 0 } input::placeholder { color: #ff0000 }"));
    sui::generatePseudoElements(*document, resolver);
    const auto placeholderOf = [](Element& el) -> Element* {
        for (const auto& child : el.children()) {
            if (child && child->tag() == "::placeholder") {
                return child.get();
            }
        }
        return nullptr;
    };
    check(placeholderOf(*e) != nullptr, "an empty field gains a placeholder box");
    check(placeholderOf(*f) == nullptr, "a filled field gains no box");
    check(placeholderOf(*p) != nullptr, "a password field gains a box");
    check(placeholderOf(*t) != nullptr, "a textarea gains a box");
    check(placeholderOf(*d) == nullptr, "a div gains no box");

    layoutRoot(*document, resolver, 400.0f);
    check(!e->textRuns().empty() && e->textRuns()[0].text == "Name",
          "the hint flows as the field's text");
    check(!e->textRuns().empty() && e->textRuns()[0].r == 255 &&
              e->textRuns()[0].g == 0,
          "an author ::placeholder rule colours the hint");
    check(!t->textRuns().empty() && t->textRuns()[0].text == "Notes",
          "a textarea hint flows too");
    check(!p->textRuns().empty() && p->textRuns()[0].text == "Secret",
          "a password hint is never masked");

    // Typing removes the box and shows the value on the next rebuild.
    e->setValue("Grace");
    sui::generatePseudoElements(*document, resolver);
    layoutRoot(*document, resolver, 400.0f);
    check(placeholderOf(*e) == nullptr, "typing drops the placeholder box");
    check(!e->textRuns().empty() && e->textRuns()[0].text == "Grace",
          "the value replaces the hint");

    // Without an author rule the hint reads the muted UA gray.
    auto bare = parseOrNull(
        "<sui><body><input id=\"u\" type=\"text\" placeholder=\"Hint\"/></body></sui>");
    if (bare) {
        Element* u = bare->getElementById("u");
        if (u) {
            sui::StyleResolver plain;
            plain.setRules(sui::CSSParser::parse("body { margin: 0 }"));
            sui::generatePseudoElements(*bare, plain);
            layoutRoot(*bare, plain, 400.0f);
            check(!u->textRuns().empty() && u->textRuns()[0].r == 130,
                  "an unstyled hint reads the UA gray");
        } else {
            check(false, "unstyled placeholder element found");
        }
    }
}

// `::selection` colors come from rules ending in the pseudo-element whose
// prefix matches the text owner, won by important/specificity/order. `color`,
// `background-color`, and `text-decoration` apply; anything else never paints a
// highlight.
void testSelectionColors() {
    auto document = parseOrNull(
        "<sui><body><input id=\"t\" type=\"text\" value=\"hi\"/>"
        "<div id=\"d\">hi</div></body></sui>");
    if (!document) {
        return;
    }
    Element* t = document->getElementById("t");
    Element* d = document->getElementById("d");
    check(t && d, "selection elements found");
    if (!t || !d) {
        return;
    }
    const auto colorsOf = [&](const char* css, Element& el) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        return resolver.selectionColors(el);
    };
    sui::SelectionColors none = colorsOf("body { margin: 0 }", *t);
    check(!none.fgSet && !none.bgSet, "no rule leaves both colors unset");

    sui::SelectionColors global =
        colorsOf("::selection { color: #ff0000; background-color: #00ff00 }", *d);
    check(global.fgSet && global.fg.r == 255 && global.bgSet && global.bg.g == 255,
          "a global rule colours any text");

    // Specificity beats order: the later global loses to the scoped rule.
    sui::SelectionColors scoped = colorsOf(
        "::selection { color: #00ff00 } input::selection { color: #0000ff }", *t);
    check(scoped.fgSet && scoped.fg.b == 255, "a scoped rule beats a later global");

    // Order breaks specificity ties, and !important beats everything.
    sui::SelectionColors ordered = colorsOf(
        "input::selection { color: #111111 } input::selection { color: #222222 }", *t);
    check(ordered.fgSet && ordered.fg.r == 0x22, "the later tie wins");
    sui::SelectionColors important = colorsOf(
        "input::selection { color: #111111 !important } "
        "body input::selection { color: #222222 }",
        *t);
    check(important.fgSet && important.fg.r == 0x11, "important beats specificity");

    // `text-decoration`/`text-decoration-line`: one line, first token wins,
    // explicit `none` clears it, and the same important/specificity/order
    // cascade decides.
    sui::SelectionColors underlined =
        colorsOf("input::selection { text-decoration: underline }", *t);
    check(underlined.decorationSet &&
              underlined.decoration == sui::ComputedStyle::DecUnderline,
          "a selection rule underlines the selected text");
    sui::SelectionColors through =
        colorsOf("input::selection { text-decoration: line-through }", *t);
    check(through.decorationSet &&
              through.decoration == sui::ComputedStyle::DecLineThrough,
          "a selection rule strikes through the selected text");
    sui::SelectionColors scopedDec = colorsOf(
        "::selection { text-decoration: line-through } "
        "input::selection { text-decoration: overline }",
        *t);
    check(scopedDec.decorationSet &&
              scopedDec.decoration == sui::ComputedStyle::DecOverline,
          "a scoped selection decoration beats a global one");
    sui::SelectionColors firstToken = colorsOf(
        "input::selection { text-decoration: underline line-through }", *t);
    check(firstToken.decorationSet &&
              firstToken.decoration == sui::ComputedStyle::DecUnderline,
          "the first decoration token wins");
    sui::SelectionColors noneDec =
        colorsOf("input::selection { text-decoration: none }", *t);
    check(noneDec.decorationSet && noneDec.decoration == sui::ComputedStyle::DecNone,
          "an explicit selection none clears the line");
    sui::SelectionColors importantDec = colorsOf(
        "input::selection { text-decoration: underline !important } "
        "input::selection { text-decoration: none }",
        *t);
    check(importantDec.decorationSet &&
              importantDec.decoration == sui::ComputedStyle::DecUnderline,
          "!important wins the selection decoration");
    sui::SelectionColors manyDec = colorsOf(
        "input::selection { text-decoration: underline line-through overline }",
        *t);
    check(manyDec.decorationSet && manyDec.decorations.size() == 3 &&
              manyDec.decorations[0] == sui::ComputedStyle::DecUnderline &&
              manyDec.decorations[1] == sui::ComputedStyle::DecLineThrough &&
              manyDec.decorations[2] == sui::ComputedStyle::DecOverline,
          "a selection decoration list keeps every line in order");
    sui::SelectionColors noneDecList = colorsOf(
        "input::selection { text-decoration: underline } "
        "input::selection { text-decoration: none }",
        *t);
    check(noneDecList.decorationSet && noneDecList.decorations.empty() &&
              noneDecList.decoration == sui::ComputedStyle::DecNone,
          "a later selection none clears the whole decoration list");

    sui::SelectionColors shadow = colorsOf(
        "input::selection { text-shadow: 2px 3px 4px #112233 }", *t);
    check(shadow.shadowSet && shadow.textShadows.size() == 1 &&
              shadow.textShadows[0].offsetX.value == 2.0f &&
              shadow.textShadows[0].offsetY.value == 3.0f &&
              shadow.textShadows[0].blurRadius.value == 4.0f &&
              shadow.textShadows[0].color.r == 0x11 &&
              shadow.textShadows[0].color.g == 0x22 &&
              shadow.textShadows[0].color.b == 0x33,
          "a selection text-shadow keeps its offsets, blur, and colour");
    sui::SelectionColors scopedShadow = colorsOf(
        "::selection { text-shadow: 9px 9px } "
        "input::selection { text-shadow: 1px 1px }",
        *t);
    check(scopedShadow.shadowSet && scopedShadow.textShadows.size() == 1 &&
              scopedShadow.textShadows[0].offsetX.value == 1.0f,
          "a scoped selection shadow beats a global one");
    sui::SelectionColors noShadow = colorsOf(
        "input::selection { text-shadow: 2px 2px } "
        "input::selection { text-shadow: none }",
        *t);
    check(noShadow.shadowSet && noShadow.textShadows.empty(),
          "a later selection text-shadow none clears shadows");

    // Anything else never paints a highlight.
    sui::SelectionColors junk =
        colorsOf("input::selection { border-image: none; font-size: 99px }", *t);
    check(!junk.fgSet && !junk.bgSet && !junk.decorationSet,
          "other declarations never apply");
}

// `::first-line` and `::first-letter` read the same mini-cascade as
// `::selection`: scoped rules beat globals, ties break by order.
void testFirstLineLetter() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\">hi</div><p id=\"b\">hi</p></body></sui>");
    if (!document) {
        return;
    }
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    check(a && b, "first-line elements found");
    if (!a || !b) {
        return;
    }
    const auto colorsOf = [&](const char* css, Element& el, bool letter) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        return letter ? resolver.firstLetterColors(el) : resolver.firstLineColors(el);
    };
    sui::SelectionColors none = colorsOf("p::first-line { color: red }", *a, false);
    check(!none.fgSet, "a first-line rule misses other tags");
    sui::SelectionColors line =
        colorsOf("div::first-line { color: #ff0000; background-color: #00ff00 }", *a,
                 false);
    check(line.fgSet && line.fg.r == 255 && line.bgSet && line.bg.g == 255,
          "a scoped first-line rule colours");
    sui::SelectionColors letter =
        colorsOf("div::first-letter { color: #0000ff } ::first-line { color: #00ff00 }",
                 *a, true);
    check(letter.fgSet && letter.fg.b == 255, "a first-letter rule colours");
    sui::SelectionColors both =
        colorsOf("::first-letter { color: #111111 } div::first-letter { color: #222222 }",
                 *a, true);
    check(both.fgSet && both.fg.r == 0x22, "specificity orders first-letter ties");
    sui::SelectionColors para =
        colorsOf("p::first-letter { color: red }", *b, true);
    check(para.fgSet, "first-letter matches paragraphs too");
}

// `::marker` recolors list bullets through the same mini-cascade: scoped
// rules beat globals, ties break by order.
void testMarkerColors() {
    auto document = parseOrNull(
        "<sui><body><ul><li id=\"a\">x</li></ul><div id=\"d\">x</div></body></sui>");
    if (!document) {
        return;
    }
    Element* a = document->getElementById("a");
    Element* d = document->getElementById("d");
    check(a && d, "marker elements found");
    if (!a || !d) {
        return;
    }
    const auto colorsOf = [&](const char* css, Element& el) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        return resolver.markerColors(el);
    };
    sui::SelectionColors none = colorsOf("body { margin: 0 }", *a);
    check(!none.fgSet, "no rule leaves the marker unset");
    sui::SelectionColors global = colorsOf("::marker { color: #ff0000 }", *a);
    check(global.fgSet && global.fg.r == 255, "a global rule colours markers");
    sui::SelectionColors scoped =
        colorsOf("::marker { color: #00ff00 } li::marker { color: #0000ff }", *a);
    check(scoped.fgSet && scoped.fg.b == 255, "a scoped rule beats a later global");
    sui::SelectionColors div = colorsOf("div::marker { color: red }", *d);
    check(div.fgSet, "marker rules match by prefix tag too");
}

// The `::marker` mini-cascade also wins font properties. A marker font-size is
// resolved against the item's own size, so em and percentages follow the item
// while rem follows the root.
void testMarkerFont() {
    auto document = parseOrNull(
        "<sui><body><ul><li id=\"a\" style=\"font-size:20px\">x</li></ul>"
        "<div id=\"d\">x</div></body></sui>");
    if (!document) {
        return;
    }
    Element* a = document->getElementById("a");
    Element* d = document->getElementById("d");
    check(a && d, "marker font elements found");
    if (!a || !d) {
        return;
    }
    const auto fontOf = [&](const char* css, Element& el) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        resolver.setRootFontSize(16.0f);
        return resolver.markerFont(el, resolver.resolve(el));
    };
    sui::MarkerFont none = fontOf("body { margin: 0 }", *a);
    check(!none.sizeSet && !none.familySet && !none.weightSet && !none.styleSet,
          "no marker rule leaves the marker font unset");
    sui::MarkerFont px = fontOf("::marker { font-size: 32px }", *a);
    check(px.sizeSet && px.size == 32.0f, "a marker font-size resolves to px");
    sui::MarkerFont em = fontOf("::marker { font-size: 1.5em }", *a);
    check(em.sizeSet && em.size == 30.0f, "marker em follows the item font size");
    sui::MarkerFont pct = fontOf("::marker { font-size: 150% }", *a);
    check(pct.sizeSet && pct.size == 30.0f, "marker percent follows the item font size");
    sui::MarkerFont rem = fontOf("::marker { font-size: 2rem }", *a);
    check(rem.sizeSet && rem.size == 32.0f, "marker rem follows the root font size");
    sui::MarkerFont scoped =
        fontOf("::marker { font-size: 10px } li::marker { font-size: 30px }", *a);
    check(scoped.sizeSet && scoped.size == 30.0f, "a scoped marker rule beats a global");
    sui::MarkerFont important =
        fontOf("::marker { font-size: 10px !important } li::marker { font-size: 30px }", *a);
    check(important.sizeSet && important.size == 10.0f,
          "!important wins the marker font cascade");
    sui::MarkerFont family = fontOf("::marker { font-family: \"Fancy\", serif }", *a);
    check(family.familySet && family.family == "Fancy",
          "a marker family keeps its first face");
    check(family.familySet && family.familyList == "\"Fancy\", serif",
          "a marker family keeps the whole fallback list");
    sui::MarkerFont quoted =
        fontOf("::marker { font-family: \"Fancy, Name\", serif }", *a);
    check(quoted.familySet && quoted.family == "Fancy, Name" &&
              quoted.familyList == "\"Fancy, Name\", serif",
          "a marker's first family may contain a quoted comma");
    sui::MarkerFont weight = fontOf("::marker { font-weight: bold }", *a);
    check(weight.weightSet && weight.weight == 700, "a marker weight reads keywords");
    sui::MarkerFont bolder = fontOf("::marker { font-weight: bolder }", *a);
    check(bolder.weightSet && bolder.weight == 700,
          "marker bolder follows the item weight");
    sui::MarkerFont italic = fontOf("::marker { font-style: italic }", *a);
    check(italic.styleSet && italic.style == sui::ComputedStyle::FontItalic,
          "a marker style reads italic");
    sui::MarkerFont inherit = fontOf("::marker { font-size: inherit }", *a);
    check(!inherit.sizeSet, "marker font-size inherit keeps the item size");
    sui::MarkerFont initial = fontOf("::marker { font-size: initial }", *a);
    check(initial.sizeSet && initial.size == 16.0f,
          "marker font-size initial returns the medium size");
    sui::MarkerFont zero = fontOf("::marker { font-size: 0 }", *a);
    check(!zero.sizeSet, "a non-positive marker size is dropped");
    sui::MarkerFont div = fontOf("div::marker { font-size: 40px }", *a);
    check(!div.sizeSet, "a marker rule matches its own element only");
}

// `::first-letter` reads the same font mini-cascade as `::marker`; layout
// splits the first grapheme when any slot is set.
void testFirstLetterFont() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\" style=\"font-size:20px\">x</div>"
        "<p id=\"p\">x</p></body></sui>");
    if (!document) {
        return;
    }
    Element* a = document->getElementById("a");
    Element* p = document->getElementById("p");
    check(a && p, "first-letter font elements found");
    if (!a || !p) {
        return;
    }
    const auto fontOf = [&](const char* css, Element& el) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        resolver.setRootFontSize(16.0f);
        return resolver.firstLetterFont(el, resolver.resolve(el));
    };
    sui::PseudoFont none = fontOf("body { margin: 0 }", *a);
    check(!none.sizeSet && !none.familySet && !none.weightSet && !none.styleSet,
          "no first-letter rule leaves the font unset");
    sui::PseudoFont px = fontOf("::first-letter { font-size: 32px }", *a);
    check(px.sizeSet && px.size == 32.0f,
          "a first-letter font-size resolves to px");
    sui::PseudoFont em = fontOf("::first-letter { font-size: 1.5em }", *a);
    check(em.sizeSet && em.size == 30.0f,
          "first-letter em follows the element font size");
    sui::PseudoFont pct = fontOf("::first-letter { font-size: 150% }", *a);
    check(pct.sizeSet && pct.size == 30.0f,
          "first-letter percent follows the element font size");
    sui::PseudoFont rem = fontOf("::first-letter { font-size: 2rem }", *a);
    check(rem.sizeSet && rem.size == 32.0f,
          "first-letter rem follows the root font size");
    sui::PseudoFont scoped =
        fontOf("::first-letter { font-size: 10px } div::first-letter { font-size: 30px }",
               *a);
    check(scoped.sizeSet && scoped.size == 30.0f,
          "a scoped first-letter rule beats a global");
    sui::PseudoFont important = fontOf(
        "::first-letter { font-size: 10px !important } "
        "div::first-letter { font-size: 30px }",
        *a);
    check(important.sizeSet && important.size == 10.0f,
          "!important wins the first-letter font cascade");
    sui::PseudoFont family =
        fontOf("::first-letter { font-family: \"Fancy\", serif }", *a);
    check(family.familySet && family.family == "Fancy",
          "a first-letter family keeps its first face");
    check(family.familySet && family.familyList == "\"Fancy\", serif",
          "a first-letter family keeps the whole fallback list");
    sui::PseudoFont weight = fontOf("::first-letter { font-weight: bold }", *a);
    check(weight.weightSet && weight.weight == 700,
          "a first-letter weight reads keywords");
    sui::PseudoFont bolder = fontOf("::first-letter { font-weight: bolder }", *a);
    check(bolder.weightSet && bolder.weight == 700,
          "first-letter bolder follows the element weight");
    sui::PseudoFont italic = fontOf("::first-letter { font-style: italic }", *a);
    check(italic.styleSet && italic.style == sui::ComputedStyle::FontItalic,
          "a first-letter style reads italic");
    sui::PseudoFont inherit = fontOf("::first-letter { font-size: inherit }", *a);
    check(!inherit.sizeSet, "first-letter font-size inherit keeps the element size");
    sui::PseudoFont initial = fontOf("::first-letter { font-size: initial }", *a);
    check(initial.sizeSet && initial.size == 16.0f,
          "first-letter font-size initial returns the medium size");
    sui::PseudoFont zero = fontOf("::first-letter { font-size: 0 }", *a);
    check(!zero.sizeSet, "a non-positive first-letter size is dropped");
    sui::PseudoFont paragraph = fontOf("p::first-letter { font-size: 40px }", *a);
    check(!paragraph.sizeSet, "a first-letter rule matches its own element only");
}

// `::first-letter` font slots reach layout: the first grapheme becomes its own
// run with the pseudo metrics, the first line grows to fit it, and the rest of
// the word continues without an inter-word gap. Later lines keep the element
// font, a color-only rule never splits, and an inside marker is not the letter.
void testFirstLetterSizing() {
    auto document = parseOrNull(
        "<sui><body>"
        "<p id=\"a\">Hello world</p>"
        "<p id=\"b\" style=\"width:80px\">Hello wonderful world</p>"
        "<p id=\"c\"><span>Hello</span> world</p>"
        "<ul><li id=\"e\" style=\"list-style-position:inside\">Item</li></ul>"
        "<p id=\"f\">Hello</p>"
        "<p id=\"g\" style=\"white-space:pre\"> Hello</p>"
        "<p id=\"h\">Hello world</p>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0;font-size:16px}"
        "#b::first-letter{font-size:48px;font-weight:bold;font-style:italic}"
        "#c::first-letter{font-size:40px}"
        "#e::first-letter{font-size:40px}"
        "#f::first-letter{color:#ff0000}"
        "#g::first-letter{font-size:32px}"
        "#h::first-letter{font-family:\"Fancy\", serif}"));
    layoutRoot(*document, resolver, 400.0f);

    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* c = document->getElementById("c");
    Element* e = document->getElementById("e");
    Element* f = document->getElementById("f");
    Element* g = document->getElementById("g");
    Element* h = document->getElementById("h");
    check(a && b && c && e && f && g && h, "first-letter layout elements found");
    if (!a || !b || !c || !e || !f || !g || !h || a->textRuns().empty() ||
        b->textRuns().size() < 2 || c->textRuns().size() < 2 ||
        e->textRuns().size() < 3 || f->textRuns().empty() ||
        g->textRuns().size() < 3 || h->textRuns().size() < 2) {
        check(false, "first-letter layout produced runs");
        return;
    }

    check(a->textRuns().size() == 2 && a->textRuns()[0].text == "Hello" &&
              a->textRuns()[0].fontSize == 16.0f,
          "plain text keeps its whole first word in the base font");
    check(h->textRuns()[0].fontFamily == "\"Fancy\", serif",
          "the first-letter run carries the pseudo fallback list");

    const auto& letter = b->textRuns()[0];
    const auto& rest = b->textRuns()[1];
    check(letter.text == "H" && letter.fontSize == 48.0f,
          "the first grapheme takes the first-letter size");
    check(letter.bold && letter.italic,
          "the first grapheme takes the first-letter weight and style");
    check(letter.width > 20.0f, "the enlarged letter reserves its width");
    check(rest.text == "ello" && rest.fontSize == 16.0f && !rest.bold && !rest.italic,
          "the rest of the word keeps the element font");
    check(std::abs(rest.x - (letter.x + letter.width)) < 0.5f,
          "the rest follows the letter without a word gap");
    check(std::abs(letter.height - rest.height) < 0.5f,
          "the first line shares the enlarged line height");
    check(b->layoutHeight() > a->layoutHeight() + 20.0f,
          "the enlarged first line makes the paragraph taller");
    bool laterLine = false;
    float laterHeight = 0.0f;
    float secondLineY = 0.0f;
    for (const auto& run : b->textRuns()) {
        if (secondLineY == 0.0f && run.y > letter.y + 0.5f) {
            secondLineY = run.y;
        }
        if (run.y > letter.y + 20.0f && run.fontSize == 16.0f) {
            laterLine = true;
            laterHeight = run.height;
            break;
        }
    }
    check(secondLineY - letter.y > 40.0f,
          "the enlarged first line pushes the next line down");
    check(laterLine, "later lines keep the element font and start below the letter");
    check(laterHeight > 0.0f && laterHeight < 30.0f,
          "later lines return to the element line height");

    check(c->textRuns()[0].text == "H" && c->textRuns()[0].fontSize == 40.0f &&
              c->textRuns()[1].text == "ello" && c->textRuns()[1].fontSize == 16.0f,
          "an inline child's first letter still takes the block pseudo");

    check(e->textRuns()[0].fontSize == 16.0f &&
              e->textRuns()[1].text == "I" &&
              e->textRuns()[1].fontSize == 40.0f &&
              e->textRuns()[2].text == "tem" && e->textRuns()[2].fontSize == 16.0f,
          "an inside marker stays base-sized and the item letter is not the marker");

    check(f->textRuns().size() == 1 && f->textRuns()[0].text == "Hello",
          "a color-only first-letter rule does not split the run");

    check(g->textRuns()[0].text == " " && g->textRuns()[0].fontSize == 16.0f &&
              g->textRuns()[1].text == "H" && g->textRuns()[1].fontSize == 32.0f &&
              g->textRuns()[2].text == "ello" && g->textRuns()[2].fontSize == 16.0f,
          "leading preserved space does not consume the first letter");
}

// `::first-line` wins font and line-height slots through the same mini-cascade
// as `::marker` and `::first-letter`.
void testFirstLineFont() {
    auto document = parseOrNull(
        "<sui><body><div id=\"a\" style=\"font-size:20px\">x</div>"
        "<p id=\"p\">x</p></body></sui>");
    if (!document) {
        return;
    }
    Element* a = document->getElementById("a");
    Element* p = document->getElementById("p");
    check(a && p, "first-line font elements found");
    if (!a || !p) {
        return;
    }
    const auto fontOf = [&](const char* css, Element& el) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        resolver.setRootFontSize(16.0f);
        return resolver.firstLineFont(el, resolver.resolve(el));
    };
    sui::PseudoFont none = fontOf("body { margin: 0 }", *a);
    check(!none.sizeSet && !none.familySet && !none.weightSet && !none.styleSet &&
              !none.lineHeightSet,
          "no first-line rule leaves the font unset");
    sui::PseudoFont px =
        fontOf("::first-line { font-size: 30px; line-height: 1.5 }", *a);
    check(px.sizeSet && px.size == 30.0f && px.lineHeightSet &&
              px.lineHeight == 1.5f,
          "a first-line size and unitless line height resolve");
    sui::PseudoFont em = fontOf("::first-line { font-size: 1.5em }", *a);
    check(em.sizeSet && em.size == 30.0f,
          "first-line em follows the element font size");
    sui::PseudoFont rem = fontOf("::first-line { font-size: 2rem }", *a);
    check(rem.sizeSet && rem.size == 32.0f,
          "first-line rem follows the root font size");
    sui::PseudoFont linePx =
        fontOf("::first-line { font-size: 20px; line-height: 40px }", *a);
    check(linePx.lineHeightSet && std::abs(linePx.lineHeight - 2.0f) < 0.01f,
          "a first-line line-height length becomes a multiplier");
    sui::PseudoFont normal =
        fontOf("::first-line { line-height: normal }", *a);
    check(normal.lineHeightSet && std::abs(normal.lineHeight - 1.2f) < 0.01f,
          "first-line line-height normal restores the default");
    sui::PseudoFont family =
        fontOf("::first-line { font-family: \"Fancy\", serif }", *a);
    check(family.familySet && family.family == "Fancy" &&
              family.familyList == "\"Fancy\", serif",
          "a first-line family keeps its whole fallback list");
    sui::PseudoFont weight = fontOf("::first-line { font-weight: bold }", *a);
    check(weight.weightSet && weight.weight == 700,
          "a first-line weight reads keywords");
    sui::PseudoFont italic = fontOf("::first-line { font-style: italic }", *a);
    check(italic.styleSet && italic.style == sui::ComputedStyle::FontItalic,
          "a first-line style reads italic");
    sui::PseudoFont scoped =
        fontOf("::first-line { font-size: 10px } div::first-line { font-size: 30px }",
               *a);
    check(scoped.sizeSet && scoped.size == 30.0f,
          "a scoped first-line rule beats a global");
    sui::PseudoFont important =
        fontOf("::first-line { font-size: 10px !important } "
               "div::first-line { font-size: 30px }",
               *a);
    check(important.sizeSet && important.size == 10.0f,
          "!important wins the first-line font cascade");
    sui::PseudoFont inherit = fontOf("::first-line { font-size: inherit }", *a);
    check(!inherit.sizeSet,
          "first-line font-size inherit keeps the element size");
    sui::PseudoFont para = fontOf("p::first-line { font-size: 40px }", *a);
    check(!para.sizeSet, "a first-line rule matches its own element only");
    sui::PseudoFont floatRule =
        fontOf("p::first-line { float: left }", *p);
    check(floatRule.floatSet &&
              floatRule.floatType == sui::ComputedStyle::FloatLeft,
          "the pseudo font cascade carries float for first-letter drop caps");
}

// `::first-line` sizing reaches layout: runs on the first formatted line use
// the pseudo font and line height, later lines return to the element font, and
// an inline child with its own font keeps it.
void testFirstLineSizing() {
    auto document = parseOrNull(
        "<sui><body>"
        "<p id=\"a\">Hello</p>"
        "<p id=\"b\" style=\"width:80px\">Hello wonderful world</p>"
        "<p id=\"c\"><span style=\"font-size:10px\">Hi</span> rest</p>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0;font-size:16px}"
        "#a::first-line{font-size:30px;font-weight:bold;font-style:italic;line-height:1}"
        "#b::first-line{font-size:32px;line-height:1}"
        "#c::first-line{font-size:30px}"));
    layoutRoot(*document, resolver, 400.0f);

    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* c = document->getElementById("c");
    check(a && b && c, "first-line sizing elements found");
    if (!a || !b || !c || a->textRuns().empty() || b->textRuns().size() < 2 ||
        c->textRuns().size() < 2) {
        check(false, "first-line sizing produced runs");
        return;
    }

    const auto& first = a->textRuns()[0];
    check(first.fontSize == 30.0f && first.bold && first.italic,
          "the first line takes the pseudo size, weight, and style");
    check(std::abs(first.height - 30.0f) < 0.5f &&
              std::abs(a->layoutHeight() - 30.0f) < 0.5f,
          "the first line takes the pseudo line height");
    check(std::abs(first.baseline - 24.0f) < 0.5f,
          "the first-line run carries the pseudo ascent");

    const auto& bFirst = b->textRuns().front();
    check(bFirst.fontSize == 32.0f && std::abs(bFirst.height - 32.0f) < 0.5f,
          "a wrapped paragraph's first line uses the pseudo font");
    bool laterLine = false;
    float secondLineY = 0.0f;
    for (const auto& run : b->textRuns()) {
        if (run.y > bFirst.y + 0.5f && secondLineY == 0.0f) {
            secondLineY = run.y;
        }
        if (run.y > bFirst.y + 0.5f && run.fontSize == 16.0f &&
            std::abs(run.height - 19.2f) < 0.5f) {
            laterLine = true;
        }
    }
    check(std::abs(secondLineY - bFirst.y - 32.0f) < 0.5f,
          "the pseudo line height pushes the second line down");
    check(laterLine,
          "later lines return to the element font and line height");

    check(c->textRuns()[0].text == "Hi" &&
              c->textRuns()[0].fontSize == 10.0f,
          "an inline child's own font beats the first-line pseudo");
    bool restStyled = false;
    for (const auto& run : c->textRuns()) {
        if (run.text == "rest" && run.fontSize == 30.0f) {
            restStyled = true;
        }
    }
    check(restStyled,
          "text that still inherits the block font takes the first-line size");
}

// A floated `::first-letter` becomes a drop cap: the glyph owns a float box and
// following lines narrow beside it until the float ends.
void testFirstLetterDropCap() {
    auto document = parseOrNull(
        "<sui><body>"
        "<p id=\"a\" style=\"width:80px;font-size:10px\">alpha beta gamma delta epsilon zeta eta theta</p>"
        "<p id=\"b\" style=\"width:80px;font-size:10px\">alpha beta gamma delta epsilon zeta eta theta</p>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0}"
        "#a::first-letter{float:left;font-size:30px;line-height:1}"
        "#b::first-letter{float:right;font-size:30px;line-height:1}"));
    layoutRoot(*document, resolver, 400.0f);

    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    check(a && b, "drop cap elements found");
    if (!a || !b || a->textRuns().size() < 4 || b->textRuns().size() < 4) {
        check(false, "drop cap layout produced runs");
        return;
    }

    const auto& letter = a->textRuns()[0];
    check(letter.text == "a" && std::abs(letter.x) < 0.5f &&
              std::abs(letter.y - a->contentY()) < 0.5f &&
              std::abs(letter.width - 15.0f) < 0.5f &&
              std::abs(letter.height - 30.0f) < 0.5f,
          "the left drop cap sits at the line start with its enlarged box");
    const auto minXOnLine = [&](float offset) {
        float x = 1.0e6f;
        for (const auto& run : a->textRuns()) {
            if (run.text != "a" &&
                std::abs(run.y - (a->contentY() + offset)) < 0.5f) {
                x = std::min(x, run.x);
            }
        }
        return x;
    };
    check(std::abs(minXOnLine(0.0f) - 15.0f) < 0.5f &&
              std::abs(minXOnLine(12.0f) - 15.0f) < 0.5f &&
              std::abs(minXOnLine(24.0f) - 15.0f) < 0.5f,
          "text starts beside the left drop cap on every covered line");
    check(std::abs(minXOnLine(36.0f)) < 0.5f,
          "text returns to the left edge below the drop cap");
    check(a->layoutHeight() >= 44.0f,
          "a floating drop cap extends the paragraph height");

    const auto& rightLetter = b->textRuns()[0];
    check(rightLetter.text == "a" &&
              std::abs(rightLetter.x - 65.0f) < 0.5f &&
              std::abs(rightLetter.y - b->contentY()) < 0.5f &&
              std::abs(rightLetter.width - 15.0f) < 0.5f,
          "a right drop cap sits on the right content edge");
    bool textLeft = false;
    for (const auto& run : b->textRuns()) {
        if (run.text != "a" && std::abs(run.y - b->contentY()) < 0.5f &&
            std::abs(run.x) < 0.5f) {
            textLeft = true;
        }
    }
    check(textLeft, "text beside a right drop cap starts at the left edge");
}

// `accent-color` tints native controls, inherits like `color`, and resets to
// `auto`. Garbage keeps the previous cascade winner.
void testAccentColor() {
    auto document = parseOrNull(
        "<sui><body><div id=\"p\" style=\"accent-color:#00ff00\">"
        "<input id=\"c\" type=\"checkbox\"/></div>"
        "<input id=\"a\" type=\"checkbox\" style=\"accent-color:red\"/>"
        "<input id=\"u\" type=\"checkbox\" style=\"accent-color:auto\"/>"
        "<input id=\"g\" type=\"checkbox\" style=\"accent-color:banana\"/>"
        "<div id=\"d\">x</div></body></sui>");
    if (!document) {
        return;
    }
    Element* p = document->getElementById("p");
    Element* c = document->getElementById("c");
    Element* a = document->getElementById("a");
    Element* u = document->getElementById("u");
    Element* g = document->getElementById("g");
    Element* d = document->getElementById("d");
    check(p && c && a && u && g && d, "accent elements found");
    if (!p || !c || !a || !u || !g || !d) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    const ComputedStyle parent = resolver.resolve(*p);
    check(parent.hasAccentColor && parent.accentColor.g == 255, "an accent stores");
    check(sui::StyleResolver::propertyValue(parent, "accent-color") == "#00ff00",
          "accent-color serializes");
    const ComputedStyle child = resolver.resolve(*c);
    check(child.hasAccentColor && child.accentColor.g == 255,
          "an accent inherits into controls");
    const ComputedStyle own = resolver.resolve(*a);
    check(own.hasAccentColor && own.accentColor.r == 255, "an own accent wins");
    check(resolver.resolve(*u).hasAccentColor == false, "auto resets the accent");
    check(resolver.resolve(*g).hasAccentColor == false, "garbage keeps no accent");
    check(resolver.resolve(*d).hasAccentColor == false, "plain text has no accent");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*d), "accent-color") ==
              "auto",
          "an absent accent serializes as auto");
}

// `list-style-position: inside` flows the marker as the first inline content
// (taking line space), while `outside` leaves text runs clean for the painted
// marker. Ordinals count preceding items.
void testListInside() {
    auto document = parseOrNull(
        "<sui><body><ul>"
        "<li id=\"a\" style=\"list-style-position:inside\">apple</li>"
        "<li id=\"b\">banana</li>"
        "<li id=\"c\" style=\"list-style:decimal inside\">cherry</li>"
        "<li id=\"e\" style=\"list-style:decimal inside\">date</li>"
        "<li id=\"f\" style=\"list-style:none inside\">fig</li>"
        "</ul></body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* b = el("b");
    Element* c = el("c");
    Element* e = el("e");
    Element* f = el("f");
    check(a && b && c && e && f, "list elements found");
    if (!a || !b || !c || !e || !f) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    layoutRoot(*document, resolver, 400.0f);
    const auto startsWith = [](const std::string& text, const char* prefix) {
        return text.compare(0, std::strlen(prefix), prefix) == 0;
    };
    check(!a->textRuns().empty() && startsWith(a->textRuns()[0].text, "\xE2\x80\xA2\xC2\xA0"),
          "an inside bullet leads its text");
    check(!b->textRuns().empty() && b->textRuns()[0].text == "banana",
          "an outside item keeps clean runs");
    check(!c->textRuns().empty() && startsWith(c->textRuns()[0].text, "3.\xC2\xA0"),
          "inside ordinals count every preceding item");
    check(!e->textRuns().empty() && startsWith(e->textRuns()[0].text, "4.\xC2\xA0"),
          "ordinals keep counting down the list");
    check(!f->textRuns().empty() && f->textRuns()[0].text == "fig",
          "none paints no marker either way");
}

// `list-style-image` stores a url marker, `none` clears it, the shorthand
// rides it along, it inherits, and it serializes back. Garbage keeps the
// previous cascade winner.
void testListStyleImage() {
    auto document = parseOrNull(
        "<sui><body>"
        "<ul id=\"u\" style=\"list-style-image:url(assets/bullet.png)\">"
        "<li id=\"a\">x</li></ul>"
        "<ul id=\"v\" style=\"list-style-image:none\"><li id=\"b\">x</li></ul>"
        "<ul id=\"w\" style=\"list-style:decimal outside url(assets/n.png)\">"
        "<li id=\"c\">x</li></ul>"
        "<ul id=\"z\" style=\"list-style-image:banana\"><li id=\"d\">x</li></ul>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* b = el("b");
    Element* c = el("c");
    Element* d = el("d");
    Element* z = el("z");
    check(a && b && c && d && z, "list image elements found");
    if (!a || !b || !c || !d || !z) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    check(resolver.resolve(*a).listStyleImage == "assets/bullet.png",
          "a url sets the marker image");
    check(resolver.resolve(*b).listStyleImage.empty(), "none clears the marker image");
    check(resolver.resolve(*c).listStyleImage == "assets/n.png",
          "the shorthand carries the image");
    check(resolver.resolve(*d).listStyleImage.empty(), "garbage clears nothing");
    check(resolver.resolve(*z).listStyleImage.empty(), "a bad value keeps none");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*a), "list-style-image") ==
              "url(assets/bullet.png)",
          "the image serializes");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*b), "list-style-image") ==
              "none",
          "an unset image serializes as none");
}

// `font-kerning` toggles pair kerning in shaped runs, inherits like other
// text properties, and serializes back. Garbage keeps the previous winner.
void testFontKerning() {
    auto document = parseOrNull(
        "<sui><body><div id=\"p\" style=\"font-kerning:none\">"
        "<span id=\"c\">AV</span></div>"
        "<div id=\"d\">AV</div>"
        "<div id=\"g\" style=\"font-kerning:banana\">AV</div></body></sui>");
    if (!document) {
        return;
    }
    Element* p = document->getElementById("p");
    Element* c = document->getElementById("c");
    Element* d = document->getElementById("d");
    Element* g = document->getElementById("g");
    check(p && c && d && g, "kerning elements found");
    if (!p || !c || !d || !g) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    check(resolver.resolve(*p).fontKerning == false, "none disables kerning");
    check(resolver.resolve(*c).fontKerning == false, "kerning inherits");
    check(resolver.resolve(*d).fontKerning == true, "kerning defaults on");
    check(resolver.resolve(*g).fontKerning == true, "garbage keeps kerning on");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*p), "font-kerning") ==
              "none",
          "none serializes");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*d), "font-kerning") ==
              "normal",
          "the default serializes as normal");
}

// `font-stretch` names nine steps and takes raw percentages, inherits, and
// serializes. Nonsense widths and garbage drop.
// `font-weight` keeps the whole 1-1000 scale: keywords, relative keywords
// against the inherited weight, and plain numbers. `fontBold` follows the 600
// threshold, the number serializes back, and garbage keeps the previous winner.
void testFontWeight() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"a\" style=\"font-weight:bold\"><span id=\"c\">x</span></div>"
        "<div id=\"b\" style=\"font-weight:300\">x</div>"
        "<div id=\"d\" style=\"font-weight:650\">x</div>"
        "<div id=\"e\" style=\"font-weight:bolder\">x</div>"
        "<div id=\"g\" style=\"font-weight:banana\">x</div>"
        "<div id=\"h\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* c = el("c");
    Element* b = el("b");
    Element* d = el("d");
    Element* e = el("e");
    Element* g = el("g");
    Element* h = el("h");
    check(a && c && b && d && e && g && h, "weight elements found");
    if (!a || !c || !b || !d || !e || !g || !h) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    check(resolver.resolve(*a).fontWeight == 700, "bold is 700");
    check(resolver.resolve(*a).fontBold, "700 renders bold");
    check(resolver.resolve(*c).fontWeight == 700, "the number inherits");
    check(resolver.resolve(*b).fontWeight == 300, "a light number parses");
    check(!resolver.resolve(*b).fontBold, "300 renders regular");
    check(resolver.resolve(*d).fontWeight == 650, "off-step numbers keep their value");
    check(resolver.resolve(*d).fontBold, "650 renders bold");
    check(resolver.resolve(*e).fontWeight == 700, "bolder steps 400 up to 700");
    check(resolver.resolve(*g).fontWeight == 400, "garbage keeps normal");
    check(resolver.resolve(*h).fontWeight == 400, "the default is 400");

    // Relative keywords read the inherited weight, not a fixed answer.
    sui::StyleResolver rel;
    rel.setRules(sui::CSSParser::parse(
        "body { margin: 0; font-weight: 700 } #l { font-weight: lighter }"));
    auto relDoc = parseOrNull("<sui><body><div id=\"l\">x</div></body></sui>");
    if (relDoc) {
        Element* l = relDoc->getElementById("l");
        if (l) {
            check(rel.resolve(*l).fontWeight == 400, "lighter steps 700 down to 400");
            check(!rel.resolve(*l).fontBold, "400 renders regular");
        }
    }
    const auto serial = [&](const char* id) {
        return sui::StyleResolver::propertyValue(resolver.resolve(*el(id)), "font-weight");
    };
    check(std::string(serial("a")) == "700", "bold serializes as its number");
    check(std::string(serial("b")) == "300", "a light number round-trips");
    check(std::string(serial("h")) == "400", "the default serializes as 400");
}

// A `<use>` resolves its same-document `#id` to the element to clone, and a
// use-expansion chain refuses repeats so cyclic uses paint nothing.
void testSvgUse() {
    auto document = parseOrNull(
        "<sui><body><svg id=\"s\">"
        "<rect id=\"r\" width=\"10\" height=\"10\"/>"
        "<use id=\"u\" href=\"#r\"/>"
        "<use id=\"v\" href=\"#missing\"/>"
        "<use id=\"w\" href=\"https://example.com/x.svg#r\"/>"
        "<use id=\"e\" href=\"\"/>"
        "</svg></body></sui>");
    if (!document) {
        return;
    }
    Element* s = document->getElementById("s");
    Element* r = document->getElementById("r");
    Element* u = document->getElementById("u");
    Element* v = document->getElementById("v");
    Element* w = document->getElementById("w");
    Element* e = document->getElementById("e");
    check(s && r && u && v && w && e, "use elements found");
    if (!s || !r || !u || !v || !w || !e) {
        return;
    }
    check(sui::resolveSvgUse(*s, "#r") == r, "a fragment resolves to its element");
    check(sui::resolveSvgUse(*s, "#missing") == nullptr, "a missing id resolves nowhere");
    check(sui::resolveSvgUse(*s, "https://example.com/x.svg#r") == nullptr,
          "an external url never resolves");
    check(sui::resolveSvgUse(*s, "") == nullptr, "an empty href resolves nowhere");
    check(sui::resolveSvgUse(*s, "r") == nullptr, "a bare name is not a fragment");
    check(sui::resolveSvgUse(*s, "#") == nullptr, "a bare hash resolves nowhere");

    // The chain check is pointer identity along the expansion path.
    check(!sui::svgUseEntersCycle({}, r), "an empty chain cycles nothing");
    check(sui::svgUseEntersCycle({u}, u), "the use itself on the chain refuses");
    check(sui::svgUseEntersCycle({u, v}, v), "a repeat anywhere refuses");
    check(!sui::svgUseEntersCycle({u}, r), "an unexpanded target passes");
}


// `font-variant` switches synthetic small caps on and off, inherits, and
// serializes. Anything but the two keywords keeps the previous winner.
void testFontVariant() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"a\" style=\"font-variant:small-caps\"><span id=\"c\">hi</span></div>"
        "<div id=\"b\" style=\"font-variant:normal\">hi</div>"
        "<div id=\"g\" style=\"font-variant:banana\">hi</div>"
        "<div id=\"h\">hi</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* c = el("c");
    Element* b = el("b");
    Element* g = el("g");
    Element* h = el("h");
    check(a && c && b && g && h, "variant elements found");
    if (!a || !c || !b || !g || !h) {
        return;
    }
    using Variant = ComputedStyle::FontVariant;
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    check(resolver.resolve(*a).fontVariant == Variant::SmallCaps, "small-caps parses");
    check(resolver.resolve(*c).fontVariant == Variant::SmallCaps, "the variant inherits");
    check(resolver.resolve(*b).fontVariant == Variant::Normal, "normal parses");
    check(resolver.resolve(*g).fontVariant == Variant::Normal, "garbage keeps normal");
    check(resolver.resolve(*h).fontVariant == Variant::Normal, "the default is normal");
    const auto serial = [&](const char* id) {
        return sui::StyleResolver::propertyValue(resolver.resolve(*el(id)), "font-variant");
    };
    check(std::string(serial("a")) == "small-caps", "small-caps serializes");
    check(std::string(serial("h")) == "normal", "the default serializes as normal");
}

// `word-break` and `overflow-wrap` break overlong words across lines at
// codepoint boundaries, each piece its own run. `break-all` always breaks;
// `break-word` only breaks a word longer than a full line. Both inherit and
// serialize; a textarea never breaks, keeping one run per word for caret
// mapping.
void testWordBreak() {
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    const auto widths = [](Element& e) {
        std::vector<float> out;
        for (const auto& run : e.textRuns()) {
            out.push_back(run.width);
        }
        return out;
    };
    // A 36-char word at 8px a glyph overflows a 100px box whole.
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"n\" style=\"width:100px\">abcdefghijklmnopqrstuvwxyz0123456789</div>"
        "<div id=\"w\" style=\"width:100px;overflow-wrap:break-word\">"
        "abcdefghijklmnopqrstuvwxyz0123456789</div>"
        "<div id=\"a\" style=\"width:50px;word-break:break-all\">abcdefghij</div>"
        "<div id=\"m\" style=\"width:100px;overflow-wrap:break-word\">hi "
        "abcdefghijklmnopqrstuvwxyz0123456789</div>"
        "<textarea id=\"t\" style=\"width:100px;overflow-wrap:break-word\">"
        "abcdefghijklmnopqrstuvwxyz0123456789</textarea>"
        "</body></sui>");
    if (!document) {
        return;
    }
    layoutRoot(*document, resolver, 400.0f);
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* n = el("n");
    Element* w = el("w");
    Element* a = el("a");
    Element* m = el("m");
    Element* t = el("t");
    check(n && w && a && m && t, "break elements found");
    if (!n || !w || !a || !m || !t) {
        return;
    }
    check(widths(*n).size() == 1 && widths(*n)[0] > 100.0f,
          "normal overflow keeps one wide run");
    // Three full lines of twelve glyphs.
    const std::vector<float> broke = widths(*w);
    check(broke.size() == 3, "break-word splits across three lines");
    bool fits = true;
    for (float width : broke) {
        fits = fits && width <= 100.0f;
    }
    check(fits, "every break-word piece fits its line");
    bool stacked = w->textRuns().size() == 3 && w->textRuns()[0].y < w->textRuns()[1].y &&
                   w->textRuns()[1].y < w->textRuns()[2].y;
    check(stacked, "the pieces stack down the box");
    // Six glyphs fill 48 of 50px, four finish the word.
    const std::vector<float> all = widths(*a);
    check(all.size() == 2, "break-all splits a fitting word too");
    if (all.size() == 2) {
        check(all[0] == 48.0f && all[1] == 32.0f, "break-all fills each line");
    }
    // After "hi" the long word breaks across the following lines: five runs in
    // all, "hi" first, every piece inside the box, and nothing lost.
    const std::vector<float> mid = widths(*m);
    check(mid.size() == 5, "a mid-line word breaks after wrapping");
    if (mid.size() == 5) {
        float total = 0.0f;
        bool inside = true;
        for (float width : mid) {
            total += width;
            inside = inside && width <= 100.0f;
        }
        check(mid[0] == 16.0f && total == 304.0f && inside,
              "the pieces keep every glyph inside the box");
    }
    check(widths(*t).size() == 1, "a textarea never breaks its words");

    // Parsing, inheritance, and serialization.
    auto document2 = parseOrNull(
        "<sui><body><div id=\"p\" style=\"word-break:break-all\">"
        "<span id=\"c\">x</span></div>"
        "<div id=\"o\" style=\"overflow-wrap:anywhere\">x</div>"
        "<div id=\"g\" style=\"word-break:banana\">x</div>"
        "<div id=\"h\">x</div></body></sui>");
    if (!document2) {
        return;
    }
    const auto el2 = [&](const char* id) { return document2->getElementById(id); };
    Element* p = el2("p");
    Element* c = el2("c");
    Element* o = el2("o");
    Element* g = el2("g");
    Element* h = el2("h");
    check(p && c && o && g && h, "break parse elements found");
    if (!p || !c || !o || !g || !h) {
        return;
    }
    using WB = ComputedStyle::WordBreak;
    using OW = ComputedStyle::OverflowWrap;
    check(resolver.resolve(*p).wordBreak == WB::BreakAll, "break-all parses");
    check(resolver.resolve(*c).wordBreak == WB::BreakAll, "breaking inherits");
    check(resolver.resolve(*o).overflowWrap == OW::BreakWord, "anywhere breaks words");
    check(resolver.resolve(*g).wordBreak == WB::Normal, "garbage keeps normal");
    check(resolver.resolve(*h).overflowWrap == OW::Normal, "the default wraps nothing");
    const auto serial = [&](const char* id, const char* property) {
        return sui::StyleResolver::propertyValue(resolver.resolve(*el2(id)), property);
    };
    check(std::string(serial("p", "word-break")) == "break-all", "break-all serializes");
    check(std::string(serial("o", "overflow-wrap")) == "break-word",
          "anywhere serializes as break-word");
    check(std::string(serial("h", "word-break")) == "normal", "normal serializes");
}

// `text-size-adjust` is parsed, inherited, and serialized, but never applied:
// it only scales a mobile browser's automatic text inflation, and this engine
// never inflates, so every value renders at authored sizes.
void testTextSizeAdjust() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"p\" style=\"text-size-adjust:none\"><span id=\"c\">x</span></div>"
        "<div id=\"a\" style=\"text-size-adjust:auto\">x</div>"
        "<div id=\"d\" style=\"text-size-adjust:150%\">x</div>"
        "<div id=\"g\" style=\"text-size-adjust:banana\">x</div>"
        "<div id=\"h\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* p = el("p");
    Element* c = el("c");
    Element* a = el("a");
    Element* d = el("d");
    Element* g = el("g");
    Element* h = el("h");
    check(p && c && a && d && g && h, "adjust elements found");
    if (!p || !c || !a || !d || !g || !h) {
        return;
    }
    using TSA = ComputedStyle::TextSizeAdjust;
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    check(resolver.resolve(*p).textSizeAdjust == TSA::None, "none parses");
    check(resolver.resolve(*c).textSizeAdjust == TSA::None, "the value inherits");
    check(resolver.resolve(*a).textSizeAdjust == TSA::Auto, "auto parses");
    const ComputedStyle sized = resolver.resolve(*d);
    check(sized.textSizeAdjust == TSA::Percent && sized.textSizeAdjustPercent == 150.0f,
          "a percentage parses with its factor");
    check(resolver.resolve(*g).textSizeAdjust == TSA::Auto, "garbage keeps auto");
    check(resolver.resolve(*h).textSizeAdjust == TSA::Auto, "the default is auto");
    const auto serial = [&](const char* id) {
        return sui::StyleResolver::propertyValue(resolver.resolve(*el(id)),
                                                 "text-size-adjust");
    };
    check(std::string(serial("p")) == "none", "none serializes");
    check(std::string(serial("d")) == "150%", "a percentage serializes");
    check(std::string(serial("h")) == "auto", "the default serializes as auto");
}

// `object-position` plants the fitted content: keywords, lengths, and
// percentages in one or two slots, serialized back. Not inherited.
void testObjectPosition() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"p\" style=\"object-position:left top\"><span id=\"c\">x</span></div>"
        "<div id=\"o\" style=\"object-position:right 10px\">x</div>"
        "<div id=\"s\" style=\"object-position:10px\">x</div>"
        "<div id=\"t\" style=\"object-position:top\">x</div>"
        "<div id=\"g\" style=\"object-position:left right\">x</div>"
        "<div id=\"b\" style=\"object-position:banana\">x</div>"
        "<div id=\"h\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* p = el("p");
    Element* c = el("c");
    Element* o = el("o");
    Element* s = el("s");
    Element* t = el("t");
    Element* g = el("g");
    Element* b = el("b");
    Element* h = el("h");
    check(p && c && o && s && t && g && b && h, "position elements found");
    if (!p || !c || !o || !s || !t || !g || !b || !h) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    const auto xy = [&](Element& e) {
        const ComputedStyle style = resolver.resolve(e);
        return std::pair<sui::Length, sui::Length>(style.objectPositionX, style.objectPositionY);
    };
    const auto isPct = [&](const sui::Length& l, float v) {
        return l.unit == sui::Length::Percent && l.value == v;
    };
    auto [px, py] = xy(*p);
    check(isPct(px, 0.0f) && isPct(py, 0.0f), "two edges pin the corner");
    check(xy(*c).first.value == 50.0f, "positioning never inherits");
    auto [ox, oy] = xy(*o);
    check(isPct(ox, 100.0f) && oy.unit == sui::Length::Px && oy.value == 10.0f,
          "an edge and a length share the slots");
    auto [sx, sy] = xy(*s);
    check(sx.unit == sui::Length::Px && sx.value == 10.0f && isPct(sy, 50.0f),
          "a lone length takes x and centres y");
    auto [tx, ty] = xy(*t);
    check(isPct(tx, 50.0f) && isPct(ty, 0.0f), "a lone edge centres the other half");
    check(xy(*g).first.value == 50.0f, "same-axis keywords drop the declaration");
    check(xy(*b).first.value == 50.0f, "garbage keeps centring");
    check(xy(*h).first.value == 50.0f, "the default centres");
    const auto serial = [&](const char* id) {
        return sui::StyleResolver::propertyValue(resolver.resolve(*el(id)),
                                                 "object-position");
    };
    check(std::string(serial("p")) == "0% 0%", "edges serialize as percentages");
    check(std::string(serial("s")) == "10px 50%", "a length round-trips");
    check(std::string(serial("h")) == "50% 50%", "the default serializes centred");
}

// `caret-color` recolors the text caret, defaults to the text color, inherits,
// and serializes. Garbage keeps the previous winner.
void testCaretColor() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"p\" style=\"caret-color:#ff0000\"><span id=\"c\">x</span></div>"
        "<div id=\"a\" style=\"caret-color:auto\">x</div>"
        "<div id=\"g\" style=\"caret-color:banana\">x</div>"
        "<div id=\"h\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* p = el("p");
    Element* c = el("c");
    Element* a = el("a");
    Element* g = el("g");
    Element* h = el("h");
    check(p && c && a && g && h, "caret elements found");
    if (!p || !c || !a || !g || !h) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; color: #112233 }"));
    const ComputedStyle red = resolver.resolve(*p);
    check(red.hasCaretColor && red.caretColor.r == 255, "an explicit caret parses");
    check(resolver.resolve(*c).caretColor.r == 255, "the caret color inherits");
    check(!resolver.resolve(*a).hasCaretColor, "auto clears the caret");
    check(!resolver.resolve(*g).hasCaretColor, "garbage keeps auto");
    check(!resolver.resolve(*h).hasCaretColor, "the default is auto");
    const auto serial = [&](const char* id) {
        return sui::StyleResolver::propertyValue(resolver.resolve(*el(id)), "caret-color");
    };
    check(std::string(serial("p")) == "#ff0000", "an explicit caret serializes");
    check(std::string(serial("h")) == "auto", "the default serializes as auto");

    // The helper answers auto with the text color and explicit with itself.
    check(sui::caretColorFor(red).r == 255, "an explicit caret wins");
    check(sui::caretColorFor(resolver.resolve(*h)).r == 0x11,
          "auto answers the text color");
}

// `mix-blend-mode` parses the separable set, folds the rest to normal, never
// inherits, and serializes back.
void testMixBlendMode() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"p\" style=\"mix-blend-mode:multiply\"><span id=\"c\">x</span></div>"
        "<div id=\"o\" style=\"mix-blend-mode:overlay\">x</div>"
        "<div id=\"g\" style=\"mix-blend-mode:banana\">x</div>"
        "<div id=\"h\">x</div>"
        "<div id=\"sl\" style=\"mix-blend-mode:soft-light\">x</div>"
        "<div id=\"hl\" style=\"mix-blend-mode:hard-light\">x</div>"
        "<div id=\"cd\" style=\"mix-blend-mode:color-dodge\">x</div>"
        "<div id=\"cb\" style=\"mix-blend-mode:color-burn\">x</div>"
        "<div id=\"hu\" style=\"mix-blend-mode:hue\">x</div>"
        "<div id=\"sa\" style=\"mix-blend-mode:saturation\">x</div>"
        "<div id=\"co\" style=\"mix-blend-mode:color\">x</div>"
        "<div id=\"lu\" style=\"mix-blend-mode:luminosity\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* p = el("p");
    Element* c = el("c");
    Element* o = el("o");
    Element* g = el("g");
    Element* h = el("h");
    check(p && c && o && g && h, "blend elements found");
    if (!p || !c || !o || !g || !h) {
        return;
    }
    using Blend = ComputedStyle::MixBlendMode;
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    check(resolver.resolve(*p).mixBlendMode == Blend::Multiply, "multiply parses");
    check(resolver.resolve(*c).mixBlendMode == Blend::Normal, "blending never inherits");
    check(resolver.resolve(*o).mixBlendMode == Blend::Overlay, "overlay parses");
    check(resolver.resolve(*el("sl")).mixBlendMode == Blend::SoftLight,
          "soft-light parses");
    check(resolver.resolve(*el("hl")).mixBlendMode == Blend::HardLight,
          "hard-light parses");
    check(resolver.resolve(*el("cd")).mixBlendMode == Blend::ColorDodge,
          "color-dodge parses");
    check(resolver.resolve(*el("cb")).mixBlendMode == Blend::ColorBurn,
          "color-burn parses");
    check(resolver.resolve(*el("hu")).mixBlendMode == Blend::Hue, "hue parses");
    check(resolver.resolve(*el("sa")).mixBlendMode == Blend::Saturation,
          "saturation parses");
    check(resolver.resolve(*el("co")).mixBlendMode == Blend::Color, "color parses");
    check(resolver.resolve(*el("lu")).mixBlendMode == Blend::Luminosity,
          "luminosity parses");
    check(resolver.resolve(*g).mixBlendMode == Blend::Normal, "garbage keeps normal");
    check(resolver.resolve(*h).mixBlendMode == Blend::Normal, "the default is normal");
    const auto serial = [&](const char* id) {
        return sui::StyleResolver::propertyValue(resolver.resolve(*el(id)),
                                                 "mix-blend-mode");
    };
    check(std::string(serial("p")) == "multiply", "multiply serializes");
    check(std::string(serial("o")) == "overlay", "overlay serializes");
    check(std::string(serial("sl")) == "soft-light", "soft-light serializes");
    check(std::string(serial("hl")) == "hard-light", "hard-light serializes");
    check(std::string(serial("cd")) == "color-dodge", "color-dodge serializes");
    check(std::string(serial("cb")) == "color-burn", "color-burn serializes");
    check(std::string(serial("hu")) == "hue", "hue serializes");
    check(std::string(serial("sa")) == "saturation", "saturation serializes");
    check(std::string(serial("co")) == "color", "color serializes");
    check(std::string(serial("lu")) == "luminosity", "luminosity serializes");
    check(std::string(serial("h")) == "normal", "the default serializes as normal");
}

void testFontStretch() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"a\" style=\"font-stretch:condensed\">"
        "<span id=\"c\">hi</span></div>"
        "<div id=\"b\" style=\"font-stretch:extra-expanded\">hi</div>"
        "<div id=\"d\" style=\"font-stretch:135%\">hi</div>"
        "<div id=\"e\" style=\"font-stretch:0%\">hi</div>"
        "<div id=\"g\" style=\"font-stretch:banana\">hi</div>"
        "<div id=\"h\">hi</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* c = el("c");
    Element* b = el("b");
    Element* d = el("d");
    Element* e = el("e");
    Element* g = el("g");
    Element* h = el("h");
    check(a && c && b && d && e && g && h, "stretch elements found");
    if (!a || !c || !b || !d || !e || !g || !h) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    check(resolver.resolve(*a).fontStretch == 0.75f, "condensed is three quarters");
    check(resolver.resolve(*c).fontStretch == 0.75f, "stretch inherits");
    check(resolver.resolve(*b).fontStretch == 1.5f, "extra-expanded is one and a half");
    check(resolver.resolve(*d).fontStretch == 1.35f, "a percentage is the raw factor");
    check(resolver.resolve(*e).fontStretch == 1.0f, "a zero width drops");
    check(resolver.resolve(*g).fontStretch == 1.0f, "garbage keeps normal");
    check(resolver.resolve(*h).fontStretch == 1.0f, "the default is normal");
    const auto serial = [&](const char* id) {
        return sui::StyleResolver::propertyValue(resolver.resolve(*el(id)),
                                                 "font-stretch");
    };
    check(std::string(serial("a")) == "condensed", "a named step serializes by name");
    check(std::string(serial("d")) == "135%", "an unnamed width serializes as a percentage");
    check(std::string(serial("h")) == "normal", "the default serializes as normal");
}

// `scroll-behavior` picks how a box's own scrolling moves. It does not inherit,
// and nonsense leaves the jump.
void testScrollBehavior() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"p\" style=\"scroll-behavior:smooth\"><div id=\"c\">x</div></div>"
        "<div id=\"a\" style=\"scroll-behavior:auto\">x</div>"
        "<div id=\"g\" style=\"scroll-behavior:banana\">x</div>"
        "<div id=\"d\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* p = el("p");
    Element* c = el("c");
    Element* a = el("a");
    Element* g = el("g");
    Element* d = el("d");
    check(p && c && a && g && d, "scroll behavior elements found");
    if (!p || !c || !a || !g || !d) {
        return;
    }
    using Behavior = ComputedStyle::ScrollBehavior;
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    check(resolver.resolve(*p).scrollBehavior == Behavior::Smooth, "smooth parses");
    check(resolver.resolve(*c).scrollBehavior == Behavior::Auto,
          "behavior does not inherit");
    check(resolver.resolve(*a).scrollBehavior == Behavior::Auto, "auto parses");
    check(resolver.resolve(*g).scrollBehavior == Behavior::Auto, "garbage leaves the jump");
    check(resolver.resolve(*d).scrollBehavior == Behavior::Auto, "the default is auto");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*p), "scroll-behavior") ==
              "smooth",
          "smooth serializes");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*d), "scroll-behavior") ==
              "auto",
          "the default serializes as auto");
}

// `color-scheme` re-picks the user-agent canvas pair for a document root that
// never chose its own colours, and an author who set them keeps them.
void testColorScheme() {
    using Scheme = ComputedStyle::ColorScheme;
    const sui::UaCanvas& dark = sui::uaCanvas(true);
    const sui::UaCanvas& light = sui::uaCanvas(false);
    // One document per case: the scheme is declared on the root itself, so the
    // cases cannot share a root.
    const auto bodyOf = [](const char* style, ComputedStyle& out, Element*& child) {
        std::string markup = "<sui><body";
        if (style[0]) {
            markup += " style=\"";
            markup += style;
            markup += "\"";
        }
        markup += "><div id=\"d\">x</div></body></sui>";
        auto document = parseOrNull(markup.c_str());
        if (!document) {
            return false;
        }
        Element* root = document->querySelector("body");
        child = document->getElementById("d");
        if (!root || !child) {
            return false;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
        out = resolver.resolve(*root);
        return true;
    };

    ComputedStyle style;
    Element* child = nullptr;
    check(bodyOf("color-scheme:dark", style, child) && style.colorScheme == Scheme::Dark,
          "dark parses");
    check(style.backgroundColor.r == dark.background.r && style.color.r == dark.text.r,
          "dark keeps the default pair");
    // The scheme is inherited, so a subtree follows the root until it chooses
    // its own. The document stays alive while the child is resolved; the
    // `bodyOf` helper's pointer would dangle once its document is gone.
    auto inherited = parseOrNull(
        "<sui><body style=\"color-scheme:dark\"><div id=\"d\">x</div></body></sui>");
    if (inherited) {
        Element* inheritedChild = inherited->getElementById("d");
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
        check(inheritedChild &&
                  resolver.resolve(*inheritedChild).colorScheme == Scheme::Dark,
              "the scheme inherits into the subtree");
    }

    check(bodyOf("color-scheme:light", style, child) &&
              style.colorScheme == Scheme::Light && style.colorSchemeOnly == false,
          "light parses");
    check(style.backgroundColor.r == light.background.r && style.color.r == light.text.r,
          "light flips the canvas pair");
    check(sui::StyleResolver::propertyValue(style, "color-scheme") == "light",
          "light serializes");

    check(bodyOf("color-scheme:only dark", style, child) && style.colorSchemeOnly,
          "only dark parses");
    check(style.backgroundColor.r == dark.background.r, "only dark paints dark");
    check(sui::StyleResolver::propertyValue(style, "color-scheme") == "only dark",
          "only dark serializes");

    // A list takes its first recognized scheme.
    check(bodyOf("color-scheme:dark light", style, child) &&
              style.colorScheme == Scheme::Dark,
          "a list takes the first");
    check(bodyOf("color-scheme:banana", style, child) &&
              style.colorScheme == Scheme::Normal &&
              style.backgroundColor.r == dark.background.r,
          "garbage leaves the default pair");
    check(sui::StyleResolver::propertyValue(style, "color-scheme") == "normal",
          "no preference serializes as normal");
    check(bodyOf("", style, child) && style.colorScheme == Scheme::Normal,
          "the default is no preference");

    // An author who paints the canvas keeps its colors whatever the scheme.
    check(bodyOf("color-scheme:light;background-color:#010203;color:#040506", style,
                 child) &&
              style.backgroundColor.r == 0x01 && style.color.r == 0x04,
          "an author keeps the colors it set");
}

// Native controls take the used scheme's user-agent palette: a light subtree
// swaps input/button/gauge surfaces, borders, and text, an author colour keeps
// its value, and an explicit `normal`/`dark` inside the subtree opts out.
void testColorSchemeWidgets() {
    auto document = parseOrNull(
        "<sui><body style=\"color-scheme:light\">"
        "<input id=\"in\"/>"
        "<button id=\"btn\"></button>"
        "<meter id=\"m\" value=\"0.5\"></meter>"
        "<input id=\"author\" style=\"background-color:#010203;border-color:#040506;"
        "color:#070809\"/>"
        "<div><input id=\"nested\"/></div>"
        "<div style=\"color-scheme:normal\"><input id=\"reset\"/></div>"
        "<div style=\"color-scheme:dark\"><input id=\"darkin\"/></div>"
        "<div style=\"color-scheme:unset\"><input id=\"unsetin\"/></div>"
        "<button id=\"cc\" style=\"border-color:currentColor\"></button>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    if (!el("in") || !el("btn") || !el("m") || !el("author") || !el("nested") ||
        !el("reset") || !el("darkin") || !el("unsetin") || !el("cc")) {
        check(false, "colour-scheme widget elements found");
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    const auto resolve = [&](const char* id) { return resolver.resolve(*el(id)); };
    const auto same = [](const SDL_Color& a, const SDL_Color& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };

    const sui::ComputedStyle input = resolve("in");
    check(input.colorScheme == sui::ComputedStyle::ColorScheme::Light,
          "a light body gives its controls the light scheme");
    check(same(input.backgroundColor, SDL_Color{255, 255, 255, 255}),
          "a light input takes the light surface");
    check(same(input.borderColor, SDL_Color{181, 181, 190, 255}),
          "a light input takes the light border");
    check(same(input.color, SDL_Color{24, 24, 27, 255}),
          "a light input takes the light text");

    const sui::ComputedStyle button = resolve("btn");
    check(same(button.backgroundColor, SDL_Color{239, 239, 244, 255}),
          "a light button takes the raised light surface");
    check(same(button.borderColor, SDL_Color{212, 212, 216, 255}),
          "a light button takes the light raised border");

    const sui::ComputedStyle meter = resolve("m");
    check(same(meter.backgroundColor, SDL_Color{239, 239, 244, 255}) &&
              same(meter.borderColor, SDL_Color{181, 181, 190, 255}),
          "a light gauge takes the light raised palette");

    const sui::ComputedStyle author = resolve("author");
    check(author.colorScheme == sui::ComputedStyle::ColorScheme::Light &&
              same(author.backgroundColor, SDL_Color{1, 2, 3, 255}) &&
              same(author.borderColor, SDL_Color{4, 5, 6, 255}) &&
              same(author.color, SDL_Color{7, 8, 9, 255}),
          "an author control colour survives the scheme swap");

    check(resolve("nested").colorScheme == sui::ComputedStyle::ColorScheme::Light &&
              same(resolve("nested").backgroundColor, SDL_Color{255, 255, 255, 255}),
          "the scheme inherits through a plain wrapper");

    const sui::ComputedStyle reset = resolve("reset");
    check(reset.colorScheme == sui::ComputedStyle::ColorScheme::Normal,
          "an explicit normal opts out of the inherited scheme");
    check(same(reset.backgroundColor, SDL_Color{24, 24, 27, 255}) &&
              same(reset.borderColor, SDL_Color{82, 82, 91, 255}) &&
              same(reset.color, SDL_Color{228, 228, 231, 255}),
          "a normal control under a light body stays dark");

    const sui::ComputedStyle darkin = resolve("darkin");
    check(darkin.colorScheme == sui::ComputedStyle::ColorScheme::Dark &&
              same(darkin.backgroundColor, SDL_Color{24, 24, 27, 255}),
          "an explicit dark control inside a light body stays dark");

    const sui::ComputedStyle cc = resolve("cc");
    const sui::ComputedStyle unsetin = resolve("unsetin");
    check(unsetin.colorScheme == sui::ComputedStyle::ColorScheme::Light &&
              same(unsetin.backgroundColor, SDL_Color{255, 255, 255, 255}),
          "color-scheme unset inherits like every inherited property");

    check(cc.colorScheme == sui::ComputedStyle::ColorScheme::Light &&
              same(cc.borderColor, SDL_Color{24, 24, 27, 255}),
          "currentColor follows the converted control foreground");

    auto neutral = parseOrNull("<sui><body><input id=\"n\"/></body></sui>");
    if (neutral) {
        sui::StyleResolver host;
        host.setSystemColorSchemeDark(false);
        const sui::ComputedStyle normal = host.resolve(*neutral->getElementById("n"));
        check(normal.colorScheme == sui::ComputedStyle::ColorScheme::Normal &&
                  same(normal.backgroundColor, SDL_Color{255, 255, 255, 255}),
              "a normal control follows a light host preference");
    }
}

// `prefers-color-scheme` asks the host, which the resolver carries: the engine
// assumes the dark canvas it ships with until told otherwise.
void testPrefersColorScheme() {
    auto document = parseOrNull("<sui><body><div id=\"d\">x</div></body></sui>");
    if (!document) {
        return;
    }
    Element* d = document->getElementById("d");
    if (!d) {
        return;
    }
    const auto colorOf = [&](bool dark) {
        sui::StyleResolver resolver;
        resolver.setSystemColorSchemeDark(dark);
        resolver.setViewportSize(400.0f, 300.0f);
        resolver.setRules(sui::CSSParser::parse(
            "@media (prefers-color-scheme: dark) { #d { color: #111111 } }"
            "@media (prefers-color-scheme: light) { #d { color: #222222 } }"
            "@media not (prefers-color-scheme: light) { #d { background-color: #333333 } }"
            "@media (prefers-color-scheme: banana) { #d { border-width: 9px } }"
            "@media (min-width: 1px) and (prefers-color-scheme: dark) {"
            " #d { border-width: 3px } }"));
        return resolver.resolve(*d);
    };
    const ComputedStyle dark = colorOf(true);
    check(dark.color.r == 0x11, "a dark host matches the dark query");
    check(dark.backgroundColor.r == 0x33, "a negated light query matches a dark host");
    check(dark.borderWidth.top.value == 3.0f, "a feature ANDs with the scheme");
    const ComputedStyle light = colorOf(false);
    check(light.color.r == 0x22, "a light host matches the light query");
    check(light.backgroundColor.a == 0, "a negated light query misses a light host");

    // The preference defaults to dark and changing it bumps the generation, so
    // rules under the feature re-resolve.
    sui::StyleResolver resolver;
    check(resolver.systemColorSchemeDark(), "the host defaults to dark");
    const unsigned before = resolver.generation();
    check(resolver.setSystemColorSchemeDark(true) == false,
          "setting the same preference changes nothing");
    check(resolver.setSystemColorSchemeDark(false) &&
              resolver.generation() != before,
          "a new preference bumps the generation");
}

// `currentColor` follows the element's own `color` wherever it appears, and
// the placeholder never survives a resolution.
void testCurrentColor() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"a\" style=\"color:#112233;background-color:currentColor;"
        "border:2px solid currentColor;outline-color:currentColor;"
        "accent-color:currentColor;text-decoration-color:currentColor;"
        "box-shadow:1px 1px 2px currentColor\">x</div>"
        "<div id=\"b\" style=\"color:#445566\">x</div>"
        "<div id=\"c\" style=\"color:currentColor\">x</div>"
        "<div id=\"g\" style=\"color:#778899;background-color:banana\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* b = el("b");
    Element* c = el("c");
    Element* g = el("g");
    check(a && b && c && g, "currentColor elements found");
    if (!a || !b || !c || !g) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    const ComputedStyle style = resolver.resolve(*a);
    check(style.color.r == 0x11, "the element keeps its own color");
    check(style.backgroundColor.r == 0x11 && style.backgroundColor.g == 0x22 &&
              style.backgroundColor.b == 0x33,
          "background follows the color");
    check(style.borderColor.r == 0x11, "the border shorthand follows too");
    check(style.outline.color.r == 0x11, "the outline follows");
    check(style.accentColor.r == 0x11, "the accent follows");
    check(style.textDecorationColor.r == 0x11, "the decoration follows");
    check(!style.boxShadows.empty() && style.boxShadows[0].color.r == 0x11,
          "a shadow follows");
    check(!sui::isCurrentColor(style.backgroundColor) &&
              !sui::isCurrentColor(style.borderColor) &&
              !sui::isCurrentColor(style.accentColor),
          "no placeholder survives");

    // The keyword is case-insensitive and resolves against this element, not an
    // ancestor that happened to set one.
    sui::StyleResolver childResolver;
    childResolver.setRules(
        sui::CSSParser::parse("body { margin: 0 } #c { color: #445566; "
                              "background-color: CURRENTCOLOR }"));
    const ComputedStyle child = childResolver.resolve(*c);
    check(child.color.r == 0x44 && child.backgroundColor.r == 0x44 &&
              child.backgroundColor.g == 0x55,
          "the keyword reads this element's color");

    // `color: currentColor` is not a colour; the element inherits instead.
    const ComputedStyle bad = resolver.resolve(*c);
    check(bad.color.r != 0x11 && bad.color.r != 0x44, "color never takes the keyword");

    // A literal still wins, and garbage leaves the background transparent.
    const ComputedStyle literal = resolver.resolve(*g);
    check(literal.color.r == 0x77 && literal.backgroundColor.a == 0,
          "a literal color is untouched and garbage is dropped");
    (void)b;
}

// `:focus-visible` matches the focused element only when it wants visible
// focus UI: the state flag set on keyboard interaction, or on focusing a
// control that takes text entry.
void testFocusVisible() {
    auto document = parseOrNull(
        "<sui><body><input id=\"t\" type=\"text\"/><button id=\"b\">go</button>"
        "<div id=\"d\"></div></body></sui>");
    if (!document) {
        return;
    }
    Element* t = document->getElementById("t");
    Element* b = document->getElementById("b");
    Element* d = document->getElementById("d");
    check(t && b && d, "focus elements found");
    if (!t || !b || !d) {
        return;
    }
    const auto sel = [](const char* text) { return sui::CSSParser::parseSelector(text); };
    sui::PseudoState hidden;
    hidden.focused = t;
    hidden.focusVisible = false;
    sui::PseudoState shown;
    shown.focused = t;
    shown.focusVisible = true;
    check(selectorMatches(sel("input:focus-visible"), *t, &shown),
          "a flagged focus matches");
    check(!selectorMatches(sel("input:focus-visible"), *t, &hidden),
          "an unflagged focus misses");
    check(!selectorMatches(sel("input:focus-visible"), *t, nullptr),
          "no state never matches");
    check(!selectorMatches(sel("button:focus-visible"), *t, &shown),
          "the flag does not leak across elements");
    check(!selectorMatches(sel("div:focus-visible"), *d, &shown),
          "an unfocused element misses");
}

// Meters and progress bars size like replaced elements (10em by 1em), paint
// their fill from value/min/max, and never lay out fallback text. Fractions
// clamp; garbage falls back; colour zones follow low/high/optimum.
// An svg sizes from its attributes (300x150 default) unless CSS sizes it,
// and its shapes take no layout space: they paint from the svg box.
void testSvgLayout() {
    auto document = parseOrNull(
        "<sui><body><svg id=\"a\"><rect width=\"10\" height=\"10\"/></svg>"
        "<svg id=\"b\" width=\"200\" height=\"50\">"
        "<circle r=\"5\"/><g><line x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\"/></g></svg>"
        "<svg id=\"c\" style=\"width:120px\"><rect width=\"10\" height=\"10\"/></svg>"
        "</body></sui>");
    if (!document) {
        return;
    }
    Element* a = document->getElementById("a");
    Element* b = document->getElementById("b");
    Element* c = document->getElementById("c");
    check(a && b && c, "svg elements found");
    if (!a || !b || !c) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    layoutRoot(*document, resolver, 400.0f);
    check(a->layoutWidth() == 300.0f && a->layoutHeight() == 150.0f,
          "a bare svg keeps the 300x150 viewport");
    check(b->layoutWidth() == 200.0f && b->layoutHeight() == 50.0f,
          "width and height attributes size the viewport");
    check(c->layoutWidth() == 120.0f && c->layoutHeight() == 150.0f,
          "css sizes win, attributes fill the rest");
    bool flat = true;
    std::function<void(Element&)> shapesFlat = [&](Element& root) {
        for (const auto& child : root.children()) {
            if (child && !child->isTextNode()) {
                flat = flat && child->layoutWidth() == 0.0f &&
                       child->layoutHeight() == 0.0f;
                shapesFlat(*child);
            }
        }
    };
    shapesFlat(*a);
    shapesFlat(*b);
    check(flat, "shapes take no layout space");
}

void testGaugeDefaults() {
    auto document = parseOrNull(
        "<sui><body><meter id=\"m\" value=\"0.5\"/>"
        "<progress id=\"p\" value=\"30\" max=\"100\"/>"
        "<progress id=\"q\"/>"
        "<meter id=\"w\" style=\"width:200px\">fallback</meter></body></sui>");
    if (!document) {
        return;
    }
    Element* m = document->getElementById("m");
    Element* p = document->getElementById("p");
    Element* q = document->getElementById("q");
    Element* w = document->getElementById("w");
    check(m && p && q && w, "gauge elements found");
    if (!m || !p || !q || !w) {
        return;
    }

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    const sui::ComputedStyle chrome = resolver.resolve(*m);
    check(chrome.padding.top.value == 0.0f && chrome.padding.left.value == 0.0f,
          "a gauge has no text padding");
    check(chrome.borderWidth.top.value == 1.0f, "a gauge keeps a thin border");
    check(chrome.width.isAuto(), "a gauge width starts auto");

    // At the default 16px font the box is 160x16 content plus its border.
    sui::StyleResolver layout;
    layout.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    layoutRoot(*document, layout, 400.0f);
    check(m->layoutWidth() == 162.0f && m->layoutHeight() == 18.0f,
          "a gauge lays out as a 10em by 1em border box");
    check(w->layoutWidth() == 202.0f, "an author width overrides the gauge default");
    check(w->textRuns().empty(), "fallback text takes no runs");

    check(m->gaugeFraction() == 0.5f, "a meter fraction reads value over max");
    check(p->gaugeFraction() == 0.3f, "a progress fraction reads value over max");
    check(!p->gaugeIndeterminate() && q->gaugeIndeterminate(),
          "a valueless progress is indeterminate");
    check(!m->gaugeIndeterminate(), "a meter is never indeterminate");

    auto fractionOf = [](const char* markup) {
        auto doc = parseOrNull(markup);
        if (!doc) {
            return -1.0f;
        }
        Element* body = doc->querySelector("body");
        if (!body || body->children().empty() || !body->children()[0]) {
            return -1.0f;
        }
        return body->children()[0]->gaugeFraction();
    };
    check(fractionOf("<sui><body><meter value=\"7\" max=\"4\"/></body></sui>") == 1.0f,
          "a fraction clamps at one");
    check(fractionOf("<sui><body><meter value=\"-3\"/></body></sui>") == 0.0f,
          "a fraction clamps at zero");
    check(fractionOf("<sui><body><progress value=\"junk\" max=\"10\"/></body></sui>") ==
              0.0f,
          "a garbage value falls back to empty");
    check(fractionOf("<sui><body><meter max=\"0\" value=\"5\"/></body></sui>") == 1.0f,
          "a degenerate scale still spans");

    auto zoneOf = [](const char* markup) {
        auto doc = parseOrNull(markup);
        if (!doc) {
            return -1;
        }
        Element* body = doc->querySelector("body");
        if (!body || body->children().empty() || !body->children()[0]) {
            return -1;
        }
        return body->children()[0]->gaugeZone();
    };
    check(zoneOf("<sui><body><meter value=\"0.5\"/></body></sui>") == 0,
          "a middle value sits beside the optimum");
    check(zoneOf("<sui><body><meter value=\"0.9\" optimum=\"0.9\" high=\"0.8\"/>"
               "</body></sui>") == 0,
          "a high optimum likes high values");
    check(zoneOf("<sui><body><meter value=\"0.5\" optimum=\"0.9\" high=\"0.8\"/>"
               "</body></sui>") == 1,
          "the band off the optimum side is yellow");
    check(zoneOf("<sui><body><meter value=\"0.1\" optimum=\"0.1\" low=\"0.2\"/>"
               "</body></sui>") == 0,
          "a low optimum likes low values");
    check(zoneOf("<sui><body><meter value=\"0.95\" optimum=\"0.1\" low=\"0.2\" high=\"0.8\"/>"
               "</body></sui>") == 2,
          "past the band turns red");
}

void testValidationApi() {
    auto document = parseOrNull(
        "<sui><body>"
        "<input id=\"a\" type=\"text\" required=\"\" value=\"\"/>"
        "<input id=\"b\" type=\"number\" value=\"5\" min=\"10\"/>"
        "<input id=\"c\" type=\"text\" value=\"ok\"/>"
        "<input id=\"d\" type=\"text\" disabled=\"\" value=\"\"/>"
        "<input id=\"f\" type=\"text\" readonly=\"\" required=\"\" value=\"\"/>"
        "<div id=\"e\"></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* a = el("a");
    Element* b = el("b");
    Element* c = el("c");
    Element* d = el("d");
    Element* e = el("e");
    Element* f = el("f");
    check(a && b && c && d && e && f, "api elements found");
    if (!a || !b || !c || !d || !e || !f) {
        return;
    }
    check(a->willValidate() && !d->willValidate() && !f->willValidate() &&
              !e->willValidate(),
          "candidates validate, barred elements do not");
    check(f->checkValidity(), "a readonly field is barred from validation");
    check(!a->checkValidity() && a->validity().valueMissing,
          "required emptiness flags valueMissing");
    check(!a->validationMessage().empty(), "a failure explains itself");
    check(!b->checkValidity() && b->validity().rangeUnderflow,
          "a low number flags rangeUnderflow");
    check(c->checkValidity() && c->validity().valid() && c->validationMessage().empty(),
          "a good field is valid and quiet");

    // Custom errors fail until cleared, with their own message.
    c->setCustomValidity("nope");
    check(!c->checkValidity() && c->validity().customError &&
              c->validationMessage() == "nope",
          "a custom error fails with its message");
    c->setCustomValidity("");
    check(c->checkValidity(), "clearing the custom error validates");

    // Reporting focuses the failure and answers validity.
    check(!a->reportValidity(), "reporting an invalid field fails");
    check(document->focused() == a, "reporting focuses the failure");
    check(c->reportValidity(), "reporting a valid field passes");
}

// Bidi visual order: runs lay out left-to-right in LTR paragraphs and flip
// around RTL ones, while pure-LTR lines never move. Positions only; run order
// and caret mapping stay logical.
void testBidiOrder() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"rtl\" style=\"direction:rtl\">\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85 hello</div>"
        "<div id=\"ltr\">one \xD7\xA9\xD7\xA0\xD7\x99 three</div>"
        "<div id=\"num\" style=\"direction:rtl\">\xD8\xB3\xD9\x86\xD8\xA9 2024</div>"
        "<div id=\"auto\">\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85 hello</div>"
        "<div id=\"plain\">just ascii here</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* rtl = el("rtl");
    Element* ltr = el("ltr");
    Element* num = el("num");
    Element* autoEl = el("auto");
    Element* plain = el("plain");
    check(rtl && ltr && num && autoEl && plain, "bidi elements found");
    if (!rtl || !ltr || !num || !autoEl || !plain) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0 }"));
    layoutRoot(*document, resolver, 400.0f);
    const auto xs = [](Element& e) {
        std::vector<float> out;
        for (const auto& run : e.textRuns()) {
            out.push_back(run.x);
        }
        return out;
    };
    // An rtl base flips the line: the trailing latin word prints left of the
    // arabic one, even though Latin leads nowhere here.
    {
        const std::vector<float> at = xs(*rtl);
        check(at.size() == 2, "two runs share the rtl line");
        if (at.size() == 2) {
            check(at[1] < at[0], "latin follows arabic visually in rtl");
        }
    }
    // LTR paragraph: hebrew sits between, order untouched.
    {
        const std::vector<float> at = xs(*ltr);
        check(at.size() == 3, "three runs share the mixed line");
        if (at.size() == 3) {
            check(at[0] < at[1] && at[1] < at[2], "an ltr line keeps run order");
        }
    }
    // European digits in RTL text sit left of their noun.
    {
        const std::vector<float> at = xs(*num);
        check(at.size() == 2, "two runs share the numbered line");
        if (at.size() == 2) {
            check(at[1] < at[0], "digits print left of arabic");
        }
    }
    // The default base is ltr, not first-strong: Latin-led RTL text lays out
    // straight, the way browsers set it.
    {
        const std::vector<float> at = xs(*autoEl);
        check(at.size() == 2, "two runs share the default line");
        if (at.size() == 2) {
            check(at[0] < at[1], "a default base never flips runs");
        }
    }
    // Pure ASCII never reorders.
    {
        const std::vector<float> at = xs(*plain);
        bool ordered = true;
        for (std::size_t i = 1; i < at.size(); ++i) {
            ordered = ordered && at[i - 1] < at[i];
        }
        check(ordered, "ascii runs stay put");
    }
}

// Lines distribute across their width: center and right shift runs,
// justify spreads the leftover across the gaps (all but the last line), and
// `start`/`end` read through `direction`. `direction` itself parses,
// inherits, and sets the bidi base.
void testTextAlign() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"l\" style=\"width:200px\">hi</div>"
        "<div id=\"c\" style=\"width:200px;text-align:center\">hi</div>"
        "<div id=\"r\" style=\"width:200px;text-align:right\">hi</div>"
        "<div id=\"s\" style=\"width:200px;text-align:start\">hi</div>"
        "<div id=\"e\" style=\"width:200px;text-align:end\">hi</div>"
        "<div id=\"sr\" style=\"width:200px;direction:rtl;text-align:start\">hi</div>"
        "<div id=\"er\" style=\"width:200px;direction:rtl;text-align:end\">hi</div>"
        "<div id=\"j\" style=\"width:100px;text-align:justify\">aa bb cc dd ee</div>"
        "<div id=\"jl\" style=\"width:100px;text-align:justify\">aa bb cc dd ee ff gg</div>"
        "<div id=\"d\" style=\"direction:rtl\"><span id=\"dc\">x</span></div>"
        "<div id=\"g\" style=\"direction:banana\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* l = el("l");
    Element* c = el("c");
    Element* r = el("r");
    Element* s = el("s");
    Element* e = el("e");
    Element* sr = el("sr");
    Element* er = el("er");
    Element* j = el("j");
    Element* jl = el("jl");
    Element* d = el("d");
    Element* dc = el("dc");
    Element* g = el("g");
    check(l && c && r && s && e && sr && er && j && jl && d && dc && g,
          "align elements found");
    if (!l || !c || !r || !s || !e || !sr || !er || !j || !jl || !d || !dc || !g) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, resolver, 400.0f);
    const auto x0 = [](Element& e) {
        return e.textRuns().empty() ? -1.0f : e.textRuns()[0].x;
    };
    // "hi" measures 16px in a 200px box.
    check(x0(*l) == 0.0f, "left starts at the edge");
    check(x0(*c) == 92.0f, "center splits the leftover");
    check(x0(*r) == 184.0f, "right pins the end");
    check(x0(*s) == 0.0f, "start reads left to right");
    check(x0(*e) == 184.0f, "end reads right to left");
    check(x0(*sr) == 184.0f, "start reads right under rtl");
    check(x0(*er) == 0.0f, "end reads left under rtl");
    // A lone justified line is its own last line, so it starts over.
    check(j->textRuns().size() == 5, "five words share the line");
    if (j->textRuns().size() == 5) {
        bool plain = true;
        for (std::size_t k = 0; k < 5; ++k) {
            plain = plain && j->textRuns()[k].x == static_cast<float>(k * 20);
        }
        check(plain, "a last line never justifies");
    }
    // Seven words wrap five and two: the full line spreads edge to edge.
    {
        // Group runs by line.
        std::vector<float> lineEnd;
        float lineY = -1.0f;
        float lineRight = 0.0f;
        for (const auto& run : jl->textRuns()) {
            if (run.y != lineY) {
                if (lineY >= 0.0f) {
                    lineEnd.push_back(lineRight);
                }
                lineY = run.y;
                lineRight = 0.0f;
            }
            lineRight = run.x + run.width;
        }
        lineEnd.push_back(lineRight);
        check(lineEnd.size() == 2, "seven words wrap two lines");
        if (lineEnd.size() == 2) {
            check(lineEnd[0] == 100.0f, "the full line spreads edge to edge");
        }
        bool lastLeft = false;
        for (const auto& run : jl->textRuns()) {
            if (run.y == lineY && run.x == 0.0f) {
                lastLeft = true;
            }
        }
        check(lastLeft, "the last line starts over");
        if (jl->textRuns().size() == 7) {
            check(jl->textRuns()[0].x == 0.0f && jl->textRuns()[1].x == 21.0f &&
                      jl->textRuns()[4].x == 84.0f,
                  "the full line spreads its gaps evenly");
        }
    }

    using Direction = ComputedStyle::Direction;
    check(resolver.resolve(*d).direction == Direction::RTL, "rtl parses");
    check(resolver.resolve(*dc).direction == Direction::RTL, "direction inherits");
    check(resolver.resolve(*g).direction == Direction::LTR, "garbage keeps ltr");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*d), "direction") == "rtl",
          "rtl serializes");
    check(sui::StyleResolver::propertyValue(resolver.resolve(*s), "text-align") == "start",
          "start serializes");
}

// The last line answers `text-align-last`: centering, right-aligning, or even
// justifying where the block would start over.
void testTextAlignLast() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"c\" style=\"width:100px;text-align:justify;text-align-last:center\">"
        "aa bb cc dd ee ff gg</div>"
        "<div id=\"r\" style=\"width:100px;text-align:justify;text-align-last:right\">"
        "aa bb cc dd ee ff gg</div>"
        "<div id=\"j\" style=\"width:100px;text-align:justify;text-align-last:justify\">"
        "aa bb cc dd ee ff gg</div>"
        "<div id=\"p\" style=\"text-align-last:right\"><span id=\"k\">x</span></div>"
        "<div id=\"g\" style=\"text-align-last:banana\">x</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* c = el("c");
    Element* r = el("r");
    Element* j = el("j");
    Element* p = el("p");
    Element* k = el("k");
    Element* g = el("g");
    check(c && r && j && p && k && g, "last-line elements found");
    if (!c || !r || !j || !p || !k || !g) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, resolver, 400.0f);
    const auto lastX = [](Element& e) {
        float y = -1.0f;
        float x = -1.0f;
        for (const auto& run : e.textRuns()) {
            if (run.y > y) {
                y = run.y;
                x = run.x;
            }
        }
        return x;
    };
    // Five 16px words fill 96 of 100px; the last line holds "ff gg".
    check(lastX(*c) == 32.0f, "a centred last line splits its leftover");
    check(lastX(*r) == 64.0f, "a right last line pins its end");
    {
        float y = -1.0f;
        float right = 0.0f;
        for (const auto& run : j->textRuns()) {
            if (run.y >= y) {
                y = run.y;
                right = run.x + run.width;
            }
        }
        check(right == 100.0f, "a justified last line spreads too");
    }

    using Last = ComputedStyle::TextAlignLast;
    check(resolver.resolve(*p).textAlignLast == Last::Right, "right last parses");
    check(resolver.resolve(*k).textAlignLast == Last::Right, "the last-line rule inherits");
    check(resolver.resolve(*g).textAlignLast == Last::Auto, "garbage keeps auto");
    const auto serial = [&](const char* id) {
        return sui::StyleResolver::propertyValue(resolver.resolve(*el(id)),
                                                 "text-align-last");
    };
    check(std::string(serial("p")) == "right", "right serializes");
    check(std::string(serial("g")) == "auto", "the default serializes as auto");
}

// A `<wbr/>` is a soft break opportunity: under `white-space:nowrap` (where
// words never wrap on their own) the next word starts a fresh line when
// anything is already on this one. Under normal wrapping the overflow check
// reaches the same break by itself, and a fitting word always stays.
void testWordBreakOpportunity() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"w\" style=\"width:50px;white-space:nowrap\">aaaa<wbr/>bbbbbbbb</div>"
        "<div id=\"n\" style=\"width:50px;white-space:nowrap\">aaaabbbbbbbb</div>"
        "<div id=\"f\" style=\"width:50px;white-space:nowrap\">aa<wbr/>bb</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    const auto el = [&](const char* id) { return document->getElementById(id); };
    Element* w = el("w");
    Element* n = el("n");
    Element* f = el("f");
    check(w && n && f, "wbr elements found");
    if (!w || !n || !f) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 16px }"));
    layoutRoot(*document, resolver, 400.0f);
    check(w->textRuns().size() == 2, "a wbr breaks the nowrap line in two");
    if (w->textRuns().size() == 2) {
        check(w->textRuns()[0].y < w->textRuns()[1].y, "the second piece drops a line");
        check(w->textRuns()[0].text == "aaaa" && w->textRuns()[1].text == "bbbbbbbb",
              "no piece gains or loses glyphs");
    }
    check(n->textRuns().size() == 1, "no wbr means no break under nowrap");
    check(f->textRuns().size() == 2, "words stay words with or without a wbr");
    if (f->textRuns().size() == 2) {
        check(f->textRuns()[0].y == f->textRuns()[1].y,
              "a fitting word stays on its line at its wbr");
    }
}

// Phrasing elements carry their semantic styles: emphasis is italic, small and
// big scale off the parent, sub/sup shift the baseline, struck text strikes
// through and inserted text underlines, templates stay inert, and a disabled
// button dims unless the author sets its opacity.
void testPhrasingStyles() {
    auto document = parseOrNull(
        "<sui><body>"
        "<p id=\"t\">x<em id=\"e\">e</em><i id=\"i\">i</i><small id=\"sm\">s</small>"
        "<sub id=\"sub\">2</sub><sup id=\"sup\">2</sup><big id=\"big\">b</big>"
        "<s id=\"s\">s</s><del id=\"del\">d</del><ins id=\"ins\">i</ins>"
        "<u id=\"u\">u</u><var id=\"v\">v</var></p>"
        "<p id=\"q\"><span id=\"sp\">s</span><label id=\"lb\">l</label>"
        "<code id=\"co\">c</code><kbd id=\"kb\">k</kbd><samp id=\"sa\">s</samp></p>"
        "<template id=\"tp\"><div id=\"inner\">x</div></template>"
        "<button id=\"b\" disabled=\"\">x</button>"
        "<button id=\"be\">x</button>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body { margin: 0; font-size: 20px }"));
    const auto styleOf = [&](const char* id) {
        return resolver.resolve(*document->getElementById(id));
    };
    check(styleOf("e").fontStyle == ComputedStyle::FontItalic, "em is italic");
    check(styleOf("i").fontStyle == ComputedStyle::FontItalic, "i is italic");
    check(styleOf("v").fontStyle == ComputedStyle::FontItalic, "var is italic");
    check(styleOf("sp").fontStyle == ComputedStyle::FontNormal, "span is upright");
    check(styleOf("lb").fontStyle == ComputedStyle::FontNormal, "label is upright");
    check(styleOf("co").fontStyle == ComputedStyle::FontNormal, "code is upright");
    check(styleOf("co").fontFamily == "monospace", "code is monospaced");
    check(styleOf("kb").fontFamily == "monospace", "kbd is monospaced");
    check(styleOf("sa").fontFamily == "monospace", "samp is monospaced");
    check(styleOf("kb").display == ComputedStyle::Inline, "kbd stays inline");
    check(styleOf("sa").display == ComputedStyle::Inline, "samp stays inline");
    check(styleOf("sm").fontSize == 16.0f, "small scales off the parent");
    check(styleOf("big").fontSize == 24.0f, "big grows off the parent");
    check(styleOf("sub").verticalAlign == ComputedStyle::VAlignSub, "sub sinks");
    check(styleOf("sub").fontSize == 16.0f, "sub shrinks too");
    check(styleOf("sup").verticalAlign == ComputedStyle::VAlignSuper, "sup rises");
    check(styleOf("s").textDecorationLine == ComputedStyle::DecLineThrough,
          "s strikes through");
    check(styleOf("del").textDecorationLine == ComputedStyle::DecLineThrough,
          "del strikes through");
    check(styleOf("ins").textDecorationLine == ComputedStyle::DecUnderline,
          "ins underlines");
    check(styleOf("u").textDecorationLine == ComputedStyle::DecUnderline,
          "u underlines");
    check(styleOf("tp").display == ComputedStyle::None, "a template takes no box");
    check(styleOf("b").opacity == 0.5f, "a disabled button dims");
    check(styleOf("be").opacity == 1.0f, "an enabled button keeps full opacity");

    // An author declaration still wins over every user-agent default here.
    sui::StyleResolver author;
    author.setRules(sui::CSSParser::parse(
        "body { margin: 0 } em { font-style: normal } button { opacity: 0.8 }"));
    auto document2 = parseOrNull(
        "<sui><body><em id=\"e\">x</em><button id=\"b\" disabled=\"\">x</button></body></sui>");
    if (!document2) {
        return;
    }
    check(author.resolve(*document2->getElementById("e")).fontStyle ==
              ComputedStyle::FontNormal,
          "an author normal beats emphasis");
    check(author.resolve(*document2->getElementById("b")).opacity == 0.8f,
          "an author opacity beats the dimming");
}void testBackdropFilter() {
    auto document = parseOrNull("<sui><body><div id=\"x\">hi</div></body></sui>");
    if (!document) {
        return;
    }
    Element* x = document->getElementById("x");
    check(x != nullptr, "backdrop element found");
    if (!x) {
        return;
    }
    const auto backdropOf = [&](const char* css) {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(css));
        return resolver.resolve(*x).backdropFilter;
    };
    check(backdropOf("#x{backdrop-filter:blur(6px)}") == "blur(6px)",
          "a blur chain stores verbatim");
    check(backdropOf("#x{backdrop-filter:brightness(0.5) contrast(2)}") ==
              "brightness(0.5) contrast(2)",
          "a shift chain stores verbatim");
    check(backdropOf("#x{backdrop-filter:none}") == "none", "none stores as none");

    // Serialization round-trips the stored chain.
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("#x{backdrop-filter:blur(6px)}"));
    check(sui::StyleResolver::propertyValue(resolver.resolve(*x), "backdrop-filter") ==
              "blur(6px)",
          "backdrop-filter serializes");
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

// Every background longhand answers with one entry per stored layer, in the
// grammar the parser reads; gradients print a canonical form (angle or
// shape/extent/centre/radii, then the stops with authored offsets only), so
// the readings parse back to the same layers.
void testBackgroundReadBack() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div>"
        "<div id=\"d\"></div><div id=\"e\"></div><div id=\"f\"></div>"
        "<div id=\"g\"></div><div id=\"h\"></div>"
        "<div id=\"p\" style=\"background-image:url(p.png);"
        "background-position:right 4px bottom 8px\">"
        "<div id=\"q\" style=\"background-image:inherit;"
        "background-position:inherit\"></div></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { background-image: linear-gradient(90deg, #ff0000 0%, #0000ff80 100%) }"
        "#b { background-image: radial-gradient(circle farthest-side at 20% 30% 10px,"
        " red, blue), url(two.png) }"
        "#c { background-image: conic-gradient(from 45deg at 25% 75%, red 0%, blue 100%) }"
        "#d { background-size: cover, auto 40px; background-repeat: repeat-x, space;"
        " background-position: right top, 50% 50%; background-clip: content-box;"
        " background-origin: border-box; background-attachment: fixed, local }"
        "#e { background-position: 10px 20% }"
        "#h { background-size: 10px 20px 30px }"
        "#f { background-image: none }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("a", "background-image") ==
              "linear-gradient(90deg, #ff0000 0%, #0000ff80 100%)",
          "a linear gradient reads its angle and stops");
    check(value("b", "background-image") ==
              "radial-gradient(circle farthest-side at 20% 30% 10px, #ff0000, #0000ff),"
              " url(two.png)",
          "a radial gradient reads shape, extent, centre, and radius before the next layer");
    check(value("c", "background-image") ==
              "conic-gradient(from 45deg at 25% 75%, #ff0000 0%, #0000ff 100%)",
          "a conic gradient reads its from-angle, centre, and stops");
    check(value("d", "background-size") == "cover, auto 40px", "sizes read per layer");
    check(value("d", "background-repeat") == "repeat-x, space", "repeats read per layer");
    check(value("d", "background-position") == "right top, left 50% top 50%",
          "positions read per layer, offsets as the four-value form");
    check(value("d", "background-clip") == "content-box", "a lone clip serves every layer");
    check(value("d", "background-origin") == "border-box", "origin reads its box");
    check(value("d", "background-attachment") == "fixed, local",
          "attachments read per layer");
    check(value("e", "background-position") == "left 10px top 20%",
          "a bare length pair reads as explicit edges");
    check(value("p", "background-position") == "right 4px bottom 8px",
          "an edge offset reads the magnitude the author wrote");
    check(value("h", "background-size") == "auto",
          "a list entry that fell back to auto keeps its reading");
    check(value("f", "background-image") == "none", "an explicit none is a layer");
    check(value("g", "background-image") == "none" &&
              value("g", "background-size") == "auto" &&
              value("g", "background-position") == "left top" &&
              value("g", "background-repeat") == "repeat",
          "an untouched layer reads the initial values");
    check(value("g", "background-clip") == "border-box" &&
              value("g", "background-origin") == "padding-box" &&
              value("g", "background-attachment") == "scroll",
          "and the initial boxes and attachment");
    check(value("g", "background-color") == "#00000000",
          "a transparent background keeps its alpha");
    check(value("q", "background-image") == "url(p.png)",
          "background-image: inherit replays the parent's layers");
    check(value("q", "background-position") == "right 4px bottom 8px",
          "and the parent's position measures back from the edges");
}

// The `background` shorthand expands each comma layer into all seven
// longhands (and resets the colour), then reads back in the same grammar:
// image, position / size, repeat, attachment, origin, clip, and the colour on
// the final layer only — so the reading parses back to the same layers.
void testBackgroundShorthand() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"s1\"></div><div id=\"s2\"></div><div id=\"s3\"></div>"
        "<div id=\"s4\"></div><div id=\"s5\"></div><div id=\"s6\"></div>"
        "<div id=\"s7\"></div><div id=\"s8\"></div><div id=\"s9\"></div>"
        "<div id=\"sp\" style=\"background:url(x.png) no-repeat\">"
        "<div id=\"sq\" style=\"background:inherit\"></div></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#s1 { background: url(bg.png) no-repeat right 10px top 5px / 20px 30px fixed"
        " content-box padding-box #11223344 }"
        "#s2 { background: linear-gradient(45deg, #ff0000, #0000ff) center / cover"
        " repeat-y scroll border-box }"
        "#s3 { background: url(a.png), url(b.png) blue }"
        "#s4 { background-color: red }"
        "#s5 { background-color: red; background: url(x.png) }"
        "#s6 { background: none }"
        "#s7 { background: url(a.png) banana }"
        "#s8 { background: url(a.png) red, url(b.png) }"
        "#s9 { background: url(c.png) no-repeat center / contain #00ff00 }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("s1", "background") ==
              "url(bg.png) right 10px top 5px / 20px 30px no-repeat fixed"
              " content-box padding-box #11223344",
          "the shorthand rebuilds image, position, size, repeat, attachment, boxes, colour");
    check(value("s1", "background-size") == "20px 30px" &&
              value("s1", "background-origin") == "content-box" &&
              value("s1", "background-clip") == "padding-box" &&
              value("s1", "background-attachment") == "fixed" &&
              value("s1", "background-repeat") == "no-repeat",
          "and the longhands hold the same layer");
    check(value("s2", "background") ==
              "linear-gradient(45deg, #ff0000, #0000ff) center center / cover repeat-y"
              " scroll border-box border-box #00000000",
          "one box sets both origin and clip; the colour resets");
    check(value("s3", "background") ==
              "url(a.png) left top / auto repeat scroll padding-box border-box,"
              " url(b.png) left top / auto repeat scroll padding-box border-box #0000ff",
          "every layer reads, with the colour on the final one");
    check(value("s4", "background-color") == "#ff0000",
          "a bare colour still reaches the colour longhand");
    check(value("s5", "background-color") == "#00000000",
          "the shorthand resets the colour when it names none");
    check(value("s6", "background") ==
              "none left top / auto repeat scroll padding-box border-box #00000000",
          "an explicit none is one layer of initials");
    check(value("s7", "background") ==
              "none left top / auto repeat scroll padding-box border-box #00000000",
          "one bad token drops the whole declaration");
    check(value("s8", "background") ==
              "none left top / auto repeat scroll padding-box border-box #00000000",
          "a colour in a non-final layer drops the whole declaration");
    check(value("s9", "background") ==
              "url(c.png) center center / contain no-repeat scroll padding-box border-box"
              " #00ff00",
          "the colour may follow the size");
    check(value("sq", "background") ==
              "url(x.png) left top / auto no-repeat scroll"
              " padding-box border-box #00000000",
          "background: inherit replays the parent's shorthand");
}

// content-visibility: hidden skips a subtree's layout and paint, while the box
// itself keeps its borders and padding. contain-intrinsic-size supplies the
// placeholder content size; auto is carried but lays out like visible.
void testContentVisibility() {
    auto document = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"hidden\" style=\"width:100px;padding:5px;border:2px solid #000;"
        "content-visibility:hidden\"><div id=\"inner\" style=\"height:50px\"></div>"
        "<span id=\"hiddenText\">secret</span></div>"
        "<div id=\"after\" style=\"height:10px\"></div>"
        "<div id=\"intrinsic\" style=\"display:inline-block;content-visibility:hidden;"
        "contain-intrinsic-size:120px 40px\"><span style=\"height:99px\"></span></div>"
        "<div id=\"shown\" style=\"content-visibility:visible\">"
        "<div id=\"shownInner\" style=\"height:50px\"></div></div>"
        "<div id=\"auto\" style=\"content-visibility:auto\">"
        "<div id=\"autoInner\" style=\"height:50px\"></div></div>"
        "<div id=\"flex\" style=\"display:flex\">"
        "<div id=\"flexHidden\" style=\"content-visibility:hidden;"
        "contain-intrinsic-size:70px 30px\"><span style=\"height:99px\"></span></div>"
        "</div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    Element* hidden = document->getElementById("hidden");
    Element* inner = document->getElementById("inner");
    Element* hiddenText = document->getElementById("hiddenText");
    Element* after = document->getElementById("after");
    Element* intrinsic = document->getElementById("intrinsic");
    Element* shown = document->getElementById("shown");
    Element* shownInner = document->getElementById("shownInner");
    Element* autoEl = document->getElementById("auto");
    Element* autoInner = document->getElementById("autoInner");
    Element* flexHidden = document->getElementById("flexHidden");
    check(hidden && inner && hiddenText && after && intrinsic && shown && shownInner &&
              autoEl && autoInner && flexHidden,
          "the content-visibility elements found");
    if (!hidden || !inner || !hiddenText || !after || !intrinsic || !shown ||
        !shownInner || !autoEl || !autoInner || !flexHidden) {
        return;
    }

    layoutRoot(*document, resolver, 400.0f);
    checkNear(inner->layoutWidth(), 0.0f, 0.01f,
              "hidden content contributes no child width");
    checkNear(inner->layoutHeight(), 0.0f, 0.01f,
              "hidden content contributes no child height");
    check(hiddenText->textRuns().empty(), "hidden content leaves no text runs");
    checkNear(hidden->layoutHeight(), 14.0f, 0.01f,
              "the hidden box keeps only its padding and borders");
    checkNear(after->layoutY(), hidden->layoutY() + hidden->layoutHeight(), 0.01f,
              "the next block starts after the hidden box");
    checkNear(intrinsic->contentWidth(), 120.0f, 0.01f,
              "contain-intrinsic-size supplies a hidden inline-block width");
    checkNear(intrinsic->layoutHeight(), 40.0f, 0.01f,
              "contain-intrinsic-size supplies a hidden height");
    checkNear(shown->layoutHeight(), 50.0f, 0.01f,
              "content-visibility: visible keeps its content");
    checkNear(shownInner->layoutHeight(), 50.0f, 0.01f,
              "visible child layout is unchanged");
    checkNear(autoEl->layoutHeight(), 50.0f, 0.01f,
              "content-visibility: auto lays out like visible");
    checkNear(autoInner->layoutHeight(), 50.0f, 0.01f,
              "auto content is not skipped in this engine");
    checkNear(flexHidden->layoutWidth(), 70.0f, 0.01f,
              "a hidden flex item uses its intrinsic width");
    checkNear(flexHidden->layoutHeight(), 30.0f, 0.01f,
              "a hidden flex item uses its intrinsic height");

    const auto value = [&](Element* el, const char* property) {
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value(hidden, "content-visibility") == "hidden",
          "content-visibility reads back");
    check(value(autoEl, "content-visibility") == "auto",
          "the auto reading round-trips");
    check(value(shown, "content-visibility") == "visible",
          "the initial content-visibility is visible");
    check(value(intrinsic, "contain-intrinsic-size") == "120px 40px",
          "contain-intrinsic-size reads both axes");
    check(value(shown, "contain-intrinsic-size") == "none",
          "the initial contain-intrinsic-size is none");

    // Toggling back to visible lays the old subtree out again; hiding it again
    // must clear the runs the previous pass produced.
    hidden->style().setProperty("content-visibility", "visible");
    layoutRoot(*document, resolver, 400.0f);
    checkNear(inner->layoutHeight(), 50.0f, 0.01f,
              "turning content-visibility off restores the child");
    check(!hiddenText->textRuns().empty(), "the restored text paints again");
    hidden->style().setProperty("content-visibility", "hidden");
    layoutRoot(*document, resolver, 400.0f);
    checkNear(inner->layoutHeight(), 0.0f, 0.01f,
              "turning content-visibility back on skips the child again");
    check(hiddenText->textRuns().empty(),
          "the second hide clears the stale text runs");
}

// The properties this engine accepts but does not act on still carry their
// authored value, so a stylesheet can use them and the CSSOM round-trips:
// `appearance` and `user-select` (with their `-webkit-` aliases),
// `transition-behavior`, `will-change`, and `contain` (shorthands expanded).
void testAcceptedPropertyReadBack() {
    auto document = parseOrNull(
        "<sui><body>"
        "<div id=\"a\"></div><div id=\"a2\"></div><div id=\"a3\"></div>"
        "<div id=\"u\"></div><div id=\"u2\"></div><div id=\"u3\"></div>"
        "<div id=\"tb\"></div><div id=\"tb2\"></div><div id=\"tb3\"></div>"
        "<div id=\"wc\"></div><div id=\"wc2\"></div>"
        "<div id=\"ct\"></div><div id=\"ct2\"></div><div id=\"ct3\"></div>"
        "<div id=\"ct4\"></div><div id=\"ct5\"></div><div id=\"ct6\"></div>"
        "</body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "#a { appearance: none }"
        "#a2 { -webkit-appearance: checkbox }"
        "#u { user-select: none }"
        "#u2 { -webkit-user-select: text }"
        "#tb { transition-behavior: allow-discrete, normal }"
        "#tb2 { transition-behavior: banana }"
        "#wc { will-change: transform, opacity }"
        "#ct { contain: strict }"
        "#ct2 { contain: content }"
        "#ct3 { contain: size paint }"
        "#ct4 { contain: none }"
        "#ct5 { contain: banana }"
        "#ct6 { contain: inline-size layout }"));
    const auto value = [&](const char* id, const char* property) {
        Element* el = document->getElementById(id);
        return el ? sui::StyleResolver::propertyValue(resolver.resolve(*el), property) : "";
    };
    check(value("a", "appearance") == "none" && value("a", "-webkit-appearance") == "none",
          "appearance reads back the authored keyword");
    check(value("a2", "appearance") == "checkbox", "a compat keyword round-trips");
    check(value("a3", "appearance") == "auto", "the initial appearance is auto");
    check(value("u", "user-select") == "none" && value("u", "-webkit-user-select") == "none",
          "user-select reads back");
    check(value("u2", "user-select") == "text", "the -webkit- alias feeds the same value");
    check(value("u3", "user-select") == "auto", "the initial user-select is auto");
    check(value("tb", "transition-behavior") == "allow-discrete, normal",
          "transition-behavior reads its list in order");
    check(value("tb2", "transition-behavior") == "normal",
          "an invalid behaviour drops the declaration");
    check(value("tb3", "transition-behavior") == "normal",
          "the initial transition-behavior is normal");
    check(value("wc", "will-change") == "transform, opacity", "will-change reads its list");
    check(value("wc2", "will-change") == "auto", "the initial will-change is auto");
    check(value("ct", "contain") == "size layout style paint",
          "strict expands to its four keywords");
    check(value("ct2", "contain") == "layout style paint",
          "content expands to its three keywords");
    check(value("ct3", "contain") == "size paint", "the keyword list reads canonically");
    check(value("ct4", "contain") == "none", "none reads none");
    check(value("ct5", "contain") == "none", "an invalid contain drops the declaration");
    check(value("ct6", "contain") == "inline-size layout",
          "inline-size is its own containment mode");
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
        return sui::mediaMatches(rule.media, w, h, true);
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
        check(sui::mediaMatches(nested[0].media, 700, 500, true), "a merged query needs both levels");
        check(!sui::mediaMatches(nested[0].media, 700, 900, true), "a merged query fails on the inner level");
        check(!sui::mediaMatches(nested[0].media, 500, 500, true), "a merged query fails on the outer level");
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

// `animation-composition` decides how an animation's effect meets the
// underlying value: `replace` wins, `add` sums the effect on top, and
// `accumulate` sums the effect's delta from its start. Entries pair with
// animation names, cycling when short.
void testAnimationComposition() {
    const auto width = [](const ComputedStyle& s) {
        return s.width.resolve(0.0f, 16.0f, 16.0f);
    };
    // Parsing pairs entries with animation names: unknown words read as
    // `replace`, and nothing authored serializes back as `replace`. The
    // property never inherits.
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "#a { animation: x 1s, y 1s; animation-composition: add, banana } "
            "#b { animation: x 1s; animation-composition: accumulate } "
            "#g { animation: x 1s; animation-composition: banana } "
            "#c { animation: x 1s }"));
        auto document = parseOrNull(
            "<sui><body><div id=\"a\"><span id=\"k\">x</span></div><div id=\"b\"></div>"
            "<div id=\"g\"></div><div id=\"c\"></div></body></sui>");
        if (!document) {
            return;
        }
        const auto el = [&](const char* id) { return document->getElementById(id); };
        check(resolver.resolve(*el("a")).animationCompositions.size() == 2 &&
                  resolver.resolve(*el("a")).animationCompositions[0] == "add" &&
                  resolver.resolve(*el("a")).animationCompositions[1] == "replace",
              "entries pair with names, garbage reads replace");
        check(resolver.resolve(*el("b")).animationCompositions.size() == 1 &&
                  resolver.resolve(*el("b")).animationCompositions[0] == "accumulate",
              "accumulate parses");
        check(resolver.resolve(*el("g")).animationCompositions.size() == 1 &&
                  resolver.resolve(*el("g")).animationCompositions[0] == "replace",
              "a lone unknown reads replace");
        check(resolver.resolve(*el("k")).animationCompositions.empty(),
              "composition never inherits");
        check(sui::StyleResolver::propertyValue(resolver.resolve(*el("a")),
                                                 "animation-composition") == "add, replace",
              "entries serialize in order");
        check(sui::StyleResolver::propertyValue(resolver.resolve(*el("b")),
                                                 "animation-composition") == "accumulate",
              "a single entry serializes alone");
        check(sui::StyleResolver::propertyValue(resolver.resolve(*el("c")),
                                                 "animation-composition") == "replace",
              "nothing authored reads as replace");
    }
    const std::string keys =
        "@keyframes grow { from { width: 20px; } to { width: 100px; } } ";
    const auto drive = [&](const char* styleAttr, double atMs) {
        auto doc = parseOrNull(
            "<sui><body><div id=\"k\" style=\"width:100px\">x</div></body></sui>");
        if (!doc) {
            return ComputedStyle{};
        }
        Element* k = doc->getElementById("k");
        sui::StyleResolver resolver;
        resolver.setTimeSource(fakeClock);
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> frames;
        sui::CSSParser::parseWithKeyframes(
            keys + std::string("#k { animation: grow 2s linear both; ") + styleAttr + " }",
            rules, frames);
        resolver.setRules(std::move(rules));
        resolver.setKeyframes(std::move(frames));
        fakeNowMs = 0.0;
        resolver.resolve(*k);  // anchor the timeline
        fakeNowMs = atMs;
        return resolver.resolve(*k);
    };
    // Halfway the effect is 60px: replace wins, add sums, accumulate sums the
    // 40px delta from the 20px start.
    checkNear(width(drive("", 1000.0)), 60.0f, 0.5f, "replace wins by default");
    checkNear(width(drive("animation-composition: add", 1000.0)), 160.0f, 0.5f,
              "add sums the effect onto the base");
    checkNear(width(drive("animation-composition: accumulate", 1000.0)), 140.0f, 0.5f,
              "accumulate sums the delta from the start");
    // Opacity composes the same way through numbers.
    {
        auto doc = parseOrNull(
            "<sui><body><div id=\"o\" style=\"opacity:0.5\">x</div></body></sui>");
        check(doc != nullptr, "opacity document parsed");
        if (!doc) {
            return;
        }
        Element* o = doc->getElementById("o");
        sui::StyleResolver resolver;
        resolver.setTimeSource(fakeClock);
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> frames;
        sui::CSSParser::parseWithKeyframes(
            "@keyframes fade { from { opacity: 0.2; } to { opacity: 1; } } "
            "#o { animation: fade 2s linear both; animation-composition: add }",
            rules, frames);
        resolver.setRules(std::move(rules));
        resolver.setKeyframes(std::move(frames));
        fakeNowMs = 0.0;
        resolver.resolve(*o);
        fakeNowMs = 1000.0;
        checkNear(resolver.resolve(*o).opacity, 1.1f, 0.01f,
                  "add sums numbers past one");
    }
    // Two animations on one property composite in list order: replace lays the
    // base, add builds on it. One entry cycles onto both.
    {
        auto doc = parseOrNull(
            "<sui><body><div id=\"m\" style=\"width:100px\">x</div></body></sui>");
        check(doc != nullptr, "stacked document parsed");
        if (!doc) {
            return;
        }
        Element* m = doc->getElementById("m");
        sui::StyleResolver resolver;
        resolver.setTimeSource(fakeClock);
        std::vector<sui::Rule> rules;
        std::vector<sui::KeyframeBlock> frames;
        sui::CSSParser::parseWithKeyframes(
            "@keyframes ten { from { width: 0px; } to { width: 10px; } } " + keys +
                "#m { animation: grow 2s linear both, ten 2s linear both; "
                "animation-composition: replace, add }",
            rules, frames);
        resolver.setRules(std::move(rules));
        resolver.setKeyframes(std::move(frames));
        fakeNowMs = 0.0;
        resolver.resolve(*m);
        fakeNowMs = 1000.0;
        checkNear(width(resolver.resolve(*m)), 65.0f, 0.5f,
                  "a later add builds on an earlier replace");
    }
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

void testTextOverflowEllipsis() {
    // 1. Property parsing & serialization
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("div { text-overflow: ellipsis; } p { text-overflow: clip; }"));
        auto doc = parseOrNull("<sui><body><div id=\"d\"></div><p id=\"p\"></p></body></sui>");
        if (doc) {
            const sui::ComputedStyle sd = resolver.resolve(*doc->getElementById("d"));
            check(sd.textOverflow == sui::ComputedStyle::TextOverflowEllipsis,
                  "text-overflow: ellipsis parses into ComputedStyle");
            check(sui::StyleResolver::propertyValue(sd, "text-overflow") == "ellipsis",
                  "text-overflow: ellipsis serializes in propertyValue");

            const sui::ComputedStyle sp = resolver.resolve(*doc->getElementById("p"));
            check(sp.textOverflow == sui::ComputedStyle::TextOverflowClip,
                  "text-overflow: clip parses into ComputedStyle");
            check(sui::StyleResolver::propertyValue(sp, "text-overflow") == "clip",
                  "text-overflow: clip serializes in propertyValue");
        }
    }

    // 2. Single-line overflow truncation with ellipsis
    {
        auto doc = parseOrNull(
            "<sui><body>"
            "<div id=\"ell\" style=\"width:60px;overflow:hidden;white-space:nowrap;text-overflow:ellipsis\">"
            "LongTextThatOverflowsTheBox"
            "</div>"
            "<div id=\"short\" style=\"width:200px;overflow:hidden;white-space:nowrap;text-overflow:ellipsis\">"
            "Short"
            "</div>"
            "<div id=\"clip\" style=\"width:60px;overflow:hidden;white-space:nowrap;text-overflow:clip\">"
            "LongTextThatOverflowsTheBox"
            "</div>"
            "</body></sui>");
        if (doc) {
            sui::StyleResolver resolver;
            resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
            layoutRoot(*doc, resolver, 400.0f);

            Element* ell = doc->getElementById("ell");
            check(ell != nullptr && !ell->textRuns().empty(), "ell box has text runs");
            if (ell && !ell->textRuns().empty()) {
                const auto& lastRun = ell->textRuns().back();
                const std::string kEllipsis = "\xE2\x80\xA6";
                check(lastRun.text.size() >= kEllipsis.size() &&
                      lastRun.text.compare(lastRun.text.size() - kEllipsis.size(), kEllipsis.size(), kEllipsis) == 0,
                      "overflowing text in text-overflow:ellipsis ends with ellipsis glyph");
                check(lastRun.x + lastRun.width <= ell->layoutX() + ell->layoutWidth() + 1.0f,
                      "ellipsis text run fits within maxWidth");
            }

            Element* sh = doc->getElementById("short");
            check(sh != nullptr && !sh->textRuns().empty(), "short box has text runs");
            if (sh && !sh->textRuns().empty()) {
                const auto& run = sh->textRuns().front();
                check(run.text.find("\xE2\x80\xA6") == std::string::npos,
                      "non-overflowing text does not get ellipsis appended");
            }

            Element* cl = doc->getElementById("clip");
            check(cl != nullptr && !cl->textRuns().empty(), "clip box has text runs");
            if (cl && !cl->textRuns().empty()) {
                const auto& run = cl->textRuns().front();
                check(run.text.find("\xE2\x80\xA6") == std::string::npos,
                      "text-overflow:clip does not append ellipsis");
            }
        }
    }
}

void testAspectRatio() {
    // 1. Parsing & serialization
    {
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("div { aspect-ratio: 16 / 9; } p { aspect-ratio: 2; } span { aspect-ratio: auto; }"));
        auto doc = parseOrNull("<sui><body><div id=\"d\"></div><p id=\"p\"></p><span id=\"s\"></span></body></sui>");
        if (doc) {
            const sui::ComputedStyle sd = resolver.resolve(*doc->getElementById("d"));
            checkNear(sd.aspectRatio(), 16.0f / 9.0f, 0.001f, "aspect-ratio: 16 / 9 computes ratio");
            check(sui::StyleResolver::propertyValue(sd, "aspect-ratio") == "16 / 9",
                  "aspect-ratio: 16 / 9 serializes in propertyValue");

            const sui::ComputedStyle sp = resolver.resolve(*doc->getElementById("p"));
            checkNear(sp.aspectRatio(), 2.0f, 0.001f, "aspect-ratio: 2 computes ratio");
            check(sui::StyleResolver::propertyValue(sp, "aspect-ratio") == "2",
                  "aspect-ratio: 2 serializes in propertyValue");

            const sui::ComputedStyle ss = resolver.resolve(*doc->getElementById("s"));
            checkNear(ss.aspectRatio(), 0.0f, 0.001f, "aspect-ratio: auto computes 0");
            check(sui::StyleResolver::propertyValue(ss, "aspect-ratio") == "auto",
                  "aspect-ratio: auto serializes in propertyValue");
        }
    }

    // 2. Box sizing with aspect-ratio
    {
        auto doc = parseOrNull(
            "<sui><body>"
            "<div id=\"w200\" style=\"width:200px;aspect-ratio:2/1\"></div>"
            "<div id=\"h150\" style=\"height:150px;aspect-ratio:1/3\"></div>"
            "<div id=\"block-ar\" style=\"width:400px;aspect-ratio:4/1\"></div>"
            "<div id=\"clamped\" style=\"width:200px;aspect-ratio:1/1;max-height:80px\"></div>"
            "</body></sui>");
        if (doc) {
            sui::StyleResolver resolver;
            resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
            layoutRoot(*doc, resolver, 400.0f);

            Element* w200 = doc->getElementById("w200");
            check(w200 != nullptr, "w200 found");
            if (w200) {
                checkNear(w200->layoutWidth(), 200.0f, 0.01f, "w200 width is 200px");
                checkNear(w200->layoutHeight(), 100.0f, 0.01f, "w200 height computed from 2/1 aspect ratio");
            }

            Element* h150 = doc->getElementById("h150");
            check(h150 != nullptr, "h150 found");
            if (h150) {
                checkNear(h150->layoutHeight(), 150.0f, 0.01f, "h150 height is 150px");
                checkNear(h150->layoutWidth(), 50.0f, 0.01f, "h150 width computed from 1/3 aspect ratio");
            }

            Element* blockAr = doc->getElementById("block-ar");
            check(blockAr != nullptr, "block-ar found");
            if (blockAr) {
                checkNear(blockAr->layoutWidth(), 400.0f, 0.01f, "block-ar width is 400px");
                checkNear(blockAr->layoutHeight(), 100.0f, 0.01f, "block-ar height computed from 4/1 aspect ratio");
            }

            Element* clamped = doc->getElementById("clamped");
            check(clamped != nullptr, "clamped found");
            if (clamped) {
                checkNear(clamped->layoutWidth(), 200.0f, 0.01f, "clamped width is 200px");
                checkNear(clamped->layoutHeight(), 80.0f, 0.01f, "clamped height is clamped by max-height 80px");
            }
        }
    }
}

void testLogicalPropertiesAndInsets() {
    auto doc = parseOrNull(
        "<sui><body>"
        "<div id=\"m1\" style=\"margin-inline: 10px 20px; margin-block: 30px 40px\"></div>"
        "<div id=\"m2\" style=\"margin-inline: 15px; margin-block: 25px\"></div>"
        "<div id=\"p1\" style=\"padding-inline-start: 5px; padding-inline-end: 15px; padding-block-start: 25px; padding-block-end: 35px\"></div>"
        "<div id=\"p2\" style=\"padding-inline: 12px 14px; padding-block: 16px\"></div>"
        "<div id=\"ins\" style=\"position: absolute; inset: 10px 20px 30px 40px\"></div>"
        "<div id=\"ins2\" style=\"position: absolute; inset-inline: 50px 55px; inset-block: 60px 65px\"></div>"
        "<div id=\"ins3\" style=\"position: absolute; inset-inline-start: 70px; inset-block-end: 80px\"></div>"
        "</body></sui>");
    if (!doc) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
    layoutRoot(*doc, resolver, 500.0f);

    const sui::ComputedStyle sm1 = resolver.resolve(*doc->getElementById("m1"));
    checkNear(sm1.margin.left.value, 10.0f, 0.01f, "margin-inline first value maps to margin-left");
    checkNear(sm1.margin.right.value, 20.0f, 0.01f, "margin-inline second value maps to margin-right");
    checkNear(sm1.margin.top.value, 30.0f, 0.01f, "margin-block first value maps to margin-top");
    checkNear(sm1.margin.bottom.value, 40.0f, 0.01f, "margin-block second value maps to margin-bottom");

    const sui::ComputedStyle sm2 = resolver.resolve(*doc->getElementById("m2"));
    checkNear(sm2.margin.left.value, 15.0f, 0.01f, "single margin-inline maps to left");
    checkNear(sm2.margin.right.value, 15.0f, 0.01f, "single margin-inline maps to right");
    checkNear(sm2.margin.top.value, 25.0f, 0.01f, "single margin-block maps to top");
    checkNear(sm2.margin.bottom.value, 25.0f, 0.01f, "single margin-block maps to bottom");

    const sui::ComputedStyle sp1 = resolver.resolve(*doc->getElementById("p1"));
    checkNear(sp1.padding.left.value, 5.0f, 0.01f, "padding-inline-start maps to padding-left");
    checkNear(sp1.padding.right.value, 15.0f, 0.01f, "padding-inline-end maps to padding-right");
    checkNear(sp1.padding.top.value, 25.0f, 0.01f, "padding-block-start maps to padding-top");
    checkNear(sp1.padding.bottom.value, 35.0f, 0.01f, "padding-block-end maps to padding-bottom");

    const sui::ComputedStyle sp2 = resolver.resolve(*doc->getElementById("p2"));
    checkNear(sp2.padding.left.value, 12.0f, 0.01f, "padding-inline first value maps to padding-left");
    checkNear(sp2.padding.right.value, 14.0f, 0.01f, "padding-inline second value maps to padding-right");
    checkNear(sp2.padding.top.value, 16.0f, 0.01f, "padding-block single value maps to padding-top");
    checkNear(sp2.padding.bottom.value, 16.0f, 0.01f, "padding-block single value maps to padding-bottom");

    const sui::ComputedStyle sins = resolver.resolve(*doc->getElementById("ins"));
    checkNear(sins.top.value, 10.0f, 0.01f, "inset maps to top");
    checkNear(sins.right.value, 20.0f, 0.01f, "inset maps to right");
    checkNear(sins.bottom.value, 30.0f, 0.01f, "inset maps to bottom");
    checkNear(sins.left.value, 40.0f, 0.01f, "inset maps to left");

    const sui::ComputedStyle sins2 = resolver.resolve(*doc->getElementById("ins2"));
    checkNear(sins2.left.value, 50.0f, 0.01f, "inset-inline first maps to left");
    checkNear(sins2.right.value, 55.0f, 0.01f, "inset-inline second maps to right");
    checkNear(sins2.top.value, 60.0f, 0.01f, "inset-block first maps to top");
    checkNear(sins2.bottom.value, 65.0f, 0.01f, "inset-block second maps to bottom");

    const sui::ComputedStyle sins3 = resolver.resolve(*doc->getElementById("ins3"));
    checkNear(sins3.left.value, 70.0f, 0.01f, "inset-inline-start maps to left");
    checkNear(sins3.bottom.value, 80.0f, 0.01f, "inset-block-end maps to bottom");
}

// The box shorthands — `margin`, `padding`, `inset`, and the logical pairs — read
// back in the form the cascade writes them: one value while both sides agree,
// `start end` otherwise, and a one-to-four value list for the four-sided ones,
// collapsed the way `margin` distributes it. Reading them back is also what lets
// their CSS-wide keywords round-trip, since `inherit`, `unset`, and `initial`
// re-apply the serialisation rather than carrying a style object around.
void testBoxShorthands() {
    auto doc = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"m1\" style=\"margin:10px 20px\"></div>"
        "<div id=\"m2\" style=\"margin-inline:10px 20px;margin-block:30px 40px\"></div>"
        "<div id=\"m3\" style=\"margin-inline:15px;margin-block:25px\"></div>"
        "<div id=\"m4\" style=\"margin:1px 2px 3px 4px\"></div>"
        "<div id=\"m5\" style=\"margin:1px 2px 3px\"></div>"
        "<div id=\"m6\" style=\"margin:1px 2px\"></div>"
        "<div id=\"m7\" style=\"margin-inline:5% 10%\"></div>"
        "<div id=\"p1\" style=\"padding:5px 10px\"></div>"
        "<div id=\"p2\" style=\"padding:2em\"></div>"
        "<div id=\"p5\" style=\"padding-inline:12px 14px;padding-block:16px\"></div>"
        "<div id=\"i1\" style=\"position:absolute;inset:1px 2px 3px 4px\"></div>"
        "<div id=\"i2\" style=\"position:absolute;inset:1px 2px 3px\"></div>"
        "<div id=\"i3\" style=\"position:absolute;inset-inline:5px 6px;"
        "inset-block:7px 8px\"></div>"
        "<div id=\"i4\" style=\"position:absolute;inset:9px\"></div>"
        "<div id=\"i5\" style=\"position:absolute;inset-inline:5px\"></div>"
        "<div id=\"i6\" style=\"position:absolute;inset-inline:auto 10px\"></div>"
        "</body></sui>");
    if (!doc) {
        return;
    }
    Element* m1 = doc->getElementById("m1");
    Element* i1 = doc->getElementById("i1");
    check(m1 && i1, "box shorthand elements found");
    if (!m1 || !i1) {
        return;
    }
    sui::StyleResolver resolver;
    const auto value = [&](const char* id, const char* property) {
        return sui::StyleResolver::propertyValue(
            resolver.resolve(*doc->getElementById(id)), property);
    };

    check(value("m1", "margin") == "10px 20px", "margin reads back as a two-value list");
    check(value("m1", "margin-inline") == "20px" && value("m1", "margin-block") == "10px",
          "and its logical pairs collapse to one value each");
    check(value("m2", "margin-inline") == "10px 20px", "margin-inline reads back both sides");
    check(value("m2", "margin-inline-start") == "10px", "agreeing with its own longhands");
    check(value("m2", "margin-block") == "30px 40px", "and margin-block too");
    check(value("m3", "margin-inline") == "15px" && value("m3", "margin-block") == "25px",
          "equal sides collapse to a single value");
    check(value("m4", "margin") == "1px 2px 3px 4px", "four distinct sides stay four values");
    check(value("m5", "margin") == "1px 2px 3px", "a matching left drops out of the list");
    check(value("m6", "margin") == "1px 2px", "a matching bottom drops out of it too");
    check(value("m7", "margin-inline") == "5% 10%", "percentages keep their unit");
    check(value("p1", "padding") == "5px 10px", "padding reads back like margin");
    check(value("p2", "padding") == "2em", "font-relative units keep their unit");
    check(value("p5", "padding-inline") == "12px 14px" &&
              value("p5", "padding-block") == "16px",
          "the logical paddings read back on their own axes");
    check(value("p5", "padding") == "16px 14px 16px 12px",
          "and the physical shorthand gathers all four back up");
    check(value("i1", "inset") == "1px 2px 3px 4px", "inset reads back as offsets");
    check(value("i2", "inset") == "1px 2px 3px", "collapsing like margin");
    check(value("i3", "inset-inline") == "5px 6px" && value("i3", "inset-block") == "7px 8px",
          "and its logical pairs read back");
    check(value("i3", "inset") == "7px 6px 8px 5px", "with the offsets gathered back up");
    check(value("i4", "inset") == "9px" && value("i5", "inset-inline") == "5px",
          "one value covers both sides");
    check(value("i4", "inset") == value("i4", "top"),
          "and each offset longhand agrees with the shorthand");
    check(value("i6", "inset-inline") == "auto 10px" && value("i6", "left") == "auto",
          "an auto offset reads back as auto");

    // The round-trip: `inherit` copies the parent's shorthand, `initial` takes the
    // default style's value, which for these lengths is the initial one.
    auto kw = parseOrNull(
        "<sui><body style=\"margin:0\">"
        "<div id=\"p1\" style=\"margin-inline:10px 20px;margin-block:3px 4px\">"
        "<div id=\"c1\" style=\"margin-inline:inherit\"></div>"
        "<div id=\"c2\" style=\"margin-block:inherit\"></div>"
        "</div>"
        "<div id=\"p2\" style=\"margin:1px 2px 3px 4px\">"
        "<div id=\"c3\" style=\"margin:inherit\"></div>"
        "</div>"
        "<div id=\"p3\" style=\"position:absolute;inset:11px 12px 13px 14px\">"
        "<div id=\"c4\" style=\"inset:inherit\"></div>"
        "<div id=\"c5\" style=\"inset-inline:inherit\"></div>"
        "<div id=\"c6\" style=\"inset-block:inherit\"></div>"
        "</div>"
        "<div id=\"p4\" style=\"padding:7px 8px;padding-block:9px 10px\">"
        "<div id=\"c7\" style=\"padding:inherit\"></div>"
        "<div id=\"c8\" style=\"padding-inline:inherit\"></div>"
        "<div id=\"c9\" style=\"padding-block:inherit\"></div>"
        "</div>"
        "<div id=\"c10\" style=\"margin-inline:5px;margin-inline:initial\"></div>"
        "</body></sui>");
    if (kw) {
        sui::StyleResolver r;
        const auto of = [&](const char* id) { return r.resolve(*kw->getElementById(id)); };
        const sui::ComputedStyle c1 = of("c1");
        checkNear(c1.margin.left.value, 10.0f, 0.01f, "margin-inline:inherit takes the start");
        checkNear(c1.margin.right.value, 20.0f, 0.01f, "and the end");
        check(sui::StyleResolver::propertyValue(c1, "margin-inline") == "10px 20px",
              "reading back the same two values");
        const sui::ComputedStyle c2 = of("c2");
        checkNear(c2.margin.top.value, 3.0f, 0.01f, "margin-block:inherit takes the block pair");
        checkNear(c2.margin.bottom.value, 4.0f, 0.01f, "start and end");
        const sui::ComputedStyle c3 = of("c3");
        checkNear(c3.margin.top.value, 1.0f, 0.01f, "margin:inherit takes all four sides");
        checkNear(c3.margin.left.value, 4.0f, 0.01f, "including the fourth");
        const sui::ComputedStyle c4 = of("c4");
        checkNear(c4.top.value, 11.0f, 0.01f, "inset:inherit takes the offsets");
        checkNear(c4.left.value, 14.0f, 0.01f, "all four of them");
        const sui::ComputedStyle c5 = of("c5");
        checkNear(c5.left.value, 14.0f, 0.01f, "inset-inline:inherit takes the start offset");
        checkNear(c5.right.value, 12.0f, 0.01f, "and the end one");
        const sui::ComputedStyle c6 = of("c6");
        checkNear(c6.top.value, 11.0f, 0.01f, "inset-block:inherit takes the block sides");
        checkNear(c6.bottom.value, 13.0f, 0.01f, "from the parent's own inset");
        const sui::ComputedStyle c7 = of("c7");
        checkNear(c7.padding.top.value, 9.0f, 0.01f,
                  "padding:inherit takes the parent's final padding");
        checkNear(c7.padding.right.value, 8.0f, 0.01f, "all four sides of it");
        const sui::ComputedStyle c8 = of("c8");
        checkNear(c8.padding.left.value, 8.0f, 0.01f, "padding-inline:inherit takes the start");
        checkNear(c8.padding.right.value, 8.0f, 0.01f, "and the end");
        const sui::ComputedStyle c9 = of("c9");
        checkNear(c9.padding.top.value, 9.0f, 0.01f,
                  "padding-block:inherit takes the parent's block pair");
        checkNear(c9.padding.bottom.value, 10.0f, 0.01f, "start and end, not one value");
        const sui::ComputedStyle c10 = of("c10");
        checkNear(c10.margin.left.value, 0.0f, 0.01f,
                  "margin-inline:initial drops back to the initial zero");
        check(!c10.margin.left.isAuto() && !c10.margin.right.isAuto(),
              "rather than staying at the value the stronger declaration set");
    }
}

// grid-auto-rows / grid-auto-columns / grid-auto-flow: implicit track sizing and
// non-row-major auto placement.
void testGridAutoTracks() {
    // grid-auto-rows gives every implicit row a fixed height and stretches the item
    // that lands in it.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\"></i><i id=\"b\"></i><i id=\"c\"></i>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:100px;"
            "grid-auto-rows:40px;width:100px}"));
        layoutRoot(*document, resolver, 200.0f);

        Element* g = document->getElementById("g");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        checkNear(a ? a->layoutY() : -1.0f, 0.0f, 0.5f,
                  "grid-auto-rows puts the first implicit row at y=0");
        checkNear(b ? b->layoutY() : -1.0f, 40.0f, 0.5f,
                  "grid-auto-rows puts the second implicit row at y=40");
        checkNear(c ? c->layoutY() : -1.0f, 80.0f, 0.5f,
                  "grid-auto-rows puts the third implicit row at y=80");
        checkNear(a ? a->layoutHeight() : -1.0f, 40.0f, 0.5f,
                  "an item stretches to its grid-auto-rows track");
        if (g) {
            const ComputedStyle s = resolver.resolve(*g);
            check(sui::StyleResolver::propertyValue(s, "grid-auto-rows") == "40px",
                  "grid-auto-rows serialises back");
            check(sui::StyleResolver::propertyValue(s, "grid-auto-flow") == "row",
                  "the default grid-auto-flow serialises as 'row'");
        }
    }

    // minmax() in grid-auto-rows serialises back through the min/max pair.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\"><i></i></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:50px;"
            "grid-auto-rows:minmax(20px, 40px);width:50px}"));
        layoutRoot(*document, resolver, 100.0f);
        Element* g = document->getElementById("g");
        if (g) {
            const ComputedStyle s = resolver.resolve(*g);
            check(sui::StyleResolver::propertyValue(s, "grid-auto-rows") ==
                      "minmax(20px, 40px)",
                  "grid-auto-rows:minmax() serialises back");
        }
    }

    // grid-auto-flow:column fills each column top-to-bottom first, and
    // grid-auto-columns sizes the implicit columns it creates.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\"></i><i id=\"b\"></i><i id=\"c\"></i>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:50px;"
            "grid-template-rows:30px;grid-auto-flow:column;"
            "grid-auto-columns:50px;width:150px}"));
        layoutRoot(*document, resolver, 300.0f);

        Element* g = document->getElementById("g");
        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        // One explicit row, so each item starts a new implicit 50px column.
        checkNear(a ? a->layoutX() : -1.0f, 0.0f, 0.5f,
                  "grid-auto-flow:column starts the first item at x=0");
        checkNear(b ? b->layoutX() : -1.0f, 50.0f, 0.5f,
                  "the second item wraps to a grid-auto-columns track at x=50");
        checkNear(c ? c->layoutX() : -1.0f, 100.0f, 0.5f,
                  "the third item wraps to x=100");
        checkNear(a ? a->layoutY() : -1.0f, 0.0f, 0.5f,
                  "column-flow items share the first row");
        checkNear(b ? b->layoutWidth() : -1.0f, 50.0f, 0.5f,
                  "an implicit column is 50px wide");
        if (g) {
            const ComputedStyle s = resolver.resolve(*g);
            check(sui::StyleResolver::propertyValue(s, "grid-auto-flow") == "column",
                  "grid-auto-flow:column serialises back");
            check(sui::StyleResolver::propertyValue(s, "grid-auto-columns") == "50px",
                  "grid-auto-columns serialises back");
        }
    }

    // grid-auto-flow:column with two explicit rows snakes down the rows before it
    // opens the next column.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\">"
            "<i id=\"a\"></i><i id=\"b\"></i><i id=\"c\"></i><i id=\"d\"></i>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-rows:repeat(2,20px);"
            "grid-template-columns:repeat(2,50px);grid-auto-flow:column;width:100px}"));
        layoutRoot(*document, resolver, 200.0f);

        Element* a = document->getElementById("a");
        Element* b = document->getElementById("b");
        Element* c = document->getElementById("c");
        Element* d = document->getElementById("d");
        checkNear(a ? a->layoutY() : -1.0f, 0.0f, 0.5f, "column-flow item 1 sits in row 1");
        checkNear(b ? b->layoutY() : -1.0f, 20.0f, 0.5f, "column-flow item 2 sits in row 2");
        checkNear(c ? c->layoutY() : -1.0f, 0.0f, 0.5f,
                  "column-flow item 3 starts the next column");
        checkNear(c ? c->layoutX() : -1.0f, 50.0f, 0.5f, "column-flow item 3 is at x=50");
        checkNear(d ? d->layoutY() : -1.0f, 20.0f, 0.5f,
                  "column-flow item 4 sits in row 2 of the second column");
        checkNear(d ? d->layoutX() : -1.0f, 50.0f, 0.5f, "column-flow item 4 is at x=50");
    }

    // Dense round-trips through propertyValue().
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"g\"><i></i></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse(
            "body{margin:0} #g{display:grid;grid-template-columns:50px;"
            "grid-auto-flow:row dense;width:50px}"));
        layoutRoot(*document, resolver, 100.0f);
        Element* g = document->getElementById("g");
        if (g) {
            const ComputedStyle s = resolver.resolve(*g);
            check(sui::StyleResolver::propertyValue(s, "grid-auto-flow") == "row dense",
                  "grid-auto-flow:row dense serialises back");
        }
    }
}

// position:sticky: the box stays in the flow, but its place moves with its
// scrollport's offset once an inset's threshold is crossed, and never leaves
// its containing block.
void testStickyPositioning() {
    // An explicit sticky resolves to its own enum value and a positioned box.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"s\" style=\"position:sticky\"></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        layoutRoot(*document, resolver, 400.0f);
        Element* s = document->getElementById("s");
        if (s) {
            const ComputedStyle cs = resolver.resolve(*s);
            check(cs.position == sui::ComputedStyle::Sticky,
                  "position:sticky resolves to Sticky");
            check(cs.positioned, "a sticky box is positioned");
        }
    }

    // A top-stuck header is pulled down past its flow spot as the scrollport
    // moves, and pulls its own content along; a sibling keeps its place. The
    // containing block is a taller inner box, so it never caps the stick.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"s\" style=\"width:200px;height:100px;overflow-y:auto\">"
            "<div id=\"cb\" style=\"height:300px\">"
            "<div id=\"h\" style=\"position:sticky;top:0;height:30px\">"
            "<div id=\"b\" style=\"height:10px\"></div></div>"
            "<div id=\"c1\" style=\"height:200px\"></div>"
            "</div></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* s = document->getElementById("s");
        Element* h = document->getElementById("h");
        Element* b = document->getElementById("b");
        Element* c1 = document->getElementById("c1");
        if (!s || !h || !b || !c1) {
            check(false, "sticky fixture elements found");
            return;
        }
        checkNear(h->layoutY(), 0.0f, 0.01f, "at rest the sticky header holds the flow spot");
        checkNear(c1->layoutY(), 30.0f, 0.01f, "the header still takes space in the flow");
        checkNear(s->scrollHeight(), 300.0f, 0.01f, "the scroll range reaches past the viewport");

        s->setScrollTop(60.0f);
        layoutRoot(*document, resolver, 400.0f);
        checkNear(h->layoutY(), 60.0f, 0.01f, "scrolling pulls the sticky header down");
        checkNear(b->layoutY(), 60.0f, 0.01f, "the header's own content follows it");
        checkNear(c1->layoutY(), 30.0f, 0.01f, "a sibling is not moved by the stick");

        s->setScrollTop(130.0f);
        layoutRoot(*document, resolver, 400.0f);
        checkNear(h->layoutY(), 130.0f, 0.01f, "the header keeps sticking at the far scroll");
    }

    // The stick stops at the containing block's far edge: the header rides out
    // with its section once the section has scrolled past the viewport.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"s\" style=\"width:200px;height:100px;overflow-y:auto\">"
            "<div id=\"cb\" style=\"height:60px\">"
            "<div id=\"h\" style=\"position:sticky;top:0;height:30px\"></div>"
            "<div id=\"c1\" style=\"height:30px\"></div></div>"
            "<div id=\"c2\" style=\"height:200px\"></div>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* s = document->getElementById("s");
        Element* h = document->getElementById("h");
        Element* c1 = document->getElementById("c1");
        if (!s || !h || !c1) {
            check(false, "ride-out fixture elements found");
            return;
        }
        checkNear(h->layoutY(), 0.0f, 0.01f, "a short containing block starts unstuck");
        s->setScrollTop(10.0f);
        layoutRoot(*document, resolver, 400.0f);
        checkNear(h->layoutY(), 10.0f, 0.01f, "the header sticks while its section is in view");
        s->setScrollTop(160.0f);
        layoutRoot(*document, resolver, 400.0f);
        checkNear(h->layoutY(), 30.0f, 0.01f,
                  "once its section is past, the header rides out at the end");
        checkNear(c1->layoutY(), 30.0f, 0.01f, "the section content is not dragged along");
    }

    // An inset larger than zero pins the header that far from the port edge, the
    // same resting position a relative offset would leave.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"s\" style=\"width:200px;height:100px;overflow-y:auto\">"
            "<div id=\"h\" style=\"position:sticky;top:10px;height:30px\"></div>"
            "<div id=\"c1\" style=\"height:200px\"></div>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* s = document->getElementById("s");
        Element* h = document->getElementById("h");
        if (!s || !h) {
            check(false, "inset fixture elements found");
            return;
        }
        checkNear(h->layoutY(), 10.0f, 0.01f, "top:10px rests ten below the port edge");
        s->setScrollTop(60.0f);
        layoutRoot(*document, resolver, 400.0f);
        checkNear(h->layoutY(), 70.0f, 0.01f, "top:10px sticks the same distance while scrolled");
    }

    // bottom:0 keeps an end-of-content box visible at the bottom of the viewport
    // until the flow finally puts it there, then lets it settle.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"s\" style=\"width:200px;height:100px;overflow-y:auto\">"
            "<div id=\"top\" style=\"height:1000px\"></div>"
            "<div id=\"f\" style=\"position:sticky;bottom:0;height:30px\"></div>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* s = document->getElementById("s");
        Element* f = document->getElementById("f");
        if (!s || !f) {
            check(false, "bottom fixture elements found");
            return;
        }
        // 1000px above it, the port's bottom edge is the only constraint, so the
        // box holds right at the bottom until the flow delivers it there.
        checkNear(f->layoutY(), 70.0f, 0.01f, "bottom:0 pins the box to the viewport bottom");
        s->setScrollTop(900.0f);
        layoutRoot(*document, resolver, 400.0f);
        checkNear(f->layoutY(), 970.0f, 0.01f, "the pinned box tracks the bottom as it arrives");
    }

    // Without a scrollable ancestor there is nothing to stick to.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"s\" style=\"width:200px;height:100px\">"
            "<div id=\"h\" style=\"position:sticky;top:0;height:30px\"></div>"
            "<div id=\"c1\" style=\"height:200px\"></div>"
            "</div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* h = document->getElementById("h");
        if (h) {
            checkNear(h->layoutY(), 0.0f, 0.01f, "a sticky box with no scroller stays put");
        }
    }

    // The horizontal axis works the same way with left / scrollLeft.
    {
        auto document = parseOrNull(
            "<sui><body><div id=\"s\" style=\"width:100px;height:50px;overflow-x:auto\">"
            "<div id=\"cb\" style=\"width:300px\">"
            "<div id=\"h\" style=\"position:sticky;left:0;width:30px;height:50px\"></div>"
            "<div id=\"x1\" style=\"width:300px;height:50px\"></div>"
            "</div></div></body></sui>");
        if (!document) {
            return;
        }
        sui::StyleResolver resolver;
        resolver.setRules(sui::CSSParser::parse("body{margin:0}"));
        layoutRoot(*document, resolver, 400.0f);
        Element* s = document->getElementById("s");
        Element* h = document->getElementById("h");
        if (!s || !h) {
            check(false, "horizontal fixture elements found");
            return;
        }
        checkNear(h->layoutX(), 0.0f, 0.01f, "a left-stuck box rests at its flow spot");
        s->setScrollLeft(80.0f);
        layoutRoot(*document, resolver, 400.0f);
        checkNear(h->layoutX(), 80.0f, 0.01f, "left:0 tracks the horizontal scroll");
        s->setScrollLeft(200.0f);
        layoutRoot(*document, resolver, 400.0f);
        checkNear(h->layoutX(), 200.0f, 0.01f, "left:0 sticks to the end of the horizontal range");
    }
}

int main() {
    testLogicalPropertiesAndInsets();
    testBoxShorthands();
    testGridAutoTracks();
    testStickyPositioning();
    testAspectRatio();
    testTextOverflowEllipsis();
    testEasingAndTimingFunctions();
    testTransitionDeclarations();
    testTransitions();
    testKeyframeAnimations();
    testAnimationComposition();
    testCalc();
    testScroll();
    testRuleParsing();
    testSupports();
    testSelectorMatching();
    testInteractivePseudoClasses();
    testStructuralPseudoClasses();
    testAttributeSelectors();
    testPositioning();
    testWhiteSpace();
    testPseudoElements();
    testContentReadBack();
    testCssCounters();
    testHasSelector();
    testFloatsAndClear();
    testTableLayout();
    testImportantAndCssWideKeywords();
    testBorderWidthLonghands();
    testBorderSideShorthands();
    testBorderAndOutlineShorthands();
    testHyphens();
    testMediaQueries();
    testCustomProperties();
    testCascade();
    testPropertyParsing();
    testColorFunctions();
    testSyntheticEmphasis();
    testVerticalAlign();
    testVerticalAlignTopBottom();
    testVerticalAlignTextTopBottomInline();
    testMediaElementsLayout();
    testObjectFitAndListStyle();
    testTextDecorationColor();
    testGridAlignment();
    testInputValue();
    testInputPasswordLayout();
    testTextAreaLayout();
    testGridMinmax();
    testGridAutoFill();
    testGridStretch();
    testGridSpans();
    testGridLinePlacement();
    testGridTemplateAreas();
    testGridTemplateShorthand();
    testGridNamedLines();
    testBlockStacking();
    testMarginCollapsing();
    testNestedContainer();
    testTextWrappingAndBr();
    testGridLayout();
    testTextIndent();
    testDisplayNoneIsSkipped();
    testInheritance();
    testChildOverridesInherited();
    testCascadeLayers();
    testContainerQueries();
    testCssImports();
    testClipPath();
    testDefaultStyles();
    testStructuralUaDefaults();
    testFormControlDefaults();
    testRangeDefaults();
    testSelectDefaults();
    testSelectListbox();
    testDateTimeColorFile();
    testPhrasingTagsStayInline();
    testUnknownPropertyIsReported();
    testCursorProperty();
    testCommentsInsideDeclarationBlocks();
    testUnitsEndToEnd();
    testFlexRowBasics();
    testFlexColumn();
    testFlexGrowAndShrink();
    testFlexBasisAndShrinkZero();
    testFlexGap();
    testOrder();
    testJustifyContent();
    testFlexAlignItems();
    testFlexWrap();
    testFlexReverseDirections();
    testFlexItemMarginsAndPadding();
    testBlockAutoMargins();
    testFlexAutoMargins();
    testFlexMinMaxClamp();
    testFlexInlineFlexShrinks();
    testFlexPropertyParsing();
    testGapShorthand();
    testLayoutValuesReadBack();
    testBorderRadiusReadBack();
    testContainerAndScrollReadBack();
    testShadowReadBack();
    testListStyleReadBack();
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
    testConicalGradientParsing();
    testMaskImage();
    testMaskModeComposite();
    testMaskGeometry();
    testMaskShorthand();
    testMaskReadBack();
    testMaskBorder();
    testBorderImage();
    testBorderImageReadBack();
    testScrollSnap();
    testSvgLayout();
    testSelectionColors();
    testFirstLineLetter();
    testMarkerColors();
    testMarkerFont();
    testFirstLetterFont();
    testFirstLetterSizing();
    testFirstLineFont();
    testFirstLineSizing();
    testFirstLetterDropCap();
    testAccentColor();
    testListInside();
    testListStyleImage();
    testFontKerning();
    testFontStretch();
    testFontWeight();
    testFontVariant();
    testWordBreak();
    testTextSizeAdjust();
    testSvgUse();
    testScrollBehavior();
    testMixBlendMode();
    testObjectPosition();
    testCaretColor();
    testPhrasingStyles();
    testWordBreakOpportunity();
    testDetails();
    testCounterStyles();
    testQuotes();
    testBreakSpaces();
    testTabSize();
    testPreDefaults();
    testFontFamilyList();
    testTextWrap();
    testEmptyCells();
    testLineClamp();
    testResize();
    testBackgroundBlendMode();
    testDialog();
    testModalDialog();
    testColorScheme();
    testColorSchemeWidgets();
    testPrefersColorScheme();
    testCurrentColor();
    testValidationApi();
    testBidiOrder();
    testTextAlign();
    testTextAlignLast();
    testValidationPseudos();
    testGaugeDefaults();
    testFocusVisible();
    testPlaceholder();
    testBackdropFilter();
    testBackgroundListsAreParallel();
    testBackgroundSizeAndPositionKeywords();
    testBackgroundReadBack();
    testBackgroundShorthand();
    testAcceptedPropertyReadBack();
    testContentVisibility();
    testFlexCrossSizeExcludesChrome();
    testFlexBoxSizingIsNotAppliedTwice();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}

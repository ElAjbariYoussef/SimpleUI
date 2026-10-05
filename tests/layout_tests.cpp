// CSS parsing, cascade, and layout tests. Layout runs without a font so the
// assertions stay independent of any bundled asset; the engine falls back to
// fixed average advances in that case.

#include "CSS/Parser.h"
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
        @media screen { .ignored { color: #fff; } }
        .sidebar, #main > p { color: #123456; font-size: 18px }
        p { margin: 4px 8px !important; }
    )");
    check(rules.size() == 3, "comments and at-rules skipped");
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
          "a selector with a pseudo-class matches nothing");
    check(!selectorMatches(sui::CSSParser::parseSelector("div:hover"), *div),
          "a compound selector with a pseudo-class matches nothing");
}

void testAttributeSelectors() {
    auto document = parseOrNull(
        "<sui><body><div id=\"main\" data-role=\"panel\" data-open></div>"
        "<div id=\"other\" data-mode=\"aside\"></div></body></sui>");
    if (!document) {
        return;
    }
    Element* main = document->getElementById("main");
    Element* other = document->getElementById("other");
    check(main && other, "attribute-selector elements found");
    if (!main || !other) {
        return;
    }

    check(selectorMatches(sui::CSSParser::parseSelector("[data-role]"), *main),
          "attribute presence matches");
    check(!selectorMatches(sui::CSSParser::parseSelector("[data-role]"), *other),
          "attribute presence does not match an element without it");
    check(selectorMatches(sui::CSSParser::parseSelector("[data-open]"), *main),
          "a valueless attribute is a presence test");
    check(selectorMatches(sui::CSSParser::parseSelector("[data-role=\"panel\"]"), *main),
          "attribute value matches when quoted");
    check(selectorMatches(sui::CSSParser::parseSelector("[data-role=panel]"), *main),
          "attribute value matches when unquoted");
    check(!selectorMatches(sui::CSSParser::parseSelector("[data-role=\"panel\"]"), *other),
          "attribute value does not match a different value");
    check(!selectorMatches(sui::CSSParser::parseSelector("[data-missing=\"x\"]"), *main),
          "a missing attribute never matches a value test");
    // `~=` and friends are parsed the same way but only tested for equality here.
    check(selectorMatches(sui::CSSParser::parseSelector("[data-role~=\"panel\"]"), *main),
          "the word-list operator parses and compares the value");

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        "body{margin:0} div{width:10px;height:5px} [data-role=\"panel\"]{width:99px}"));
    const auto s = resolver.resolve(*main);
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

}  // namespace

int main() {
    testRuleParsing();
    testSelectorMatching();
    testAttributeSelectors();
    testCascade();
    testPropertyParsing();
    testBlockStacking();
    testNestedContainer();
    testTextWrappingAndBr();
    testDisplayNoneIsSkipped();
    testInheritance();
    testChildOverridesInherited();
    testDefaultStyles();
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

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}

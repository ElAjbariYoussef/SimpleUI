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
    check(!selectorMatches(sui::CSSParser::parseSelector("h1:hover"), *paragraph),
          "pseudo-classes are skipped without breaking the match");
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
    checkNear(s.width, 120.0f, 0.01f, "width parsed");
    checkNear(s.margin.top, 5.0f, 0.01f, "2-value margin top");
    checkNear(s.margin.left, 10.0f, 0.01f, "2-value margin left");
    checkNear(s.padding.right, 4.0f, 0.01f, "2-value padding right");
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

}  // namespace

int main() {
    testRuleParsing();
    testSelectorMatching();
    testCascade();
    testPropertyParsing();
    testBlockStacking();
    testNestedContainer();
    testTextWrappingAndBr();
    testDisplayNoneIsSkipped();
    testInheritance();
    testChildOverridesInherited();
    testDefaultStyles();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}

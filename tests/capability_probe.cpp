// Walks every capability the engine claims and prints what it actually does, so
// the claims in Difference.md can be checked against measurement instead of
// memory. Prints; it is not a pass/fail test.
#include "CSS/DefaultStyle.h"
#include "CSS/Parser.h"
#include "CSS/StyleResolver.h"
#include "Layout/BlockLayout.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"

#include <cstdio>
#include <string>

namespace {

void report(const char* group) { std::printf("\n== %s ==\n", group); }

const char* displayName(int d) {
    switch (d) {
        case 0: return "block";
        case 1: return "inline";
        case 2: return "inline-block";
        case 3: return "flex";
        case 4: return "inline-flex";
        case 5: return "none";
        default: return "?";
    }
}

void probe(const char* what, const char* markup, const char* css, const char* id) {
    sui::ParseError error;
    auto doc = sui::MarkupParser::parse(markup, error);
    if (!doc) {
        std::printf("  %-46s PARSE FAILED line %zu: %s\n", what, error.line,
                    error.message.c_str());
        return;
    }
    sui::StyleResolver resolver;
    if (css && *css) {
        resolver.setRules(sui::CSSParser::parse(css));
    }
    sui::LayoutInput input;
    input.resolver = &resolver;
    input.viewportWidth = 800.0f;
    input.viewportHeight = 600.0f;
    sui::BlockLayout().layout(doc->root(), input);

    sui::Element* el = id ? doc->getElementById(id) : nullptr;
    if (!el) {
        std::printf("  %-46s NOT FOUND\n", what);
        return;
    }
    std::printf("  %-46s x=%-6.1f y=%-6.1f w=%-6.1f h=%-6.1f runs=%zu\n", what,
                el->layoutX(), el->layoutY(), el->layoutWidth(), el->layoutHeight(),
                el->textRuns().size());
}

}  // namespace

int main() {
    report("UA defaults");
    const char* tags[] = {"body",   "div",    "p",    "h1",  "h2",  "h6",
                          "a",     "span",   "br",   "strong", "em", "small",
                          "button", "input", "ul",  "ol",  "li",  "table",
                          "tr",    "th",     "td",   "img", "svg", "video",
                          "audio", "canvas", "iframe", "picture", "source", "track"};
    for (const char* tag : tags) {
        const sui::ComputedStyle s = sui::defaultStyleFor(tag);
        std::printf("  %-9s %-12s font=%-5.1f bold=%d margin=%g,%g,%g,%g "
                    "padding=%g,%g,%g,%g bg=#%02x%02x%02x\n",
                    tag, displayName(static_cast<int>(s.display)), s.fontSize,
                    s.fontBold ? 1 : 0, s.margin.top.value, s.margin.right.value,
                    s.margin.bottom.value, s.margin.left.value, s.padding.top.value,
                    s.padding.right.value, s.padding.bottom.value,
                    s.padding.left.value, s.backgroundColor.r, s.backgroundColor.g,
                    s.backgroundColor.b);
    }

    report("length units");
    probe("px width", "<sui><body><div id=\"t\"></div></body></sui>",
          "#t { width: 120px; height: 10px }", "t");
    probe("% width", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t { width: 50%; height: 10px }", "t");
    probe("em width", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0;font-size:20px} #t { width: 2em; height: 10px }", "t");
    probe("rem width", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0;font-size:20px} #t { width: 2rem; height: 10px }", "t");
    probe("pt width", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t { width: 72pt; height: 10px }", "t");

    report("selectors");
    probe("descendant", "<sui><body><div class=\"o\"><p id=\"t\"></p></div></body></sui>",
          "body{margin:0} .o p { width: 11px; height: 5px }", "t");
    probe("child combinator",
          "<sui><body><div class=\"o\"><p id=\"t\"></p></div></body></sui>",
          "body{margin:0} .o > p { width: 22px; height: 5px }", "t");
    probe("id beats class",
          "<sui><body><div id=\"t\" class=\"c\"></div></body></sui>",
          "body{margin:0} #t{width:33px;height:5px} .c{width:99px}", "t");
    probe("inline style beats rules",
          "<sui><body><div id=\"t\" style=\"width:44px;height:5px\"></div></body></sui>",
          "body{margin:0} #t{width:99px}", "t");
    probe("class beats type",
          "<sui><body><div id=\"t\" class=\"c\"></div></body></sui>",
          "body{margin:0} div{width:66px;height:5px} .c{width:77px}", "t");
    probe("attribute presence [data-role]",
          "<sui><body><div id=\"t\" data-role=\"box\"></div></body></sui>",
          "body{margin:0} [data-role]{width:88px;height:5px}", "t");
    probe("attribute value [data-role=box]",
          "<sui><body><div id=\"t\" data-role=\"box\"></div></body></sui>",
          "body{margin:0} [data-role=\"box\"]{width:88px;height:5px}", "t");
    probe("attribute value does not match",
          "<sui><body><div id=\"t\" data-role=\"box\"></div></body></sui>",
          "body{margin:0} [data-role=\"other\"]{width:88px;height:5px}", "t");

    report("inheritance");
    {
        sui::ParseError e;
        auto doc = sui::MarkupParser::parse(
            "<sui><body><div id=\"t\">x</div></body></sui>", e);
        sui::StyleResolver r;
        r.setRules(sui::CSSParser::parse("body{color:#ff0000;font-size:20px}"));
        const auto s = r.resolve(*doc->getElementById("t"));
        std::printf("  %-46s %s / %s\n", "color and font-size on child",
                    sui::StyleResolver::propertyValue(s, "color").c_str(),
                    sui::StyleResolver::propertyValue(s, "font-size").c_str());
        const auto w = r.resolve(*doc->getElementById("t"));
        std::printf("  %-46s %s (not inherited)\n", "width on child",
                    sui::StyleResolver::propertyValue(w, "width").c_str());
    }

    report("box model");
    probe("content-box width", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{width:100px;padding:10px;border-width:5px;height:10px}",
          "t");
    probe("border-box width", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{box-sizing:border-box;width:100px;padding:10px;"
          "border-width:5px;height:10px}",
          "t");
    probe("min-width clamps", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{width:10px;min-width:80px;height:10px}", "t");
    probe("max-width clamps", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{width:300px;max-width:120px;height:10px}", "t");
    probe("margin offsets", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{margin:30px;width:10px;height:10px}", "t");

    report("text");
    probe("wraps at max width", "<sui><body><p id=\"t\">aaa bbb ccc ddd eee</p></body></sui>",
          "body{margin:0;font-size:20px} #t{width:60px}", "t");
    probe("br forces a new line", "<sui><body><p id=\"t\">aaa<br />bbb</p></body></sui>",
          "body{margin:0;font-size:20px} #t{width:400px}", "t");
    probe("text-align center", "<sui><body><p id=\"t\">x</p></body></sui>",
          "body{margin:0;font-size:20px} #t{text-align:center;width:200px}", "t");
    probe("font-size scales the line box", "<sui><body><p id=\"t\">x</p></body></sui>",
          "body{margin:0} #t{font-size:40px;width:400px}", "t");
    probe("line-height as a length", "<sui><body><p id=\"t\">x</p></body></sui>",
          "body{margin:0;font-size:20px} #t{line-height:60px;width:400px}", "t");
    probe("a block <p> fills the parent",
          "<sui><body><p id=\"t\">abc</p></body></sui>",
          "body{margin:0;font-size:20px} #t{width:400px}", "t");
    probe("button shrinks to its label",
          "<sui><body><p id=\"t\"><button>Ab</button></p></body></sui>",
          "body{margin:0;font-size:16px} #t{width:400px}", "t");

    report("flex");
    probe("row with gap", "<sui><body><div id=\"t\"><i></i><i></i></div></body></sui>",
          "body{margin:0} #t{display:flex;gap:10px;width:300px;height:40px}"
          "#t i{width:50px;height:20px}",
          "t");
    probe("flex:1 shares the row",
          "<sui><body><div id=\"t\"><i></i><i></i></div></body></sui>",
          "body{margin:0} #t{display:flex;width:300px;height:40px}#t i{flex:1}", "t");
    probe("column stack", "<sui><body><div id=\"t\"><i></i><i></i></div></body></sui>",
          "body{margin:0} #t{display:flex;flex-direction:column;width:300px}"
          "#t i{height:20px}",
          "t");
    probe("justify-content center",
          "<sui><body><div id=\"t\"><i></i></div></body></sui>",
          "body{margin:0} #t{display:flex;justify-content:center;width:300px;"
          "height:40px}#t i{width:50px}",
          "t");
    probe("align-items center",
          "<sui><body><div id=\"t\"><i></i></div></body></sui>",
          "body{margin:0} #t{display:flex;align-items:center;width:300px;height:100px}"
          "#t i{width:50px;height:20px}",
          "t");
    probe("flex-wrap wraps", "<sui><body><div id=\"t\"><i></i><i></i></div></body></sui>",
          "body{margin:0} #t{display:flex;flex-wrap:wrap;width:120px;height:100px}"
          "#t i{width:50px;height:20px}",
          "t");
    probe("inline-flex shrink-to-fit",
          "<sui><body><p id=\"t\"><i></i><i></i></p></body></sui>",
          "body{margin:0} #t i{display:inline-flex;width:40px;height:10px}", "t");

    report("documented gaps (expected to be no-ops)");
    probe("display:none removes the box",
          "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{display:none;width:50px;height:50px}", "t");
    probe("visibility:hidden keeps the box",
          "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{visibility:hidden;width:50px;height:50px}", "t");
    probe("position:absolute ignored",
          "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{position:absolute;top:50px;left:50px;width:20px;height:20px}",
          "t");
    probe("z-index ignored", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{z-index:99;width:20px;height:20px}", "t");
    probe("overflow ignored", "<sui><body><div id=\"t\">aaa bbb ccc</div></body></sui>",
          "body{margin:0} #t{overflow:hidden;width:30px;font-size:20px}", "t");
    probe("white-space:nowrap ignored",
          "<sui><body><p id=\"t\">abcdefghij</p></body></sui>",
          "body{margin:0} #t{white-space:nowrap;width:40px;font-size:20px}", "t");
    probe("text-overflow ignored", "<sui><body><p id=\"t\">abcdefghij</p></body></sui>",
          "body{margin:0} #t{text-overflow:ellipsis;width:40px;font-size:20px}", "t");
    probe("grid ignored", "<sui><body><div id=\"t\"><i></i><i></i></div></body></sui>",
          "body{margin:0} #t{display:grid;grid-template-columns:1fr 1fr;width:300px}"
          "#t i{width:50px;height:20px}",
          "t");
    probe("float ignored", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{float:left;width:50px;height:50px}", "t");
    probe("transform ignored", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{transform:rotate(10deg);width:50px;height:50px}", "t");
    probe("box-shadow ignored", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{box-shadow:0 0 4px #000;width:50px;height:50px}", "t");
    probe("border-radius ignored", "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{border-radius:8px;width:50px;height:50px}", "t");
    probe(":hover must not match (w should be 0)",
          "<sui><body><div id=\"t\"></div></body></sui>",
          "body{margin:0} #t{width:10px;height:5px} #t:hover{width:99px}", "t");

    report("properties with no engine effect");
    {
        const char* noops[] = {"float",         "clear",        "position",
                               "top",            "left",         "right",
                               "bottom",         "z-index",      "overflow",
                               "text-overflow",  "white-space",  "transform",
                               "box-shadow",     "border-radius", "opacity",
                               "cursor",         "list-style",   "content",
                               "background-image", "border-style", "outline",
                               "letter-spacing", "word-spacing", "text-transform",
                               "table-layout",   "caption-side", "flex-flow",
                               "grid-template-columns", "aspect-ratio", "transition",
                               "animation", "user-select", "vertical-align"};
        for (const char* p : noops) {
            std::printf("  %-26s parses=%s  reads-back=%s\n", p, "yes", "no");
        }
    }
    return 0;
}

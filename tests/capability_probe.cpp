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
    probe("grid 1fr 1fr", "<sui><body><div id=\"t\"><i></i><i></i></div></body></sui>",
          "body{margin:0} #t{display:grid;grid-template-columns:1fr 1fr;width:300px}"
          "#t i{height:20px}",
          "t");

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

    report("float/clear");
    probe("float:left indents the text",
          "<sui><body><div id=\"t\"><img id=\"f\"/>aaa bbb ccc ddd</div></body></sui>",
          "body{margin:0;font-size:20px} #t{width:200px}"
          "#f{float:left;width:60px;height:40px}",
          "f");
    probe("float:right anchors right",
          "<sui><body><div id=\"t\"><img id=\"f\"/>aaa bbb ccc</div></body></sui>",
          "body{margin:0;font-size:20px} #t{width:200px}"
          "#f{float:right;width:60px;height:40px}",
          "f");
    probe("clear:left drops a block",
          "<sui><body><div id=\"t\"><img id=\"f\"/><div id=\"c\"></div></div></body></sui>",
          "body{margin:0} #t{width:200px}#f{float:left;width:50px;height:40px}"
          "#c{clear:left;height:20px}",
          "c");

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
        // Measured, not asserted: each name goes through the real parser and the
        // real computed-value reader, so the table in Difference.md cannot drift.
        struct Case {
            const char* property;
            const char* value;
            const char* shown;  // what a browser would resolve to
        };
        const Case cases[] = {
            {"float", "left", "left"},
            {"clear", "both", "both"},
            {"position", "absolute", "absolute"},
            {"position", "relative", "relative"},
            {"top", "10px", "10px"},
            {"left", "10px", "10px"},
            {"right", "10px", "10px"},
            {"bottom", "10px", "10px"},
            {"z-index", "99", "99"},
            {"overflow", "hidden", "hidden"},
            {"text-overflow", "ellipsis", "ellipsis"},
            {"white-space", "nowrap", "nowrap"},
            {"transform", "rotate(10deg)", "rotate(10deg)"},
            {"box-shadow", "0 0 4px #000", "0 0 4px #000"},
            {"border-radius", "8px", "8px"},
            {"opacity", "0.5", "0.5"},
            {"cursor", "pointer", "pointer"},
            {"list-style", "none", "none"},
            {"content", "\"x\"", "\"x\""},
            {"background-image", "url(a.png)", "url(a.png)"},
            {"border-style", "dashed", "dashed"},
            {"outline", "none", "none"},
            {"letter-spacing", "2px", "2px"},
            {"word-spacing", "2px", "2px"},
            {"text-transform", "uppercase", "uppercase"},
            {"table-layout", "fixed", "fixed"},
            {"caption-side", "top", "top"},
            {"flex-flow", "row wrap", "row wrap"},
            {"grid-template-columns", "1fr 1fr", "1fr 1fr"},
            {"aspect-ratio", "16/9", "16/9"},
            {"transition", "all 1s", "all 1s"},
            {"animation", "x 1s", "x 1s"},
            {"user-select", "none", "none"},
            {"vertical-align", "middle", "middle"},
            {"var-test", "12px", "12px"},
        };
        sui::ParseError e;
        auto doc = sui::MarkupParser::parse("<sui><body><div id=\"t\"></div></body></sui>", e);
        for (const Case& c : cases) {
            sui::StyleResolver resolver;
            resolver.setRules(sui::CSSParser::parse(std::string("#t{") + c.property +
                                                    ":" + c.value + "}"));
            const auto style = resolver.resolve(*doc->getElementById("t"));
            const std::string read =
                sui::StyleResolver::propertyValue(style, c.property);
            // A rule that fails to parse produces no rules at all, which shows up
            // as the id selector being gone from the selector list.
            const bool ruleSurvived =
                !sui::CSSParser::parse(std::string("#t{") + c.property + ":" + c.value +
                                       "}")
                     .empty();
            std::printf("  %-22s %-14s parses=%-3s reads-back=%-8s effect=%s\n",
                        c.property, c.value, ruleSurvived ? "yes" : "NO",
                        read.empty() ? "(none)" : read.c_str(),
                        read.empty() ? "none" : "CHECK");
        }
    }
    return 0;
}

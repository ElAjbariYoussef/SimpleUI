// Minimal assertion harness; keeps tests dependency-free.

#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"

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

using sui::Document;
using sui::Element;

// The exact sample from .cache/instructions.
constexpr const char* kSample = R"(<sui>
    <head>
        <window state="maximized"
                title="Main Window"
                width="1280"
                height="720" />
        <link href="/assets/style.css" rel="stylesheet" />
    </head>
    <script href="/src/loading.cpp" /> // code loaded before the screen is drawn
    <body>
        <div class="sidebar">
            <button onclick="addone();"> Click to Add </button>
        </div>

        <div class="content">
            <p id="counter">0</p>
        </div>
    </body>
    <script href="/src/calculate.cpp" />
</sui>)";

void testParsesSample() {
    sui::ParseError error;
    auto document = sui::MarkupParser::parse(kSample, error);
    check(document != nullptr, "sample parses");
    if (!document) {
        std::printf("      line %zu: %s\n", error.line, error.message.c_str());
        return;
    }

    Element* window = document->windowElement();
    check(window != nullptr, "finds <window>");
    if (window) {
        check(window->attribute("state") == "maximized", "window state parsed");
        check(window->attribute("title") == "Main Window", "window title parsed");
        check(window->attribute("width") == "1280", "window width parsed");
        check(window->attribute("height") == "720", "window height parsed");
    }

    check(document->stylesheets().size() == 1, "one stylesheet linked");
    if (!document->stylesheets().empty()) {
        check(document->stylesheets().front().href == "/assets/style.css",
              "stylesheet href parsed");
    }

    check(document->scripts().size() == 2, "two scripts referenced");
    if (document->scripts().size() == 2) {
        check(document->scripts()[0].href == "/src/loading.cpp",
              "pre-body script ordered first");
        check(!document->scripts()[0].afterBody,
              "script before <body> is tagged pre-body");
        check(document->scripts()[1].href == "/src/calculate.cpp",
              "post-body script ordered second");
        check(document->scripts()[1].afterBody,
              "script after <body> is tagged post-body");
    }

    Element* counter = document->getElementById("counter");
    check(counter != nullptr, "getElementById finds counter");
    if (counter) {
        check(counter->textContent() == "0", "counter text content");
        check(counter->isVisual(), "counter is a visual node");
    }

    Element* body = document->querySelector("body");
    check(body != nullptr, "querySelector finds body");
    check(body && body->isVisual(), "body is a visual node");

    Element* link = document->querySelector("link");
    check(link && !link->isVisual(), "link is retained but non-visual");

    Element* sidebar = document->querySelector("div");
    check(sidebar && sidebar->classList() == "sidebar", "div class parsed");

    Element* button = document->querySelector("button");
    check(button && button->attribute("onclick") == "addone();",
          "inline onclick handler preserved");
}

void testRejectsMalformed() {
    sui::ParseError error;
    auto document = sui::MarkupParser::parse("<sui><div></sui>", error);
    check(document == nullptr, "mismatched closing tag rejected");
    check(error.line > 0, "error carries a line number");
}

void testAcceptsQuoting() {
    sui::ParseError error;
    auto document =
        sui::MarkupParser::parse("<sui><div class='x' data=n></div></sui>", error);
    check(document != nullptr, "single-quoted and bare attributes accepted");
    if (document) {
        Element* div = document->querySelector("div");
        check(div && div->classList() == "x", "single-quoted class parsed");
        check(div && div->attribute("data") == "n", "bare attribute parsed");
    }
}

void testIgnoresComments() {
    sui::ParseError error;
    auto document = sui::MarkupParser::parse(
        "<sui><!-- block --><body>// line\n</body></sui>", error);
    check(document != nullptr, "html and line comments skipped");
    if (document) {
        Element* body = document->querySelector("body");
        check(body && body->textContent().find("// line") != std::string::npos,
              "comment text retained as content");
    }
}

void testMixedContentOrder() {
    sui::ParseError error;
    auto document = sui::MarkupParser::parse(
        "<sui><body><p>before<br />after</p></body></sui>", error);
    check(document != nullptr, "mixed content parses");
    if (!document) {
        return;
    }
    Element* paragraph = document->querySelector("p");
    check(paragraph != nullptr, "p found");
    if (!paragraph) {
        return;
    }
    check(paragraph->textContent().find("before") != std::string::npos &&
              paragraph->textContent().find("after") != std::string::npos,
          "text on both sides of <br> survives");
    check(document->querySelector("br") != nullptr, "br kept as a child node");
}

// Every HTML tag is classified so a tag can never be silently both styled and
// invisible. This is the list Difference.md publishes.
// Parses markup and returns its document, or nullptr.
std::unique_ptr<Document> document_for(const std::string& markup) {
    sui::ParseError error;
    return sui::MarkupParser::parse(markup, error);
}

void testEveryTagIsClassified() {
    struct Row {
        const char* tag;
        bool visual;
    };
    // The engine's own tag plus the tags the UA stylesheet describes.
    const Row rows[] = {
        {"sui", false}, {"head", false}, {"window", false}, {"script", false},
        {"style", false}, {"link", false}, {"meta", false}, {"title", false},
        {"base", false}, {"defs", false}, {"symbol", false}, {"use", false},
        {"clipPath", false}, {"mask", false}, {"pattern", false}, {"marker", false},
        {"filter", false}, {"linearGradient", false}, {"radialGradient", false},
        {"stop", false}, {"animate", false}, {"animateTransform", false},
        {"animateMotion", false}, {"set", false}, {"foreignObject", false},
        {"desc", false}, {"metadata", false}, {"textPath", false}, {"tspan", false},
        {"tref", false}, {"colgroup", false}, {"col", false}, {"thead", false},
        {"tbody", false}, {"tfoot", false}, {"template", false}, {"noscript", false},
        {"slot", false},
        {"body", true}, {"div", true}, {"p", true}, {"h1", true}, {"h2", true},
        {"h3", true}, {"h4", true}, {"h5", true}, {"h6", true}, {"br", true},
        {"a", true}, {"span", true}, {"label", true}, {"button", true},
        {"table", true}, {"tr", true}, {"th", true}, {"td", true}, {"input", true},
        {"img", true}, {"svg", true}, {"video", true}, {"audio", true},
        {"canvas", true}, {"iframe", true}, {"picture", true}, {"source", true},
        {"track", true}, {"section", true}, {"article", true}, {"header", true},
        {"footer", true}, {"main", true}, {"nav", true}, {"aside", true},
        {"code", true}, {"em", true}, {"i", true}, {"small", true}, {"sub", true},
        {"sup", true}, {"strong", true}, {"b", true}, {"ul", true}, {"ol", true},
        {"li", true},
    };
    for (const Row& row : rows) {
        auto document =
            document_for(std::string("<sui><body><") + row.tag + " /></body></sui>");
        Element* el = document ? document->querySelector(row.tag) : nullptr;
        if (!el) {
            // A non-visual wrapper keeps its children, so querySelector finds it.
            check(false, row.tag);
            continue;
        }
        if (row.visual) {
            check(el->isVisual(), row.tag);
        } else {
            check(!el->isVisual(), row.tag);
        }
    }
}

void testAcceptsVoidElements() {
    // Every HTML void element ends at its opening tag, with or without the XML
    // slash, and text after it remains in the parent.
    const char* tags[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
    };
    for (const char* tag : tags) {
        auto document = document_for(std::string("<sui><body><") + tag +
                                     " id=\"void\"></body></sui>");
        check(document != nullptr, tag);
        check(document && document->getElementById("void") != nullptr, tag);
    }

    auto document = document_for(
        "<sui><body><p id=\"p\">before<br>after<img id=\"i\"></p></body></sui>");
    check(document != nullptr, "void tags parse without slashes");
    Element* paragraph = document ? document->getElementById("p") : nullptr;
    check(paragraph && paragraph->textContent() == "beforeafter",
          "text around a void tag stays in its parent");
    check(document && document->querySelector("br") != nullptr,
          "br is a child even without a slash");
    check(document && document->getElementById("i") != nullptr,
          "img is a child even without a slash");

    auto closed = document_for(
        "<sui><body><input id=\"i\" value=\"hi\"></input>"
        "<div id=\"d\">x</div></body></sui>");
    check(closed != nullptr, "a void end tag does not break parsing");
    Element* input = closed ? closed->getElementById("i") : nullptr;
    check(input && input->value() == "hi", "the void control keeps its value");
    check(closed && closed->getElementById("d") != nullptr,
          "parsing continues after a void end tag");
}

}  // namespace

int main() {
    testParsesSample();
    testRejectsMalformed();
    testAcceptsQuoting();
    testIgnoresComments();
    testMixedContentOrder();
    testEveryTagIsClassified();
    testAcceptsVoidElements();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
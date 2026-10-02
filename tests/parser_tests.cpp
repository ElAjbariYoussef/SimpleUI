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

    check(document->scriptHrefs().size() == 2, "two scripts referenced");
    if (document->scriptHrefs().size() == 2) {
        check(document->scriptHrefs()[0] == "/src/loading.cpp",
              "pre-body script ordered first");
        check(document->scriptHrefs()[1] == "/src/calculate.cpp",
              "post-body script ordered second");
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
        check(body && body->text().find("// line") != std::string::npos,
              "comment text retained as content");
    }
}

}  // namespace

int main() {
    testParsesSample();
    testRejectsMalformed();
    testAcceptsQuoting();
    testIgnoresComments();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
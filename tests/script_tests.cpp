// Script registry, inline handler dispatch, and the web-named DOM surface.
// Registration happens here exactly as a user script would do it, so the tests
// exercise the real macros rather than a stand-in.

#include "CSS/StyleResolver.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"
#include "SUI/Script/Script.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int callLog = 0;
std::vector<std::string> calls;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

}  // namespace

// Handlers a user script would publish.
void addone() {
    ++callLog;
    calls.emplace_back("addone");
}

void toggleHighlight() {
    ++callLog;
    calls.emplace_back("toggleHighlight");
}

void noop() {}
void loadingEntry() {}
void calculateEntry() {}

SUI_HANDLER(addone)
SUI_HANDLER(toggleHighlight)
SUI_HANDLER(noop)

SUI_SCRIPT("/src/calculate.cpp", calculateEntry)
SUI_SCRIPT("/src/loading.cpp", loadingEntry)

namespace {

using sui::Document;
using sui::Element;

// Installs a context over `document` for the duration of one check, then
// restores the previous one so tests stay independent.
class ScopedContext {
public:
    explicit ScopedContext(Document& document, sui::StyleResolver& resolver)
        : previous_(sui::script::context()) {
        context_.document = &document;
        context_.styles = &resolver;
        sui::script::setContext(&context_);
    }
    ~ScopedContext() { sui::script::setContext(previous_); }

private:
    sui::script::Context context_;
    sui::script::Context* previous_;
};

std::unique_ptr<Document> parseOrNull(const char* source) {
    sui::ParseError error;
    auto document = sui::MarkupParser::parse(source, error);
    if (!document) {
        std::printf("FAIL: could not parse document: %s\n", error.message.c_str());
        ++failures;
    }
    return document;
}

void testRegistry() {
    check(sui::script::findModule("/src/loading.cpp") != nullptr,
          "module registered under its href");
    check(sui::script::findModule("/src/missing.cpp") == nullptr,
          "unknown href resolves to nothing");
    check(sui::script::findHandler("addone") != nullptr, "handler found by name");
    check(sui::script::findHandler("nosuchfunction") == nullptr,
          "unknown handler name resolves to nothing");
    check(sui::script::registeredHrefs().size() == 2, "both modules registered");
}

void testInlineHandlerParsing() {
    callLog = 0;
    calls.clear();

    sui::script::invokeInlineHandler("addone();", nullptr);
    check(callLog == 1, "parenthesised call runs once");

    sui::script::invokeInlineHandler("addone", nullptr);
    check(callLog == 2, "bare name without parentheses also runs");

    callLog = 0;
    calls.clear();
    sui::script::invokeInlineHandler("  addone(); toggleHighlight();  ", nullptr);
    check(callLog == 2 && calls.size() == 2 && calls[0] == "addone" &&
              calls[1] == "toggleHighlight",
          "two statements run in order");

    callLog = 0;
    sui::script::invokeInlineHandler("addone();nope();", nullptr);
    check(callLog == 1, "an unknown name is skipped, known ones still run");

    callLog = 0;
    sui::script::invokeInlineHandler("   ", nullptr);
    check(callLog == 0, "blank handler does nothing");
}

void testScriptPhaseTagging() {
    auto document = parseOrNull(
        "<sui><head></head><script href=\"/src/loading.cpp\" />"
        "<body><p>hi</p></body><script href=\"/src/calculate.cpp\" /></sui>");
    if (!document) {
        return;
    }
    check(document->scripts().size() == 2, "both scripts indexed");
    if (document->scripts().size() == 2) {
        check(!document->scripts()[0].afterBody, "script above <body> is pre-body");
        check(document->scripts()[1].afterBody, "script below </body> is post-body");
    }
}

void testDocumentApi() {
    auto doc = parseOrNull(
        "<sui><body><div id=\"wrap\" class=\"box wide\"><p class=\"row\">one</p></div>"
        "<span class=\"row\">two</span></body></sui>");
    if (!doc) {
        return;
    }
    sui::StyleResolver resolver;
    ScopedContext scoped(*doc, resolver);

    check(document.getElementById("wrap") != nullptr, "getElementById finds the div");
    check(document.getElementById("nope") == nullptr, "getElementById misses cleanly");

    Element* byTag = document.querySelector("span");
    check(byTag != nullptr, "querySelector by tag");
    Element* byId = document.querySelector("#wrap");
    check(byId != nullptr, "querySelector by id");
    Element* byClass = document.querySelector(".row");
    check(byClass != nullptr, "querySelector by class");
    check(byClass != nullptr && byClass->tag() == "p",
          "querySelector returns the first in document order");
}

void testClassListApi() {
    auto document = parseOrNull("<sui><body><div id=\"d\" class=\"a b\"></div></body></sui>");
    if (!document) {
        return;
    }
    Element* d = document->getElementById("d");
    check(d != nullptr, "div found");
    if (!d) {
        return;
    }

    check(d->hasClass("a") && d->hasClass("b"), "existing classes reported");
    check(!d->hasClass("c"), "absent class reported absent");

    d->addClass("c");
    check(d->hasClass("c") && d->classList() == "a b c", "addClass appends");

    d->addClass("c");
    check(d->classList() == "a b c", "addClass is idempotent");

    d->removeClass("b");
    check(d->classList() == "a c", "removeClass drops only the named class");

    d->toggleClass("a", false);
    check(d->classList() == "c", "toggleClass off removes");

    d->toggleClass("a", true);
    check(d->classList() == "c a", "toggleClass on appends");

    d->removeClass("c");
    d->removeClass("a");
    check(d->classList().empty(), "removing every class leaves no stray spaces");
}

void testInnerTextAndValue() {
    auto document = parseOrNull(
        "<sui><body><p id=\"p\">hello <b>world</b></p>"
        "<input id=\"i\" value=\"typed\" /></body></sui>");
    if (!document) {
        return;
    }
    Element* p = document->getElementById("p");
    check(p != nullptr, "p found");
    if (!p) {
        return;
    }
    check(p->innerText() == "hello world", "innerText concatenates descendants");
    p->setInnerText("replaced");
    check(p->textContent() == "replaced", "setInnerText replaces the subtree");
    check(p->children().size() == 1 && p->children()[0]->isTextNode(),
          "setInnerText leaves exactly one text node");

    Element* input = document->getElementById("i");
    check(input != nullptr, "input found");
    if (!input) {
        return;
    }
    check(input->value() == "typed", "value reads the attribute");
    input->setValue("changed");
    check(input->value() == "changed", "setValue writes the attribute");
}

void testMutationCounter() {
    auto doc = parseOrNull("<sui><body><p id=\"p\">0</p></body></sui>");
    if (!doc) {
        return;
    }
    Element* p = doc->getElementById("p");
    if (!p) {
        check(false, "p found");
        return;
    }
    const std::uint64_t before = doc->mutationCount();
    p->setInnerText("1");
    const std::uint64_t afterText = doc->mutationCount();
    check(afterText > before, "text mutation bumps the counter");

    p->addClass("hot");
    check(doc->mutationCount() > afterText, "class mutation bumps the counter");

    // A read must not invalidate anything.
    const std::uint64_t beforeRead = doc->mutationCount();
    (void)p->classList();
    (void)p->textContent();
    check(doc->mutationCount() == beforeRead, "reads leave the counter alone");
}

void testScriptDrivenRestyle() {
    // The whole point of a script surface: change the DOM or a class, and the
    // cascade produces a different computed style with no relayout code.
    auto document = parseOrNull(
        "<sui><body><div id=\"s\" class=\"sidebar\"></div></body></sui>");
    if (!document) {
        return;
    }
    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(
        ".sidebar { background-color: #27272a }"
        ".sidebar.highlight { background-color: #3f3f46 }"));

    Element* sidebar = document->getElementById("s");
    if (!sidebar) {
        check(false, "sidebar found");
        return;
    }

    check(resolver.resolve(*sidebar).backgroundColor.r == 0x27,
          "base rule applies before the script runs");

    sidebar->toggleClass("highlight", true);
    check(resolver.resolve(*sidebar).backgroundColor.r == 0x3f,
          "the class a script added selects the more specific rule");

    sidebar->removeClass("highlight");
    check(resolver.resolve(*sidebar).backgroundColor.r == 0x27,
          "removing the class falls back to the base rule");
}

}  // namespace

int main() {
    testRegistry();
    testInlineHandlerParsing();
    testScriptPhaseTagging();
    testDocumentApi();
    testClassListApi();
    testInnerTextAndValue();
    testMutationCounter();
    testScriptDrivenRestyle();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
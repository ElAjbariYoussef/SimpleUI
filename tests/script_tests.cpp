// Script registry, inline handler dispatch, and the web-named DOM surface.
// Registration happens here exactly as a user script would do it, so the tests
// exercise the real macros rather than a stand-in.

#include "CSS/StyleResolver.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"
#include "SUI/Script/Script.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int callLog = 0;
std::vector<std::string> calls;

// Timer callbacks must be plain functions — TimerCallback is `void (*)()` — so
// the counters they touch are file-scope statics.
int timerFires = 0;
void timerCb() { ++timerFires; }
int intervalFires = 0;
void intervalCb() { ++intervalFires; }
int rafFires = 0;
void rafCb() { ++rafFires; }

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

void testInlineStyleWrites() {
    auto doc = parseOrNull(
        "<sui><body><p id=\"p\" style=\"color: #111111; font-size: 12px\">x</p></body></sui>");
    if (!doc) {
        return;
    }
    Element* p = doc->getElementById("p");
    if (!p) {
        check(false, "p found");
        return;
    }

    check(p->style().getPropertyValue("color") == "#111111", "reads a declared property");
    check(p->style().hasProperty("color"), "reports a declared property");
    check(!p->style().hasProperty("margin-top"), "reports an absent property");
    check(p->style().getPropertyValue("margin-top").empty(), "absent property reads empty");

    p->style().setProperty("color", "#ef4444");
    check(p->style().getPropertyValue("color") == "#ef4444", "setProperty replaces in place");
    check(p->attribute("style").find("font-size") != std::string_view::npos,
          "a write leaves the other declarations alone");

    p->style().setProperty("background-color", "#000000");
    check(p->style().getPropertyValue("background-color") == "#000000",
          "setProperty appends a new declaration");

    // The attribute is the single source of truth, so it stays inspectable in
    // the markup exactly as a browser's does.
    check(p->attribute("style").find("color: #ef4444") != std::string_view::npos,
          "the write is visible in the style attribute");

    const std::string removed = p->style().removeProperty("color");
    check(removed == "#ef4444", "removeProperty reports the old value");
    check(!p->style().hasProperty("color"), "removeProperty drops the property");
    check(p->style().hasProperty("font-size"), "removeProperty spares the others");
    check(p->style().removeProperty("color").empty(), "removing twice reports nothing");

    p->style().setProperty("color", "#00ff00", true);
    check(p->style().getPropertyValue("color") == "#00ff00", "an important write reads back");
    check(p->attribute("style").find("!important") != std::string_view::npos,
          "an important write records the flag");
}

void testComputedStyleReads() {
    auto doc = parseOrNull("<sui><body><p id=\"p\">x</p></body></sui>");
    if (!doc) {
        return;
    }
    Element* p = doc->getElementById("p");
    if (!p) {
        check(false, "p found");
        return;
    }

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse("#p { font-size: 24px; text-align: center }"));
    const ScopedContext scope(*doc, resolver);

    check(document.getComputedStyle(p).getPropertyValue("font-size") == "24px",
          "reads a resolved length as px");
    check(document.getComputedStyle(p).getPropertyValue("text-align") == "center",
          "reads a keyword");
    check(document.getComputedStyle(p).getPropertyValue("display") == "block",
          "reads the user-agent default when no rule set it");
    check(document.getComputedStyle(p).getPropertyValue("line-height") ==
              document.getComputedStyle(p->parent()).getPropertyValue("line-height"),
          "an unset line-height reads the inherited value, unitless");
    check(document.getComputedStyle(nullptr).getPropertyValue("color").empty(),
          "a null element reads empty instead of crashing");

    // The view re-resolves per read, so a restyle is visible immediately.
    p->style().setProperty("font-size", "30px");
    check(document.getComputedStyle(p).getPropertyValue("font-size") == "30px",
          "an inline write wins over the stylesheet and reads back");
}

void testRuntimeStyleSheet() {
    auto doc = parseOrNull("<sui><body><div id=\"d\" class=\"box\"></div></body></sui>");
    if (!doc) {
        return;
    }
    Element* d = doc->getElementById("d");
    if (!d) {
        check(false, "d found");
        return;
    }

    sui::StyleResolver resolver;
    resolver.setRules(sui::CSSParser::parse(".box { background-color: #111111 }"));
    const ScopedContext scope(*doc, resolver);

    const int before = document.styleSheets().ruleCount();
    check(before == 1, "ruleCount sees the linked stylesheet");

    const std::uint64_t generationBefore = resolver.generation();
    const int index = document.styleSheets().insertRule(".box { background-color: #222222 }");
    check(index == before, "insertRule reports the index it used");
    check(resolver.generation() > generationBefore, "insertRule marks styles stale");
    check(resolver.resolve(*d).backgroundColor.r == 0x22,
          "an injected rule beats an equal-specificity rule from a file");

    // A file rule that is genuinely more specific still wins, as CSS requires.
    resolver.addRule(sui::CSSParser::parse("#d { background-color: #333333 }").front());
    check(resolver.resolve(*d).backgroundColor.r == 0x33,
          "a more specific file rule still outranks the injected one");

    check(document.styleSheets().insertRule(".box { color: red } .box { color: blue }") == -1,
          "insertRule rejects a text holding more than one rule");
    check(document.styleSheets().insertRule("nonsense") == -1,
          "insertRule rejects text that is not a rule");
    check(document.styleSheets().ruleCount() == 3, "rejected inserts leave the count alone");

    check(document.styleSheets().deleteRule(index), "deleteRule removes the injected rule");
    check(document.styleSheets().ruleCount() == 2, "deleteRule drops the count by one");
    check(resolver.resolve(*d).backgroundColor.r == 0x33,
          "after the delete only the file rules remain");
    check(!document.styleSheets().deleteRule(99), "deleteRule rejects an out-of-range index");
    check(!document.styleSheets().deleteRule(-1), "deleteRule rejects a negative index");
}

// Timers and the animation frame: the scheduler is pumped by the event loop, so
// these exercise pumpTimers() directly the way Application::run() does. Delays
// and sleeps use generous margins because the machine's scheduler is not part of
// the contract under test.
void testTimersAndAnimationFrames() {
    timerFires = 0;
    intervalFires = 0;
    rafFires = 0;

    check(sui::script::setTimeout(nullptr, 100) == 0, "a null timeout registers nothing");
    check(sui::script::requestAnimationFrame(nullptr) == 0,
          "a null rAF registers nothing");
    check(!sui::script::pumpTimers(), "an idle scheduler pumps for free");

    sui::script::setTimeout(timerCb, 60);
    const std::uint64_t interval = sui::script::setInterval(intervalCb, 30);
    check(!sui::script::pumpTimers(), "future timeouts do not fire early");
    check(timerFires == 0 && intervalFires == 0 && rafFires == 0,
          "nothing ran ahead of time");

    // rAF is due on the very next pump, regardless of the clock.
    sui::script::requestAnimationFrame(rafCb);
    check(sui::script::pumpTimers() && rafFires == 1, "rAF fires on the first pump");
    check(!sui::script::pumpTimers() && rafFires == 1, "one rAF per arm: the next pump is free");
    sui::script::cancelAnimationFrame(sui::script::requestAnimationFrame(rafCb));
    check(!sui::script::pumpTimers() && rafFires == 1,
          "a cancelled rAF never runs");

    std::this_thread::sleep_for(std::chrono::milliseconds(90));

    check(sui::script::pumpTimers(), "a due timeout makes the pump report work");
    check(timerFires == 1, "the timeout fired exactly once");
    check(intervalFires >= 1, "the interval fired at least once per caught-up period");

    const int beforeClear = intervalFires;
    sui::script::clearInterval(interval);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    sui::script::pumpTimers();
    check(intervalFires == beforeClear, "clearInterval stops the interval");

    const int beforeClearTimeout = timerFires;
    sui::script::clearTimeout(sui::script::setTimeout(timerCb, 40));
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    check(!sui::script::pumpTimers() && timerFires == beforeClearTimeout,
          "clearTimeout stops a one-shot before it is due");

    // A zero-delay timeout is due on the next pump, fires once, and drains.
    const int beforeImmediate = timerFires;
    sui::script::setTimeout(timerCb, 0);
    check(sui::script::pumpTimers() && timerFires == beforeImmediate + 1,
          "a zero-delay timeout fires once on the next pump");
    check(!sui::script::pumpTimers(), "that fire drained the queue");

    // The clocks: monotonic performance.now() and wall-clock Date.now().
    const double tickA = sui::script::now();
    const double tickB = sui::script::now();
    check(tickA > 0.0 && tickB >= tickA, "now() is set and monotonic");
    check(performance.now() >= tickB, "performance.now() reads the same clock");
    const double epoch = sui::script::unixTimeMs();
    check(epoch > 1700000000000.0, "Date.now() is milliseconds since the epoch");
    check(Date::now() >= epoch - 1.0, "Date::now() reads the wall clock");
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
    testInlineStyleWrites();
    testComputedStyleReads();
    testRuntimeStyleSheet();
    testTimersAndAnimationFrames();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
// The script surface user code sees.
//
// SimpleUI links scripts statically rather than loading them at runtime, so a
// script is a translation unit the build already compiled into the executable.
// It claims the `href` the .sui file refers to, and the engine calls it in the
// phase the markup places it in: a <script> before <body> runs before the first
// paint, one after it runs afterwards.
//
// Naming follows web development wherever C++ allows it, so a <script> can use
// document.getElementById("counter") the same way a browser page would.
#pragma once

#include "SUI/DOM/Element.h"
#include "SUI/Script/Timers.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Application;
class Document;
class StyleResolver;

namespace script {

// Every registered entry point takes no arguments and returns nothing. Keeping
// the signature this narrow is what lets the registry stay a plain table of
// function pointers.
using EntryPoint = void (*)();

// Live handles the engine exposes while the application is running. Null before
// start and after shutdown, so scripts can tell "not ready yet" from a bug.
struct Context {
    Document* document = nullptr;
    StyleResolver* styles = nullptr;
    // The running application, or null before start and after shutdown. A page
    // reaches navigation and the rest of the host through this.
    Application* application = nullptr;

    void invalidate() noexcept { generation += 1; }

    std::uint64_t generation = 0;
};

// Set by Application for the lifetime of the event loop.
Context* context() noexcept;
void setContext(Context* ctx) noexcept;

// --- registry -------------------------------------------------------------
//
// Modules register from static initializers, before main() runs, so by the time
// the .sui file is parsed every compiled script is already known.

void registerModule(std::string_view href, EntryPoint entry);
void registerHandler(std::string_view name, EntryPoint handler);

// Module backing a <script href>, or null when the href was never compiled in.
// A null result is worth reporting: it means the .sui references a script the
// build does not contain.
EntryPoint findModule(std::string_view href);
EntryPoint findHandler(std::string_view name);

// hrefs that were compiled in, for diagnostics.
std::vector<std::string> registeredHrefs();

// Runs the entry point named by an inline attribute such as `onclick="addone();"`.
// A bare call is the supported form; anything the engine cannot resolve is logged
// and skipped rather than throwing.
void invokeInlineHandler(std::string_view attributeValue, Element* target);

// console.log / warn / error, routed through SDL_Log.
void log(std::string_view message);
void warn(std::string_view message);
void error(std::string_view message);

// getComputedStyle(element).getPropertyValue("color") — reads what the element
// actually resolved to, as a browser devtools readout would report it. The
// object is a view: it re-resolves on each read, so it stays correct after a
// restyle instead of caching a snapshot.
class ComputedStyleApi {
public:
    explicit ComputedStyleApi(Element* element) noexcept : element_(element) {}

    std::string getPropertyValue(std::string_view property) const;

private:
    Element* element_;
};

// document.styleSheets.insertRule(".hot { color: red; }") — a stylesheet a
// script builds at runtime. Rules land in the author origin after the linked
// sheets, so they win ties of specificity the same way a later <style> block
// would, while a more specific rule from a file still wins outright.
class StyleSheetApi {
public:
    // Inserts one rule and returns its index, or -1 when the text is not a
    // single well-formed rule.
    int insertRule(std::string_view cssText);
    bool deleteRule(int index);
    int ruleCount() const;
};

// document.getElementById / querySelector, plus the invalidate() a script calls
// after changing something. Layout and paint only re-run when a mutation, a
// stylesheet change, an explicit invalidate, or a resize marks the frame stale,
// so invalidate is what a script needs after touching anything the engine
// cannot observe on its own.
class DocumentApi {
public:
    Document* get() const noexcept;

    Element* getElementById(std::string_view id) const;
    Element* querySelector(std::string_view selector) const;
    void invalidate() const;

    ComputedStyleApi getComputedStyle(Element* element) const {
        return ComputedStyleApi(element);
    }
    StyleSheetApi styleSheets() const { return StyleSheetApi{}; }
};

struct ConsoleApi {
    void log(std::string_view message) const { sui::script::log(message); }
    void warn(std::string_view message) const { sui::script::warn(message); }
    void error(std::string_view message) const { sui::script::error(message); }
};

}  // namespace script
}  // namespace sui

// Web-named globals so a script reads the way a web developer expects:
// document.getElementById("counter"), console.log(...). Defined inline, so every
// script translation unit shares one instance. A translation unit that needs
// different names can define SUI_NO_WEB_GLOBALS before including this header.
#ifndef SUI_NO_WEB_GLOBALS
using sui::Document;
using sui::Element;

inline sui::script::DocumentApi document{};
inline sui::script::ConsoleApi console{};
inline sui::script::PerformanceApi performance{};

// Date.now() is not expressible in C++ — a class name cannot take the dot
// syntax — so the closest C++ spelling, Date::now(), is the one on offer.
struct Date {
    static double now() { return sui::script::unixTimeMs(); }
};

using sui::script::TimerCallback;
inline std::uint64_t setTimeout(sui::script::TimerCallback callback,
                                std::uint64_t delayMs) {
    return sui::script::setTimeout(callback, delayMs);
}
inline std::uint64_t setInterval(sui::script::TimerCallback callback,
                                 std::uint64_t periodMs) {
    return sui::script::setInterval(callback, periodMs);
}
inline void clearTimeout(std::uint64_t id) { sui::script::clearTimeout(id); }
inline void clearInterval(std::uint64_t id) { sui::script::clearInterval(id); }
inline std::uint64_t requestAnimationFrame(sui::script::TimerCallback callback) {
    return sui::script::requestAnimationFrame(callback);
}
inline void cancelAnimationFrame(std::uint64_t id) {
    sui::script::cancelAnimationFrame(id);
}
#endif

// Claims the href a <script> tag in the .sui refers to. The function name is
// used for the generated symbol, so it must be unique per script. The named
// namespace keeps the helper out of the script's own namespace; a namespace
// definition needs no trailing semicolon, so neither does the macro.
#define SUI_SCRIPT(hrefLiteral, entryFunction)                            \
    namespace suiScriptRegistration {                                    \
    struct suiScriptModule_##entryFunction {                             \
        suiScriptModule_##entryFunction() {                              \
            ::sui::script::registerModule(hrefLiteral, entryFunction);   \
        }                                                                \
    } suiScriptModuleInstance_##entryFunction;                           \
    }

// Publishes a C++ function under its own name so an inline handler such as
// `onclick="addone();"` can resolve `addone`.
#define SUI_HANDLER(entryFunction)                                       \
    namespace suiScriptRegistration {                                    \
    struct suiScriptHandler_##entryFunction {                            \
        suiScriptHandler_##entryFunction() {                             \
            ::sui::script::registerHandler(#entryFunction, entryFunction); \
        }                                                                \
    } suiScriptHandlerInstance_##entryFunction;                          \
    }

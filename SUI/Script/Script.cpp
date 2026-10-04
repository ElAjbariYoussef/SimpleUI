#include "SUI/Script/Script.h"

#include "CSS/StyleResolver.h"
#include "SUI/DOM/Document.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace sui::script {
namespace {

struct Registry {
    std::vector<std::pair<std::string, EntryPoint>> modules;
    std::vector<std::pair<std::string, EntryPoint>> handlers;
};

Registry& registry() {
    // Function-local so registration from other translation units' static
    // initializers cannot run before this object exists.
    static Registry instance;
    return instance;
}

Context* g_context = nullptr;

std::string_view trim(std::string_view s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

void logWithLevel(const char* level, std::string_view message) {
    SDL_Log("SimpleUI [%s]: %.*s", level, static_cast<int>(message.size()), message.data());
}

}  // namespace

Context* context() noexcept {
    return g_context;
}

void setContext(Context* ctx) noexcept {
    g_context = ctx;
}

void registerModule(std::string_view href, EntryPoint entry) {
    auto& modules = registry().modules;
    const auto existing = std::find_if(modules.begin(), modules.end(),
                                       [href](const auto& pair) { return pair.first == href; });
    if (existing != modules.end()) {
        existing->second = entry;
        return;
    }
    modules.emplace_back(std::string(href), entry);
}

void registerHandler(std::string_view name, EntryPoint handler) {
    if (name.empty()) {
        return;
    }
    auto& handlers = registry().handlers;
    const auto existing = std::find_if(handlers.begin(), handlers.end(),
                                       [name](const auto& pair) { return pair.first == name; });
    if (existing != handlers.end()) {
        existing->second = handler;
        return;
    }
    handlers.emplace_back(std::string(name), handler);
}

EntryPoint findModule(std::string_view href) {
    for (const auto& [registered, entry] : registry().modules) {
        if (registered == href) {
            return entry;
        }
    }
    return nullptr;
}

EntryPoint findHandler(std::string_view name) {
    for (const auto& [registered, entry] : registry().handlers) {
        if (registered == name) {
            return entry;
        }
    }
    return nullptr;
}

std::vector<std::string> registeredHrefs() {
    std::vector<std::string> out;
    out.reserve(registry().modules.size());
    for (const auto& [href, entry] : registry().modules) {
        out.push_back(href);
    }
    return out;
}

void invokeInlineHandler(std::string_view attributeValue, Element* target) {
    if (g_context && g_context->document) {
        g_context->document->setFocused(target);
    }

    std::string_view rest = trim(attributeValue);
    int invoked = 0;
    while (!rest.empty()) {
        if (rest.front() == ';' || rest.front() == ',') {
            rest.remove_prefix(1);
            rest = trim(rest);
            continue;
        }

        const std::size_t start = 0;
        std::size_t i = 0;
        if (i < rest.size() && !isIdentChar(rest[i])) {
            rest.remove_prefix(1);
            continue;
        }
        while (i < rest.size() && isIdentChar(rest[i])) {
            ++i;
        }
        const std::string_view name = rest.substr(start, i - start);

        // Accept both `name()` and a bare `name`, matching how forgiving the
        // web platform is about inline handler bodies.
        if (i < rest.size() && rest[i] == '(') {
            ++i;
            int depth = 1;
            while (i < rest.size() && depth > 0) {
                if (rest[i] == '(') ++depth;
                if (rest[i] == ')') --depth;
                ++i;
            }
        }

        if (EntryPoint handler = findHandler(name)) {
            handler();
            ++invoked;
        } else {
            warn("inline handler references unknown function '" + std::string(name) + "'");
        }

        rest = trim(rest.substr(i));
    }

    if (invoked == 0 && trim(attributeValue).empty()) {
        warn("inline handler attribute is empty");
    }
}

void log(std::string_view message) {
    logWithLevel("info", message);
}

void warn(std::string_view message) {
    logWithLevel("warn", message);
}

void error(std::string_view message) {
    logWithLevel("error", message);
}

Document* DocumentApi::get() const noexcept {
    return g_context ? g_context->document : nullptr;
}

Element* DocumentApi::getElementById(std::string_view id) const {
    Document* doc = get();
    return doc ? doc->getElementById(id) : nullptr;
}

Element* DocumentApi::querySelector(std::string_view selector) const {
    Document* doc = get();
    if (!doc) {
        return nullptr;
    }
    const std::string_view trimmed = trim(selector);
    if (trimmed.starts_with('#')) {
        return doc->getElementById(trimmed.substr(1));
    }
    if (trimmed.starts_with('.')) {
        const std::string_view wanted = trimmed.substr(1);
        Element* found = nullptr;
        doc->root()->visit([&](Element& element) {
            if (!found && !element.isTextNode() && element.hasClass(wanted)) {
                found = &element;
            }
        });
        return found;
    }
    return doc->querySelector(trimmed);
}

void DocumentApi::invalidate() const {
    if (g_context) {
        g_context->invalidate();
    }
}

std::string ComputedStyleApi::getPropertyValue(std::string_view property) const {
    if (!element_ || !g_context || !g_context->styles) {
        return {};
    }
    return StyleResolver::propertyValue(g_context->styles->resolve(*element_), property);
}

int StyleSheetApi::insertRule(std::string_view cssText) {
    if (!g_context || !g_context->styles) {
        warn("insertRule called before the stylesheet is ready");
        return -1;
    }
    const std::vector<Rule> parsed = CSSParser::parse(cssText);
    if (parsed.size() != 1) {
        warn("insertRule expects exactly one rule; got " + std::to_string(parsed.size()));
        return -1;
    }
    const int index = static_cast<int>(g_context->styles->rules().size());
    g_context->styles->addRule(parsed.front());
    return index;
}

bool StyleSheetApi::deleteRule(int index) {
    if (!g_context || !g_context->styles || index < 0) {
        return false;
    }
    const auto count = static_cast<int>(g_context->styles->rules().size());
    if (index >= count) {
        return false;
    }
    g_context->styles->eraseRule(static_cast<std::size_t>(index));
    return true;
}

int StyleSheetApi::ruleCount() const {
    return g_context && g_context->styles ? static_cast<int>(g_context->styles->rules().size())
                                          : 0;
}

}  // namespace sui::script
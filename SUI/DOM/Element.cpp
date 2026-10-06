#include "SUI/DOM/Element.h"

#include "CSS/Parser.h"
#include "SUI/DOM/Document.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace sui {
namespace {

// Non-visual tags never take part in layout or painting. Everything else does,
// including the semantic containers the UA stylesheet already describes.
bool isVisualTag(std::string_view tag) {
    static const std::vector<std::string_view> nonVisual = {
        "head",  "script", "style",  "link",   "meta",   "title",  "base",
        "sui",   "window", "root",   "template", "noscript", "defs", "symbol",
        "use",   "clipPath", "mask", "pattern", "marker", "filter",
        "linearGradient", "radialGradient", "stop", "animate", "animateTransform",
        "animateMotion", "set", "foreignObject", "desc", "metadata", "textPath",
        "tspan", "tref", "colgroup", "col", "thead", "tbody", "tfoot", "slot",
    };
    return std::find(nonVisual.begin(), nonVisual.end(), tag) == nonVisual.end();
}

}  // namespace

Element::Element(std::string tag, Document* document)
    : Object(std::string("Element:") + tag),
      tag_(std::move(tag)),
      document_(document),
      visual_(isVisualTag(tag_)) {}

Element::~Element() = default;

std::unique_ptr<Element> Element::createText(std::string data, Document* document) {
    auto node = std::make_unique<Element>(std::string(kTextTag), document);
    node->text_ = std::move(data);
    return node;
}

void Element::appendChild(std::unique_ptr<Element> child) {
    insertChild(std::move(child), children_.size());
}

void Element::insertChild(std::unique_ptr<Element> child, std::size_t index) {
    if (!child) {
        return;
    }
    child->parent_ = this;
    children_.insert(children_.begin() +
                         static_cast<std::ptrdiff_t>(std::min(index, children_.size())),
                     std::move(child));
}

std::unique_ptr<Element> Element::removeChild(std::size_t index) {
    if (index >= children_.size()) {
        return nullptr;
    }
    std::unique_ptr<Element> child = std::move(children_[index]);
    children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(index));
    child->parent_ = nullptr;
    return child;
}

std::vector<std::unique_ptr<Element>> Element::releaseChildren() {
    for (auto& child : children_) {
        child->parent_ = nullptr;
    }
    return std::move(children_);
}

bool Element::hasAttribute(std::string_view key) const {
    return attributes_.find(key) != attributes_.end();
}

std::string_view Element::attribute(std::string_view key) const {
    auto it = attributes_.find(key);
    return it == attributes_.end() ? std::string_view{} : std::string_view{it->second};
}

void Element::setAttribute(std::string key, std::string value) {
    attributes_.insert_or_assign(std::move(key), std::move(value));
    touch();
}

void Element::removeAttribute(std::string_view key) {
    auto it = attributes_.find(key);
    if (it != attributes_.end()) {
        attributes_.erase(it);
        touch();
    }
}

std::string_view Element::id() const {
    return attribute("id");
}

std::string_view Element::classList() const {
    return attribute("class");
}

bool Element::hasClass(std::string_view name) const {
    if (name.empty()) {
        return false;
    }
    const std::string_view classes = classList();
    std::size_t i = 0;
    while (i < classes.size()) {
        while (i < classes.size() && std::isspace(static_cast<unsigned char>(classes[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < classes.size() && !std::isspace(static_cast<unsigned char>(classes[i]))) {
            ++i;
        }
        if (classes.substr(start, i - start) == name) {
            return true;
        }
    }
    return false;
}

void Element::addClass(std::string_view name) {
    if (name.empty() || hasClass(name)) {
        return;
    }
    std::string updated(classList());
    if (!updated.empty()) {
        updated += ' ';
    }
    updated.append(name);
    setAttribute("class", std::move(updated));
}

void Element::removeClass(std::string_view name) {
    if (name.empty()) {
        return;
    }
    const std::string_view classes = classList();
    std::string updated;
    updated.reserve(classes.size());
    std::size_t i = 0;
    while (i < classes.size()) {
        while (i < classes.size() && std::isspace(static_cast<unsigned char>(classes[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < classes.size() && !std::isspace(static_cast<unsigned char>(classes[i]))) {
            ++i;
        }
        const std::string_view token = classes.substr(start, i - start);
        if (token.empty() || token == name) {
            continue;
        }
        if (!updated.empty()) {
            updated += ' ';
        }
        updated.append(token);
    }
    if (updated != classes) {
        setAttribute("class", std::move(updated));
    }
}

void Element::toggleClass(std::string_view name, bool enabled) {
    if (enabled) {
        addClass(name);
    } else {
        removeClass(name);
    }
}

void Element::touch() {
    if (document_) {
        document_->noteMutation();
    }
}

namespace {

// A scroll offset lives in `[0, scroll - client]`. Once the layout pass has a
// client size the range is known, and the offset is pinned into it; before that
// the raw value stands and the next pass fixes it.
float clampOffset(float value, float scrollExtent, float clientExtent) {
    if (clientExtent > 0.0f) {
        return std::min(std::max(value, 0.0f), std::max(scrollExtent - clientExtent, 0.0f));
    }
    return value;
}

}  // namespace

void Element::setScrollExtents(float clientW, float clientH, float scrollW, float scrollH,
                               bool scrollsX, bool scrollsY) noexcept {
    clientW_ = clientW;
    clientH_ = clientH;
    scrollW_ = scrollW;
    scrollH_ = scrollH;
    scrollsX_ = scrollsX;
    scrollsY_ = scrollsY;
    scrollLeft_ = scrollsX ? clampOffset(scrollLeft_, scrollW_, clientW_) : 0.0f;
    scrollTop_ = scrollsY ? clampOffset(scrollTop_, scrollH_, clientH_) : 0.0f;
}

float Element::scrollLeft() const noexcept {
    return scrollsX_ ? clampOffset(scrollLeft_, scrollW_, clientW_) : 0.0f;
}

void Element::setScrollLeft(float v) {
    scrollLeft_ = scrollsX_ ? clampOffset(v, scrollW_, clientW_) : 0.0f;
    touch();
}

float Element::scrollTop() const noexcept {
    return scrollsY_ ? clampOffset(scrollTop_, scrollH_, clientH_) : 0.0f;
}

void Element::setScrollTop(float v) {
    scrollTop_ = scrollsY_ ? clampOffset(v, scrollH_, clientH_) : 0.0f;
    touch();
}

void Element::setText(std::string text) {
    text_ = std::move(text);
    touch();
}

void Element::collectText(std::string& out) const {
    out += text_;
    for (const auto& child : children_) {
        child->collectText(out);
    }
}

std::string Element::textContent() const {
    std::string out;
    collectText(out);
    return out;
}

void Element::setTextContent(std::string text) {
    children_.clear();
    touch();
    if (isTextNode()) {
        text_ = std::move(text);
        return;
    }
    if (text.empty()) {
        return;
    }
    appendChild(createText(std::move(text), document_));
}

std::string Element::value() const {
    return std::string(attribute("value"));
}

void Element::setValue(std::string text) {
    setAttribute("value", std::move(text));
}

InlineStyle::InlineStyle(Element& owner) noexcept : owner_(owner) {}

void InlineStyle::setProperty(std::string property, std::string value, bool important) {
    if (property.empty()) {
        return;
    }
    std::vector<Declaration> declarations = parseDeclarations(owner_.attribute("style"));
    bool replaced = false;
    for (auto& declaration : declarations) {
        if (declaration.property == property) {
            declaration.value = std::move(value);
            declaration.important = important;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        declarations.push_back(Declaration{std::move(property), std::move(value), important});
    }

    // Keep the property order stable so repeated writes produce a stable
    // attribute string.
    std::string serialized;
    for (const auto& declaration : declarations) {
        if (!serialized.empty()) {
            serialized += ' ';
        }
        serialized += declaration.property;
        serialized += ": ";
        serialized += declaration.value;
        if (declaration.important) {
            serialized += " !important";
        }
        serialized += ';';
    }
    owner_.setAttribute("style", std::move(serialized));
}

std::string InlineStyle::getPropertyValue(std::string_view property) const {
    // Later declarations win, matching the order the cascade applies them in.
    std::string found;
    for (const auto& declaration : parseDeclarations(owner_.attribute("style"))) {
        if (declaration.property == property) {
            found = declaration.value;
        }
    }
    return found;
}

bool InlineStyle::hasProperty(std::string_view property) const {
    for (const auto& declaration : parseDeclarations(owner_.attribute("style"))) {
        if (declaration.property == property) {
            return true;
        }
    }
    return false;
}

std::string InlineStyle::removeProperty(std::string_view property) {
    const std::vector<Declaration> declarations = parseDeclarations(owner_.attribute("style"));
    std::string removed;
    std::vector<Declaration> kept;
    kept.reserve(declarations.size());
    for (const auto& declaration : declarations) {
        if (declaration.property == property) {
            removed = declaration.value;
            continue;
        }
        kept.push_back(declaration);
    }
    if (removed.empty()) {
        return removed;
    }

    std::string serialized;
    for (const auto& declaration : kept) {
        if (!serialized.empty()) {
            serialized += ' ';
        }
        serialized += declaration.property;
        serialized += ": ";
        serialized += declaration.value;
        if (declaration.important) {
            serialized += " !important";
        }
        serialized += ';';
    }
    owner_.setAttribute("style", std::move(serialized));
    return removed;
}

void Element::visit(const std::function<void(Element&)>& fn) {
    fn(*this);
    for (const auto& child : children_) {
        child->visit(fn);
    }
}

}  // namespace sui

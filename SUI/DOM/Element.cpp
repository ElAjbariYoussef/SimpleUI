#include "SUI/DOM/Element.h"

#include "SUI/DOM/Document.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace sui {
namespace {

bool isVisualTag(std::string_view tag) {
    // Visual/front-end tags (HTML-like subset for SimpleUI)
    return tag == "body" || tag == "div" || tag == "p" || tag == "h1" ||
           tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" ||
           tag == "h6" || tag == "br" || tag == "button" || tag == "a" ||
           tag == "table" || tag == "tr" || tag == "th" || tag == "td" ||
           tag == "span" || tag == "label" || tag == "input" || tag == "img" ||
           tag == "svg" || tag == "video" || tag == "audio" || tag == "canvas" ||
           tag == "iframe" || tag == "picture" || tag == "source" || tag == "track";
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

void Element::visit(const std::function<void(Element&)>& fn) {
    fn(*this);
    for (const auto& child : children_) {
        child->visit(fn);
    }
}

}  // namespace sui

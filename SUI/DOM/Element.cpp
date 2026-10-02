#include "SUI/DOM/Element.h"

#include "SUI/DOM/Document.h"

#include <utility>

namespace sui {
namespace {

bool isVisualTag(std::string_view tag) {
    return tag == "body" || tag == "div" || tag == "p" || tag == "h1" ||
           tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" ||
           tag == "h6" || tag == "br" || tag == "button" || tag == "a" ||
           tag == "table" || tag == "tr" || tag == "th" || tag == "td" ||
           tag == "span" || tag == "label" || tag == "input";
}

}  // namespace

Element::Element(std::string tag, Document* document)
    : Object(std::string("Element:") + tag),
      tag_(std::move(tag)),
      document_(document),
      visual_(isVisualTag(tag_)) {}

void Element::appendChild(std::unique_ptr<Element> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
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
}

std::string_view Element::id() const {
    return attribute("id");
}

std::string_view Element::classList() const {
    return attribute("class");
}

void Element::setText(std::string text) {
    text_ = std::move(text);
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

void Element::visit(const std::function<void(Element&)>& fn) {
    fn(*this);
    for (const auto& child : children_) {
        child->visit(fn);
    }
}

}  // namespace sui
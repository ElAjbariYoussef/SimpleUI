#include "SUI/DOM/Document.h"

#include "SUI/DOM/Element.h"

#include <algorithm>
#include <utility>

namespace sui {

Document::Document() : root_(std::make_unique<Element>("document", this)) {}

void Document::setRoot(std::unique_ptr<Element> root) {
    root_ = std::move(root);
    index();
}

Element* Document::getElementById(std::string_view id) {
    if (!root_ || id.empty()) {
        return nullptr;
    }
    Element* found = nullptr;
    root_->visit([&](Element& el) {
        if (!found && el.id() == id) {
            found = &el;
        }
    });
    return found;
}

Element* Document::querySelector(std::string_view tag) {
    if (!root_) {
        return nullptr;
    }
    Element* found = nullptr;
    root_->visit([&](Element& el) {
        if (!found && el.tag() == tag) {
            found = &el;
        }
    });
    return found;
}

Element* Document::windowElement() {
    if (!root_) {
        return nullptr;
    }
    Element* found = nullptr;
    root_->visit([&](Element& el) {
        if (!found && el.tag() == "window") {
            found = &el;
        }
    });
    return found;
}

void Document::index() {
    stylesheets_.clear();
    scriptHrefs_.clear();
    if (!root_) {
        return;
    }
    root_->visit([&](Element& el) {
        if (el.tag() == "link" && el.attribute("rel") == "stylesheet") {
            stylesheets_.push_back(StylesheetLink{std::string(el.attribute("href"))});
        } else if (el.tag() == "script" && !el.attribute("href").empty()) {
            scriptHrefs_.emplace_back(el.attribute("href"));
        }
    });
}

}  // namespace sui
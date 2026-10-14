#include "SUI/DOM/Element.h"

#include "CSS/Parser.h"
#include "SUI/DOM/Document.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <regex>
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
    // A textarea's value is its text content, not an attribute.
    if (tag_ == "textarea") {
        return textContent();
    }
    // A select's value is the selected option's `value`, or its text when the
    // attribute is absent; with no options there is no value at all.
    if (tag_ == "select") {
        Element* selected = selectedOption();
        if (!selected) {
            return {};
        }
        if (selected->hasAttribute("value")) {
            return std::string(selected->attribute("value"));
        }
        return selected->optionLabel();
    }
    return std::string(attribute("value"));
}

void Element::setValue(std::string text) {
    if (tag_ == "textarea") {
        setTextContent(std::move(text));
        return;
    }
    // Selecting by value: the first option whose value-or-text matches takes
    // `selected`, the rest drop it. No match clears every mark, and value()
    // then falls back to the first option.
    if (tag_ == "select") {
        for (Element* option : selectOptions()) {
            const std::string optionValue = option->hasAttribute("value")
                                                ? std::string(option->attribute("value"))
                                                : option->optionLabel();
            if (optionValue == text) {
                option->setAttribute("selected", "");
            } else {
                option->removeAttribute("selected");
            }
        }
        return;
    }
    setAttribute("value", std::move(text));
}

std::string Element::optionLabel() const {
    const std::string text = textContent();
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    bool started = false;
    for (char c : text) {
        const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if (space) {
            pendingSpace = started;
            continue;
        }
        if (pendingSpace) {
            out += ' ';
            pendingSpace = false;
        }
        out += c;
        started = true;
    }
    return out;
}

void Element::collectOptions(std::vector<Element*>& out) const {
    for (const auto& child : children_) {
        if (!child || child->isTextNode()) {
            continue;
        }
        if (child->tag() == "option") {
            out.push_back(child.get());
        } else {
            // An <optgroup> (or anything else the markup nests) groups options
            // without being one itself.
            child->collectOptions(out);
        }
    }
}

std::vector<Element*> Element::selectOptions() const {
    std::vector<Element*> options;
    collectOptions(options);
    return options;
}

Element* Element::selectedOption() const {
    std::vector<Element*> options;
    collectOptions(options);
    for (Element* option : options) {
        if (option->hasAttribute("selected")) {
            return option;
        }
    }
    return options.empty() ? nullptr : options[0];
}

std::string Element::fileName() const {
    const std::string value(attribute("value"));
    const std::size_t slash = value.find_last_of("/\\");
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

// A decimal attribute or a fallback when it is missing or unparsable, so
// markup can never smuggle a NaN into a gauge.
float decimalAttribute(const Element& el, std::string_view key, float fallback) {
    const std::string_view raw = el.attribute(key);
    std::size_t first = raw.find_first_not_of(" \t\n\r\f");
    if (first == std::string_view::npos) {
        return fallback;
    }
    std::size_t last = raw.find_last_not_of(" \t\n\r\f");
    const std::string text(raw.substr(first, last - first + 1));
    char* stop = nullptr;
    const double value = std::strtod(text.c_str(), &stop);
    if (!stop || *stop != '\0') {
        return fallback;
    }
    return static_cast<float>(value);
}

bool Element::gaugeIndeterminate() const {
    return tag_ == "progress" && !hasAttribute("value");
}

float Element::gaugeFraction() const {
    // Progress defaults to a 0..1 scale; a meter without bounds does too, and
    // a meter without a value reads 0. A max that does not exceed its min
    // resets both, like the constraint rules.
    float minimum = tag_ == "meter" ? decimalAttribute(*this, "min", 0.0f) : 0.0f;
    float maximum = tag_ == "meter" ? decimalAttribute(*this, "max", 1.0f) : 1.0f;
    if (tag_ == "progress") {
        minimum = 0.0f;
        maximum = decimalAttribute(*this, "max", 1.0f);
        if (maximum <= 0.0f) {
            maximum = 1.0f;
        }
    } else if (maximum <= minimum) {
        minimum = 0.0f;
        maximum = 1.0f;
    }
    const float span = maximum - minimum;
    if (span <= 0.0f) {
        return 0.0f;
    }
    float value = minimum;
    if (tag_ == "meter") {
        value = decimalAttribute(*this, "value", 0.0f);
    } else if (hasAttribute("value")) {
        value = decimalAttribute(*this, "value", 0.0f);
    } else {
        return 0.0f;
    }
    if (value <= minimum) {
        return 0.0f;
    }
    if (value >= maximum) {
        return 1.0f;
    }
    return (value - minimum) / span;
}

int Element::gaugeZone() const {
    // Green beside the optimum, yellow inside the low/high band but off the
    // optimum side, red outside the band. An optimum under low likes low
    // values; over high likes high ones.
    const float value = decimalAttribute(*this, "value", 0.0f);
    const float low = decimalAttribute(*this, "low", 0.0f);
    const float high = decimalAttribute(*this, "high", 1.0f);
    const float optimum = decimalAttribute(*this, "optimum", 0.5f);
    if (optimum < low) {
        return value <= low ? 0 : (value <= high ? 1 : 2);
    }
    if (optimum > high) {
        return value >= high ? 0 : (value >= low ? 1 : 2);
    }
    if (value >= low && value <= high) {
        return 0;
    }
    return 2;
}

bool Element::willValidate() const {
    if (tag_ != "input" && tag_ != "select" && tag_ != "textarea") {
        return false;
    }
    if (hasAttribute("disabled")) {
        return false;
    }
    if (tag_ == "input") {
        const std::string_view type = attribute("type");
        if (type == "hidden" || type == "submit" || type == "button" ||
            type == "reset" || type == "image") {
            return false;
        }
    }
    return true;
}

Element::ValidityState Element::validity() const {
    ValidityState state;
    if (!willValidate()) {
        return state;
    }
    if (!customValidity_.empty()) {
        state.customError = true;
        return state;
    }
    const bool required = hasAttribute("required");
    const std::string value = this->value();
    const std::string_view inputType = tag_ == "input" ? attribute("type") : std::string_view{};
    if (tag_ == "select" || tag_ == "textarea") {
        if (required && value.empty()) {
            state.valueMissing = true;
            return state;
        }
        if (tag_ == "select" || value.empty()) {
            return state;
        }
        // A textarea still answers its minlength.
        if (hasAttribute("minlength")) {
            const std::string_view raw = attribute("minlength");
            const std::size_t first = raw.find_first_not_of(" \t\n\r\f");
            if (first != std::string_view::npos) {
                const std::size_t last = raw.find_last_not_of(" \t\n\r\f");
                const std::string digits(raw.substr(first, last - first + 1));
                char* stop = nullptr;
                const long least = std::strtol(digits.c_str(), &stop, 10);
                if (stop && *stop == '\0' && least > 0) {
                    std::size_t codepoints = 0;
                    for (unsigned char c : value) {
                        if ((c & 0xC0) != 0x80) {
                            ++codepoints;
                        }
                    }
                    if (codepoints < static_cast<std::size_t>(least)) {
                        state.tooShort = true;
                    }
                }
            }
        }
        return state;
    } else {
        const std::string_view type = attribute("type");
        if (type == "checkbox") {
            if (required && !hasAttribute("checked")) {
                state.valueMissing = true;
            }
            return state;
        }
        if (type == "radio") {
            if (required && !hasAttribute("checked")) {
                // The group is document-wide, like the click handler's: any
                // checked same-name radio validates the whole group.
                bool shared = false;
                const std::string_view name = attribute("name");
                const Element* root = this;
                while (root->parent_) {
                    root = root->parent_;
                }
                std::vector<const Element*> stack{root};
                while (!stack.empty() && !shared) {
                    const Element* node = stack.back();
                    stack.pop_back();
                    if (node != this && node->tag_ == "input" &&
                        node->attribute("type") == "radio" &&
                        node->attribute("name") == name &&
                        node->hasAttribute("checked")) {
                        shared = true;
                        break;
                    }
                    for (const auto& child : node->children_) {
                        if (child && !child->isTextNode()) {
                            stack.push_back(child.get());
                        }
                    }
                }
                if (!shared) {
                    state.valueMissing = true;
                }
            }
            return state;
        }
        if (value.empty()) {
            if (required) {
                state.valueMissing = true;
            }
            return state;
        }
        if (type == "number" || type == "range") {
            char* stop = nullptr;
            const double number = std::strtod(value.c_str(), &stop);
            if (!stop || *stop != '\0') {
                state.typeMismatch = true;
                return state;
            }
            const std::string_view minText = attribute("min");
            const std::string_view maxText = attribute("max");
            if (!minText.empty()) {
                char* end = nullptr;
                const double minimum = std::strtod(std::string(minText).c_str(), &end);
                if (end && *end == '\0' && number < minimum) {
                    state.rangeUnderflow = true;
                    return state;
                }
            }
            if (!maxText.empty()) {
                char* end = nullptr;
                const double maximum = std::strtod(std::string(maxText).c_str(), &end);
                if (end && *end == '\0' && number > maximum) {
                    state.rangeOverflow = true;
                    return state;
                }
            }
            return state;
        }
        if (type == "email") {
            const std::size_t at = value.find('@');
            if (at == std::string::npos || at == 0 || at + 1 >= value.size() ||
                value.find('@', at + 1) != std::string::npos) {
                state.typeMismatch = true;
                return state;
            }
        }
    }
    // Length and pattern judge non-empty text-like values (selects return
    // above; radios and checkboxes never reach here).
    if ((inputType == "text" || inputType == "search" || inputType == "url" ||
         inputType == "tel" || inputType == "email" || inputType == "password") &&
        hasAttribute("minlength")) {
        const std::string_view raw = attribute("minlength");
        const std::size_t first = raw.find_first_not_of(" \t\n\r\f");
        if (first != std::string_view::npos) {
            const std::size_t last = raw.find_last_not_of(" \t\n\r\f");
            const std::string digits(raw.substr(first, last - first + 1));
            char* stop = nullptr;
            const long least = std::strtol(digits.c_str(), &stop, 10);
            if (stop && *stop == '\0' && least > 0) {
                std::size_t codepoints = 0;
                for (unsigned char c : value) {
                    if ((c & 0xC0) != 0x80) {
                        ++codepoints;
                    }
                }
                if (codepoints < static_cast<std::size_t>(least)) {
                    state.tooShort = true;
                    return state;
                }
            }
        }
    }
    if ((inputType == "text" || inputType == "search" || inputType == "url" ||
         inputType == "tel" || inputType == "email" || inputType == "password") &&
        hasAttribute("pattern")) {
        try {
            if (!std::regex_match(value, std::regex(std::string(attribute("pattern"))))) {
                state.patternMismatch = true;
                return state;
            }
        } catch (const std::regex_error&) {
        }
    }
    return state;
}

bool Element::checkValidity() const {
    return validity().valid();
}

bool Element::reportValidity() {
    if (checkValidity()) {
        return true;
    }
    // Focus the failure for the author to find; the application picks up
    // visible focus from its own modality on the next event.
    if (document_) {
        document_->setFocused(this);
    }
    return false;
}

std::string Element::validationMessage() const {
    const ValidityState state = validity();
    if (state.customError) {
        return customValidity_;
    }
    if (state.valueMissing) {
        return "Please fill out this field.";
    }
    if (state.typeMismatch) {
        return "Please match the requested format.";
    }
    if (state.patternMismatch) {
        return "Please match the requested format.";
    }
    if (state.tooShort) {
        return "Please lengthen this text.";
    }
    if (state.rangeUnderflow) {
        return "Value is below the minimum.";
    }
    if (state.rangeOverflow) {
        return "Value is above the maximum.";
    }
    return {};
}

void Element::setCustomValidity(std::string message) {
    customValidity_ = std::move(message);
}

int Element::listboxRows() const {
    if (tag_ != "select") {
        return 0;
    }
    const bool multi = hasAttribute("multiple");
    int size = multi ? 4 : 1;
    const std::string_view raw = attribute("size");
    if (!raw.empty()) {
        int parsed = 0;
        bool ok = true;
        for (char c : raw) {
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            parsed = parsed * 10 + (c - '0');
            if (parsed > 1000) {
                break;
            }
        }
        if (ok && parsed > 0) {
            size = parsed > 1000 ? 1000 : parsed;
        } else if (!ok) {
            size = multi ? 4 : 1;
        }
    }
    if (multi) {
        return size < 1 ? 1 : size;
    }
    return size > 1 ? size : 0;
}

Element* Element::owningSelect() const {
    const Element* ancestor = parent_;
    if (ancestor && ancestor->tag_ == "optgroup") {
        ancestor = ancestor->parent_;
    }
    return ancestor && ancestor->tag_ == "select" ? const_cast<Element*>(ancestor)
                                                 : nullptr;
}

Element* Element::datalistFor() const {
    // Only text-like inputs take suggestions; buttons, checkboxes, and hidden
    // fields never pop one up.
    if (tag_ != "input") {
        return nullptr;
    }
    const std::string_view type = attribute("type");
    if (!type.empty() && type != "text" && type != "search" && type != "url" &&
        type != "tel" && type != "email") {
        return nullptr;
    }
    const std::string_view id = attribute("list");
    if (id.empty() || !document_) {
        return nullptr;
    }
    Element* named = document_->getElementById(id);
    return named && named->tag_ == "datalist" ? named : nullptr;
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

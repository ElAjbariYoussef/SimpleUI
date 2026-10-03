#include "CSS/StyleResolver.h"

#include "SUI/DOM/Element.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sui {
namespace {

std::string_view trim(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

// Splits a value on whitespace into up to `maxParts` numbers.
std::vector<float> parseNumberList(std::string_view value) {
    std::vector<float> out;
    std::size_t i = 0;
    while (i < value.size() && out.size() < 4) {
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        if (i == start) {
            break;
        }
        out.push_back(std::strtof(std::string(value.substr(start, i - start)).c_str(),
                                  nullptr));
    }
    return out;
}

float parseLength(std::string_view value) {
    const std::vector<float> parts = parseNumberList(value);
    return parts.empty() ? 0.0f : parts.front();
}

bool parseBool(std::string_view value) {
    return value != "none" && value != "hidden" && value != "0" && value != "false";
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool parseHexColor(std::string_view value, SDL_Color& out) {
    std::string_view hex = trim(value);
    if (hex.size() > 4 && hex[0] == '#') {
        hex.remove_prefix(1);
    } else if (hex.size() > 3 && hex.substr(0, 3) == "rgb") {
        const std::size_t open = hex.find('(');
        if (open == std::string_view::npos) {
            return false;
        }
        const std::vector<float> parts =
            parseNumberList(hex.substr(open + 1, hex.size() - open - 2));
        if (parts.size() < 3) {
            return false;
        }
        out = {static_cast<Uint8>(std::clamp(parts[0], 0.0f, 255.0f)),
               static_cast<Uint8>(std::clamp(parts[1], 0.0f, 255.0f)),
               static_cast<Uint8>(std::clamp(parts[2], 0.0f, 255.0f)), 255};
        return true;
    } else {
        return false;
    }

    if (hex.size() == 3 || hex.size() == 4) {
        int channels[4] = {0, 0, 0, 15};
        for (std::size_t k = 0; k < hex.size(); ++k) {
            channels[k] = hexDigit(hex[k]);
            if (channels[k] < 0) {
                return false;
            }
            channels[k] *= 17;  // #abc -> #aabbcc
        }
        out = {static_cast<Uint8>(channels[0]), static_cast<Uint8>(channels[1]),
               static_cast<Uint8>(channels[2]), static_cast<Uint8>(channels[3])};
        return true;
    }
    if (hex.size() == 6 || hex.size() == 8) {
        int channels[4] = {0, 0, 0, 255};
        for (std::size_t k = 0; k < hex.size(); k += 2) {
            const int hi = hexDigit(hex[k]);
            const int lo = hexDigit(hex[k + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            channels[k / 2] = (hi << 4) | lo;
        }
        out = {static_cast<Uint8>(channels[0]), static_cast<Uint8>(channels[1]),
               static_cast<Uint8>(channels[2]), static_cast<Uint8>(channels[3])};
        return true;
    }
    return false;
}

void applyBoxEdges(std::string_view value, BoxEdge& edges) {
    const std::vector<float> parts = parseNumberList(value);
    switch (parts.size()) {
        case 1:
            edges = {parts[0], parts[0], parts[0], parts[0]};
            break;
        case 2:
            edges = {parts[0], parts[1], parts[0], parts[1]};
            break;
        case 3:
            edges = {parts[0], parts[1], parts[2], parts[1]};
            break;
        case 4:
            edges = {parts[0], parts[1], parts[2], parts[3]};
            break;
        default:
            break;
    }
}

void applyDeclaration(ComputedStyle& s, const Declaration& decl) {
    const std::string& property = decl.property;
    const std::string_view value = trim(decl.value);

    if (property == "display") {
        if (value == "none") {
            s.display = ComputedStyle::None;
        } else if (value == "inline") {
            s.display = ComputedStyle::Inline;
        } else if (value == "inline-block" || value == "inline-flex") {
            s.display = ComputedStyle::InlineBlock;
        } else {
            s.display = ComputedStyle::Block;
        }
    } else if (property == "width") {
        s.width = parseLength(value);
    } else if (property == "height") {
        s.height = parseLength(value);
    } else if (property == "min-width") {
        s.minWidth = parseLength(value);
    } else if (property == "min-height") {
        s.minHeight = parseLength(value);
    } else if (property == "max-width") {
        s.maxWidth = parseLength(value);
    } else if (property == "max-height") {
        s.maxHeight = parseLength(value);
    } else if (property == "margin") {
        applyBoxEdges(value, s.margin);
    } else if (property == "margin-top") {
        s.margin.top = parseLength(value);
    } else if (property == "margin-right") {
        s.margin.right = parseLength(value);
    } else if (property == "margin-bottom") {
        s.margin.bottom = parseLength(value);
    } else if (property == "margin-left") {
        s.margin.left = parseLength(value);
    } else if (property == "padding") {
        applyBoxEdges(value, s.padding);
    } else if (property == "padding-top") {
        s.padding.top = parseLength(value);
    } else if (property == "padding-right") {
        s.padding.right = parseLength(value);
    } else if (property == "padding-left") {
        s.padding.left = parseLength(value);
    } else if (property == "padding-bottom") {
        s.padding.bottom = parseLength(value);
    } else if (property == "border-width") {
        applyBoxEdges(value, s.borderWidth);
    } else if (property == "border") {
        // Only the width component is meaningful here; colour is ignored.
        applyBoxEdges(value, s.borderWidth);
    } else if (property == "background-color" || property == "background") {
        SDL_Color color{};
        if (parseHexColor(value, color)) {
            s.backgroundColor = color;
        }
    } else if (property == "color") {
        SDL_Color color{};
        if (parseHexColor(value, color)) {
            s.color = color;
        }
    } else if (property == "border-color") {
        SDL_Color color{};
        if (parseHexColor(value, color)) {
            s.borderColor = color;
        }
    } else if (property == "font-size") {
        const float size = parseLength(value);
        if (size > 0.0f) {
            s.fontSize = size;
        }
    } else if (property == "font-weight") {
        s.fontBold = value == "bold" || value == "bolder" ||
                     std::strtof(std::string(value).c_str(), nullptr) >= 600.0f;
    } else if (property == "font-family") {
        std::string_view family = value;
        if (const std::size_t comma = family.find(','); comma != std::string_view::npos) {
            family = family.substr(0, comma);
        }
        if (!family.empty() && (family.front() == '"' || family.front() == '\'')) {
            family.remove_prefix(1);
        }
        if (!family.empty() && (family.back() == '"' || family.back() == '\'')) {
            family.remove_suffix(1);
        }
        s.fontFamily = std::string(trim(family));
    } else if (property == "line-height") {
        const float numeric = std::strtof(std::string(value).c_str(), nullptr);
        if (numeric > 0.0f) {
            s.lineHeight = numeric;
        }
    } else if (property == "text-align") {
        if (value == "center") {
            s.textAlign = ComputedStyle::Center;
        } else if (value == "right") {
            s.textAlign = ComputedStyle::Right;
        } else if (value == "justify") {
            s.textAlign = ComputedStyle::Justify;
        } else {
            s.textAlign = ComputedStyle::Left;
        }
    } else if (property == "visibility") {
        s.visible = parseBool(value);
    }
}

// Inline `width`/`height` attributes are a SimpleUI convenience on top of CSS.
void applyDimensionAttributes(ComputedStyle& s, Element& element) {
    for (const auto* key : {"width", "height"}) {
        if (!element.hasAttribute(key)) {
            continue;
        }
        const std::string_view raw = trim(element.attribute(key));
        if (raw.empty()) {
            continue;
        }
        const float v = parseLength(raw);
        if (v <= 0.0f) {
            continue;
        }
        if (std::string_view(key) == "width") {
            s.width = v;
        } else {
            s.height = v;
        }
    }
}

int specificity(const SimpleSelector& selector) {
    int ids = 0;
    int classes = 0;
    int types = 0;
    for (const auto& part : selector.parts) {
        switch (part.kind) {
            case Selector::Kind::Id:
                ++ids;
                break;
            case Selector::Kind::Class:
                ++classes;
                break;
            case Selector::Kind::Type:
                ++types;
                break;
            case Selector::Kind::Universal:
                break;
        }
    }
    return ids * 10000 + classes * 100 + types;
}

bool hasClass(const Element& element, std::string_view wanted) {
    const std::string_view classes = element.classList();
    std::size_t i = 0;
    while (i < classes.size()) {
        while (i < classes.size() && std::isspace(static_cast<unsigned char>(classes[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < classes.size() && !std::isspace(static_cast<unsigned char>(classes[i]))) {
            ++i;
        }
        if (classes.substr(start, i - start) == wanted) {
            return true;
        }
    }
    return false;
}

bool compoundMatches(const Selector& part, const Element& element) {
    switch (part.kind) {
        case Selector::Kind::Type:
            return element.tag() == part.value;
        case Selector::Kind::Class:
            return hasClass(element, part.value);
        case Selector::Kind::Id:
            return element.id() == part.value;
        case Selector::Kind::Universal:
            return true;
    }
    return false;
}

}  // namespace

void StyleResolver::setRules(std::vector<Rule> rules) {
    rules_ = std::move(rules);
    std::stable_sort(rules_.begin(), rules_.end(), [](const Rule& a, const Rule& b) {
        return specificity(a.selector) < specificity(b.selector);
    });
}

void StyleResolver::addRule(Rule rule) {
    const int rank = specificity(rule.selector);
    const auto pos = std::upper_bound(
        rules_.begin(), rules_.end(), rank,
        [](int value, const Rule& existing) { return value < specificity(existing.selector); });
    rules_.insert(pos, std::move(rule));
}

bool selectorMatches(const SimpleSelector& selector, const Element& element) {
    const auto& parts = selector.parts;
    if (parts.empty()) {
        return false;
    }
    // Match right to left so descendant combinators can walk ancestors.
    std::size_t partIndex = parts.size() - 1;
    const Element* current = &element;
    while (true) {
        if (!compoundMatches(parts[partIndex], *current)) {
            return false;
        }
        if (partIndex == 0) {
            return true;
        }
        const Combinator link = partIndex - 1 < selector.combinators.size()
                                    ? selector.combinators[partIndex - 1]
                                    : Combinator::None;
        if (link == Combinator::None) {
            partIndex -= 1;  // same element, e.g. "div.primary"
            continue;
        }
        if (link == Combinator::Child) {
            current = current->parent();
            if (!current) {
                return false;
            }
            partIndex -= 1;
            continue;
        }
        const Element* ancestor = current->parent();
        while (ancestor && !compoundMatches(parts[partIndex - 1], *ancestor)) {
            ancestor = ancestor->parent();
        }
        if (!ancestor) {
            return false;
        }
        current = ancestor;
        partIndex -= 1;
    }
}

ComputedStyle StyleResolver::resolve(Element& element) const {
    // Inheritance starts from the closest styled ancestor. The document root
    // and non-visual wrappers still carry inheritable values downwards.
    ComputedStyle parent;
    if (Element* p = element.parent()) {
        parent = resolve(*p);
    }
    return resolveWithParent(element, parent);
}

ComputedStyle StyleResolver::resolveWithParent(Element& element,
                                               const ComputedStyle& parent) const {
    ComputedStyle style = defaultStyleFor(element.tag());
    inheritInto(style, parent);

    // Author rules, weakest first, so later/specific rules overwrite.
    for (const auto& rule : rules_) {
        if (!selectorMatches(rule.selector, element)) {
            continue;
        }
        for (const auto& decl : rule.declarations) {
            applyDeclaration(style, decl);
        }
    }

    if (element.hasAttribute("style")) {
        for (const auto& decl : parseDeclarations(element.attribute("style"))) {
            applyDeclaration(style, decl);
        }
    }

    applyDimensionAttributes(style, element);
    return style;
}

}  // namespace sui

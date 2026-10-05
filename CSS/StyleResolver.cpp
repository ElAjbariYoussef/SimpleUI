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

// Renders a float without a trailing ".000000" and without the trailing zeros a
// to_string would leave, so a script reading fontSize gets "16" not "16.000000".
std::string unitless(float value) {
    std::string text = std::to_string(value);
    if (const std::size_t dot = text.find('.'); dot != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text;
}

// Splits a value on whitespace, keeping the pieces as views into `value`. Used by
// the shorthands that need to look at tokens rather than only their numbers.
std::vector<std::string_view> splitOnSpaces(std::string_view value) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        if (i > start) {
            out.push_back(value.substr(start, i - start));
        }
    }
    return out;
}

float parseNumber(std::string_view value) {
    return std::strtof(std::string(value).c_str(), nullptr);
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

// Parses one CSS length, keeping the unit so layout can resolve it against the
// containing block. `auto` and `none` both come back as Auto, which is what the
// width/height/max-* properties mean by them.
Length parseLength(std::string_view value) {
    const std::string_view text = trim(value);
    if (text.empty() || text == "auto" || text == "none") {
        return Length{};
    }

    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        ++i;
    }
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) ||
                               text[i] == '.')) {
        ++i;
    }
    if (i == 0) {
        return Length{};
    }
    const float number =
        std::strtof(std::string(text.substr(0, i)).c_str(), nullptr);

    const std::string_view unit = trim(text.substr(i));
    if (unit.empty() || unit == "px") {
        return Length{number};
    }
    if (unit == "%") {
        return Length{Length::Percent, number};
    }
    if (unit == "em") {
        return Length{Length::Em, number};
    }
    if (unit == "rem") {
        return Length{Length::Rem, number};
    }
    // pt is the one other unit worth accepting, since desktop designs use it.
    if (unit == "pt") {
        return Length{number * (96.0f / 72.0f)};
    }
    return Length{};
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

// Splits on whitespace and parses each piece as a length, so `margin: 5px 10%`
// keeps both units rather than flattening them to pixels.
std::vector<Length> parseLengthList(std::string_view value) {
    std::vector<Length> out;
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
        out.push_back(parseLength(value.substr(start, i - start)));
    }
    return out;
}

void applyBoxEdges(std::string_view value, BoxEdge& edges) {
    const std::vector<Length> parts = parseLengthList(value);
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

const char* justifyName(ComputedStyle::FlexAlign v) {
    switch (v) {
        case ComputedStyle::AlignStart: return "flex-start";
        case ComputedStyle::AlignEnd: return "flex-end";
        case ComputedStyle::AlignCenter: return "center";
        case ComputedStyle::AlignSpaceBetween: return "space-between";
        case ComputedStyle::AlignSpaceAround: return "space-around";
        case ComputedStyle::AlignSpaceEvenly: return "space-evenly";
        default: return "stretch";
    }
}

const char* alignName(ComputedStyle::FlexAlign v) {
    switch (v) {
        case ComputedStyle::AlignStretch: return "stretch";
        case ComputedStyle::AlignStart: return "flex-start";
        case ComputedStyle::AlignEnd: return "flex-end";
        case ComputedStyle::AlignCenter: return "center";
        case ComputedStyle::AlignBaseline: return "baseline";
        default: return "normal";
    }
}

// `content: "x"` and `content: "a" "b"` concatenate their string arguments.
// `none` and `normal` mean "no generated box", which an empty result expresses.
std::string parseContent(std::string_view v) {
    std::string out;
    std::size_t i = 0;
    while (i < v.size()) {
        const char quote = v[i];
        if (quote != '"' && quote != '\'') {
            ++i;
            continue;
        }
        ++i;
        const std::size_t start = i;
        while (i < v.size() && v[i] != quote) {
            ++i;
        }
        out.append(v.substr(start, i - start));
        if (i < v.size()) {
            ++i;  // the closing quote
        }
    }
    return out;
}

ComputedStyle::Overflow parseOverflow(std::string_view v) {
    if (v == "hidden" || v == "clip") {
        return ComputedStyle::Hidden;
    }
    if (v == "scroll" || v == "auto") {
        return ComputedStyle::Scroll;
    }
    return ComputedStyle::Visible;
}

ComputedStyle::FlexAlign parseJustify(std::string_view v) {
    if (v == "flex-end" || v == "end" || v == "right") {
        return ComputedStyle::AlignEnd;
    }
    if (v == "center") {
        return ComputedStyle::AlignCenter;
    }
    if (v == "space-between") {
        return ComputedStyle::AlignSpaceBetween;
    }
    if (v == "space-around") {
        return ComputedStyle::AlignSpaceAround;
    }
    if (v == "space-evenly") {
        return ComputedStyle::AlignSpaceEvenly;
    }
    return ComputedStyle::AlignStart;
}

ComputedStyle::FlexAlign parseAlign(std::string_view v) {
    if (v == "flex-start" || v == "start" || v == "left") {
        return ComputedStyle::AlignStart;
    }
    if (v == "flex-end" || v == "end" || v == "right") {
        return ComputedStyle::AlignEnd;
    }
    if (v == "center") {
        return ComputedStyle::AlignCenter;
    }
    if (v == "baseline") {
        return ComputedStyle::AlignBaseline;
    }
    return ComputedStyle::AlignStretch;
}

// `flex` is a shorthand over grow, shrink and basis, where the one-value form
// `flex: 1` means `1 1 0%` and anything numeric other than 0 sets shrink to 0.
void applyFlexShorthand(ComputedStyle& s, std::string_view value, float rootFontSize) {
    const std::string_view text = trim(value);
    const std::vector<std::string_view> parts = splitOnSpaces(text);
    s.flex = ComputedStyle::FlexFactors{};

    // The keywords are checked first: they are single tokens, so the one-number
    // form would otherwise read them as zero.
    if (text == "auto") {
        s.flex.grow = 1.0f;
        s.flex.shrink = 1.0f;
        s.flex.basis = Length(Length::Auto, 0.0f);
        return;
    }
    if (text == "none") {
        s.flex.grow = 0.0f;
        s.flex.shrink = 0.0f;
        s.flex.basis = Length{};
        return;
    }
    if (text == "initial") {
        s.flex = ComputedStyle::FlexFactors{};
        return;
    }
    if (parts.empty()) {
        return;
    }

    if (parts.size() == 1) {
        // One number is `flex: <grow> 1 0%`: the item may shrink, and its size comes
        // only from sharing free space.
        const float n = parseNumber(parts[0]);
        s.flex.grow = n;
        s.flex.shrink = 1.0f;
        s.flex.basis = Length(Length::Percent, 0.0f);
        return;
    }
    if (parts.size() >= 3) {
        s.flex.grow = parseNumber(parts[0]);
        s.flex.shrink = parseNumber(parts[1]);
        s.flex.basis = parseLength(parts[2]);
        return;
    }
    // Two values: CSS reads the first as both factors and the second as the basis,
    // so `flex: 1 100px` grows freely but keeps a 100px starting size.
    s.flex.grow = parseNumber(parts[0]);
    s.flex.shrink = parseNumber(parts[0]);
    s.flex.basis = parseLength(parts[1]);
    (void)rootFontSize;
}

void applyDeclaration(ComputedStyle& s, const Declaration& decl, float rootFontSize) {
    const std::string& property = decl.property;
    const std::string_view value = trim(decl.value);

    if (property == "display") {
        if (value == "none") {
            s.display = ComputedStyle::None;
        } else if (value == "inline") {
            s.display = ComputedStyle::Inline;
        } else if (value == "inline-block") {
            s.display = ComputedStyle::InlineBlock;
        } else if (value == "flex") {
            s.display = ComputedStyle::Flex;
        } else if (value == "inline-flex") {
            s.display = ComputedStyle::InlineFlex;
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
    } else if (property == "flex-direction") {
        if (value == "row-reverse") {
            s.flexDirection = ComputedStyle::RowReverse;
        } else if (value == "column") {
            s.flexDirection = ComputedStyle::Column;
        } else if (value == "column-reverse") {
            s.flexDirection = ComputedStyle::ColumnReverse;
        } else {
            s.flexDirection = ComputedStyle::Row;
        }
    } else if (property == "flex-wrap") {
        if (value == "wrap") {
            s.flexWrap = ComputedStyle::Wrap;
        } else if (value == "wrap-reverse") {
            s.flexWrap = ComputedStyle::WrapReverse;
        } else {
            s.flexWrap = ComputedStyle::NoWrap;
        }
    } else if (property == "justify-content") {
        s.justifyContent = parseJustify(value);
    } else if (property == "align-items") {
        s.alignItems = parseAlign(value);
    } else if (property == "align-self") {
        s.alignSelf = parseAlign(value);
    } else if (property == "align-content") {
        s.alignContent = parseAlign(value);
    } else if (property == "gap" || property == "grid-gap") {
        const Length g = parseLength(value);
        const float px = g.resolve(0.0f, s.fontSize, rootFontSize);
        s.gap = px;
        s.rowGap = px;
        s.columnGap = px;
    } else if (property == "row-gap") {
        s.rowGap = parseLength(value).resolve(0.0f, s.fontSize, rootFontSize);
    } else if (property == "column-gap") {
        s.columnGap = parseLength(value).resolve(0.0f, s.fontSize, rootFontSize);
    } else if (property == "flex") {
        applyFlexShorthand(s, value, rootFontSize);
    } else if (property == "flex-grow") {
        s.flex.grow = parseNumber(value);
    } else if (property == "flex-shrink") {
        s.flex.shrink = parseNumber(value);
    } else if (property == "flex-basis") {
        s.flex.basis = parseLength(value);
    } else if (property == "order") {
        // Only `order` above zero changes anything this engine does, and it is
        // applied at layout time, so nothing is stored here yet.
    } else if (property == "content") {
        s.content = parseContent(value);
    } else if (property == "position") {
        if (value == "relative") {
            s.position = ComputedStyle::Relative;
        } else if (value == "absolute") {
            s.position = ComputedStyle::Absolute;
        } else if (value == "fixed") {
            s.position = ComputedStyle::Fixed;
        } else {
            s.position = ComputedStyle::Static;
        }
        // Only a box that is out of flow joins the paint order.
        s.positioned = s.position != ComputedStyle::Static;
    } else if (property == "top") {
        s.top = parseLength(value);
    } else if (property == "right") {
        s.right = parseLength(value);
    } else if (property == "bottom") {
        s.bottom = parseLength(value);
    } else if (property == "left") {
        s.left = parseLength(value);
    } else if (property == "z-index") {
        s.zIndex = static_cast<int>(parseNumber(value));
    } else if (property == "overflow") {
        s.overflowX = s.overflowY = parseOverflow(value);
    } else if (property == "overflow-x") {
        s.overflowX = parseOverflow(value);
    } else if (property == "overflow-y") {
        s.overflowY = parseOverflow(value);
    } else if (property == "white-space") {
        if (value == "nowrap") {
            s.whiteSpace = ComputedStyle::WrapNone;
        } else if (value == "pre") {
            s.whiteSpace = ComputedStyle::WrapPre;
        } else if (value == "pre-wrap") {
            s.whiteSpace = ComputedStyle::WrapPreWrap;
        } else {
            s.whiteSpace = ComputedStyle::WrapText;
        }
    } else if (property == "box-sizing") {
        s.boxSizing =
            value == "border-box" ? BoxSizing::BorderBox : BoxSizing::ContentBox;
    } else if (property == "font-size") {
        // Font size is one of the few lengths style resolution can finish on its
        // own: em, rem and % are all relative to font sizes, not to a box.
        const Length length = parseLength(value);
        if (!length.isAuto()) {
            const float size = length.resolve(s.fontSize, s.fontSize, rootFontSize);
            if (size > 0.0f) {
                s.fontSize = size;
            }
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
        // Stored as a multiplier, so a length has to become a ratio against this
        // element's font size — which is what CSS computes it to as well. A bare
        // number is already a ratio and must not be divided.
        const std::string_view text = trim(value);
        if (text == "normal") {
            s.lineHeight = 1.2f;
        } else {
            const Length length = parseLength(text);
            const bool unitless =
                !text.empty() && (std::isdigit(static_cast<unsigned char>(text.back())) ||
                                  text.back() == '.');
            const float numeric =
                length.isAuto() ? 0.0f : length.resolve(s.fontSize, s.fontSize, rootFontSize);
            const float ratio = unitless ? numeric : numeric / s.fontSize;
            if (ratio > 0.0f) {
                s.lineHeight = ratio;
            }
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
        const Length v = parseLength(raw);
        if (v.isAuto()) {
            continue;
        }
        if (std::string_view(key) == "width") {
            s.width = v;
        } else {
            s.height = v;
        }
    }
}

int selectorRank(const SimpleSelector& selector) {
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
            case Selector::Kind::Attribute:
                // CSS counts an attribute selector with the class group.
                ++classes;
                break;
            case Selector::Kind::Universal:
                break;
            case Selector::Kind::PseudoElement:
                // CSS counts a pseudo-element with the type group, so `li::before`
                // and `li` have the same specificity.
                ++types;
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
        case Selector::Kind::PseudoElement:
            return element.isPseudoElement() && element.tag() == part.value;
        case Selector::Kind::Attribute: {
            if (!element.hasAttribute(part.value)) {
                return false;
            }
            // An empty `attributeValue` is the presence test `[data-role]`.
            return part.attributeValue.empty() ||
                   element.attribute(part.value) == part.attributeValue;
        }
    }
    return false;
}

}  // namespace

int StyleResolver::specificityOf(const SimpleSelector& selector) {
    return selectorRank(selector);
}

void StyleResolver::setRules(std::vector<Rule> rules) {
    rules_ = std::move(rules);
    std::stable_sort(rules_.begin(), rules_.end(), [](const Rule& a, const Rule& b) {
        return selectorRank(a.selector) < selectorRank(b.selector);
    });
    ++generation_;
}

void StyleResolver::addRule(Rule rule) {
    const int rank = selectorRank(rule.selector);
    const auto pos = std::upper_bound(
        rules_.begin(), rules_.end(), rank,
        [](int value, const Rule& existing) { return value < selectorRank(existing.selector); });
    rules_.insert(pos, std::move(rule));
    ++generation_;
}

void StyleResolver::eraseRule(std::size_t index) {
    if (index >= rules_.size()) {
        return;
    }
    rules_.erase(rules_.begin() + static_cast<std::ptrdiff_t>(index));
    ++generation_;
}

std::string StyleResolver::propertyValue(const ComputedStyle& style,
                                         std::string_view property) {
    auto number = [](float value) {
        return unitless(value) + "px";
    };
    auto color = [](const SDL_Color& c) {
        if (c.a == 0) {
            return std::string("transparent");
        }
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", c.r, c.g, c.b);
        return std::string(buffer);
    };
    // A computed length keeps whatever unit the author wrote, because that is the
    // only reading that can be turned back into CSS without losing meaning.
    auto length = [&](const Length& l, const char* autoKeyword = "0px") {
        if (l.isAuto()) {
            return std::string(autoKeyword);
        }
        const char* suffix = l.unit == Length::Px        ? "px"
                             : l.unit == Length::Percent ? "%"
                             : l.unit == Length::Em      ? "em"
                                                          : "rem";
        return unitless(l.value) + suffix;
    };

    if (property == "display") {
        switch (style.display) {
            case ComputedStyle::Block: return "block";
            case ComputedStyle::Inline: return "inline";
            case ComputedStyle::InlineBlock: return "inline-block";
            case ComputedStyle::Flex: return "flex";
            case ComputedStyle::InlineFlex: return "inline-flex";
            case ComputedStyle::None: return "none";
        }
        return {};
    }
    if (property == "color") return color(style.color);
    if (property == "background-color") return color(style.backgroundColor);
    if (property == "border-color") return color(style.borderColor);
    if (property == "font-size") return number(style.fontSize);
    // line-height is kept as a multiplier, so it serialises without a unit.
    if (property == "line-height") return unitless(style.lineHeight);
    if (property == "font-weight") return style.fontBold ? "bold" : "normal";
    if (property == "font-family") return style.fontFamily;
    if (property == "text-align") {
        switch (style.textAlign) {
            case ComputedStyle::Left: return "left";
            case ComputedStyle::Center: return "center";
            case ComputedStyle::Right: return "right";
            case ComputedStyle::Justify: return "justify";
        }
        return {};
    }
    if (property == "width") return length(style.width, "auto");
    if (property == "height") return length(style.height, "auto");
    if (property == "min-width") return length(style.minWidth);
    if (property == "max-width") return length(style.maxWidth, "none");
    if (property == "min-height") return length(style.minHeight);
    if (property == "max-height") return length(style.maxHeight, "none");
    if (property == "box-sizing") {
        return style.boxSizing == BoxSizing::BorderBox ? "border-box" : "content-box";
    }
    if (property == "visibility") return style.visible ? "visible" : "hidden";
    if (property == "margin-top") return length(style.margin.top);
    if (property == "margin-right") return length(style.margin.right);
    if (property == "margin-bottom") return length(style.margin.bottom);
    if (property == "margin-left") return length(style.margin.left);
    if (property == "padding-top") return length(style.padding.top);
    if (property == "padding-right") return length(style.padding.right);
    if (property == "padding-bottom") return length(style.padding.bottom);
    if (property == "padding-left") return length(style.padding.left);
    if (property == "border-width-top") return length(style.borderWidth.top);
    if (property == "border-width-right") return length(style.borderWidth.right);
    if (property == "border-width-bottom") return length(style.borderWidth.bottom);
    if (property == "border-width-left") return length(style.borderWidth.left);
    if (property == "flex-direction") {
        switch (style.flexDirection) {
            case ComputedStyle::Row: return "row";
            case ComputedStyle::RowReverse: return "row-reverse";
            case ComputedStyle::Column: return "column";
            case ComputedStyle::ColumnReverse: return "column-reverse";
        }
        return {};
    }
    if (property == "flex-wrap") {
        switch (style.flexWrap) {
            case ComputedStyle::NoWrap: return "nowrap";
            case ComputedStyle::Wrap: return "wrap";
            case ComputedStyle::WrapReverse: return "wrap-reverse";
        }
        return {};
    }
    if (property == "justify-content") return justifyName(style.justifyContent);
    if (property == "align-items") return alignName(style.alignItems);
    if (property == "align-self") return alignName(style.alignSelf);
    if (property == "align-content") return alignName(style.alignContent);
    if (property == "flex-grow") return unitless(style.flex.grow);
    if (property == "flex-shrink") return unitless(style.flex.shrink);
    if (property == "flex-basis") return length(style.flex.basis, "auto");
    return {};
}

bool selectorMatches(const SimpleSelector& selector, const Element& element,
                     const PseudoState* state) {
    if (selector.unsupported) {
        return false;
    }
    // `:hover` and `:active` reach ancestors too, the way a browser lights up every
    // box in the chain under the pointer; `:focus` belongs to one element alone.
    for (const PseudoClass pseudo : selector.pseudoClasses) {
        const Element* target = nullptr;
        switch (pseudo) {
            case PseudoClass::Hover: target = state ? state->hovered : nullptr; break;
            case PseudoClass::Active: target = state ? state->active : nullptr; break;
            case PseudoClass::Focus: target = state ? state->focused : nullptr; break;
        }
        if (!target) {
            return false;
        }
        if (pseudo == PseudoClass::Focus) {
            if (target != &element) {
                return false;
            }
            continue;
        }
        bool reached = false;
        for (const Element* a = target; a; a = a->parent()) {
            if (a == &element) {
                reached = true;
                break;
            }
        }
        if (!reached) {
            return false;
        }
    }
    const auto& parts = selector.parts;
    if (parts.empty()) {
        return false;
    }

    // A generated `::before`/`::after` is a real child box, but the compound that
    // creates it describes the element it hangs off. So `#btn::before` must be
    // matched with the pseudo box's parent, and a selector with no pseudo-element
    // part (`#btn`, `body`) has to reach the box the same way — a pseudo element
    // takes its inherited values and any declarations on its originating element.
    const Element* current = &element;
    std::size_t partIndex = parts.size() - 1;
    if (parts[partIndex].kind == Selector::Kind::PseudoElement) {
        if (!compoundMatches(parts[partIndex], element)) {
            return false;
        }
        current = element.parent();
        if (!current) {
            return false;
        }
        partIndex -= 1;
    } else if (element.isPseudoElement()) {
        current = element.parent();
        if (!current) {
            return false;
        }
    }
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

namespace {

// `initial` and `revert` need an initial-value table per property, which the
// engine does not have, so they are left out and handled like any other unknown
// value. These two the engine can answer exactly.
bool isCssWideKeyword(std::string_view value) {
    return value == "inherit" || value == "unset";
}

// The properties `unset` inherits when no other value applies. Mirrors the set
// inheritInto() fills in.
bool isInheritable(std::string_view property) {
    return property == "color" || property == "font-size" ||
           property == "font-family" || property == "font-weight" ||
           property == "line-height" || property == "text-align";
}

// `inherit` and `unset` are the two CSS-wide keywords the engine can answer
// exactly: the first always takes the parent's value, the second takes it for an
// inheritable property and is otherwise left to the property's own initial value,
// which is what a default-constructed `ComputedStyle` already holds.
void applyCssWideKeyword(ComputedStyle& style, const Declaration& decl,
                         const ComputedStyle& parent) {
    const std::string_view value = trim(decl.value);
    const bool take =
        value == "inherit" || (value == "unset" && isInheritable(decl.property));
    if (!take) {
        return;
    }
    // propertyValue round-trips the parent's value through the same parser, so
    // this needs no second copy of the property table.
    applyDeclaration(style,
                     Declaration{decl.property,
                                 StyleResolver::propertyValue(parent, decl.property),
                                 decl.important},
                     0.0f);
}

}  // namespace

ComputedStyle StyleResolver::resolveWithParent(Element& element,
                                               const ComputedStyle& parent) const {
    ComputedStyle style = defaultStyleFor(element.tag());
    inheritInto(style, parent);

    // `!important` is a cascade level of its own, not a tiebreak: it outranks a
    // stronger selector at an ordinary weight. So the declarations are applied in
    // two passes, each already sorted weakest-first.
    const std::vector<Declaration> inlineDecls =
        element.hasAttribute("style")
            ? parseDeclarations(element.attribute("style"))
            : std::vector<Declaration>{};
    const auto applyLevel = [&](bool important) {
        for (const auto& rule : rules_) {
            if (!selectorMatches(rule.selector, element, &state_)) {
                continue;
            }
            for (const auto& decl : rule.declarations) {
                if (decl.important != important) {
                    continue;
                }
                if (isCssWideKeyword(decl.value)) {
                    applyCssWideKeyword(style, decl, parent);
                } else {
                    applyDeclaration(style, decl, rootFontSize_);
                }
            }
        }
        for (const auto& decl : inlineDecls) {
            if (decl.important != important) {
                continue;
            }
            if (isCssWideKeyword(decl.value)) {
                applyCssWideKeyword(style, decl, parent);
            } else {
                applyDeclaration(style, decl, rootFontSize_);
            }
        }
    };
    applyLevel(false);
    applyLevel(true);

    applyDimensionAttributes(style, element);
    return style;
}

void StyleResolver::setRootFontSize(float size) {
    if (size > 0.0f && size != rootFontSize_) {
        rootFontSize_ = size;
        ++generation_;
    }
}

// Interactive state is part of the resolution input, so a change to it has to bump
// the same generation counter the frame loop already watches.
#define SUI_SET_PSEUDO(field, value)                    \
    if (state_.field == (value)) {                      \
        return false;                                   \
    }                                                   \
    state_.field = (value);                             \
    ++generation_;                                      \
    return true

bool StyleResolver::setHovered(const Element* element) {
    SUI_SET_PSEUDO(hovered, element);
}

bool StyleResolver::setActive(const Element* element) {
    SUI_SET_PSEUDO(active, element);
}

bool StyleResolver::setFocused(const Element* element) {
    SUI_SET_PSEUDO(focused, element);
}

#undef SUI_SET_PSEUDO

}  // namespace sui

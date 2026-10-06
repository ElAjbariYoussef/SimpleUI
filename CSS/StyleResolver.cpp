#include "CSS/StyleResolver.h"

#include "SUI/DOM/Element.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <string_view>

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

// CSS names three border widths; the numbers are the ones the spec fixes them at.
Length parseBorderWidth(std::string_view v) {
    if (v == "thin") return Length{1.0f};
    if (v == "medium") return Length{3.0f};
    if (v == "thick") return Length{5.0f};
    return parseLength(v);
}

std::vector<std::string_view> splitTopLevel(std::string_view value, char separator);

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The arguments of `name(...)`, or false when the token is not that function. The
// closing paren is the token's last character, so a `url()` holding one still works.
bool functionArgs(std::string_view token, std::string_view name, std::string_view& out) {
    if (token.empty() || token.back() != ')') {
        return false;
    }
    if (!startsWith(token, name) || token.size() <= name.size() ||
        token[name.size()] != '(') {
        return false;
    }
    out = trim(token.substr(name.size() + 1, token.size() - name.size() - 2));
    return true;
}

// An angle in degrees, accepting the `deg`, `grad`, `rad` and `turn` units. The unit is
// a suffix, so the number is stripped from the end rather than tested at the front. A
// bare number is not an angle in CSS; a lone `0` is the one exception worth allowing,
// since it is what a zero offset serialises to.
bool parseAngle(std::string_view token, float& out) {
    struct Unit {
        std::string_view name;
        float degrees;
    };
    static constexpr Unit kUnits[] = {
        {"grad", 0.9f}, {"turn", 360.0f}, {"rad", 57.29577951308232f}, {"deg", 1.0f}};
    for (const Unit& unit : kUnits) {
        if (!endsWith(token, unit.name)) {
            continue;
        }
        const std::string_view number = trim(token.substr(0, token.size() - unit.name.size()));
        if (number.empty()) {
            return false;
        }
        try {
            std::size_t read = 0;
            const float value = std::stof(std::string(number), &read);
            if (read != number.size()) {
                return false;
            }
            out = value * unit.degrees;
            return true;
        } catch (...) {
            return false;
        }
    }
    if (token == "0") {
        out = 0.0f;
        return true;
    }
    return false;
}

// CSS expands a one-to-four value box shorthand as: one value sets all four sides,
// two set top-and-bottom then left-and-right, three are top | left-and-right |
// bottom, and four run clockwise from the top.
template <typename T, typename Parse>
void applyToSides(std::string_view value, Parse parse, T& top, T& right, T& bottom, T& left) {
    const std::vector<std::string_view> parts = splitTopLevel(value, ' ');
    T sides[4];
    switch (parts.size()) {
        case 1:
            sides[0] = parse(parts[0]);
            sides[1] = sides[2] = sides[3] = sides[0];
            break;
        case 2:
            sides[0] = sides[2] = parse(parts[0]);
            sides[1] = sides[3] = parse(parts[1]);
            break;
        case 3:
            sides[0] = parse(parts[0]);
            sides[1] = sides[3] = parse(parts[1]);
            sides[2] = parse(parts[2]);
            break;
        case 4:
            sides[0] = parse(parts[0]);
            sides[1] = parse(parts[1]);
            sides[2] = parse(parts[2]);
            sides[3] = parse(parts[3]);
            break;
        default:
            return;
    }
    top = sides[0];
    right = sides[1];
    bottom = sides[2];
    left = sides[3];
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
    // '#' plus the shortest legal form is four characters, so the test has to allow
    // `size == 4` or the three-digit branch below is unreachable.
    if (hex.size() >= 4 && hex[0] == '#') {
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
        int channels[4] = {0, 0, 0, 255};
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

// The CSS named colours. A sorted static table is searched with a binary search rather
// than a map so nothing has to be constructed before main; the list is fixed at compile
// time either way, and a table keeps the names and their values side by side.
struct NamedColor {
    const char* name;
    std::uint8_t r, g, b;
};

constexpr NamedColor kNamedColors[] = {
    {"aliceblue", 240, 248, 255},        {"antiquewhite", 250, 235, 215},
    {"aqua", 0, 255, 255},               {"aquamarine", 127, 255, 212},
    {"azure", 240, 255, 255},            {"beige", 245, 245, 220},
    {"bisque", 255, 228, 196},           {"black", 0, 0, 0},
    {"blanchedalmond", 255, 235, 205},   {"blue", 0, 0, 255},
    {"blueviolet", 138, 43, 226},        {"brown", 165, 42, 42},
    {"burlywood", 222, 184, 135},        {"cadetblue", 95, 158, 160},
    {"chartreuse", 127, 255, 0},         {"chocolate", 210, 105, 30},
    {"coral", 255, 127, 80},             {"cornflowerblue", 100, 149, 237},
    {"cornsilk", 255, 248, 220},         {"crimson", 220, 20, 60},
    {"cyan", 0, 255, 255},               {"darkblue", 0, 0, 139},
    {"darkcyan", 0, 139, 139},           {"darkgoldenrod", 184, 134, 11},
    {"darkgray", 169, 169, 169},         {"darkgreen", 0, 100, 0},
    {"darkgrey", 169, 169, 169},         {"darkkhaki", 189, 183, 107},
    {"darkmagenta", 139, 0, 139},        {"darkolivegreen", 85, 107, 47},
    {"darkorange", 255, 140, 0},         {"darkorchid", 153, 50, 204},
    {"darkred", 139, 0, 0},              {"darksalmon", 233, 150, 122},
    {"darkseagreen", 143, 188, 143},     {"darkslateblue", 72, 61, 139},
    {"darkslategray", 47, 79, 79},       {"darkslategrey", 47, 79, 79},
    {"darkturquoise", 0, 206, 209},      {"darkviolet", 148, 0, 211},
    {"deeppink", 255, 20, 147},          {"deepskyblue", 0, 191, 255},
    {"dimgray", 105, 105, 105},          {"dimgrey", 105, 105, 105},
    {"dodgerblue", 30, 144, 255},        {"firebrick", 178, 34, 34},
    {"floralwhite", 255, 250, 240},      {"forestgreen", 34, 139, 34},
    {"fuchsia", 255, 0, 255},            {"gainsboro", 220, 220, 220},
    {"ghostwhite", 248, 248, 255},       {"gold", 255, 215, 0},
    {"goldenrod", 218, 165, 32},         {"gray", 128, 128, 128},
    {"green", 0, 128, 0},                {"greenyellow", 173, 255, 47},
    {"grey", 128, 128, 128},             {"honeydew", 240, 255, 240},
    {"hotpink", 255, 105, 180},          {"indianred", 205, 92, 92},
    {"indigo", 75, 0, 130},              {"ivory", 255, 255, 240},
    {"khaki", 240, 230, 140},            {"lavender", 230, 230, 250},
    {"lavenderblush", 255, 240, 245},    {"lawngreen", 124, 252, 0},
    {"lemonchiffon", 255, 250, 205},     {"lightblue", 173, 216, 230},
    {"lightcoral", 240, 128, 128},       {"lightcyan", 224, 255, 255},
    {"lightgoldenrodyellow", 250, 250, 210},
    {"lightgray", 211, 211, 211},         {"lightgreen", 144, 238, 144},
    {"lightgrey", 211, 211, 211},        {"lightpink", 255, 182, 193},
    {"lightsalmon", 255, 160, 122},      {"lightseagreen", 32, 178, 170},
    {"lightskyblue", 135, 206, 250},     {"lightslategray", 119, 136, 153},
    {"lightslategrey", 119, 136, 153},   {"lightsteelblue", 176, 196, 222},
    {"lightyellow", 255, 255, 224},      {"lime", 0, 255, 0},
    {"limegreen", 50, 205, 50},          {"linen", 250, 240, 230},
    {"magenta", 255, 0, 255},            {"maroon", 128, 0, 0},
    {"mediumaquamarine", 102, 205, 170}, {"mediumblue", 0, 0, 205},
    {"mediumorchid", 186, 85, 211},      {"mediumpurple", 147, 112, 219},
    {"mediumseagreen", 60, 179, 113},    {"mediumslateblue", 123, 104, 238},
    {"mediumspringgreen", 0, 250, 154},  {"mediumturquoise", 72, 209, 204},
    {"mediumvioletred", 199, 21, 133},   {"midnightblue", 25, 25, 112},
    {"mintcream", 245, 255, 250},        {"mistyrose", 255, 228, 225},
    {"moccasin", 255, 228, 181},         {"navajowhite", 255, 222, 173},
    {"navy", 0, 0, 128},                 {"oldlace", 253, 245, 230},
    {"olive", 128, 128, 0},              {"olivedrab", 107, 142, 35},
    {"orange", 255, 165, 0},             {"orangered", 255, 69, 0},
    {"orchid", 218, 112, 214},           {"palegoldenrod", 238, 232, 170},
    {"palegreen", 152, 251, 152},        {"paleturquoise", 175, 238, 238},
    {"palevioletred", 219, 112, 147},    {"papayawhip", 255, 239, 213},
    {"peachpuff", 255, 218, 185},        {"peru", 205, 133, 63},
    {"pink", 255, 192, 203},             {"plum", 221, 160, 221},
    {"powderblue", 176, 224, 230},       {"purple", 128, 0, 128},
    {"rebeccapurple", 102, 51, 153},     {"red", 255, 0, 0},
    {"rosybrown", 188, 143, 143},        {"royalblue", 65, 105, 225},
    {"saddlebrown", 139, 69, 19},        {"salmon", 250, 128, 114},
    {"sandybrown", 244, 164, 96},        {"seagreen", 46, 139, 87},
    {"seashell", 255, 245, 238},         {"sienna", 160, 82, 45},
    {"silver", 192, 192, 192},           {"skyblue", 135, 206, 235},
    {"slateblue", 106, 90, 205},         {"slategray", 112, 128, 144},
    {"slategrey", 112, 128, 144},        {"snow", 255, 250, 250},
    {"springgreen", 0, 255, 127},        {"steelblue", 70, 130, 180},
    {"tan", 210, 180, 140},              {"teal", 0, 128, 128},
    {"thistle", 216, 191, 216},          {"tomato", 255, 99, 71},
    {"turquoise", 64, 224, 208},         {"violet", 238, 130, 238},
    {"wheat", 245, 222, 179},            {"white", 255, 255, 255},
    {"whitesmoke", 245, 245, 245},       {"yellow", 255, 255, 0},
    {"yellowgreen", 154, 205, 50},
};

bool parseNamedColor(std::string_view name, SDL_Color& out) {
    if (name == "transparent") {
        out = {0, 0, 0, 0};
        return true;
    }
    // CSS keywords are case-insensitive. The table is all lowercase, so the search
    // key is lowercased here rather than a second copy of every name being carried.
    std::string lowered(name);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto found = std::lower_bound(
        kNamedColors, kNamedColors + std::size(kNamedColors), lowered,
        [](const NamedColor& entry, const std::string& key) {
            return std::string_view(entry.name) < std::string_view(key);
        });
    if (found == kNamedColors + std::size(kNamedColors) || found->name != lowered) {
        return false;
    }
    out = {found->r, found->g, found->b, 255};
    return true;
}

// Every property that takes a colour goes through here rather than through the hex
// parser directly, because `red` is at least as common as `#ff0000` and a property
// that silently ignored it would look like a bug in the stylesheet.
bool parseColor(std::string_view value, SDL_Color& out) {
    return parseHexColor(value, out) || parseNamedColor(value, out);
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

// `centre` is the legacy British spelling of `center`; CSS 2.1 allowed it and browsers
// still accept it wherever the American spelling appears.
bool isCentre(std::string_view token) {
    return token == "center" || token == "centre";
}

ComputedStyle::FlexAlign parseJustify(std::string_view v) {
    if (v == "flex-end" || v == "end" || v == "right") {
        return ComputedStyle::AlignEnd;
    }
    if (isCentre(v)) {
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
    if (isCentre(v)) {
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

// Strict on purpose: the `border` shorthand has to tell a style keyword apart from a
// colour, and a lenient parser would claim every unrecognised token as a style.
bool parseBorderStyle(std::string_view v, ComputedStyle::BorderStyle& out) {
    if (v == "none" || v == "hidden") { out = ComputedStyle::BorderStyleNone; return true; }
    if (v == "solid") { out = ComputedStyle::BorderSolid; return true; }
    if (v == "dashed") { out = ComputedStyle::BorderDashed; return true; }
    if (v == "dotted") { out = ComputedStyle::BorderDotted; return true; }
    if (v == "double") { out = ComputedStyle::BorderDouble; return true; }
    if (v == "groove") { out = ComputedStyle::BorderGroove; return true; }
    if (v == "ridge") { out = ComputedStyle::BorderRidge; return true; }
    if (v == "inset") { out = ComputedStyle::BorderInset; return true; }
    if (v == "outset") { out = ComputedStyle::BorderOutset; return true; }
    return false;
}

// Whether a token is a length rather than a keyword or a colour. Used by the
// shadow parsers, which accept their parts in any order.
bool isLengthToken(std::string_view token) {
    if (token.empty() || std::isdigit(static_cast<unsigned char>(token[0])) == 0) {
        return token.size() > 1 && (token[0] == '.' || token[0] == '+' || token[0] == '-');
    }
    return true;
}

// Splits on commas that are not inside brackets or quotes, so a gradient's own
// commas do not cut the value in half.
std::vector<std::string_view> splitTopLevel(std::string_view value, char separator = ',') {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '(' || c == '[') {
            ++depth;
        } else if (c == ')' || c == ']') {
            --depth;
        } else if (c == separator && depth == 0) {
            parts.push_back(trim(value.substr(start, i - start)));
            start = i + 1;
        }
    }
    parts.push_back(trim(value.substr(start)));
    return parts;
}

std::vector<std::string_view> splitTokens(std::string_view value) {
    return splitTopLevel(value, ' ');
}

// The 1-to-4 corner expansion every box property shares: one value sets all four,
// two set the diagonals, and so on.
template <typename T>
void expandCorners(const std::vector<T>& parts, T& topLeft, T& topRight, T& bottomRight,
                   T& bottomLeft) {
    switch (parts.size()) {
        case 1:
            topLeft = topRight = bottomRight = bottomLeft = parts[0];
            break;
        case 2:
            topLeft = bottomRight = parts[0];
            topRight = bottomLeft = parts[1];
            break;
        case 3:
            topLeft = parts[0];
            topRight = bottomLeft = parts[1];
            bottomRight = parts[2];
            break;
        default:
            topLeft = parts[0];
            topRight = parts[1];
            bottomRight = parts[2];
            bottomLeft = parts[3];
            break;
    }
}

// `border-radius` is the only corner property that takes a second group of values
// after a slash, giving each corner a horizontal and a vertical radius.
void applyBorderRadius(ComputedStyle& s, std::string_view value) {
    const std::size_t slash = value.find('/');
    const std::string_view horizontal = slash == std::string_view::npos
                                            ? value
                                            : value.substr(0, slash);
    const std::string_view vertical =
        slash == std::string_view::npos ? std::string_view{} : value.substr(slash + 1);

    const std::vector<Length> xs = parseLengthList(horizontal);
    if (xs.empty()) {
        return;
    }
    std::vector<Length> ys = vertical.empty() ? xs : parseLengthList(vertical);
    if (ys.empty()) {
        ys = xs;
    }

    ComputedStyle::RadiusPair horizontalCorners[4];
    ComputedStyle::RadiusPair verticalCorners[4];
    expandCorners(xs, horizontalCorners[0].x, horizontalCorners[1].x, horizontalCorners[2].x,
                  horizontalCorners[3].x);
    expandCorners(ys, verticalCorners[0].y, verticalCorners[1].y, verticalCorners[2].y,
                  verticalCorners[3].y);
    s.borderRadii.topLeft = {horizontalCorners[0].x, verticalCorners[0].y};
    s.borderRadii.topRight = {horizontalCorners[1].x, verticalCorners[1].y};
    s.borderRadii.bottomRight = {horizontalCorners[2].x, verticalCorners[2].y};
    s.borderRadii.bottomLeft = {horizontalCorners[3].x, verticalCorners[3].y};
}

// One shadow's parts, already sorted out from their order in the declaration.
struct ShadowParts {
    bool inset = false;
    std::vector<Length> lengths;
    SDL_Color color{};
    bool hasColor = false;
};

ShadowParts parseShadowParts(std::string_view value) {
    ShadowParts parts;
    for (const std::string_view token : splitTokens(value)) {
        if (token.empty()) {
            continue;
        }
        if (token == "inset") {
            parts.inset = true;
            continue;
        }
        if (isLengthToken(token)) {
            parts.lengths.push_back(parseLength(token));
            continue;
        }
        SDL_Color color{};
        if (parseColor(token, color)) {
            parts.color = color;
            parts.hasColor = true;
        }
    }
    return parts;
}

// `outline` is width, style, and colour in any order, and its parts are optional.
void applyOutline(ComputedStyle& s, std::string_view value) {
    for (const std::string_view token : splitTokens(value)) {
        if (token.empty()) {
            continue;
        }
        // Length first, then colour, so nothing depends on a keyword parser's
        // fallback for tokens it does not recognise.
        if (isLengthToken(token) || token == "thin" || token == "medium" || token == "thick") {
            s.outline.width = parseBorderWidth(token);
            continue;
        }
        SDL_Color color{};
        if (parseColor(token, color)) {
            s.outline.color = color;
            continue;
        }
        parseBorderStyle(token, s.outline.style);
    }
}

void applyBoxShadow(ComputedStyle& s, std::string_view value) {
    if (value == "none") {
        s.boxShadows.clear();
        return;
    }
    for (const std::string_view entry : splitTopLevel(value)) {
        const ShadowParts parts = parseShadowParts(entry);
        if (parts.lengths.size() < 2) {
            // CSS drops a shadow that has no offsets, rather than guessing one.
            continue;
        }
        ComputedStyle::BoxShadow shadow;
        shadow.inset = parts.inset;
        shadow.offsetX = parts.lengths[0];
        shadow.offsetY = parts.lengths[1];
        if (parts.lengths.size() > 2) {
            shadow.blurRadius = parts.lengths[2];
        }
        if (parts.lengths.size() > 3) {
            shadow.spread = parts.lengths[3];
        }
        if (parts.hasColor) {
            shadow.color = parts.color;
        }
        s.boxShadows.push_back(shadow);
    }
}

void applyTextShadow(ComputedStyle& s, std::string_view value) {
    if (value == "none") {
        s.textShadows.clear();
        return;
    }
    for (const std::string_view entry : splitTopLevel(value)) {
        const ShadowParts parts = parseShadowParts(entry);
        if (parts.lengths.size() < 2) {
            continue;
        }
        ComputedStyle::TextShadow shadow;
        shadow.offsetX = parts.lengths[0];
        shadow.offsetY = parts.lengths[1];
        if (parts.lengths.size() > 2) {
            shadow.blurRadius = parts.lengths[2];
        }
        if (parts.hasColor) {
            shadow.color = parts.color;
        }
        s.textShadows.push_back(shadow);
    }
}

// The inside of a `url(...)`, minus the wrapper and any quotes.
std::string_view unwrapUrl(std::string_view token) {
    std::string_view args;
    if (!functionArgs(token, "url", args)) {
        return {};
    }
    if (args.size() >= 2 && (args.front() == '"' || args.front() == '\'') &&
        args.back() == args.front()) {
        args = args.substr(1, args.size() - 2);
    }
    return args;
}

// One colour stop inside a gradient. The position is optional and may come before or
// after the colour, which is why this takes the whole comma-separated entry.
bool parseGradientStop(std::string_view entry, ComputedStyle::GradientStop& out) {
    const std::vector<std::string_view> parts = splitTokens(entry);
    if (parts.empty()) {
        return false;
    }
    for (const std::string_view part : parts) {
        SDL_Color color;
        if (parseColor(part, color)) {
            out.color = color;
            continue;
        }
        const Length length = parseLength(part);
        if (length.unit != Length::Auto) {
            out.offset = length;
            continue;
        }
        return false;
    }
    return true;
}

// The first entry that names a colour is where a gradient's stops begin; everything
// before it is the prelude, which is why the split is on commas rather than on spaces.
// A `90deg` on its own is an entry, but `90deg,` is not once the tokeniser has run.
std::size_t firstStopEntry(const std::vector<std::string_view>& entries) {
    ComputedStyle::GradientStop probe;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (parseGradientStop(entries[i], probe)) {
            return i;
        }
    }
    return entries.size();
}

// The angle `to <side-or-corner>` means, or -1 for a keyword CSS does not have. A
// diagonal is 45 degrees off an axis, which is all `to top right` amounts to.
float angleForSide(std::string_view token) {
    std::string_view side = token;
    if (startsWith(side, "to ")) {
        side = trim(side.substr(3));
    }
    if (side == "top") return 0.0f;
    if (side == "right") return 90.0f;
    if (side == "bottom") return 180.0f;
    if (side == "left") return 270.0f;
    if (side == "top right" || side == "right top") return 45.0f;
    if (side == "bottom right" || side == "right bottom") return 135.0f;
    if (side == "bottom left" || side == "left bottom") return 225.0f;
    if (side == "top left" || side == "left top") return 315.0f;
    return -1.0f;
}

// `linear-gradient()` takes either an angle or a `to <side-or-corner>` before the
// stops, and both are optional. An unrecognised prelude is a bad value, so the whole
// function is dropped — which is what tells `linear-gradient(red, blue)` apart from
// `linear-gradient(sideways, red, blue)`.
bool parseLinearGradient(std::string_view args, ComputedStyle::LinearGradient& out) {
    const std::vector<std::string_view> entries = splitTopLevel(args);
    const std::size_t first = firstStopEntry(entries);
    // A zero means the very first entry is already a colour, so there is no prelude. A
    // non-zero one means whatever sits in front of it had better be a direction, since a
    // stop that failed to parse above is indistinguishable from one that never was a stop.
    if (first > 0) {
        const float side = angleForSide(entries[0]);
        if (!parseAngle(entries[0], out.angle)) {
            if (side < 0.0f) {
                return false;
            }
            out.angle = side;
        }
    }
    for (std::size_t i = first; i < entries.size(); ++i) {
        ComputedStyle::GradientStop stop;
        // A colour and an optional position, so one bad entry makes the whole function
        // a bad value rather than silently losing a stop.
        if (!parseGradientStop(entries[i], stop)) {
            return false;
        }
        out.stops.push_back(stop);
    }
    return out.stops.size() >= 1;
}

// The shape, the extent and the sizes of a `radial-gradient()`. The first two are
// keywords in either order, and any of them may be absent.
void parseRadialPrelude(std::string_view args, ComputedStyle::RadialGradient& out,
                        std::size_t& index) {
    bool shapeSeen = false;
    bool extentSeen = false;
    const std::vector<std::string_view> tokens = splitTokens(args);
    while (index < tokens.size()) {
        const std::string_view part = tokens[index];
        if (!shapeSeen && (part == "circle" || part == "ellipse")) {
            out.shape = part == "circle" ? ComputedStyle::RadialGradient::Circle
                                         : ComputedStyle::RadialGradient::Ellipse;
            shapeSeen = true;
            ++index;
            continue;
        }
        if (!extentSeen && (part == "closest-side" || part == "farthest-side" ||
                            part == "closest-corner" || part == "farthest-corner")) {
            if (part == "closest-side") {
                out.extent = ComputedStyle::RadialGradient::ClosestSide;
            } else if (part == "farthest-side") {
                out.extent = ComputedStyle::RadialGradient::FarthestSide;
            } else if (part == "closest-corner") {
                out.extent = ComputedStyle::RadialGradient::ClosestCorner;
            } else {
                out.extent = ComputedStyle::RadialGradient::FarthestCorner;
            }
            extentSeen = true;
            ++index;
            continue;
        }
        break;
    }
}

// `at` introduces the centre and, optionally, the focus. Two lengths after it are the
// centre, three or four are the centre then the radius pair. Without `at` the lengths
// are the radii alone, and there can be at most two of them.
void parseRadialPositions(std::string_view args, ComputedStyle::RadialGradient& out,
                          std::size_t index) {
    const std::vector<std::string_view> tokens = splitTokens(args);
    const bool hasAt = index < tokens.size() && tokens[index] == "at";
    if (hasAt) {
        ++index;
    }
    std::vector<Length> numbers;
    for (std::size_t i = index; i < tokens.size(); ++i) {
        const Length length = parseLength(tokens[i]);
        if (length.unit != Length::Auto) {
            numbers.push_back(length);
        }
    }
    if (hasAt) {
        if (numbers.empty()) {
            return;
        }
        out.centreX = numbers[0];
        out.centreY = numbers.size() > 1 ? numbers[1] : Length{50.0f};
        if (numbers.size() > 2) {
            // Three or four: the centre, then the radii. A further `at` beyond two
            // lengths would be the focus, which the spec only allows with `circle`.
            out.radiusX = numbers[2];
            out.hasRadii = true;
            if (numbers.size() > 3) {
                out.radiusY = numbers[3];
            }
        }
        return;
    }
    if (numbers.size() >= 1) {
        out.radiusX = numbers[0];
        out.hasRadii = true;
    }
    if (numbers.size() >= 2) {
        out.radiusY = numbers[1];
    }
}

bool parseRadialGradient(std::string_view args, ComputedStyle::RadialGradient& out) {
    const std::vector<std::string_view> entries = splitTopLevel(args);
    const std::size_t first = firstStopEntry(entries);
    if (first == entries.size()) {
        return false;  // a gradient needs at least two stops
    }
    std::string prelude;
    for (std::size_t i = 0; i < first; ++i) {
        if (i > 0) {
            prelude += ' ';
        }
        prelude += entries[i];
    }
    std::size_t index = 0;
    parseRadialPrelude(prelude, out, index);
    parseRadialPositions(prelude, out, index);
    for (std::size_t i = first; i < entries.size(); ++i) {
        ComputedStyle::GradientStop stop;
        if (!parseGradientStop(entries[i], stop)) {
            return false;
        }
        out.stops.push_back(stop);
    }
    return out.stops.size() >= 1;
}

void applyBackgroundImage(ComputedStyle& s, std::string_view value) {
    if (value == "none") {
        s.backgroundImages.assign(1, ComputedStyle::BackgroundImage{});
        return;
    }
    for (const std::string_view entry : splitTopLevel(value)) {
        ComputedStyle::BackgroundImage image;
        if (const std::string_view href = unwrapUrl(entry); !href.empty()) {
            image.kind = ComputedStyle::BackgroundImage::Url;
            image.href = std::string(href);
        } else if (std::string_view args; functionArgs(entry, "linear-gradient", args)) {
            // The kind is only set once the arguments parse, because a value that does
            // not is invalid at computed-value time and leaves the layer as `none`. It
            // has to keep its place in the list rather than be dropped, or every layer
            // after it would slide out of step with the other longhands, which are
            // matched to it by position.
            if (parseLinearGradient(args, image.linear)) {
                image.kind = ComputedStyle::BackgroundImage::LinearGradientImage;
            }
        } else if (std::string_view args; functionArgs(entry, "radial-gradient", args)) {
            if (parseRadialGradient(args, image.radial)) {
                image.kind = ComputedStyle::BackgroundImage::RadialGradientImage;
            }
        }
        s.backgroundImages.push_back(image);
    }
}

bool parseBackgroundSize(std::string_view value, ComputedStyle::BackgroundSize& size) {
    const std::vector<std::string_view> parts = splitTokens(value);
    if (parts.empty() || parts.size() > 2) {
        return false;
    }
    if (parts.size() == 1 && (parts[0] == "cover" || parts[0] == "contain")) {
        size.mode = parts[0] == "cover" ? ComputedStyle::BackgroundSize::Cover
                                        : ComputedStyle::BackgroundSize::Contain;
        return true;
    }
    size.mode = ComputedStyle::BackgroundSize::Explicit;
    // An axis the author left out is `auto`, which is the same thing `auto` says, so
    // there is nothing extra to record for it.
    size.width = parseLength(parts[0]);
    size.height = parts.size() == 2 ? parseLength(parts[1]) : Length{};
    return true;
}

bool parseBackgroundBox(std::string_view value, ComputedStyle::BackgroundBox& out) {
    if (value == "border-box") {
        out = ComputedStyle::BackgroundBorderBox;
    } else if (value == "padding-box") {
        out = ComputedStyle::BackgroundPaddingBox;
    } else if (value == "content-box") {
        out = ComputedStyle::BackgroundContentBox;
    } else {
        return false;
    }
    return true;
}

bool parseAttachment(std::string_view value, ComputedStyle::BackgroundAttachment& out) {
    if (value == "scroll") {
        out = ComputedStyle::BackgroundScroll;
    } else if (value == "fixed") {
        out = ComputedStyle::BackgroundFixed;
    } else if (value == "local") {
        out = ComputedStyle::BackgroundLocal;
    } else {
        return false;
    }
    return true;
}

// Every background longhand but the colour and the image is a comma-separated list,
// matched against the image layers by position, so this runs once per comma-separated
// entry and always pushes something: an entry that does not parse is invalid at
// computed-value time and becomes that property's initial value, which is what keeps
// the layers after it lined up with the images.
enum class PositionAxis { Horizontal, Vertical, Both };
PositionAxis axisOf(std::string_view token) {
    if (token == "top" || token == "bottom") {
        return PositionAxis::Vertical;
    }
    if (isCentre(token)) {
        return PositionAxis::Both;
    }
    return PositionAxis::Horizontal;
}

bool assignPosition(std::string_view token, ComputedStyle::BackgroundPosition& out,
                    PositionAxis axis) {
    if (token == "left") {
        out.horizontal = ComputedStyle::BackgroundPosition::Start;
    } else if (token == "right") {
        out.horizontal = ComputedStyle::BackgroundPosition::End;
    } else if (token == "top") {
        out.vertical = ComputedStyle::BackgroundPosition::Start;
    } else if (token == "bottom") {
        out.vertical = ComputedStyle::BackgroundPosition::End;
    } else if (isCentre(token)) {
        if (axis != PositionAxis::Horizontal) {
            out.horizontal = ComputedStyle::BackgroundPosition::Center;
        }
        if (axis != PositionAxis::Vertical) {
            out.vertical = ComputedStyle::BackgroundPosition::Center;
        }
    } else {
        const Length length = parseLength(token);
        if (length.unit == Length::Auto) {
            return false;
        }
        if (axis == PositionAxis::Vertical) {
            out.offsetY = length;
        } else {
            out.offsetX = length;
        }
        return true;
    }
    return axis == PositionAxis::Both || axisOf(token) == axis;
}

bool parsePositionPair(std::string_view value, ComputedStyle::BackgroundPosition& out) {
    const std::vector<std::string_view> parts = splitTokens(value);
    ComputedStyle::BackgroundPosition position;

    if (parts.size() == 1 || parts.size() == 2) {
        // One value takes the horizontal slot unless it names a vertical edge, which is
        // what makes `top` and `center` behave differently in first and second place.
        if (!assignPosition(parts[0], position, axisOf(parts[0]))) {
            return false;
        }
        if (parts.size() == 1) {
            // A lone `center` means both axes; a lone `left` leaves the vertical slot
            // at its initial `Start`, which is `top`.
            if (isCentre(parts[0])) {
                position.vertical = ComputedStyle::BackgroundPosition::Center;
            }
            out = position;
            return true;
        }
        // Two values take different axes. When both name the same one the second is
        // taken as vertical, which is what `50% 20%` and `center top` both need.
        const PositionAxis first = axisOf(parts[0]);
        const PositionAxis second =
            axisOf(parts[1]) == first && first != PositionAxis::Both ? PositionAxis::Vertical
                                                                      : axisOf(parts[1]);
        if (!assignPosition(parts[1], position, second)) {
            return false;
        }
        out = position;
        return true;
    }

    if (parts.size() != 4) {
        return false;
    }
    // Four values: two edges and two offsets, in either clockwise or anticlockwise
    // order. An offset after `right` or `bottom` measures back from that edge, so it is
    // negated here rather than in the paint stage.
    bool horizontalSeen = false;
    bool verticalSeen = false;
    for (std::size_t i = 0; i < 4; i += 2) {
        const std::string_view edge = parts[i];
        const Length offset = parseLength(parts[i + 1]);
        if (offset.unit == Length::Auto) {
            return false;
        }
        if (edge == "left" || edge == "right") {
            if (horizontalSeen) {
                return false;
            }
            horizontalSeen = true;
            position.horizontal = edge == "left" ? ComputedStyle::BackgroundPosition::Start
                                                 : ComputedStyle::BackgroundPosition::End;
            position.offsetX = edge == "left" ? offset : Length{Length::Px, -offset.value};
        } else if (edge == "top" || edge == "bottom") {
            if (verticalSeen) {
                return false;
            }
            verticalSeen = true;
            position.vertical = edge == "top" ? ComputedStyle::BackgroundPosition::Start
                                              : ComputedStyle::BackgroundPosition::End;
            position.offsetY = edge == "top" ? offset : Length{Length::Px, -offset.value};
        } else {
            return false;
        }
    }
    if (!horizontalSeen || !verticalSeen) {
        return false;
    }
    out = position;
    return true;
}

bool parseRepeatWord(std::string_view token, bool& x, bool& y) {
    if (token == "repeat") {
        x = y = true;
    } else if (token == "no-repeat") {
        x = y = false;
    } else if (token == "repeat-x") {
        x = true;
        y = false;
    } else if (token == "repeat-y") {
        x = false;
        y = true;
    } else {
        return false;
    }
    return true;
}

bool parseRepeatValue(std::string_view value, ComputedStyle::RepeatPair& repeat) {
    const std::vector<std::string_view> parts = splitTokens(value);
    if (parts.size() == 1 && (parts[0] == "space" || parts[0] == "round")) {
        repeat.x = repeat.y = false;
        repeat.kind = parts[0] == "space" ? ComputedStyle::RepeatPair::Space
                                          : ComputedStyle::RepeatPair::Round;
        return true;
    }
    if (parts.size() == 1) {
        return parseRepeatWord(parts[0], repeat.x, repeat.y);
    }
    if (parts.size() == 2) {
        return parseRepeatWord(parts[0], repeat.x, repeat.y) &&
               parseRepeatWord(parts[1], repeat.x, repeat.y);
    }
    return false;
}

void applyBackgroundLonghand(ComputedStyle& s, std::string_view property,
                             std::string_view value) {
    if (property == "background-size") {
        ComputedStyle::BackgroundSize size;
        if (!parseBackgroundSize(value, size)) {
            size = ComputedStyle::BackgroundSize{};
        }
        s.backgroundSizes.push_back(size);
    } else if (property == "background-position") {
        ComputedStyle::BackgroundPosition position;
        if (!parsePositionPair(value, position)) {
            position = ComputedStyle::BackgroundPosition{};
        }
        s.backgroundPositions.push_back(position);
    } else if (property == "background-repeat") {
        ComputedStyle::RepeatPair repeat;
        if (!parseRepeatValue(value, repeat)) {
            repeat = ComputedStyle::RepeatPair{};
        }
        s.backgroundRepeats.push_back(repeat);
    } else if (property == "background-clip") {
        ComputedStyle::BackgroundBox box;
        if (!parseBackgroundBox(value, box)) {
            box = ComputedStyle::BackgroundBorderBox;
        }
        s.backgroundClips.push_back(box);
    } else if (property == "background-origin") {
        ComputedStyle::BackgroundBox box;
        if (!parseBackgroundBox(value, box)) {
            box = ComputedStyle::BackgroundPaddingBox;
        }
        s.backgroundOrigins.push_back(box);
    } else if (property == "background-attachment") {
        ComputedStyle::BackgroundAttachment attachment;
        if (!parseAttachment(value, attachment)) {
            attachment = ComputedStyle::BackgroundScroll;
        }
        s.backgroundAttachments.push_back(attachment);
    }
}


// A property the engine does not implement is dropped by CSS anyway, but saying so
// here is what makes the gap debuggable: it turns a hover rule with an unsupported
// `transition` in it from "hover is broken" into a message. Logged once per name,
// because resolve() walks every element of every stale frame and a repeat would
// bury everything else in the log.
void warnUnknownProperty(std::string_view property) {
    static std::set<std::string, std::less<>> reported;
    if (reported.emplace(property).second) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "ignoring unsupported CSS property '%.*s'",
                    static_cast<int>(property.size()), property.data());
    }
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
        applyToSides(value, parseBorderWidth, s.borderWidth.top, s.borderWidth.right,
                     s.borderWidth.bottom, s.borderWidth.left);
    } else if (property == "border-top-width" || property == "border-width-top") {
        // `border-top-width` is the CSS spelling and `border-width-top` is the one
        // this engine opened with, which getComputedStyle has always answered to.
        // Both are accepted: these four were readable but had no setter at all, so
        // writing one was reported as an unknown property while reading it back
        // looked correct.
        s.borderWidth.top = parseBorderWidth(value);
    } else if (property == "border-right-width" || property == "border-width-right") {
        s.borderWidth.right = parseBorderWidth(value);
    } else if (property == "border-bottom-width" || property == "border-width-bottom") {
        s.borderWidth.bottom = parseBorderWidth(value);
    } else if (property == "border-left-width" || property == "border-width-left") {
        s.borderWidth.left = parseBorderWidth(value);
    } else if (property == "border") {
        // Width, style and colour in any order, one value each; an unknown token is
        // dropped rather than guessed at, as an invalid declaration should be.
        for (const std::string_view part : splitTopLevel(value, ' ')) {
            if (part == "thin" || part == "medium" || part == "thick" ||
                isLengthToken(part)) {
                const Length width = parseBorderWidth(part);
                s.borderWidth = {width, width, width, width};
                continue;
            }
            ComputedStyle::BorderStyle style{};
            if (parseBorderStyle(part, style)) {
                s.borderStyles = {style, style, style, style};
                continue;
            }
            SDL_Color color{};
            if (parseColor(part, color)) {
                s.borderColor = color;
            }
        }
    } else if (property == "background-image") {
        applyBackgroundImage(s, value);
    } else if (property == "background-size" || property == "background-position" ||
               property == "background-repeat" || property == "background-clip" ||
               property == "background-origin" || property == "background-attachment") {
        for (const std::string_view entry : splitTopLevel(value)) {
            applyBackgroundLonghand(s, property, entry);
        }
    } else if (property == "background-color" || property == "background") {
        SDL_Color color{};
        if (parseColor(value, color)) {
            s.backgroundColor = color;
        }
    } else if (property == "color") {
        SDL_Color color{};
        if (parseColor(value, color)) {
            s.color = color;
        }
    } else if (property == "border-color") {
        SDL_Color color{};
        if (parseColor(value, color)) {
            s.borderColor = color;
        }
    } else if (property == "border-style") {
        applyToSides(value, [](std::string_view v) {
                     ComputedStyle::BorderStyle style = ComputedStyle::BorderSolid;
                     parseBorderStyle(v, style);
                     return style;
                 },
                 s.borderStyles.top, s.borderStyles.right, s.borderStyles.bottom,
                 s.borderStyles.left);
    } else if (property == "border-top-style") {
        parseBorderStyle(value, s.borderStyles.top);
    } else if (property == "border-right-style") {
        parseBorderStyle(value, s.borderStyles.right);
    } else if (property == "border-bottom-style") {
        parseBorderStyle(value, s.borderStyles.bottom);
    } else if (property == "border-left-style") {
        parseBorderStyle(value, s.borderStyles.left);
    } else if (property == "border-radius") {
        applyBorderRadius(s, value);
    } else if (property == "border-top-left-radius" ||
               property == "border-top-right-radius" ||
               property == "border-bottom-right-radius" ||
               property == "border-bottom-left-radius") {
        const std::vector<Length> parts = parseLengthList(value);
        if (parts.empty()) {
            return;
        }
        // A longhand takes the same `x / y` form as the shorthand.
        const std::size_t slash = value.find('/');
        const Length x = parts[0];
        const Length y = slash == std::string_view::npos ? parts[0] : parts[1];
        ComputedStyle::RadiusPair& corner =
            property == "border-top-left-radius"     ? s.borderRadii.topLeft
            : property == "border-top-right-radius"   ? s.borderRadii.topRight
            : property == "border-bottom-right-radius" ? s.borderRadii.bottomRight
                                                       : s.borderRadii.bottomLeft;
        corner = {x, y};
    } else if (property == "outline") {
        applyOutline(s, value);
    } else if (property == "outline-width") {
        s.outline.width = parseBorderWidth(value);
    } else if (property == "outline-style") {
        parseBorderStyle(value, s.outline.style);
    } else if (property == "outline-color") {
        SDL_Color color{};
        if (parseColor(value, color)) {
            s.outline.color = color;
        }
    } else if (property == "outline-offset") {
        s.outline.offset = parseLength(value);
    } else if (property == "opacity") {
        s.opacity = std::clamp(parseNumber(value), 0.0f, 1.0f);
    } else if (property == "box-shadow") {
        applyBoxShadow(s, value);
    } else if (property == "text-shadow") {
        applyTextShadow(s, value);
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
        if (isCentre(value)) {
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
    } else {
        warnUnknownProperty(property);
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
            if (part.attributeOp == '\0') {
                return true;  // `[data-role]`, the presence test
            }
            const std::string_view actual = element.attribute(part.value);
            const std::string& wanted = part.attributeValue;
            switch (part.attributeOp) {
                case '=':
                    return actual == wanted;
                // Word list, for `class="a b c"`. Empty wanted cannot appear in
                // practice, but would match nothing rather than everything.
                case '~': {
                    if (wanted.empty()) {
                        return false;
                    }
                    std::size_t i = 0;
                    while (i <= actual.size()) {
                        const std::size_t end = actual.find(' ', i);
                        const std::size_t stop = end == std::string::npos ? actual.size() : end;
                        if (actual.compare(i, stop - i, wanted) == 0) {
                            return true;
                        }
                        if (end == std::string::npos) {
                            break;
                        }
                        i = end + 1;
                    }
                    return false;
                }
                // `|` also treats the whole value as an exact match when it has no
                // following separator, which is why it cannot be a plain prefix test.
                case '|': {
                    if (actual == wanted) {
                        return true;
                    }
                    return actual.size() > wanted.size() &&
                           actual.compare(0, wanted.size(), wanted) == 0 &&
                           actual[wanted.size()] == '-';
                }
                case '^':
                    // An empty value is the beginning of every string, so `[att^=""]`
                    // matches any element with the attribute. That is what the spec's
                    // "begins with" means, and guarding against empty here would make
                    // it match nothing instead.
                    return actual.compare(0, wanted.size(), wanted) == 0;
                case '$':
                    if (wanted.size() > actual.size()) {
                        return false;  // also keeps the offset below from wrapping
                    }
                    return actual.compare(actual.size() - wanted.size(), wanted.size(), wanted) ==
                           0;
                case '*':
                    return actual.find(wanted) != std::string::npos;
            }
            return false;
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
    if (property == "border-width-top" || property == "border-top-width")
        return length(style.borderWidth.top);
    if (property == "border-width-right" || property == "border-right-width")
        return length(style.borderWidth.right);
    if (property == "border-width-bottom" || property == "border-bottom-width")
        return length(style.borderWidth.bottom);
    if (property == "border-width-left" || property == "border-left-width")
        return length(style.borderWidth.left);
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
            // Only a rule under an `@media` pays for evaluating one, which keeps
            // an ordinary stylesheet at the cost it had before.
            if (!rule.media.empty() &&
                !mediaMatches(rule.media, viewportWidth_, viewportHeight_)) {
                continue;
            }
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

// A viewport change re-decides which `@media` blocks apply, so it has to bump the
// same generation the frame loop watches — otherwise a resize would re-lay-out
// with the styles of the previous size.
bool StyleResolver::setViewportSize(float width, float height) {
    if (width <= 0.0f || height <= 0.0f || (width == viewportWidth_ && height == viewportHeight_)) {
        return false;
    }
    viewportWidth_ = width;
    viewportHeight_ = height;
    ++generation_;
    return true;
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

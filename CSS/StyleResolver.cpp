#include <sstream>
#include "CSS/StyleResolver.h"

#include "SUI/DOM/Element.h"
#include "SUI/Script/Timers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

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

// Splits on whitespace, but only at paren depth zero, so a function argument such
// as `minmax(100px, 1fr)` stays one token.
std::vector<std::string_view> splitTopLevelSpaces(std::string_view value) {
    std::vector<std::string_view> out;
    std::size_t start = std::string_view::npos;
    int depth = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        }
        const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (space && depth == 0) {
            if (start != std::string_view::npos) {
                out.push_back(value.substr(start, i - start));
                start = std::string_view::npos;
            }
        } else if (start == std::string_view::npos) {
            start = i;
        }
    }
    if (start != std::string_view::npos) {
        out.push_back(value.substr(start));
    }
    return out;
}

// Splits on the top-level commas of a function argument list.
std::vector<std::string_view> splitTopLevelCommas(std::string_view value) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        } else if (c == ',' && depth == 0) {
            out.push_back(value.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(value.substr(start));
    return out;
}

// Splits on `/`, which is how `grid-area` and `border-style` list their values.
std::vector<std::string_view> splitOnSlashes(std::string_view value) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t slash = value.find('/', start);
        out.push_back(value.substr(
            start, slash == std::string_view::npos ? slash : slash - start));
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return out;
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

// --- calc()/min()/max()/clamp() ---
//
// A math value stays a tree of lengths until layout resolves it, so percentages
// and font-relative units behave inside the expression exactly as they do in a
// plain length. Multiplication and division only accept a unitless operand, the
// CSS rule; a violation makes the whole expression invalid, which comes back as
// an Auto length and drops the declaration that carried it.

Length parseMathFunction(std::string_view name, std::string_view body);
std::vector<std::string_view> splitTopLevel(std::string_view value, char separator);

// `text` is `name( ... )` with the matching close parenthesis as the last
// character; `body` receives the text between them.
bool mathCallBody(std::string_view text, std::string_view name, std::string_view& body) {
    if (text.size() < name.size() + 2 || text.compare(0, name.size(), name) != 0 ||
        text[name.size()] != '(') {
        return false;
    }
    int depth = 0;
    for (std::size_t i = name.size(); i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            --depth;
            if (depth == 0) {
                body = text.substr(name.size() + 1, i - name.size() - 1);
                return i + 1 == text.size();
            }
        }
    }
    return false;
}

// `text` is `( ... )`; `inner` receives what sits between the brackets.
bool groupBody(std::string_view text, std::string_view& inner) {
    if (text.size() < 2 || text.front() != '(') {
        return false;
    }
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            --depth;
            if (depth == 0) {
                inner = text.substr(1, i - 1);
                return i + 1 == text.size();
            }
        }
    }
    return false;
}

struct CalcOp {
    char op = 0;
    std::size_t pos = 0;
};

// The binary operators at depth zero. A '-' is binary only when it follows a
// completed term (`100% - 2rem` and `100%-2rem` both split); a '-' at the head,
// or right after an operator or '(', is the sign of the upcoming term
// (`calc(-5px + 10px)`), and the term parser folds it into the number.
std::vector<CalcOp> topLevelOps(std::string_view text) {
    std::vector<CalcOp> ops;
    int depth = 0;
    bool termDone = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(') {
            ++depth;
            termDone = false;
        } else if (c == ')') {
            --depth;
            termDone = true;
        } else if (depth == 0 && (c == '*' || c == '/')) {
            ops.push_back({c, i});
            termDone = false;
        } else if (depth == 0 && (c == '+' || c == '-') && termDone) {
            ops.push_back({c, i});
            termDone = false;
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            termDone = true;
        }
    }
    return ops;
}

Length invalidLength() { return Length{}; }

Length makeMath(MathExpr::Op op, std::vector<Length> args, bool dimensioned) {
    MathExpr expr;
    expr.op = op;
    expr.args = std::move(args);
    expr.dimensioned = dimensioned;
    return Length{std::make_shared<MathExpr>(std::move(expr))};
}

// Whether a length carries a unit anywhere in its tree; the check that decides
// which * and / combinations are legal and what a +/- may mix.
bool lengthKeepsDimension(const Length& l) {
    switch (l.unit) {
        case Length::Px:
        case Length::Percent:
        case Length::Em:
        case Length::Rem:
            return true;
        case Length::Calc:
            return l.expr && l.expr->dimensioned;
        default:
            return false;
    }
}

// A plain operand inside an expression. Unlike the outer parseLength, a bare
// number comes back as a Number (unitless) so `calc(100% / 2)` can be
// distinguished from the illegal `calc(100% / 2px)`.
Length parseCalcAtom(std::string_view text) {
    const std::string_view s = trim(text);
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        ++i;
    }
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
        ++i;
    }
    if (i == 0) {
        return invalidLength();
    }
    const float number = std::strtof(std::string(s.substr(0, i)).c_str(), nullptr);
    const std::string_view unit = trim(s.substr(i));
    if (unit.empty()) {
        return Length{Length::Number, number};
    }
    if (unit == "px") {
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
    if (unit == "pt") {
        return Length{number * (96.0f / 72.0f)};
    }
    return invalidLength();
}

Length parseCalcSum(std::string_view text);

Length parseCalcPrimary(std::string_view text) {
    const std::string_view s = trim(text);
    if (s.empty()) {
        return invalidLength();
    }
    std::string_view inner;
    if (s.front() == '(' && groupBody(s, inner)) {
        return parseCalcSum(inner);
    }
    for (const char* name : {"calc", "min", "max", "clamp"}) {
        if (mathCallBody(s, name, inner)) {
            return parseMathFunction(name, inner);
        }
    }
    return parseCalcAtom(s);
}

Length parseCalcProduct(std::string_view text) {
    const std::vector<CalcOp> ops = topLevelOps(text);
    std::size_t start = 0;
    std::vector<Length> operands;
    std::vector<char> productOps;
    for (const CalcOp& op : ops) {
        if (op.op != '*' && op.op != '/') {
            continue;
        }
        operands.push_back(parseCalcPrimary(trim(text.substr(start, op.pos - start))));
        productOps.push_back(op.op);
        start = op.pos + 1;
    }
    if (productOps.empty()) {
        return parseCalcPrimary(text);
    }
    operands.push_back(parseCalcPrimary(trim(text.substr(start))));
    Length acc = operands[0];
    for (std::size_t i = 0; i < productOps.size(); ++i) {
        const bool accDim = lengthKeepsDimension(acc);
        const bool rhsDim = lengthKeepsDimension(operands[i + 1]);
        // A product needs exactly one dimensioned side, and a divisor must be
        // unitless on top of that.
        if (accDim == rhsDim || (productOps[i] == '/' && rhsDim)) {
            return invalidLength();
        }
        acc = makeMath(productOps[i] == '*' ? MathExpr::Op::Mul : MathExpr::Op::Div,
                       {acc, operands[i + 1]}, accDim || rhsDim);
    }
    return acc;
}

Length parseCalcSum(std::string_view text) {
    const std::vector<CalcOp> ops = topLevelOps(text);
    std::size_t start = 0;
    std::vector<Length> operands;
    std::vector<char> sumOps;
    for (const CalcOp& op : ops) {
        if (op.op != '+' && op.op != '-') {
            continue;
        }
        operands.push_back(parseCalcProduct(trim(text.substr(start, op.pos - start))));
        sumOps.push_back(op.op);
        start = op.pos + 1;
    }
    if (sumOps.empty()) {
        return parseCalcProduct(text);
    }
    operands.push_back(parseCalcProduct(trim(text.substr(start))));

    // + and - only mix operands of the same kind: two lengths blend, a length
    // and a unitless number do not.
    const bool dimensioned = lengthKeepsDimension(operands[0]);
    for (const Length& operand : operands) {
        if (lengthKeepsDimension(operand) != dimensioned) {
            return invalidLength();
        }
    }
    Length acc = operands[0];
    for (std::size_t i = 0; i < sumOps.size(); ++i) {
        acc = makeMath(sumOps[i] == '+' ? MathExpr::Op::Add : MathExpr::Op::Sub,
                       {acc, operands[i + 1]}, dimensioned);
    }
    return acc;
}

Length parseMathFunction(std::string_view name, std::string_view body) {
    const std::string_view s = trim(body);
    if (name == "calc") {
        const Length result = parseCalcSum(s);
        if (result.isAuto()) {
            return result;
        }
        // A calc whose whole body is one plain length ("calc(50%)") collapses to
        // that length, so the value behaves exactly as the unwrapped form;
        // a bare-number body stays invalid for a length position.
        if (result.unit == Length::Calc && result.expr &&
            result.expr->op == MathExpr::Op::Value) {
            return result.expr->args[0];
        }
        return result;
    }

    const std::vector<std::string_view> args = splitTopLevel(s, ',');
    if (args.empty()) {
        return invalidLength();
    }
    if (name == "clamp") {
        if (args.size() != 3) {
            return invalidLength();
        }
        const Length lo = parseCalcSum(args[0]);
        const Length middle = parseCalcSum(args[1]);
        const Length hi = parseCalcSum(args[2]);
        if (lo.isAuto() || middle.isAuto() || hi.isAuto() || !lengthKeepsDimension(lo) ||
            !lengthKeepsDimension(middle) || !lengthKeepsDimension(hi)) {
            return invalidLength();
        }
        return makeMath(MathExpr::Op::Clamp, {lo, middle, hi}, true);
    }
    std::vector<Length> values;
    bool dimensioned = false;
    for (const std::string_view arg : args) {
        Length value = parseCalcSum(arg);
        if (value.isAuto()) {
            return invalidLength();
        }
        const bool dim = lengthKeepsDimension(value);
        if (values.empty()) {
            dimensioned = dim;
        } else if (dim != dimensioned) {
            return invalidLength();
        }
        values.push_back(std::move(value));
    }
    return makeMath(name == "min" ? MathExpr::Op::Min : MathExpr::Op::Max,
                    std::move(values), dimensioned);
}

// The css text of a math node, so a computed calc value keeps round-tripping
// through the parser, which is also what `inherit` relies on.
std::string serializeMathNode(const MathExpr* expr) {
    if (!expr) {
        return "calc(0px)";
    }
    const auto leaf = [](const Length& l) {
        if (l.unit == Length::Calc) {
            return serializeMathNode(l.expr.get());
        }
        if (l.unit == Length::Number) {
            return unitless(l.value);
        }
        const char* suffix = l.unit == Length::Px        ? "px"
                             : l.unit == Length::Percent ? "%"
                             : l.unit == Length::Em      ? "em"
                                                         : "rem";
        return unitless(l.value) + suffix;
    };
    switch (expr->op) {
        case MathExpr::Op::Value:
            return leaf(expr->args[0]);
        case MathExpr::Op::Add:
            return "calc(" + leaf(expr->args[0]) + " + " + leaf(expr->args[1]) + ")";
        case MathExpr::Op::Sub:
            return "calc(" + leaf(expr->args[0]) + " - " + leaf(expr->args[1]) + ")";
        case MathExpr::Op::Mul:
            return "calc(" + leaf(expr->args[0]) + " * " + leaf(expr->args[1]) + ")";
        case MathExpr::Op::Div:
            return "calc(" + leaf(expr->args[0]) + " / " + leaf(expr->args[1]) + ")";
        case MathExpr::Op::Min:
        case MathExpr::Op::Max: {
            std::string out(expr->op == MathExpr::Op::Min ? "min(" : "max(");
            for (std::size_t i = 0; i < expr->args.size(); ++i) {
                if (i != 0) {
                    out += ", ";
                }
                out += leaf(expr->args[i]);
            }
            return out + ")";
        }
        case MathExpr::Op::Clamp:
            return "clamp(" + leaf(expr->args[0]) + ", " + leaf(expr->args[1]) + ", " +
                   leaf(expr->args[2]) + ")";
    }
    return "calc(0px)";
}

// Parses one CSS length, keeping the unit so layout can resolve it against the
// containing block. `auto` and `none` both come back as Auto, which is what the
// width/height/max-* properties mean by them.
Length parseLength(std::string_view value) {
    const std::string_view text = trim(value);
    if (text.empty() || text == "auto" || text == "none") {
        return Length{};
    }

    // calc()/min()/max()/clamp() stay symbolic until layout resolves them.
    for (const char* name : {"calc", "min", "max", "clamp"}) {
        std::string_view body;
        if (mathCallBody(text, name, body)) {
            const Length result = parseMathFunction(name, body);
            return result.isAuto() ? Length{} : result;
        }
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

// One or two position tokens into x/y; shared with `clip-path` positions,
// which is why it is declared early.
bool parseClipPosition(const std::vector<std::string_view>& tokens, Length& x, Length& y);

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The arguments of `name(...)`, or false when the token is not that function. The
// closing paren is the token's last character, so a `url()` holding one still works.
// Names one of the sixteen compositing modes both blend properties share.
// False for anything else, which keeps the previous cascade winner.
bool blendModeByName(std::string_view name, ComputedStyle::MixBlendMode& out) {
    using Blend = ComputedStyle::MixBlendMode;
    if (name == "normal") {
        out = Blend::Normal;
    } else if (name == "multiply") {
        out = Blend::Multiply;
    } else if (name == "screen") {
        out = Blend::Screen;
    } else if (name == "overlay") {
        out = Blend::Overlay;
    } else if (name == "darken") {
        out = Blend::Darken;
    } else if (name == "lighten") {
        out = Blend::Lighten;
    } else if (name == "color-dodge") {
        out = Blend::ColorDodge;
    } else if (name == "color-burn") {
        out = Blend::ColorBurn;
    } else if (name == "hard-light") {
        out = Blend::HardLight;
    } else if (name == "soft-light") {
        out = Blend::SoftLight;
    } else if (name == "difference") {
        out = Blend::Difference;
    } else if (name == "exclusion") {
        out = Blend::Exclusion;
    } else if (name == "hue") {
        out = Blend::Hue;
    } else if (name == "saturation") {
        out = Blend::Saturation;
    } else if (name == "color") {
        out = Blend::Color;
    } else if (name == "luminosity") {
        out = Blend::Luminosity;
    } else {
        return false;
    }
    return true;
}

const char* blendModeName(ComputedStyle::MixBlendMode mode) {
    using Blend = ComputedStyle::MixBlendMode;
    switch (mode) {
        case Blend::Normal: return "normal";
        case Blend::Multiply: return "multiply";
        case Blend::Screen: return "screen";
        case Blend::Overlay: return "overlay";
        case Blend::Darken: return "darken";
        case Blend::Lighten: return "lighten";
        case Blend::ColorDodge: return "color-dodge";
        case Blend::ColorBurn: return "color-burn";
        case Blend::HardLight: return "hard-light";
        case Blend::SoftLight: return "soft-light";
        case Blend::Difference: return "difference";
        case Blend::Exclusion: return "exclusion";
        case Blend::Hue: return "hue";
        case Blend::Saturation: return "saturation";
        case Blend::Color: return "color";
        case Blend::Luminosity: return "luminosity";
    }
    return "normal";
}

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

static bool parseHsl(std::string_view value, SDL_Color& out) {
    std::string t(value);
    for (char& c : t) {
        if (c == ',' || c == '(' || c == ')' || c == '%') c = ' ';
    }
    std::stringstream ss(t);
    std::string fn;
    float h = 0, s = 0, l = 0, a = 1.0f;
    ss >> fn >> h >> s >> l >> a;
    if (fn != "hsl" && fn != "hsla") return false;
    if (s < 0) s = 0;
    if (s > 100) s = 100;
    if (l < 0) l = 0;
    if (l > 100) l = 100;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    auto hsl_to_rgb = [](float h, float s, float l) -> std::tuple<int,int,int> {
        s /= 100.0f; l /= 100.0f;
        float c = (1.0f - std::fabs(2.0f*l - 1.0f)) * s;
        float x = c * (1.0f - std::fabs(fmodf(h/60.0f, 2.0f) - 1.0f));
        float m = l - c/2.0f;
        float r=0,g=0,b=0;
        if (h < 60) { r=c; g=x; b=0; }
        else if (h < 120) { r=x; g=c; b=0; }
        else if (h < 180) { r=0; g=c; b=x; }
        else if (h < 240) { r=0; g=x; b=c; }
        else if (h < 300) { r=x; g=0; b=c; }
        else { r=c; g=0; b=x; }
        return {int((r+m)*255+0.5f), int((g+m)*255+0.5f), int((b+m)*255+0.5f)};
    };
    auto [r,g,b] = hsl_to_rgb(h, s, l);
    out = {Uint8(r), Uint8(g), Uint8(b), Uint8(a*255)};
    return true;
}
static bool parseOklch(std::string_view value, SDL_Color& out) {
    std::string t(value);
    for (char& c : t) {
        if (c == ',' || c == '(' || c == ')' || c == '/' || c == '%') c = ' ';
    }
    std::stringstream ss(t);
    std::string fn;
    float l = 0, c = 0, h = 0, a = 1.0f;
    if (!(ss >> fn)) return false;
    if (fn != "oklch") return false;
    if (!(ss >> l >> c >> h)) return false;
    ss >> a;
    // `oklch()` accepts a percentage for any axis; the space-separated form makes
    // that hard to tell apart from a 0-1 number, so a value above 1 is read as a
    // percentage. Alpha is a plain 0-1 fraction.
    if (l > 1.0f) l /= 100.0f;
    if (a > 1.0f) a /= 100.0f;
    if (l < 0) l = 0;
    if (l > 1) l = 1;
    if (c < 0) c = 0;
    if (a < 0) a = 0;
    if (a > 1) a = 1;

    // OKLCh -> OKLab -> linear sRGB -> gamma-encoded sRGB, using the standard
    // Björn Ottosson matrices.
    const float hr = h * 3.14159265358979323846f / 180.0f;
    const float oa = c * std::cos(hr);
    const float ob = c * std::sin(hr);
    const float l_ = l + 0.3963377774f * oa + 0.2158037573f * ob;
    const float m_ = l - 0.1055613458f * oa - 0.0638541728f * ob;
    const float s_ = l - 0.0894841775f * oa - 1.2914855480f * ob;
    const float ll = l_ * l_ * l_;
    const float mm = m_ * m_ * m_;
    const float sss = s_ * s_ * s_;
    const float rl = 4.0767416621f * ll - 3.3077115913f * mm + 0.2309699292f * sss;
    const float gl = -1.2684380046f * ll + 2.6097574011f * mm - 0.3413193965f * sss;
    const float bl = -0.0041960863f * ll - 0.7034186147f * mm + 1.7076147010f * sss;
    auto encode = [](float x) {
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return 1.0f;
        if (x <= 0.0031308f) return 12.92f * x;
        return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
    };
    out = {Uint8(encode(rl) * 255.0f + 0.5f), Uint8(encode(gl) * 255.0f + 0.5f),
           Uint8(encode(bl) * 255.0f + 0.5f), Uint8(a * 255.0f + 0.5f)};
    return true;
}

// Finds the first `,` that is not inside parentheses, which separates the two
// operands of `color-mix()` even when an operand is itself a function.
static std::size_t topLevelComma(std::string_view text, std::size_t from) {
    int depth = 0;
    for (std::size_t i = from; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (ch == ',' && depth == 0) return i;
    }
    return std::string_view::npos;
}

// `color-mix(in srgb, A p%, B)`. A trailing percentage on an operand is its
// weight; a missing one splits the remainder. Mixing is a straight sRGB
// interpolation with premultiplied alpha.
static bool parseColorMix(std::string_view value, SDL_Color& out) {
    const std::string text(value);
    if (text.rfind("color-mix(", 0) != 0) return false;
    const std::size_t open = text.find('(');
    const std::size_t firstComma = topLevelComma(text, open + 1);
    if (firstComma == std::string::npos) return false;
    // The colour space is `in srgb`; other spaces are not approximated.
    std::string space = text.substr(open + 1, firstComma - open - 1);
    for (char& ch : space) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (trim(space) != "in srgb") return false;

    const std::size_t secondComma = topLevelComma(text, firstComma + 1);
    if (secondComma == std::string::npos) return false;
    const std::size_t close = text.rfind(')');
    if (close == std::string::npos) return false;

    auto parseOperand = [](std::string_view piece, SDL_Color& colour, float& weight,
                           bool& hasWeight) {
        piece = trim(piece);
        std::size_t pct = piece.rfind('%');
        hasWeight = false;
        weight = 0.0f;
        if (pct != std::string_view::npos) {
            std::size_t numStart = pct;
            while (numStart > 0 && (std::isdigit(static_cast<unsigned char>(piece[numStart - 1])) ||
                                    piece[numStart - 1] == '.')) {
                --numStart;
            }
            const std::string number(piece.substr(numStart, pct - numStart));
            char* stop = nullptr;
            const float parsed = std::strtof(number.c_str(), &stop);
            if (stop != number.c_str()) {
                hasWeight = true;
                weight = parsed / 100.0f;
                piece = trim(piece.substr(0, numStart));
            }
        }
        return parseColor(piece, colour);
    };

    SDL_Color c1{}, c2{};
    float w1 = 0.0f, w2 = 0.0f;
    bool h1 = false, h2 = false;
    if (!parseOperand(text.substr(firstComma + 1, secondComma - firstComma - 1), c1, w1, h1)) {
        return false;
    }
    if (!parseOperand(text.substr(secondComma + 1, close - secondComma - 1), c2, w2, h2)) {
        return false;
    }
    // One weight implies the other; neither defaults to an even split.
    if (!h1 && !h2) { w1 = 0.5f; w2 = 0.5f; }
    else if (h1 && !h2) { w2 = 1.0f - w1; }
    else if (!h1 && h2) { w1 = 1.0f - w2; }
    if (w1 < 0) w1 = 0;
    if (w1 > 1) w1 = 1;
    if (w2 < 0) w2 = 0;
    if (w2 > 1) w2 = 1;
    const float total = w1 + w2;
    if (total <= 0.0f) return false;
    w1 /= total;
    w2 /= total;

    auto mix = [&](Uint8 a, Uint8 b) {
        return Uint8(static_cast<float>(a) * w1 + static_cast<float>(b) * w2 + 0.5f);
    };
    out = {mix(c1.r, c2.r), mix(c1.g, c2.g), mix(c1.b, c2.b), mix(c1.a, c2.a)};
    return true;
}

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

}  // namespace

// Every property that takes a colour goes through here rather than through the hex
// parser directly, because `red` is at least as common as `#ff0000` and a property
// that silently ignored it would look like a bug in the stylesheet.
bool parseColor(std::string_view value, SDL_Color& out) {
    return parseHexColor(value, out) || parseNamedColor(value, out) || parseHsl(value, out) || parseOklch(value, out) || parseColorMix(value, out);
}

namespace {

// A colour that may also be the `currentColor` keyword, which parks a
// placeholder for the resolver to swap once `color` itself is settled. Case
// does not matter, as it does not for any other keyword.
bool parseColorValue(std::string_view value, SDL_Color& out) {
    const std::string_view text = trim(value);
    if (text.size() == 12) {
        std::string lowered(text);
        for (char& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowered == "currentcolor") {
            out = kCurrentColor;
            return true;
        }
    }
    return parseColor(text, out);
}

// Splits on whitespace and parses each piece as a length, so `margin: 5px 10%`
// keeps both units rather than flattening them to pixels. The splitter brackets
// `calc(...)` whole, so `margin: 10px calc(50% - 2rem)` stays two parts.
std::vector<Length> parseLengthList(std::string_view value) {
    std::vector<Length> out;
    for (const std::string_view part : splitTopLevel(value, ' ')) {
        if (part.empty() || out.size() == 4) {
            continue;
        }
        out.push_back(parseLength(part));
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

void applyTwoSides(std::string_view value, Length& start, Length& end) {
    const std::vector<Length> parts = parseLengthList(value);
    if (parts.size() == 1) {
        start = parts[0];
        end = parts[0];
    } else if (parts.size() >= 2) {
        start = parts[0];
        end = parts[1];
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

struct ContentResult {
    bool hasContent = false;
    std::string text;
};

// `content: "x"` and `content: "a" "b"` concatenate their string arguments.
// `content: attr(foo)` resolves against the originating element's attribute.
// `none` and `normal` mean "no generated box".
ContentResult parseContent(std::string_view v) {
    std::string_view trimmed = trim(v);
    if (trimmed == "none" || trimmed == "normal" || trimmed.empty()) {
        return {false, {}};
    }
    ContentResult res;
    res.hasContent = true;
    std::size_t i = 0;
    while (i < v.size()) {
        while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) {
            ++i;
        }
        if (i >= v.size()) break;
        const char quote = v[i];
        if (quote == '"' || quote == '\'') {
            ++i;
            const std::size_t start = i;
            while (i < v.size() && v[i] != quote) {
                ++i;
            }
            res.text.append(v.substr(start, i - start));
            if (i < v.size()) {
                ++i;  // the closing quote
            }
        } else if (v.substr(i, 5) == "attr(") {
            i += 5;
            while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
            const std::size_t start = i;
            while (i < v.size() && v[i] != ')' && !std::isspace(static_cast<unsigned char>(v[i]))) {
                ++i;
            }
            const std::string_view attrName = v.substr(start, i - start);
            while (i < v.size() && v[i] != ')') ++i;
            if (i < v.size() && v[i] == ')') ++i;
            res.text.push_back('\x01');
            res.text.append(attrName);
            res.text.push_back('\x02');
        } else if (v.substr(i, 8) == "counter(") {
            i += 8;
            while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
            const std::size_t start = i;
            while (i < v.size() && v[i] != ')' && v[i] != ',') {
                ++i;
            }
            std::string name(v.substr(start, i - start));
            // An optional style rides behind a unit separator byte, which a CSS
            // identifier can never contain.
            while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
            if (i < v.size() && v[i] == ',') {
                ++i;
                while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
                const std::size_t styleStart = i;
                while (i < v.size() && v[i] != ')') ++i;
                std::string_view styleName =
                    trim(v.substr(styleStart, i - styleStart));
                if (!styleName.empty()) {
                    name += '\x1f';
                    name += styleName;
                }
            }
            while (i < v.size() && v[i] != ')') ++i;
            if (i < v.size() && v[i] == ')') ++i;
            res.text.push_back('');
            res.text.append(name);
            res.text.push_back('');
        } else if (v.substr(i, 9) == "counters(") {
            i += 9;
            while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
            const std::size_t start = i;
            while (i < v.size() && v[i] != ')' && v[i] != ',') {
                ++i;
            }
            std::string nameWithSeparator;
            std::string_view name = v.substr(start, i - start);

            // `counters(name, sep)` joins every counter with that name in scope
            // using `sep`. The separator is folded into the name behind a unit
            // separator byte, which a CSS identifier can never contain, so the
            // generated-content reader can split the pair back apart.
            while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
            if (i < v.size() && v[i] == ',') {
                ++i;
                while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
                if (i < v.size() && (v[i] == '"' || v[i] == '\'')) {
                    const char quote = v[i++];
                    const std::size_t sepStart = i;
                    while (i < v.size() && v[i] != quote) ++i;
                    const std::string separator =
                        std::string(v.substr(sepStart, i - sepStart));
                    if (i < v.size()) ++i;  // the closing quote
                    nameWithSeparator = std::string(name) + '\x1f' + separator;
                    name = nameWithSeparator;
                }
            }
            while (i < v.size() && v[i] != ')') ++i;
            if (i < v.size() && v[i] == ')') ++i;
            res.text.push_back('');
            res.text.append(name);
            res.text.push_back('');
        } else if (v.substr(i, 10) == "open-quote" &&
                   (i + 10 >= v.size() ||
                    std::isspace(static_cast<unsigned char>(v[i + 10])))) {
            res.text.push_back('');
            i += 10;
        } else if (v.substr(i, 11) == "close-quote" &&
                   (i + 11 >= v.size() ||
                    std::isspace(static_cast<unsigned char>(v[i + 11])))) {
            res.text.push_back('');
            i += 11;
        } else if (v.substr(i, 13) == "no-open-quote" &&
                   (i + 13 >= v.size() ||
                    std::isspace(static_cast<unsigned char>(v[i + 13])))) {
            res.text.push_back('	');
            i += 13;
        } else if (v.substr(i, 14) == "no-close-quote" &&
                   (i + 14 >= v.size() ||
                    std::isspace(static_cast<unsigned char>(v[i + 14])))) {
            res.text.push_back('\x0A');
            i += 14;
        } else {
            ++i;
        }
    }
    return res;
}


// Parses a `counter-reset` / `counter-increment` list. `defaultValue` is the
// amount used when a name is written without one: 0 for `counter-reset`, 1 for
// `counter-increment`.
std::vector<ComputedStyle::CounterOp> parseCounterList(std::string_view v, int defaultValue) {
    std::vector<ComputedStyle::CounterOp> out;
    std::size_t i = 0;
    while (i < v.size()) {
        while (i < v.size() &&
               (std::isspace(static_cast<unsigned char>(v[i])) || v[i] == ',')) {
            ++i;
        }
        if (i >= v.size()) {
            break;
        }
        // `none` means no counters at all; it is the only value that ends the list.
        if (v.substr(i, 4) == "none" &&
            (i + 4 >= v.size() || std::isspace(static_cast<unsigned char>(v[i + 4])))) {
            break;
        }
        const std::size_t start = i;
        while (i < v.size() && !std::isspace(static_cast<unsigned char>(v[i])) && v[i] != ',') {
            ++i;
        }
        std::string name = std::string(v.substr(start, i - start));
        while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
        int val = defaultValue;
        if (i < v.size() && (v[i] == '+' || v[i] == '-' ||
                             std::isdigit(static_cast<unsigned char>(v[i])))) {
            bool neg = false;
            if (v[i] == '+' || v[i] == '-') {
                neg = (v[i] == '-');
                ++i;
            }
            if (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
                const std::size_t numstart = i;
                while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
                val = std::stoi(std::string(v.substr(numstart, i - numstart)));
                if (neg) val = -val;
            }
        }
        out.push_back({name, val});
    }
    return out;
}

ComputedStyle::Overflow parseOverflow(std::string_view v) {
    if (v == "hidden" || v == "clip") {
        return ComputedStyle::Hidden;
    }
    if (v == "scroll") {
        return ComputedStyle::Scroll;
    }
    if (v == "auto") {
        return ComputedStyle::Auto;
    }
    return ComputedStyle::Visible;
}

// Splits a `container-name` list on whitespace. `none` clears every name,
// alone or among others.
void parseContainerNames(std::string_view value, std::vector<std::string>& names) {
    names.clear();
    std::size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        std::size_t j = i;
        while (j < value.size() && !std::isspace(static_cast<unsigned char>(value[j]))) {
            ++j;
        }
        if (j > i) {
            const std::string_view token = value.substr(i, j - i);
            if (token != "none") {
                names.emplace_back(token);
            }
        }
        i = j;
    }
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

// `grid-template-columns` / `grid-template-rows`: a whitespace-separated track
// list, where a track is a length, a `<number>fr` share, `auto`, or a
// `repeat(<count>, <tracks>)` group. `minmax()` and named lines are not read.
bool parseGridTrack(std::string_view token, ComputedStyle::GridTrack& out) {
    token = trim(token);
    if (token.empty()) {
        return false;
    }
    if (token.size() > 7 && token.substr(0, 7) == "minmax(") {
        // `minmax(min, max)` keeps the max's kind (length, `auto`, or `fr`) and
        // records the min as a floor. A nested minmax is not meaningful here.
        const std::size_t open = token.find('(');
        const std::size_t close = token.rfind(')');
        if (open == std::string_view::npos || close == std::string_view::npos ||
            close < open) {
            return false;
        }
        const std::vector<std::string_view> parts =
            splitTopLevelCommas(token.substr(open + 1, close - open - 1));
        if (parts.size() != 2) {
            return false;
        }
        ComputedStyle::GridTrack minTrack;
        ComputedStyle::GridTrack maxTrack;
        if (!parseGridTrack(trim(parts[0]), minTrack) ||
            !parseGridTrack(trim(parts[1]), maxTrack)) {
            return false;
        }
        out = maxTrack;
        // `auto` and `0` minimums impose no floor; everything else does.
        if (!minTrack.isAuto) {
            out.hasMin = true;
            out.minSize = minTrack.size;
        }
        return true;
    }
    if (token == "auto") {
        out.isAuto = true;
        return true;
    }
    if (token.size() > 2 && token.substr(token.size() - 2) == "fr") {
        const std::string number(token.substr(0, token.size() - 2));
        char* stop = nullptr;
        const float fr = std::strtof(number.c_str(), &stop);
        if (stop == number.c_str()) {
            return false;
        }
        out.isFr = true;
        out.fr = fr;
        return true;
    }
    out.size = parseLength(token);
    return true;
}

std::vector<ComputedStyle::GridTrack> parseGridTrackList(std::string_view value,
                                                        bool* autoFill = nullptr,
                                                        bool* autoFit = nullptr) {
    std::vector<ComputedStyle::GridTrack> out;
    const std::string_view text = trim(value);
    const std::vector<std::string_view> tokens = splitTopLevelSpaces(text);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string_view token = trim(tokens[i]);
        if (token.substr(0, 7) == "repeat(") {
            // A repeat group can contain spaces, so join tokens until the parens
            // balance before reading it.
            std::string group(token);
            int depth = 1;
            for (char c : group) {
                if (c == '(') ++depth;
                else if (c == ')') --depth;
            }
            while (depth > 0 && i + 1 < tokens.size()) {
                group += " ";
                group += std::string(tokens[++i]);
                for (char c : tokens[i]) {
                    if (c == '(') ++depth;
                    else if (c == ')') --depth;
                }
            }
            const std::size_t open = group.find('(');
            const std::size_t comma = group.find(',', open);
            const std::size_t close = group.rfind(')');
            if (open == std::string::npos || comma == std::string::npos ||
                close == std::string::npos) {
                continue;
            }
            const std::string_view countText =
                trim(std::string_view(group).substr(open + 1, comma - open - 1));
            const std::string_view body =
                std::string_view(group).substr(comma + 1, close - comma - 1);

            // `auto-fill` / `auto-fit` have no fixed count: the body is stored and
            // the layout repeats it to fill the container. Only the all-in-one
            // `repeat(...)` form is handled; a mix with explicit tracks falls back
            // to ignoring the repeat.
            if (countText == "auto-fill" || countText == "auto-fit") {
                if (tokens.size() != 1) {
                    continue;
                }
                for (const std::string_view piece : splitTopLevelSpaces(trim(body))) {
                    ComputedStyle::GridTrack track;
                    if (parseGridTrack(piece, track)) {
                        out.push_back(track);
                    }
                }
                if (autoFill) {
                    *autoFill = true;
                }
                if (autoFit) {
                    *autoFit = countText == "auto-fit";
                }
                return out;
            }

            const int count = std::atoi(std::string(countText).c_str());
            if (count <= 0) {
                continue;
            }
            const std::vector<std::string_view> inner = splitTopLevelSpaces(trim(body));
            for (int n = 0; n < count; ++n) {
                for (const std::string_view piece : inner) {
                    ComputedStyle::GridTrack track;
                    if (parseGridTrack(piece, track)) {
                        out.push_back(track);
                    }
                }
            }
            continue;
        }
        ComputedStyle::GridTrack track;
        if (parseGridTrack(token, track)) {
            out.push_back(track);
        }
    }
    return out;
}

// How many cells a `grid-column` / `grid-row` / `grid-area` value spans. Only the
// `span <n>` keyword and numeric `a / b` line ranges are read; a lone line number
// spans one cell (that is what auto-placement fills), and anything unreadable
// falls back to 1.
int parseGridSpan(std::string_view value) {
    // Reads one slash-separated part: `span n` contributes its count, a plain
    // number records the line so the two ends can be subtracted.
    int explicitSpan = 0;
    int lineA = 0;
    int lineB = 0;
    bool hasA = false;
    bool hasB = false;
    bool first = true;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t slash = value.find('/', start);
        const std::string_view part =
            trim(value.substr(start, slash == std::string_view::npos ? slash : slash - start));
        int num = 0;
        const bool spanWord = part.size() > 4 && part.substr(0, 4) == "span";
        const std::string_view numText = spanWord ? trim(part.substr(4)) : part;
        if (!numText.empty()) {
            const std::string text(numText);
            char* stop = nullptr;
            num = static_cast<int>(std::strtol(text.c_str(), &stop, 10));
            if (stop != text.c_str()) {
                if (spanWord) {
                    if (num > 1) {
                        explicitSpan = std::max(explicitSpan, num);
                    }
                } else if (first) {
                    lineA = num;
                    hasA = true;
                } else {
                    lineB = num;
                    hasB = true;
                }
            }
        }
        first = false;
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    if (explicitSpan > 1) {
        return explicitSpan;
    }
    if (hasA && hasB && lineA != lineB) {
        return std::max(1, std::abs(lineB - lineA));
    }
    return 1;
}

// Parses one half of a `grid-column` / `grid-row` shorthand (or a longhand value)
// into a line number and a span count. A `span <n>` keyword sets only the span
// (leaves lineOut = 0). A bare integer sets only the line (leaves spanOut at its
// default of 1). `auto` leaves both at their defaults.
void parseGridLinePart(std::string_view part, int& lineOut, int& spanOut) {
    part = trim(part);
    if (part == "auto" || part.empty()) {
        return;
    }
    const bool spanWord = part.size() > 4 && part.substr(0, 4) == "span";
    const std::string_view numText = spanWord ? trim(part.substr(4)) : part;
    if (!numText.empty()) {
        const std::string text(numText);
        char* stop = nullptr;
        const int num = static_cast<int>(std::strtol(text.c_str(), &stop, 10));
        if (stop != text.c_str() && num != 0) {
            if (spanWord) {
                spanOut = std::max(1, num);
            } else {
                lineOut = num;
            }
        }
    }
}

// Parses `grid-template-areas: "<row1>" "<row2>" ...` into named area bounding boxes.
void parseGridTemplateAreas(std::string_view value,
                            std::map<std::string, ComputedStyle::GridAreaRect>& areasOut,
                            std::size_t& colsOut, std::size_t& rowsOut) {
    value = trim(value);
    if (value == "none" || value.empty()) {
        return;
    }
    std::vector<std::vector<std::string>> matrix;
    std::size_t pos = 0;
    while (pos < value.size()) {
        const char quote = value[pos];
        if (quote == '"' || quote == '\'') {
            const std::size_t next = value.find(quote, pos + 1);
            if (next == std::string_view::npos) {
                return;
            }
            const std::string_view rowContent = value.substr(pos + 1, next - pos - 1);
            std::vector<std::string> rowCells;
            std::size_t cp = 0;
            while (cp < rowContent.size()) {
                while (cp < rowContent.size() &&
                       std::isspace(static_cast<unsigned char>(rowContent[cp]))) {
                    ++cp;
                }
                if (cp >= rowContent.size()) {
                    break;
                }
                const std::size_t cStart = cp;
                while (cp < rowContent.size() &&
                       !std::isspace(static_cast<unsigned char>(rowContent[cp]))) {
                    ++cp;
                }
                rowCells.emplace_back(rowContent.substr(cStart, cp - cStart));
            }
            if (!rowCells.empty()) {
                matrix.push_back(std::move(rowCells));
            }
            pos = next + 1;
        } else {
            ++pos;
        }
    }
    if (matrix.empty()) {
        return;
    }
    const std::size_t numCols = matrix[0].size();
    for (const auto& r : matrix) {
        if (r.size() != numCols) {
            return;
        }
    }
    const std::size_t numRows = matrix.size();

    struct Bounds {
        std::size_t minR = static_cast<std::size_t>(-1), maxR = 0;
        std::size_t minC = static_cast<std::size_t>(-1), maxC = 0;
        std::size_t cellCount = 0;
    };
    std::map<std::string, Bounds> boundsMap;
    for (std::size_t r = 0; r < numRows; ++r) {
        for (std::size_t c = 0; c < numCols; ++c) {
            const std::string& name = matrix[r][c];
            if (name == "." || name.empty()) {
                continue;
            }
            Bounds& b = boundsMap[name];
            b.minR = std::min(b.minR, r);
            b.maxR = std::max(b.maxR, r);
            b.minC = std::min(b.minC, c);
            b.maxC = std::max(b.maxC, c);
            b.cellCount++;
        }
    }

    for (const auto& [name, b] : boundsMap) {
        const std::size_t expectedCells = (b.maxR - b.minR + 1) * (b.maxC - b.minC + 1);
        if (b.cellCount != expectedCells) {
            return;
        }
        for (std::size_t r = b.minR; r <= b.maxR; ++r) {
            for (std::size_t c = b.minC; c <= b.maxC; ++c) {
                if (matrix[r][c] != name) {
                    return;
                }
            }
        }
    }

    for (const auto& [name, b] : boundsMap) {
        ComputedStyle::GridAreaRect rect;
        rect.rowStart = static_cast<int>(b.minR + 1);
        rect.rowEnd   = static_cast<int>(b.maxR + 2);
        rect.colStart = static_cast<int>(b.minC + 1);
        rect.colEnd   = static_cast<int>(b.maxC + 2);
        areasOut[name] = rect;
    }
    colsOut = numCols;
    rowsOut = numRows;
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

// The keyword a border style reads back as. `hidden` shares a value with `none`
// in this engine, so it reads back as `none`.
std::string_view borderStyleName(ComputedStyle::BorderStyle style) {
    switch (style) {
        case ComputedStyle::BorderStyleNone: return "none";
        case ComputedStyle::BorderSolid: return "solid";
        case ComputedStyle::BorderDashed: return "dashed";
        case ComputedStyle::BorderDotted: return "dotted";
        case ComputedStyle::BorderDouble: return "double";
        case ComputedStyle::BorderGroove: return "groove";
        case ComputedStyle::BorderRidge: return "ridge";
        case ComputedStyle::BorderInset: return "inset";
        case ComputedStyle::BorderOutset: return "outset";
    }
    return "none";
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
        if (parseColorValue(token, color)) {
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
        if (parseColorValue(token, color)) {
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
        if (parseColorValue(part, color)) {
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

// One colour stop inside a `conic-gradient()`: the position is optional and
// takes a percentage or an angle, which becomes a percentage of the turn.
// Lengths are not positions here, so unlike the linear stops they fail.
bool parseConicalStop(std::string_view entry, ComputedStyle::GradientStop& out) {
    const std::vector<std::string_view> parts = splitTokens(entry);
    if (parts.empty()) {
        return false;
    }
    for (const std::string_view part : parts) {
        SDL_Color color;
        if (parseColorValue(part, color)) {
            out.color = color;
            continue;
        }
        if (!part.empty() && part.back() == '%') {
            const Length length = parseLength(part);
            if (length.unit != Length::Percent) {
                return false;
            }
            out.offset = length;
            continue;
        }
        float angle = 0.0f;
        if (!parseAngle(part, angle)) {
            return false;
        }
        out.offset = Length(Length::Percent, angle / 3.6f);
    }
    return true;
}

// The first entry that names a colour is where a conic gradient's stops
// begin; `from <angle>` and `at <position>` never do.
std::size_t firstConicalStopEntry(const std::vector<std::string_view>& entries) {
    ComputedStyle::GradientStop probe;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (parseConicalStop(entries[i], probe)) {
            return i;
        }
    }
    return entries.size();
}

// `conic-gradient()` takes an optional `from <angle>` then an optional
// `at <position>` before the stops, in that order. Anything else in the
// prelude drops the whole function.
bool parseConicalGradient(std::string_view args, ComputedStyle::ConicalGradient& out) {
    const std::vector<std::string_view> entries = splitTopLevel(args);
    const std::size_t first = firstConicalStopEntry(entries);
    if (first == entries.size()) {
        return false;
    }
    // The prelude is `from <angle>` then `at <position>`, each optional, with
    // the clauses possibly sharing one comma entry (`from 45deg at center`).
    std::vector<std::string_view> prelude;
    for (std::size_t i = 0; i < first; ++i) {
        for (std::string_view token : splitTokens(entries[i])) {
            prelude.push_back(token);
        }
    }
    std::size_t p = 0;
    if (p < prelude.size() && prelude[p] == "from") {
        if (p + 1 >= prelude.size() || !parseAngle(prelude[p + 1], out.angle)) {
            return false;
        }
        p += 2;
    }
    if (p < prelude.size() && prelude[p] == "at") {
        ++p;
        std::vector<std::string_view> position(prelude.begin() + p, prelude.end());
        if (!parseClipPosition(position, out.centreX, out.centreY)) {
            return false;
        }
        p = prelude.size();
    }
    if (p != prelude.size()) {
        return false;
    }
    for (std::size_t i = first; i < entries.size(); ++i) {
        ComputedStyle::GradientStop stop;
        if (!parseConicalStop(entries[i], stop)) {
            return false;
        }
        out.stops.push_back(stop);
    }
    return out.stops.size() >= 1;
}

// One `mask-image` layer: a gradient applies as an alpha mask stretched over
// the border box, while a `url()` source samples decoded bytes the same way.
// The caller splits the comma list; `none` is an opaque layer.
bool parseMaskImage(std::string_view value, ComputedStyle::MaskImage& mask) {
    using Mask = ComputedStyle::MaskImage;
    const std::string_view text = trim(value);
    if (text.empty() || text == "none") {
        mask = Mask();
        return true;
    }
    const std::string_view first = text;
    Mask parsed;
    if (std::string_view args; functionArgs(first, "linear-gradient", args)) {
        if (!parseLinearGradient(args, parsed.linear)) {
            return false;
        }
        parsed.kind = Mask::Kind::LinearGradient;
    } else if (functionArgs(first, "radial-gradient", args)) {
        if (!parseRadialGradient(args, parsed.radial)) {
            return false;
        }
        parsed.kind = Mask::Kind::RadialGradient;
    } else if (functionArgs(first, "conic-gradient", args)) {
        if (!parseConicalGradient(args, parsed.conical)) {
            return false;
        }
        parsed.kind = Mask::Kind::ConicalGradient;
    } else if (const std::string_view href = unwrapUrl(first); !href.empty()) {
        parsed.kind = Mask::Kind::Url;
        parsed.href = std::string(href);
    } else {
        return false;
    }
    mask = parsed;
    return true;
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
        } else if (std::string_view args; functionArgs(entry, "conic-gradient", args)) {
            if (parseConicalGradient(args, image.conical)) {
                image.kind = ComputedStyle::BackgroundImage::ConicalGradientImage;
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

// `transform-origin` is two box-relative offsets plus an optional z length --
// a length, a percentage, or a keyword edge (`left`/`center`/`right` for x,
// `top`/`center`/`bottom` for y). One value leaves the other axis centred, and
// a lone vertical keyword still belongs to y rather than being pressed into x.
// Percentages and keywords are kept symbolic and resolved against the border
// box when the transform is applied. A third value resolves as a plain length
// (never a percentage). Anything unparsable keeps the 50% 50% default.
void applyTransformOrigin(std::string_view value, ComputedStyle& s) {
    const std::vector<std::string_view> parts = splitTokens(value);
    if (parts.empty() || parts.size() > 3) {
        return;
    }
    const auto axisValue = [](std::string_view token, bool vertical) -> Length {
        if (token == "left" || (vertical && token == "top")) {
            return Length{Length::Percent, 0.0f};
        }
        if (token == "right" || (vertical && token == "bottom")) {
            return Length{Length::Percent, 100.0f};
        }
        if (isCentre(token) || (!vertical && (token == "top" || token == "bottom"))) {
            return Length{Length::Percent, 50.0f};
        }
        const Length length = parseLength(token);
        return length.isAuto() ? Length{Length::Percent, 50.0f} : length;
    };
    // The z offset is a plain length; anything else (including a percentage)
    // drops the whole declaration, leaving even x and y alone.
    Length z;
    if (parts.size() == 3) {
        z = parseLength(parts[2]);
        if (z.isAuto() || z.unit == Length::Percent) {
            return;
        }
    }
    if (parts.size() == 1 && (parts[0] == "top" || parts[0] == "bottom")) {
        // A lone vertical keyword moves the origin up the box, x staying centred.
        s.transformOrigin.x = Length{Length::Percent, 50.0f};
        s.transformOrigin.y = axisValue(parts[0], true);
    } else {
        s.transformOrigin.x = axisValue(parts[0], false);
        s.transformOrigin.y = parts.size() > 1 ? axisValue(parts[1], true)
                                               : Length{Length::Percent, 50.0f};
    }
    if (parts.size() == 3) {
        s.transformOrigin.z = z;
    }
    s.specified |= ComputedStyle::SpecifiedTransformOrigin;
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

// A bare `repeat`/`no-repeat` word names one tiling axis, for the two-word
// `mask-repeat` the shorthand pairs up. `space`/`round` only stand alone.
bool repeatAxisValue(std::string_view token, bool& out) {
    if (token == "repeat") {
        out = true;
        return true;
    }
    if (token == "no-repeat") {
        out = false;
        return true;
    }
    return false;
}

bool isRepeatWord(std::string_view token) {
    return token == "repeat" || token == "no-repeat" || token == "repeat-x" ||
           token == "repeat-y" || token == "space" || token == "round";
}

// `mask` (and `-webkit-mask`): image || position [ / size ]? || repeat ||
// origin || clip || composite || mode, comma-separated per layer. Every layer
// pushes one entry to each longhand; omitted components take their initial
// values, so the shorthand resets whatever the longhands set. One box keyword
// sets both origin and clip, two set origin then clip, like the background. A
// size needs its position, and one bad token drops the whole declaration, like
// any failed parse.
bool parseMask(std::string_view value, ComputedStyle& s) {
    using Mask = ComputedStyle::MaskImage;
    using Mode = ComputedStyle::MaskMode;
    using Composite = ComputedStyle::MaskComposite;
    using Size = ComputedStyle::BackgroundSize;
    using Position = ComputedStyle::BackgroundPosition;
    using Repeat = ComputedStyle::RepeatPair;
    using Box = ComputedStyle::BackgroundBox;
    std::vector<Mask> images;
    std::vector<Mode> modes;
    std::vector<Composite> composites;
    std::vector<Size> sizes;
    std::vector<Position> positions;
    std::vector<Repeat> repeats;
    std::vector<Box> origins;
    std::vector<Box> clips;
    const auto isBox = [](std::string_view token, Box& out) {
        return parseBackgroundBox(token, out);
    };
    for (std::string_view commaLayer : splitTopLevel(value)) {
        const std::string_view text = trim(commaLayer);
        if (text.empty()) {
            return false;
        }
        // The slash separates position from size, paren-aware so a url's own
        // slashes never cut.
        const std::vector<std::string_view> halves = splitTopLevel(text, '/');
        if (halves.size() > 2) {
            return false;
        }
        Mask image;
        bool hasImage = false;
        std::string positionText;
        Size size;
        bool hasSize = false;
        std::string sizeText;
        Repeat repeat;
        bool hasRepeat = false;
        Mode mode = Mode::MatchSource;
        bool hasMode = false;
        Composite composite = Composite::Add;
        bool hasComposite = false;
        std::vector<Box> boxes;
        // One token stream walk for both halves: before the slash a bare
        // length is a position offset, after it a size. Anything else
        // classifies the same either way.
        for (std::size_t half = 0; half < halves.size(); ++half) {
            const bool inSize = half == 1;
            const std::vector<std::string_view> tokens = splitTokens(halves[half]);
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                const std::string_view token = tokens[i];
                if (token.empty()) {
                    continue;
                }
                Mask parsed;
                if (parseMaskImage(token, parsed) &&
                    (parsed.kind != Mask::Kind::None || token == "none")) {
                    // A gradient, url, or explicit `none` names the image.
                    // (`parseMaskImage` also swallows garbage as `None`, so
                    // only `none` counts from that path.)
                    if (hasImage) {
                        return false;
                    }
                    image = parsed;
                    hasImage = true;
                } else if (isRepeatWord(token)) {
                    if (hasRepeat) {
                        return false;
                    }
                    if (repeatAxisValue(token, repeat.x) && i + 1 < tokens.size() &&
                        repeatAxisValue(tokens[i + 1], repeat.y)) {
                        ++i;
                    } else if (!parseRepeatValue(token, repeat)) {
                        return false;
                    }
                    hasRepeat = true;
                } else if (token == "match-source") {
                    if (hasMode) {
                        return false;
                    }
                    mode = Mode::MatchSource;
                    hasMode = true;
                } else if (token == "alpha") {
                    if (hasMode) {
                        return false;
                    }
                    mode = Mode::Alpha;
                    hasMode = true;
                } else if (token == "luminance") {
                    if (hasMode) {
                        return false;
                    }
                    mode = Mode::Luminance;
                    hasMode = true;
                } else if (token == "add") {
                    if (hasComposite) {
                        return false;
                    }
                    composite = Composite::Add;
                    hasComposite = true;
                } else if (token == "subtract") {
                    if (hasComposite) {
                        return false;
                    }
                    composite = Composite::Subtract;
                    hasComposite = true;
                } else if (token == "intersect") {
                    if (hasComposite) {
                        return false;
                    }
                    composite = Composite::Intersect;
                    hasComposite = true;
                } else if (token == "exclude") {
                    if (hasComposite) {
                        return false;
                    }
                    composite = Composite::Exclude;
                    hasComposite = true;
                } else if (!inSize) {
                    Box box = ComputedStyle::BackgroundBorderBox;
                    if (isBox(token, box)) {
                        if (boxes.size() == 2) {
                            return false;
                        }
                        boxes.push_back(box);
                    } else if (token == "left" || token == "right" || token == "top" ||
                               token == "bottom" || isCentre(token) ||
                               !parseLength(token).isAuto()) {
                        if (!positionText.empty()) {
                            positionText += ' ';
                        }
                        positionText += std::string(token);
                    } else {
                        return false;
                    }
                } else {
                    // Past the slash a bare length is a size; anything else is
                    // invalid there.
                    if (token == "auto" || token == "cover" || token == "contain" ||
                        !parseLength(token).isAuto()) {
                        if (!sizeText.empty()) {
                            sizeText += ' ';
                        }
                        sizeText += std::string(token);
                    } else {
                        return false;
                    }
                }
            }
        }
        if (!sizeText.empty()) {
            if (!parseBackgroundSize(sizeText, size)) {
                return false;
            }
            hasSize = true;
        } else if (halves.size() == 2) {
            // A slash with nothing after it sizes nothing.
            return false;
        }
        if (hasSize && positionText.empty()) {
            // A size needs its position, like the background.
            return false;
        }
        Position position;
        if (!positionText.empty() && !parsePositionPair(positionText, position)) {
            return false;
        }
        images.push_back(hasImage ? image : Mask());
        modes.push_back(mode);
        composites.push_back(composite);
        sizes.push_back(size);
        positions.push_back(position);
        repeats.push_back(repeat);
        if (boxes.empty()) {
            origins.push_back(ComputedStyle::BackgroundBorderBox);
            clips.push_back(ComputedStyle::BackgroundBorderBox);
        } else if (boxes.size() == 1) {
            origins.push_back(boxes[0]);
            clips.push_back(boxes[0]);
        } else {
            origins.push_back(boxes[0]);
            clips.push_back(boxes[1]);
        }
    }
    if (images.empty()) {
        return false;
    }
    s.maskImages = std::move(images);
    s.maskModes = std::move(modes);
    s.maskComposites = std::move(composites);
    s.maskSizes = std::move(sizes);
    s.maskPositions = std::move(positions);
    s.maskRepeats = std::move(repeats);
    s.maskOrigins = std::move(origins);
    s.maskClips = std::move(clips);
    return true;
}

// A bare number with no unit, for the border-slice/width/outset values that
// count source px or border multiples instead of lengths. `parseLength` reads
// the same text as px, so this re-tags it.
bool parseBorderNumber(std::string_view token, Length& out) {
    const std::string_view text = trim(token);
    if (text.empty()) {
        return false;
    }
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        ++i;
    }
    std::size_t digits = 0;
    while (i < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.')) {
        ++i;
        ++digits;
    }
    if (digits == 0 || i != text.size()) {
        return false;
    }
    const float number =
        std::strtof(std::string(text.substr(0, i)).c_str(), nullptr);
    out = Length{Length::Number, number};
    return true;
}

// One-to-four side expansion: one value sets all four, two set vertical and
// horizontal, three add the bottom, four go clockwise from the top.
template <typename T>
void expandSides(const std::vector<T>& parts, T& top, T& right, T& bottom, T& left) {
    switch (parts.size()) {
        case 1:
            top = right = bottom = left = parts[0];
            break;
        case 2:
            top = bottom = parts[0];
            right = left = parts[1];
            break;
        case 3:
            top = parts[0];
            right = left = parts[1];
            bottom = parts[2];
            break;
        default:
            top = parts[0];
            right = parts[1];
            bottom = parts[2];
            left = parts[3];
            break;
    }
}

// `mask-border-slice`: 1-4 numbers/percentages plus an optional `fill`.
// Lengths are not slices, so anything else fails the declaration.
bool parseMaskBorderSlice(std::string_view value, ComputedStyle::MaskBorder::Slice& slice) {
    using Slice = ComputedStyle::MaskBorder::Slice;
    Slice out;
    std::vector<Length> parts;
    for (std::string_view token : splitTokens(value)) {
        if (token.empty()) {
            continue;
        }
        if (token == "fill") {
            out.fill = true;
            continue;
        }
        Length length;
        if (parseBorderNumber(token, length)) {
            // A bare number counts source px.
        } else {
            length = parseLength(token);
            if (length.unit != Length::Percent) {
                return false;
            }
        }
        if (length.unit == Length::Number && length.value < 0.0f) {
            return false;
        }
        if (length.unit == Length::Percent &&
            (length.value < 0.0f || length.value > 100.0f)) {
            return false;
        }
        parts.push_back(length);
    }
    if (parts.empty() || parts.size() > 4) {
        return false;
    }
    expandSides(parts, out.top, out.right, out.bottom, out.left);
    slice = out;
    return true;
}

// `mask-border-width`: 1-4 lengths, border multiples, or `auto`.
bool parseMaskBorderWidth(std::string_view value, ComputedStyle::MaskBorder::Width& width) {
    using Width = ComputedStyle::MaskBorder::Width;
    Width out;
    std::vector<Length> parts;
    for (std::string_view token : splitTokens(value)) {
        if (token.empty()) {
            continue;
        }
        Length length;
        if (token == "auto") {
            length = Length{};
        } else if (parseBorderNumber(token, length)) {
            // A bare number multiplies the border thickness.
        } else {
            length = parseLength(token);
            if (length.unit != Length::Px && length.unit != Length::Em &&
                length.unit != Length::Rem && length.unit != Length::Calc) {
                return false;
            }
        }
        if ((length.unit == Length::Px || length.unit == Length::Number) &&
            length.value < 0.0f) {
            return false;
        }
        parts.push_back(length);
    }
    if (parts.empty() || parts.size() > 4) {
        return false;
    }
    expandSides(parts, out.top, out.right, out.bottom, out.left);
    width = out;
    return true;
}

// `mask-border-outset`: 1-4 lengths or border multiples, never negative.
bool parseMaskBorderOutset(std::string_view value,
                           ComputedStyle::MaskBorder::Outset& outset) {
    using Outset = ComputedStyle::MaskBorder::Outset;
    Outset out;
    std::vector<Length> parts;
    for (std::string_view token : splitTokens(value)) {
        if (token.empty()) {
            continue;
        }
        Length length;
        if (parseBorderNumber(token, length)) {
            // A bare number multiplies the border thickness.
        } else {
            length = parseLength(token);
            if (length.unit != Length::Px && length.unit != Length::Em &&
                length.unit != Length::Rem && length.unit != Length::Calc) {
                return false;
            }
        }
        if ((length.unit == Length::Px || length.unit == Length::Number) &&
            length.value < 0.0f) {
            return false;
        }
        parts.push_back(length);
    }
    if (parts.empty() || parts.size() > 4) {
        return false;
    }
    expandSides(parts, out.top, out.right, out.bottom, out.left);
    outset = out;
    return true;
}

// `mask-border-repeat`: one or two of stretch/repeat/round/space.
bool parseMaskBorderRepeat(std::string_view value, ComputedStyle::MaskBorder& border) {
    using Repeat = ComputedStyle::MaskBorder::Repeat;
    const auto word = [](std::string_view token, Repeat& out) {
        if (token == "stretch") {
            out = Repeat::Stretch;
        } else if (token == "repeat") {
            out = Repeat::Repeat;
        } else if (token == "round") {
            out = Repeat::Round;
        } else if (token == "space") {
            out = Repeat::Space;
        } else {
            return false;
        }
        return true;
    };
    std::vector<std::string_view> tokens;
    for (std::string_view token : splitTokens(value)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    if (tokens.empty() || tokens.size() > 2) {
        return false;
    }
    Repeat first = Repeat::Stretch;
    Repeat second = Repeat::Stretch;
    if (!word(tokens[0], first)) {
        return false;
    }
    second = first;
    if (tokens.size() == 2 && !word(tokens[1], second)) {
        return false;
    }
    border.repeatX = first;
    border.repeatY = second;
    return true;
}

// `mask-border`: source || slice [ / width [ / outset ] ]? || repeat || mode.
// A single declaration; one bad token drops it all.
bool parseMaskBorder(std::string_view value, ComputedStyle& s) {
    using Border = ComputedStyle::MaskBorder;
    Border border;
    bool hasSource = false;
    std::string sliceText;
    std::string widthText;
    std::string outsetText;
    std::string repeatText;
    bool hasMode = false;
    // Slashes separate slice from width from outset, paren-aware so a url's
    // own slashes never cut.
    const std::vector<std::string_view> sections = splitTopLevel(value, '/');
    if (sections.empty() || sections.size() > 3) {
        return false;
    }
    bool seenSlashWidth = false;
    for (std::size_t section = 0; section < sections.size(); ++section) {
        for (std::string_view token : splitTokens(sections[section])) {
            if (token.empty()) {
                continue;
            }
            ComputedStyle::MaskImage source;
            if (parseMaskImage(token, source) &&
                (source.kind != ComputedStyle::MaskImage::Kind::None || token == "none")) {
                if (section != 0 || hasSource) {
                    return false;
                }
                border.source = source;
                hasSource = true;
            } else if (token == "stretch" || token == "repeat" || token == "round" ||
                       token == "space") {
                if (!repeatText.empty()) {
                    repeatText += ' ';
                }
                repeatText += std::string(token);
            } else if (token == "luminance" || token == "alpha") {
                if (hasMode) {
                    return false;
                }
                border.mode = token == "luminance" ? Border::Mode::Luminance
                                                  : Border::Mode::Alpha;
                hasMode = true;
            } else if (section == 0 && token == "fill") {
                if (!sliceText.empty()) {
                    sliceText += ' ';
                }
                sliceText += "fill";
            } else if (section == 0) {
                Length number;
                const Length length = parseLength(token);
                if (parseBorderNumber(token, number)) {
                    // A bare number slices source px.
                    if (!sliceText.empty()) {
                        sliceText += ' ';
                    }
                    sliceText += std::string(token);
                } else if (length.unit == Length::Percent) {
                    if (!sliceText.empty()) {
                        sliceText += ' ';
                    }
                    sliceText += std::string(token);
                } else {
                    // Lengths never slice, and nothing else belongs here.
                    return false;
                }
            } else if (section == 1) {
                if (!widthText.empty()) {
                    widthText += ' ';
                }
                widthText += std::string(token);
                seenSlashWidth = true;
            } else if (section == 2) {
                if (!outsetText.empty()) {
                    outsetText += ' ';
                }
                outsetText += std::string(token);
            } else {
                return false;
            }
        }
    }
    if (!sliceText.empty() && !parseMaskBorderSlice(sliceText, border.slice)) {
        return false;
    }
    if (sections.size() >= 2 && widthText.empty()) {
        // A slash with nothing after it widths nothing.
        return false;
    }
    if (seenSlashWidth && !parseMaskBorderWidth(widthText, border.width)) {
        return false;
    }
    if (sections.size() == 3) {
        if (outsetText.empty()) {
            return false;
        }
        if (!parseMaskBorderOutset(outsetText, border.outset)) {
            return false;
        }
    }
    if (!repeatText.empty() && !parseMaskBorderRepeat(repeatText, border)) {
        return false;
    }
    if (!hasSource && sliceText.empty() && !seenSlashWidth && sections.size() < 3 &&
        repeatText.empty() && !hasMode) {
        return false;
    }
    s.maskBorder = border;
    return true;
}

// `border-image-slice`: like the mask slice, but the middle `fill` keeps the
// centre painted instead of masking it.
bool parseBorderImageSlice(std::string_view value,
                           ComputedStyle::BorderImage::Slice& slice) {
    using Slice = ComputedStyle::BorderImage::Slice;
    Slice out;
    std::vector<Length> parts;
    for (std::string_view token : splitTokens(value)) {
        if (token.empty()) {
            continue;
        }
        if (token == "fill") {
            out.fill = true;
            continue;
        }
        Length length;
        if (parseBorderNumber(token, length)) {
            // A bare number counts source px.
        } else {
            length = parseLength(token);
            if (length.unit != Length::Percent) {
                return false;
            }
        }
        if (length.unit == Length::Number && length.value < 0.0f) {
            return false;
        }
        if (length.unit == Length::Percent &&
            (length.value < 0.0f || length.value > 100.0f)) {
            return false;
        }
        parts.push_back(length);
    }
    if (parts.empty() || parts.size() > 4) {
        return false;
    }
    expandSides(parts, out.top, out.right, out.bottom, out.left);
    slice = out;
    return true;
}

// `border-image-width`: lengths, border multiples, or `auto` for the slice's
// own size.
bool parseBorderImageWidth(std::string_view value,
                           ComputedStyle::BorderImage::Width& width) {
    using Width = ComputedStyle::BorderImage::Width;
    Width out;
    std::vector<Length> parts;
    for (std::string_view token : splitTokens(value)) {
        if (token.empty()) {
            continue;
        }
        Length length;
        if (token == "auto") {
            length = Length{};
        } else if (parseBorderNumber(token, length)) {
            // A bare number multiplies the border thickness.
        } else {
            length = parseLength(token);
            if (length.unit != Length::Px && length.unit != Length::Em &&
                length.unit != Length::Rem && length.unit != Length::Calc) {
                return false;
            }
        }
        if ((length.unit == Length::Px || length.unit == Length::Number) &&
            length.value < 0.0f) {
            return false;
        }
        parts.push_back(length);
    }
    if (parts.empty() || parts.size() > 4) {
        return false;
    }
    expandSides(parts, out.top, out.right, out.bottom, out.left);
    width = out;
    return true;
}

// `border-image-outset`: lengths or border multiples, never negative.
bool parseBorderImageOutset(std::string_view value,
                            ComputedStyle::BorderImage::Outset& outset) {
    using Outset = ComputedStyle::BorderImage::Outset;
    Outset out;
    std::vector<Length> parts;
    for (std::string_view token : splitTokens(value)) {
        if (token.empty()) {
            continue;
        }
        Length length;
        if (parseBorderNumber(token, length)) {
            // A bare number multiplies the border thickness.
        } else {
            length = parseLength(token);
            if (length.unit != Length::Px && length.unit != Length::Em &&
                length.unit != Length::Rem && length.unit != Length::Calc) {
                return false;
            }
        }
        if ((length.unit == Length::Px || length.unit == Length::Number) &&
            length.value < 0.0f) {
            return false;
        }
        parts.push_back(length);
    }
    if (parts.empty() || parts.size() > 4) {
        return false;
    }
    expandSides(parts, out.top, out.right, out.bottom, out.left);
    outset = out;
    return true;
}

// `border-image-repeat`: one or two of stretch/repeat/round/space.
bool parseBorderImageRepeat(std::string_view value, ComputedStyle::BorderImage& image) {
    using Repeat = ComputedStyle::BorderImage::Repeat;
    const auto word = [](std::string_view token, Repeat& out) {
        if (token == "stretch") {
            out = Repeat::Stretch;
        } else if (token == "repeat") {
            out = Repeat::Repeat;
        } else if (token == "round") {
            out = Repeat::Round;
        } else if (token == "space") {
            out = Repeat::Space;
        } else {
            return false;
        }
        return true;
    };
    std::vector<std::string_view> tokens;
    for (std::string_view token : splitTokens(value)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    if (tokens.empty() || tokens.size() > 2) {
        return false;
    }
    Repeat first = Repeat::Stretch;
    Repeat second = Repeat::Stretch;
    if (!word(tokens[0], first)) {
        return false;
    }
    second = first;
    if (tokens.size() == 2 && !word(tokens[1], second)) {
        return false;
    }
    image.repeatX = first;
    image.repeatY = second;
    return true;
}

// One `border-image-source`: a url, a gradient, or `none`.
bool parseBorderImageSource(std::string_view value,
                            ComputedStyle::BackgroundImage& source) {
    using Image = ComputedStyle::BackgroundImage;
    const std::string_view text = trim(value);
    if (text.empty() || text == "none") {
        source = Image();
        return true;
    }
    Image parsed;
    if (const std::string_view href = unwrapUrl(text); !href.empty()) {
        parsed.kind = Image::Url;
        parsed.href = std::string(href);
    } else if (std::string_view args; functionArgs(text, "linear-gradient", args)) {
        if (!parseLinearGradient(args, parsed.linear)) {
            return false;
        }
        parsed.kind = Image::LinearGradientImage;
    } else if (functionArgs(text, "radial-gradient", args)) {
        if (!parseRadialGradient(args, parsed.radial)) {
            return false;
        }
        parsed.kind = Image::RadialGradientImage;
    } else if (functionArgs(text, "conic-gradient", args)) {
        if (!parseConicalGradient(args, parsed.conical)) {
            return false;
        }
        parsed.kind = Image::ConicalGradientImage;
    } else {
        return false;
    }
    source = parsed;
    return true;
}

// `border-image`: source || slice [ / width [ / outset ] ]? || repeat. A
// single declaration; one bad token drops it all.
bool parseBorderImage(std::string_view value, ComputedStyle& s) {
    using Image = ComputedStyle::BorderImage;
    Image image;
    bool hasSource = false;
    std::string sliceText;
    std::string widthText;
    std::string outsetText;
    std::string repeatText;
    // Slashes separate slice from width from outset, paren-aware so a url's
    // own slashes never cut.
    const std::vector<std::string_view> sections = splitTopLevel(value, '/');
    if (sections.empty() || sections.size() > 3) {
        return false;
    }
    bool seenSlashWidth = false;
    for (std::size_t section = 0; section < sections.size(); ++section) {
        for (std::string_view token : splitTokens(sections[section])) {
            if (token.empty()) {
                continue;
            }
            ComputedStyle::BackgroundImage source;
            if (parseBorderImageSource(token, source) &&
                (source.kind != ComputedStyle::BackgroundImage::None || token == "none")) {
                if (section != 0 || hasSource) {
                    return false;
                }
                image.source = source;
                hasSource = true;
            } else if (token == "stretch" || token == "repeat" || token == "round" ||
                       token == "space") {
                if (!repeatText.empty()) {
                    repeatText += ' ';
                }
                repeatText += std::string(token);
            } else if (section == 0 && token == "fill") {
                if (!sliceText.empty()) {
                    sliceText += ' ';
                }
                sliceText += "fill";
            } else if (section == 0) {
                Length number;
                const Length length = parseLength(token);
                if (parseBorderNumber(token, number) || length.unit == Length::Percent) {
                    if (!sliceText.empty()) {
                        sliceText += ' ';
                    }
                    sliceText += std::string(token);
                } else {
                    // Lengths never slice, and nothing else belongs here.
                    return false;
                }
            } else if (section == 1) {
                if (!widthText.empty()) {
                    widthText += ' ';
                }
                widthText += std::string(token);
                seenSlashWidth = true;
            } else if (section == 2) {
                if (!outsetText.empty()) {
                    outsetText += ' ';
                }
                outsetText += std::string(token);
            } else {
                return false;
            }
        }
    }
    if (!sliceText.empty() && !parseBorderImageSlice(sliceText, image.slice)) {
        return false;
    }
    if (sections.size() >= 2 && widthText.empty()) {
        // A slash with nothing after it widths nothing.
        return false;
    }
    if (seenSlashWidth && !parseBorderImageWidth(widthText, image.width)) {
        return false;
    }
    if (sections.size() == 3) {
        if (outsetText.empty()) {
            return false;
        }
        if (!parseBorderImageOutset(outsetText, image.outset)) {
            return false;
        }
    }
    if (!repeatText.empty() && !parseBorderImageRepeat(repeatText, image)) {
        return false;
    }
    if (!hasSource && sliceText.empty() && !seenSlashWidth && sections.size() < 3 &&
        repeatText.empty()) {
        return false;
    }
    s.borderImage = image;
    return true;
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

// Custom properties start with `--`; nothing else in the engine's property set
// does, so the prefix is a reliable discriminator.
bool isCustomProperty(std::string_view property) {
    return property.size() > 2 && property[0] == '-' && property[1] == '-';
}

bool containsVars(std::string_view value) {
    return value.find("var(") != std::string_view::npos;
}

// `var(--name)` and `var(--name, fallback)` resolve against the element's custom
// properties. Values are raw tokens, so a lookup recurses: `--a: var(--b)` only
// produces a value when `--b` itself resolves. `fallback` is resolved the same
// way, so `var(--x, var(--y))` works. Returns false when a name is undefined and
// has no fallback, or a substitution chain never settles — both make the whole
// declaration invalid at computed-value time, per CSS, and the caller drops it.
bool resolveVars(std::string_view in,
                 const std::map<std::string, std::string, std::less<>>& custom,
                 std::string& out, const int depth) {
    std::size_t pos = 0;
    while (true) {
        const std::size_t var = in.find("var(", pos);
        if (var == std::string_view::npos) {
            out.append(in.substr(pos));
            return true;
        }
        out.append(in.substr(pos, var - pos));

        // Match the closing paren with a balance count, so the comma-split below
        // never sees a fallback like `var(--x, calc(a + b))` cut at its first `)`.
        std::size_t end = var + 4;
        int parens = 1;
        while (end < in.size() && parens > 0) {
            if (in[end] == '(') {
                ++parens;
            } else if (in[end] == ')') {
                --parens;
            }
            if (parens > 0) {
                ++end;
            }
        }
        if (parens != 0) {
            // Unbalanced `var(`. A declaration like this is a parse-time error in
            // CSS and is never applied; keep the remainder so nothing is lost.
            out.append(in.substr(var + 4));
            return true;
        }

        const std::string_view args = in.substr(var + 4, end - (var + 4));
        const std::size_t comma = args.find(',');
        const std::string_view name = trim(comma == std::string_view::npos ? args
                                                                           : args.substr(0, comma));
        if (!isCustomProperty(name)) {
            // `var()` requires a custom-property name; anything else is a parse
            // error, which invalidates the whole declaration.
            return false;
        }

        const auto found = custom.find(std::string(name));
        if (found != custom.end()) {
            if (depth > 8) {
                // `--a: var(--b); --b: var(--a)` never settles. Reported once, and
                // the declaration drops rather than looping forever.
                static bool warned = false;
                if (!warned) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "custom property substitution exceeds depth; "
                                "the declaration is dropped (a --a: var(--b); --b: var(--a) cycle?)");
                    warned = true;
                }
                return false;
            }
            if (!resolveVars(found->second, custom, out, depth + 1)) {
                return false;
            }
        } else if (comma != std::string_view::npos) {
            std::string fallback;
            if (!resolveVars(trim(args.substr(comma + 1)), custom, fallback, depth + 1)) {
                return false;
            }
            out.append(fallback);
        } else {
            // Undefined and no fallback: the declaration is invalid at
            // computed-value time and is dropped. Told about once per name.
            static std::set<std::string, std::less<>> reported;
            if (reported.emplace(name).second) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "var(--%.*s) has no value and no fallback; the declaration is dropped",
                            static_cast<int>(name.size() - 2), name.data() + 2);
            }
            return false;
        }
        pos = end + 1;
    }
}

// The flag is derived, not authored: whoever hands the resolver a rule gets the
// right answer, including a rule built by hand rather than parsed.
void noteCustomUsage(Rule& rule) {
    for (const auto& decl : rule.declarations) {
        if (isCustomProperty(decl.property) || containsVars(decl.value)) {
            rule.usesCustom = true;
            return;
        }
    }
    rule.usesCustom = false;
}


// The CSS cursor keyword set lives in CSS/Style.h so the application layer can
// reuse the same enum-to-keyword mapping when it hands the shape to the platform.

// --- clip-path parsing ---

// Splits `name(...) rest` into its arguments and the trimmed text after the
// closing paren. False when the text is not that function.
bool splitCall(std::string_view text, std::string_view name, std::string_view& args,
               std::string_view& rest) {
    if (text.size() <= name.size() + 2 || text.compare(0, name.size(), name) != 0 ||
        text[name.size()] != '(') {
        return false;
    }
    int depth = 0;
    for (std::size_t i = name.size(); i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            if (--depth == 0) {
                args = text.substr(name.size() + 1, i - name.size() - 1);
                rest = trim(text.substr(i + 1));
                return true;
            }
        }
    }
    return false;
}

// A strict length: anything parseLength reads as auto (garbage, `auto`,
// `none`, empty) is not a length here, so a typo cannot silently clip
// everything away.
bool parseClipLength(std::string_view token, Length& out) {
    const Length length = parseLength(token);
    if (length.unit == Length::Auto) {
        return false;
    }
    out = length;
    return true;
}

bool parseClipBox(std::string_view text, ComputedStyle::ClipPath::Box& box) {
    using Box = ComputedStyle::ClipPath::Box;
    if (text.empty()) {
        return true;
    }
    if (text == "margin-box") {
        box = Box::Margin;
    } else if (text == "border-box") {
        box = Box::Border;
    } else if (text == "padding-box") {
        box = Box::Padding;
    } else if (text == "content-box") {
        box = Box::Content;
    } else if (text == "fill-box" || text == "stroke-box" || text == "view-box") {
        // SVG viewport boxes behave as the border box on HTML elements.
        box = Box::Border;
    } else {
        return false;
    }
    return true;
}

// Finds a whole word at paren depth zero, for the `round` and `at` keywords
// hiding among lengths.
std::size_t findTopLevelWord(std::string_view text, std::string_view word) {
    const auto isNameChar = [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) != 0 || c == '-' || c == '_';
    };
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            depth = depth > 0 ? depth - 1 : 0;
        } else if (depth == 0 && i + word.size() <= text.size() &&
                   text.compare(i, word.size(), word) == 0 &&
                   (i == 0 || !isNameChar(text[i - 1])) &&
                   (i + word.size() >= text.size() || !isNameChar(text[i + word.size()]))) {
            return i;
        }
    }
    return std::string_view::npos;
}

// One or two position tokens (`left`, `top`, `center`, lengths) into x/y.
// `center` and lengths fill the missing axis, so `at left` centres vertically.
bool parseClipPosition(const std::vector<std::string_view>& tokens, Length& x, Length& y) {
    if (tokens.empty() || tokens.size() > 2) {
        return false;
    }
    const Length centre(Length::Percent, 50.0f);
    bool haveX = false;
    bool haveY = false;
    for (const std::string_view token : tokens) {
        if (token == "left") {
            if (haveX) {
                return false;
            }
            x = Length(Length::Percent, 0.0f);
            haveX = true;
        } else if (token == "right") {
            if (haveX) {
                return false;
            }
            x = Length(Length::Percent, 100.0f);
            haveX = true;
        } else if (token == "top") {
            if (haveY) {
                return false;
            }
            y = Length(Length::Percent, 0.0f);
            haveY = true;
        } else if (token == "bottom") {
            if (haveY) {
                return false;
            }
            y = Length(Length::Percent, 100.0f);
            haveY = true;
        } else if (token == "center") {
            if (!haveX) {
                x = centre;
                haveX = true;
            } else if (!haveY) {
                y = centre;
                haveY = true;
            } else {
                return false;
            }
        } else {
            Length length;
            if (!parseClipLength(token, length)) {
                return false;
            }
            if (!haveX) {
                x = length;
                haveX = true;
            } else if (!haveY) {
                y = length;
                haveY = true;
            } else {
                return false;
            }
        }
    }
    if (!haveX) {
        x = centre;
    }
    if (!haveY) {
        y = centre;
    }
    return true;
}

bool parseClipRadial(std::string_view token, ComputedStyle::ClipPath::Radial& radial,
                     Length& length) {
    using Radial = ComputedStyle::ClipPath::Radial;
    if (token == "closest-side") {
        radial = Radial::ClosestSide;
    } else if (token == "farthest-side") {
        radial = Radial::FarthestSide;
    } else if (token == "closest-corner") {
        radial = Radial::ClosestCorner;
    } else if (token == "farthest-corner") {
        radial = Radial::FarthestCorner;
    } else if (parseClipLength(token, length)) {
        radial = Radial::Explicit;
    } else {
        return false;
    }
    return true;
}

bool parseClipInset(std::string_view args, std::string_view rest,
                    ComputedStyle::ClipPath& clip) {
    using CP = ComputedStyle::ClipPath;
    std::string_view main = args;
    std::string_view round;
    const std::size_t at = findTopLevelWord(args, "round");
    if (at != std::string_view::npos) {
        main = trim(args.substr(0, at));
        round = trim(args.substr(at + 5));
    }
    const std::vector<std::string_view> sides = splitTopLevelSpaces(main);
    if (sides.empty() || sides.size() > 4) {
        return false;
    }
    Length parsed[4];
    for (std::size_t i = 0; i < sides.size(); ++i) {
        if (!parseClipLength(sides[i], parsed[i])) {
            return false;
        }
    }
    Length radius;
    if (!round.empty() && !parseClipLength(round, radius)) {
        return false;
    }
    CP box;
    box.shape = CP::Shape::Inset;
    box.inset[0] = parsed[0];
    box.inset[1] = sides.size() > 1 ? parsed[1] : parsed[0];
    box.inset[2] = sides.size() > 2 ? parsed[2] : parsed[0];
    box.inset[3] = sides.size() > 3 ? parsed[3] : box.inset[1];
    box.insetRadius = radius;
    if (!parseClipBox(rest, box.box)) {
        return false;
    }
    clip = box;
    return true;
}

bool parseClipCircle(std::string_view args, std::string_view rest,
                     ComputedStyle::ClipPath& clip) {
    using CP = ComputedStyle::ClipPath;
    const std::size_t at = findTopLevelWord(args, "at");
    const std::string_view radiusPart = trim(at == std::string_view::npos
                                                 ? args
                                                 : args.substr(0, at));
    const std::string_view posPart =
        at == std::string_view::npos ? std::string_view{} : trim(args.substr(at + 2));
    CP circle;
    circle.shape = CP::Shape::Circle;
    if (!radiusPart.empty()) {
        const std::vector<std::string_view> tokens = splitTopLevelSpaces(radiusPart);
        if (tokens.size() != 1 ||
            !parseClipRadial(tokens[0], circle.radius, circle.radiusLength)) {
            return false;
        }
    } else {
        // An omitted radius inscribes the box.
        circle.radius = CP::Radial::ClosestSide;
    }
    if (at != std::string_view::npos &&
        !parseClipPosition(splitTopLevelSpaces(posPart), circle.centerX, circle.centerY)) {
        return false;
    }
    if (!parseClipBox(rest, circle.box)) {
        return false;
    }
    clip = circle;
    return true;
}

bool parseClipEllipse(std::string_view args, std::string_view rest,
                      ComputedStyle::ClipPath& clip) {
    using CP = ComputedStyle::ClipPath;
    const std::size_t at = findTopLevelWord(args, "at");
    const std::string_view radiusPart = trim(at == std::string_view::npos
                                                 ? args
                                                 : args.substr(0, at));
    const std::string_view posPart =
        at == std::string_view::npos ? std::string_view{} : trim(args.substr(at + 2));
    CP ellipse;
    ellipse.shape = CP::Shape::Ellipse;
    if (!radiusPart.empty()) {
        const std::vector<std::string_view> tokens = splitTopLevelSpaces(radiusPart);
        if (tokens.size() != 2 ||
            !parseClipRadial(tokens[0], ellipse.radiusX, ellipse.radiusXLength) ||
            !parseClipRadial(tokens[1], ellipse.radiusY, ellipse.radiusYLength)) {
            return false;
        }
    } else {
        ellipse.radiusX = CP::Radial::ClosestSide;
        ellipse.radiusY = CP::Radial::ClosestSide;
    }
    if (at != std::string_view::npos &&
        !parseClipPosition(splitTopLevelSpaces(posPart), ellipse.centerX,
                           ellipse.centerY)) {
        return false;
    }
    if (!parseClipBox(rest, ellipse.box)) {
        return false;
    }
    clip = ellipse;
    return true;
}

bool parseClipPolygon(std::string_view args, std::string_view rest,
                      ComputedStyle::ClipPath& clip) {
    using CP = ComputedStyle::ClipPath;
    const std::vector<std::string_view> parts = splitTopLevelCommas(args);
    if (parts.empty()) {
        return false;
    }
    CP polygon;
    polygon.shape = CP::Shape::Polygon;
    std::size_t first = 0;
    const std::string_view head = trim(parts[0]);
    if (head == "evenodd" || head == "nonzero") {
        polygon.evenOdd = head == "evenodd";
        first = 1;
    }
    for (std::size_t i = first; i < parts.size(); ++i) {
        const std::vector<std::string_view> xy = splitTopLevelSpaces(trim(parts[i]));
        if (xy.size() != 2) {
            return false;
        }
        CP::Point point;
        if (!parseClipLength(xy[0], point.x) || !parseClipLength(xy[1], point.y)) {
            return false;
        }
        polygon.points.push_back(point);
    }
    if (polygon.points.size() < 3) {
        return false;
    }
    if (!parseClipBox(rest, polygon.box)) {
        return false;
    }
    clip = polygon;
    return true;
}

bool parseClipPath(std::string_view value, ComputedStyle::ClipPath& clip) {
    using CP = ComputedStyle::ClipPath;
    const std::string_view text = trim(value);
    if (text == "none") {
        clip = CP();
        return true;
    }
    std::string_view args;
    std::string_view rest;
    if (splitCall(text, "inset", args, rest)) {
        return parseClipInset(args, rest, clip);
    }
    if (splitCall(text, "circle", args, rest)) {
        return parseClipCircle(args, rest, clip);
    }
    if (splitCall(text, "ellipse", args, rest)) {
        return parseClipEllipse(args, rest, clip);
    }
    if (splitCall(text, "polygon", args, rest)) {
        return parseClipPolygon(args, rest, clip);
    }
    // A bare geometry box crops to itself, like `inset(0)` against that box.
    CP::Box box = CP::Box::Border;
    if (!text.empty() && parseClipBox(text, box)) {
        clip = CP();
        clip.shape = CP::Shape::Inset;
        clip.box = box;
        return true;
    }
    return false;
}

// A `border`-style value list: one width, style, and colour at most, in any
// order. The `has*` flags record which slots the value actually named, because
// the shorthands write only those, exactly like `border` itself does.
struct BorderSlots {
    bool hasWidth = false;
    Length width;
    bool hasStyle = false;
    ComputedStyle::BorderStyle style = ComputedStyle::BorderSolid;
    bool hasColor = false;
    SDL_Color color{};
};

// Reads a per-side `border` value. An unknown token makes the whole declaration
// invalid, so it returns false and the caller drops it rather than half-apply a
// border nobody asked for; the partially filled `out` is ignored then.
bool parseBorderSlots(std::string_view value, BorderSlots& out) {
    bool any = false;
    for (const std::string_view part : splitTopLevel(value, ' ')) {
        if (part == "thin" || part == "medium" || part == "thick" || isLengthToken(part)) {
            out.width = parseBorderWidth(part);
            out.hasWidth = true;
        } else if (ComputedStyle::BorderStyle parsed{};
                   parseBorderStyle(part, parsed)) {
            out.style = parsed;
            out.hasStyle = true;
        } else if (SDL_Color parsed{}; parseColorValue(part, parsed)) {
            out.color = parsed;
            out.hasColor = true;
        } else {
            return false;
        }
        any = true;
    }
    return any;
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
        } else if (value == "grid") {
            s.display = ComputedStyle::Grid;
        } else if (value == "inline-grid") {
            s.display = ComputedStyle::InlineGrid;
        } else if (value == "table") {
            s.display = ComputedStyle::Table;
        } else if (value == "inline-table") {
            s.display = ComputedStyle::InlineTable;
        } else if (value == "table-row") {
            s.display = ComputedStyle::TableRow;
        } else if (value == "table-cell") {
            s.display = ComputedStyle::TableCell;
        } else {
            s.display = ComputedStyle::Block;
        }
    } else if (property == "grid-template-columns") {
        s.gridColumnsAutoFill = false;
        s.gridColumnsAutoFit = false;
        s.gridColumns = parseGridTrackList(value, &s.gridColumnsAutoFill,
                                          &s.gridColumnsAutoFit);
    } else if (property == "grid-template-rows") {
        s.gridRowsAutoFill = false;
        s.gridRowsAutoFit = false;
        s.gridRows = parseGridTrackList(value, &s.gridRowsAutoFill, &s.gridRowsAutoFit);
    } else if (property == "grid-auto-rows") {
        ComputedStyle::GridTrack t;
        if (parseGridTrack(trim(value), t)) {
            s.gridAutoRows = t;
            s.hasGridAutoRows = true;
        }
    } else if (property == "grid-auto-columns") {
        ComputedStyle::GridTrack t;
        if (parseGridTrack(trim(value), t)) {
            s.gridAutoColumns = t;
            s.hasGridAutoColumns = true;
        }
    } else if (property == "grid-auto-flow") {
        const std::string_view val = trim(value);
        if (val == "column" || val == "column nowrap") {
            s.gridAutoFlow = ComputedStyle::AutoFlowColumn;
        } else if (val == "row dense" || val == "dense") {
            s.gridAutoFlow = ComputedStyle::AutoFlowRowDense;
        } else if (val == "column dense") {
            s.gridAutoFlow = ComputedStyle::AutoFlowColumnDense;
        } else {
            s.gridAutoFlow = ComputedStyle::AutoFlowRow;
        }
    } else if (property == "grid-template-areas") {
        s.gridTemplateAreas.clear();
        s.gridTemplateAreasColumns = 0;
        s.gridTemplateAreasRows = 0;
        s.gridTemplateAreasRaw = std::string(trim(value));
        parseGridTemplateAreas(value, s.gridTemplateAreas,
                               s.gridTemplateAreasColumns, s.gridTemplateAreasRows);
    } else if (property == "grid-column") {
        // `grid-column: <start> / <end>` or `grid-column: span <n>`.
        // Parse both halves so an explicit start line is captured.
        s.gridColumnStart = 0;
        s.gridColumnEnd   = 0;
        s.gridColumnSpan  = 1;
        const std::size_t slash = value.find('/');
        if (slash != std::string_view::npos) {
            const std::string_view startPart = value.substr(0, slash);
            const std::string_view endPart   = value.substr(slash + 1);
            int spanFromStart = 1;
            parseGridLinePart(startPart, s.gridColumnStart, spanFromStart);
            int spanFromEnd = 1;
            parseGridLinePart(endPart,   s.gridColumnEnd,   spanFromEnd);
            // Derive the canonical span.
            if (s.gridColumnStart > 0 && s.gridColumnEnd > 0 &&
                s.gridColumnEnd > s.gridColumnStart) {
                s.gridColumnSpan = s.gridColumnEnd - s.gridColumnStart;
            } else if (s.gridColumnStart > 0 && s.gridColumnEnd == 0) {
                s.gridColumnSpan = spanFromEnd;  // `2 / span 3` -> span 3 from line 2
            } else if (s.gridColumnStart == 0 && s.gridColumnEnd > 0) {
                s.gridColumnSpan = spanFromStart; // `span 2 / 4` -> span 2 ending at 4
            } else {
                s.gridColumnSpan = parseGridSpan(value);
            }
        } else {
            // Single value: may be a bare line number or `span n`.
            parseGridLinePart(value, s.gridColumnStart, s.gridColumnSpan);
        }
    } else if (property == "grid-column-start") {
        int span = 1;
        parseGridLinePart(value, s.gridColumnStart, span);
        if (s.gridColumnStart == 0) {
            s.gridColumnSpan = span;  // `span n` form: store in span, clear start
        } else if (s.gridColumnEnd > s.gridColumnStart) {
            s.gridColumnSpan = s.gridColumnEnd - s.gridColumnStart;
        }
    } else if (property == "grid-column-end") {
        int span = 1;
        parseGridLinePart(value, s.gridColumnEnd, span);
        if (s.gridColumnEnd > 0 && s.gridColumnStart > 0 && s.gridColumnEnd > s.gridColumnStart) {
            s.gridColumnSpan = s.gridColumnEnd - s.gridColumnStart;
        }
    } else if (property == "grid-row") {
        // Same logic as grid-column, mirrored for rows.
        s.gridRowStart = 0;
        s.gridRowEnd   = 0;
        s.gridRowSpan  = 1;
        const std::size_t slash = value.find('/');
        if (slash != std::string_view::npos) {
            const std::string_view startPart = value.substr(0, slash);
            const std::string_view endPart   = value.substr(slash + 1);
            int spanFromStart = 1;
            parseGridLinePart(startPart, s.gridRowStart, spanFromStart);
            int spanFromEnd = 1;
            parseGridLinePart(endPart,   s.gridRowEnd,   spanFromEnd);
            if (s.gridRowStart > 0 && s.gridRowEnd > 0 &&
                s.gridRowEnd > s.gridRowStart) {
                s.gridRowSpan = s.gridRowEnd - s.gridRowStart;
            } else if (s.gridRowStart > 0 && s.gridRowEnd == 0) {
                s.gridRowSpan = spanFromEnd;
            } else if (s.gridRowStart == 0 && s.gridRowEnd > 0) {
                s.gridRowSpan = spanFromStart;
            } else {
                s.gridRowSpan = parseGridSpan(value);
            }
        } else {
            parseGridLinePart(value, s.gridRowStart, s.gridRowSpan);
        }
    } else if (property == "grid-row-start") {
        int span = 1;
        parseGridLinePart(value, s.gridRowStart, span);
        if (s.gridRowStart == 0) {
            s.gridRowSpan = span;
        } else if (s.gridRowEnd > s.gridRowStart) {
            s.gridRowSpan = s.gridRowEnd - s.gridRowStart;
        }
    } else if (property == "grid-row-end") {
        int span = 1;
        parseGridLinePart(value, s.gridRowEnd, span);
        if (s.gridRowEnd > 0 && s.gridRowStart > 0 && s.gridRowEnd > s.gridRowStart) {
            s.gridRowSpan = s.gridRowEnd - s.gridRowStart;
        }
    } else if (property == "grid-area") {
        // row-start / col-start / row-end / col-end; each part may be `span <n>` or a
        // line number.
        s.gridColumnStart = 0; s.gridColumnEnd = 0; s.gridColumnSpan = 1;
        s.gridRowStart    = 0; s.gridRowEnd    = 0; s.gridRowSpan    = 1;
        const std::vector<std::string_view> parts = splitOnSlashes(value);
        if (parts.size() >= 4) {
            // row-start
            int rsSpan = 1;
            parseGridLinePart(parts[0], s.gridRowStart, rsSpan);
            // col-start
            int csSpan = 1;
            parseGridLinePart(parts[1], s.gridColumnStart, csSpan);
            // row-end
            int reSpan = 1;
            parseGridLinePart(parts[2], s.gridRowEnd, reSpan);
            // col-end
            int ceSpan = 1;
            parseGridLinePart(parts[3], s.gridColumnEnd, ceSpan);
            // Derive spans from lines when both are explicit.
            if (s.gridRowStart > 0 && s.gridRowEnd > s.gridRowStart) {
                s.gridRowSpan = s.gridRowEnd - s.gridRowStart;
            } else {
                s.gridRowSpan = (s.gridRowStart > 0) ? reSpan : rsSpan;
            }
            if (s.gridColumnStart > 0 && s.gridColumnEnd > s.gridColumnStart) {
                s.gridColumnSpan = s.gridColumnEnd - s.gridColumnStart;
            } else {
                s.gridColumnSpan = (s.gridColumnStart > 0) ? ceSpan : csSpan;
            }
        } else if (parts.size() == 1) {
            const std::string_view trimmed = trim(value);
            if (!trimmed.empty() && (std::isalpha(static_cast<unsigned char>(trimmed[0])) ||
                                     trimmed[0] == '_' || trimmed[0] == '-')) {
                if (trimmed.size() > 4 && trimmed.substr(0, 4) == "span") {
                    parseGridLinePart(trimmed, s.gridRowStart, s.gridRowSpan);
                    s.gridColumnStart = s.gridRowStart;
                    s.gridColumnSpan  = s.gridRowSpan;
                } else {
                    s.gridAreaName = std::string(trimmed);
                }
            } else {
                parseGridLinePart(trimmed, s.gridRowStart, s.gridRowSpan);
                s.gridColumnStart = s.gridRowStart;
                s.gridColumnSpan  = s.gridRowSpan;
            }
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
    } else if (property == "margin-inline") {
        applyTwoSides(value, s.margin.left, s.margin.right);
    } else if (property == "margin-inline-start") {
        s.margin.left = parseLength(value);
    } else if (property == "margin-inline-end") {
        s.margin.right = parseLength(value);
    } else if (property == "margin-block") {
        applyTwoSides(value, s.margin.top, s.margin.bottom);
    } else if (property == "margin-block-start") {
        s.margin.top = parseLength(value);
    } else if (property == "margin-block-end") {
        s.margin.bottom = parseLength(value);
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
    } else if (property == "padding-inline") {
        applyTwoSides(value, s.padding.left, s.padding.right);
    } else if (property == "padding-inline-start") {
        s.padding.left = parseLength(value);
    } else if (property == "padding-inline-end") {
        s.padding.right = parseLength(value);
    } else if (property == "padding-block") {
        applyTwoSides(value, s.padding.top, s.padding.bottom);
    } else if (property == "padding-block-start") {
        s.padding.top = parseLength(value);
    } else if (property == "padding-block-end") {
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
            if (parseColorValue(part, color)) {
                s.borderColor = color;
            }
        }
    } else if (property == "border-top" || property == "border-right" ||
               property == "border-bottom" || property == "border-left" ||
               property == "border-inline" || property == "border-block" ||
               property == "border-inline-start" || property == "border-inline-end" ||
               property == "border-block-start" || property == "border-block-end") {
        // The per-side and logical spellings of `border`: the same three slots,
        // written to the sides the name covers. Only horizontal writing mode is
        // laid out, so inline is the left/right pair and block is the top/bottom
        // one, with every `*-start` the first of its pair. Like `border`, only
        // the slots the value names are written.
        const bool top = property == "border-top" || property == "border-block" ||
                         property == "border-block-start";
        const bool right = property == "border-right" || property == "border-inline" ||
                           property == "border-inline-end";
        const bool bottom = property == "border-bottom" || property == "border-block" ||
                            property == "border-block-end";
        const bool left = property == "border-left" || property == "border-inline" ||
                          property == "border-inline-start";

        BorderSlots slots;
        if (parseBorderSlots(value, slots)) {
            if (slots.hasWidth) {
                if (top) { s.borderWidth.top = slots.width; }
                if (right) { s.borderWidth.right = slots.width; }
                if (bottom) { s.borderWidth.bottom = slots.width; }
                if (left) { s.borderWidth.left = slots.width; }
            }
            if (slots.hasStyle) {
                if (top) { s.borderStyles.top = slots.style; }
                if (right) { s.borderStyles.right = slots.style; }
                if (bottom) { s.borderStyles.bottom = slots.style; }
                if (left) { s.borderStyles.left = slots.style; }
            }
            if (slots.hasColor) {
                s.borderColor = slots.color;
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
        if (parseColorValue(value, color)) {
            s.backgroundColor = color;
        }
    } else if (property == "color") {
        SDL_Color color{};
        if (parseColor(value, color)) {
            s.color = color;
        }
    } else if (property == "font-stretch") {
        // Keywords name the nine steps; a percentage is the raw factor.
        float factor = 0.0f;
        if (value == "ultra-condensed") {
            factor = 0.5f;
        } else if (value == "extra-condensed") {
            factor = 0.625f;
        } else if (value == "condensed") {
            factor = 0.75f;
        } else if (value == "semi-condensed") {
            factor = 0.875f;
        } else if (value == "normal") {
            factor = 1.0f;
        } else if (value == "semi-expanded") {
            factor = 1.125f;
        } else if (value == "expanded") {
            factor = 1.25f;
        } else if (value == "extra-expanded") {
            factor = 1.5f;
        } else if (value == "ultra-expanded") {
            factor = 2.0f;
        } else {
            const Length length = parseLength(value);
            if (length.unit == Length::Percent) {
                factor = length.value * 0.01f;
            }
        }
        // Only a sane width describes text: zero and negatives do not.
        if (factor > 0.0f) {
            s.fontStretch = factor;
            s.specified |= ComputedStyle::SpecifiedFontStretch;
        }
    } else if (property == "font-variant") {
        // Only `small-caps` renders differently; anything else keeps normal.
        // Garbage keeps the previous cascade winner.
        if (value == "small-caps") {
            s.fontVariant = ComputedStyle::FontVariant::SmallCaps;
            s.specified |= ComputedStyle::SpecifiedFontVariant;
        } else if (value == "normal") {
            s.fontVariant = ComputedStyle::FontVariant::Normal;
            s.specified |= ComputedStyle::SpecifiedFontVariant;
        }
    } else if (property == "font-kerning") {
        if (value == "none") {
            s.fontKerning = false;
            s.specified |= ComputedStyle::SpecifiedFontKerning;
        } else if (value == "normal" || value == "auto") {
            s.fontKerning = true;
            s.specified |= ComputedStyle::SpecifiedFontKerning;
        }
    } else if (property == "background-blend-mode") {
        // One entry per background layer, pairing with the image list and
        // cycling short lists. Garbage keeps the previous cascade winner,
        // like its mix-blend sibling.
        for (const std::string& part : splitCommaList(value)) {
            ComputedStyle::MixBlendMode mode;
            if (blendModeByName(part, mode)) {
                s.backgroundBlendModes.push_back(mode);
            }
        }
    } else if (property == "mix-blend-mode") {
        // Every mode composites; only garbage keeps the previous cascade
        // winner. `normal` isolates nothing.
        using Blend = ComputedStyle::MixBlendMode;
        if (value == "normal") {
            s.mixBlendMode = Blend::Normal;
        } else if (value == "overlay") {
            s.mixBlendMode = Blend::Overlay;
        } else if (value == "soft-light") {
            s.mixBlendMode = Blend::SoftLight;
        } else if (value == "hard-light") {
            s.mixBlendMode = Blend::HardLight;
        } else if (value == "color-dodge") {
            s.mixBlendMode = Blend::ColorDodge;
        } else if (value == "color-burn") {
            s.mixBlendMode = Blend::ColorBurn;
        } else if (value == "hue") {
            s.mixBlendMode = Blend::Hue;
        } else if (value == "saturation") {
            s.mixBlendMode = Blend::Saturation;
        } else if (value == "color") {
            s.mixBlendMode = Blend::Color;
        } else if (value == "luminosity") {
            s.mixBlendMode = Blend::Luminosity;
        } else if (value == "multiply") {
            s.mixBlendMode = Blend::Multiply;
        } else if (value == "screen") {
            s.mixBlendMode = Blend::Screen;
        } else if (value == "darken") {
            s.mixBlendMode = Blend::Darken;
        } else if (value == "lighten") {
            s.mixBlendMode = Blend::Lighten;
        } else if (value == "difference") {
            s.mixBlendMode = Blend::Difference;
        } else if (value == "exclusion") {
            s.mixBlendMode = Blend::Exclusion;
        }
    } else if (property == "caret-color") {
        if (trim(value) == "auto") {
            s.hasCaretColor = false;
            s.specified |= ComputedStyle::SpecifiedCaretColor;
        } else {
            SDL_Color color{};
            if (parseColorValue(value, color)) {
                s.caretColor = color;
                s.hasCaretColor = true;
                s.specified |= ComputedStyle::SpecifiedCaretColor;
            }
        }
    } else if (property == "accent-color") {
        if (value == "auto") {
            s.hasAccentColor = false;
            s.specified |= ComputedStyle::SpecifiedAccentColor;
        } else {
            SDL_Color color{};
            if (parseColorValue(value, color)) {
                s.accentColor = color;
                s.hasAccentColor = true;
                s.specified |= ComputedStyle::SpecifiedAccentColor;
            }
        }
    } else if (property == "border-color") {
        SDL_Color color{};
        if (parseColorValue(value, color)) {
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
        if (parseColorValue(value, color)) {
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
    } else if (property == "justify-items") {
        s.justifyItems = parseAlign(value);
    } else if (property == "justify-self") {
        s.justifySelf = parseAlign(value);
    } else if (property == "gap" || property == "grid-gap") {
        // One value covers both axes; two are the row gap and then the column
        // gap, in `margin`'s order. Like the longhands, the values resolve to px
        // here, so a percentage reads zero; `normal` is the unauthored state.
        const std::vector<std::string_view> parts = splitTopLevel(value, ' ');
        const auto toPx = [&](std::string_view token) {
            if (token == "normal") {
                return -1.0f;
            }
            return parseLength(token).resolve(0.0f, s.fontSize, rootFontSize);
        };
        if (parts.size() == 1) {
            const float px = toPx(parts[0]);
            s.rowGap = px;
            s.columnGap = px;
        } else if (parts.size() == 2) {
            s.rowGap = toPx(parts[0]);
            s.columnGap = toPx(parts[1]);
        }
    } else if (property == "row-gap") {
        s.rowGap = value == "normal" ? -1.0f
                                     : parseLength(value).resolve(0.0f, s.fontSize, rootFontSize);
    } else if (property == "column-gap") {
        s.columnGap = value == "normal"
                          ? -1.0f
                          : parseLength(value).resolve(0.0f, s.fontSize, rootFontSize);
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
        const auto parsed = parseContent(value);
        s.hasContent = parsed.hasContent;
        s.content = parsed.text;
    } else if (property == "position") {
        if (value == "relative") {
            s.position = ComputedStyle::Relative;
        } else if (value == "absolute") {
            s.position = ComputedStyle::Absolute;
        } else if (value == "fixed") {
            s.position = ComputedStyle::Fixed;
        } else if (value == "sticky") {
            s.position = ComputedStyle::Sticky;
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
    } else if (property == "inset") {
        BoxEdge e;
        applyBoxEdges(value, e);
        s.top = e.top;
        s.right = e.right;
        s.bottom = e.bottom;
        s.left = e.left;
    } else if (property == "inset-inline") {
        applyTwoSides(value, s.left, s.right);
    } else if (property == "inset-inline-start") {
        s.left = parseLength(value);
    } else if (property == "inset-inline-end") {
        s.right = parseLength(value);
    } else if (property == "inset-block") {
        applyTwoSides(value, s.top, s.bottom);
    } else if (property == "inset-block-start") {
        s.top = parseLength(value);
    } else if (property == "inset-block-end") {
        s.bottom = parseLength(value);
    } else if (property == "z-index") {
        s.zIndex = static_cast<int>(parseNumber(value));
    } else if (property == "overflow") {
        s.overflowX = s.overflowY = parseOverflow(value);
    } else if (property == "overflow-x") {
        s.overflowX = parseOverflow(value);
    } else if (property == "overflow-y") {
        s.overflowY = parseOverflow(value);
    } else if (property == "container-type") {
        if (value == "inline-size") {
            s.containerType = ComputedStyle::ContainerInlineSize;
        } else if (value == "size") {
            s.containerType = ComputedStyle::ContainerSize;
        } else {
            s.containerType = ComputedStyle::ContainerNormal;
        }
    } else if (property == "container-name") {
        parseContainerNames(value, s.containerNames);
    } else if (property == "container") {
        // Shorthand: `name / type`, either side alone, or `none`/`normal`
        // for both. Each side applies as whatever it parses as.
        const std::string_view trimmed = trim(value);
        if (trimmed == "none" || trimmed == "normal") {
            s.containerType = ComputedStyle::ContainerNormal;
            s.containerNames.clear();
        } else {
            const std::size_t slash = value.find('/');
            const std::string_view before =
                trim(slash == std::string_view::npos ? value : value.substr(0, slash));
            const std::string_view after = slash == std::string_view::npos
                                               ? std::string_view{}
                                               : trim(value.substr(slash + 1));
            const auto applySide = [&](std::string_view side) {
                if (side.empty()) {
                    return;
                }
                if (side == "inline-size") {
                    s.containerType = ComputedStyle::ContainerInlineSize;
                } else if (side == "size") {
                    s.containerType = ComputedStyle::ContainerSize;
                } else if (side == "normal" || side == "none") {
                    s.containerType = ComputedStyle::ContainerNormal;
                    s.containerNames.clear();
                } else {
                    parseContainerNames(side, s.containerNames);
                }
            };
            applySide(before);
            applySide(after);
        }
    } else if (property == "white-space") {
        if (value == "nowrap") {
            s.whiteSpace = ComputedStyle::WrapNone;
        } else if (value == "pre") {
            s.whiteSpace = ComputedStyle::WrapPre;
        } else if (value == "pre-wrap") {
            s.whiteSpace = ComputedStyle::WrapPreWrap;
        } else if (value == "break-spaces") {
            s.whiteSpace = ComputedStyle::WrapBreakSpaces;
        } else {
            s.whiteSpace = ComputedStyle::WrapText;
        }
        s.specified |= ComputedStyle::SpecifiedWhiteSpace;
    } else if (property == "text-wrap" || property == "text-wrap-mode" ||
               property == "text-wrap-style") {
        // The shorthand resets both longhands, a longhand leaves its sibling
        // alone, and a token that belongs to neither drops the declaration so
        // the previous cascade winner stays.
        const bool shorthand = property == "text-wrap";
        const bool modes = property != "text-wrap-style";
        const bool styles = property != "text-wrap-mode";
        ComputedStyle::TextWrapMode mode =
            shorthand ? ComputedStyle::TextWrapMode::Wrap : s.textWrapMode;
        ComputedStyle::TextWrapStyle wrap =
            shorthand ? ComputedStyle::TextWrapStyle::Auto : s.textWrapStyle;
        const std::vector<std::string_view> tokens = splitOnSpaces(value);
        bool ok = !tokens.empty();
        for (const std::string_view token : tokens) {
            if (token == "wrap" && modes) {
                mode = ComputedStyle::TextWrapMode::Wrap;
            } else if (token == "nowrap" && modes) {
                mode = ComputedStyle::TextWrapMode::Nowrap;
            } else if (token == "auto" && styles) {
                wrap = ComputedStyle::TextWrapStyle::Auto;
            } else if (token == "balance" && styles) {
                wrap = ComputedStyle::TextWrapStyle::Balance;
            } else if (token == "pretty" && styles) {
                wrap = ComputedStyle::TextWrapStyle::Pretty;
            } else {
                ok = false;
                break;
            }
        }
        if (ok) {
            s.textWrapMode = mode;
            s.textWrapStyle = wrap;
            if (modes) {
                s.specified |= ComputedStyle::SpecifiedTextWrapMode;
            }
            if (styles) {
                s.specified |= ComputedStyle::SpecifiedTextWrapStyle;
            }
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
        // Keywords, relative keywords against the inherited weight, and plain
        // numbers. The number survives for the cascade and `fontBold` follows
        // the 600 threshold; garbage keeps the previous cascade winner.
        int weight = s.fontWeight;
        bool ok = true;
        if (value == "normal") {
            weight = 400;
        } else if (value == "bold") {
            weight = 700;
        } else if (value == "bolder") {
            weight = weight < 400 ? 400 : (weight < 600 ? 700 : 900);
        } else if (value == "lighter") {
            weight = weight <= 500 ? 100 : (weight <= 700 ? 400 : 700);
        } else {
            char* stop = nullptr;
            const long parsed = std::strtol(std::string(value).c_str(), &stop, 10);
            if (!stop || *stop != '\0' || parsed < 1 || parsed > 1000) {
                ok = false;
            } else {
                weight = static_cast<int>(parsed);
            }
        }
        if (ok) {
            s.fontWeight = weight;
            s.fontBold = weight >= 600;
            s.specified |= ComputedStyle::SpecifiedFontBold;
        }
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
        s.specified |= ComputedStyle::SpecifiedFontFamily;
    } else if (property == "font-style") {
        if (value == "italic") {
            s.fontStyle = ComputedStyle::FontItalic;
        } else if (value == "oblique") {
            s.fontStyle = ComputedStyle::FontOblique;
        } else {
            s.fontStyle = ComputedStyle::FontNormal;
        }
        s.specified |= ComputedStyle::SpecifiedFontStyle;
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
        s.specified |= ComputedStyle::SpecifiedLineHeight;
    } else if (property == "text-decoration-line") {
        if (value == "underline") {
            s.textDecorationLine = ComputedStyle::DecUnderline;
        } else if (value == "line-through") {
            s.textDecorationLine = ComputedStyle::DecLineThrough;
        } else if (value == "overline") {
            s.textDecorationLine = ComputedStyle::DecOverline;
        } else {
            s.textDecorationLine = ComputedStyle::DecNone;
        }
        s.specified |= ComputedStyle::SpecifiedTextDecorationLine;
    } else if (property == "text-decoration-color") {
        if (parseColorValue(value, s.textDecorationColor)) {
            s.hasTextDecorationColor = true;
            s.specified |= ComputedStyle::SpecifiedTextDecorationColor;
        }
    } else if (property == "text-decoration") {
        // shorthand: line || color
        if (value.find("underline") != std::string_view::npos) {
            s.textDecorationLine = ComputedStyle::DecUnderline;
        } else if (value.find("line-through") != std::string_view::npos) {
            s.textDecorationLine = ComputedStyle::DecLineThrough;
        } else if (value.find("overline") != std::string_view::npos) {
            s.textDecorationLine = ComputedStyle::DecOverline;
        } else if (value.find("none") != std::string_view::npos) {
            s.textDecorationLine = ComputedStyle::DecNone;
        }
        s.specified |= ComputedStyle::SpecifiedTextDecorationLine;
        for (std::string_view token : splitTopLevelSpaces(value)) {
            if (token != "underline" && token != "line-through" &&
                token != "overline" && token != "none" &&
                token != "solid" && token != "dashed" && token != "dotted" &&
                token != "double" && token != "wavy") {
                SDL_Color c;
                if (parseColorValue(token, c)) {
                    s.textDecorationColor = c;
                    s.hasTextDecorationColor = true;
                    s.specified |= ComputedStyle::SpecifiedTextDecorationColor;
                }
            }
        }
    } else if (property == "letter-spacing") {
        s.letterSpacing = parseLength(value);
        s.specified |= ComputedStyle::SpecifiedLetterSpacing;
    } else if (property == "word-spacing") {
        s.wordSpacing = parseLength(value);
        s.specified |= ComputedStyle::SpecifiedWordSpacing;
    } else if (property == "text-indent") {
        s.textIndent = parseLength(value);
        s.specified |= ComputedStyle::SpecifiedTextIndent;
    } else if (property == "text-transform") {
        if (value == "uppercase") {
            s.textTransform = ComputedStyle::TransformUppercase;
        } else if (value == "lowercase") {
            s.textTransform = ComputedStyle::TransformLowercase;
        } else if (value == "capitalize") {
            s.textTransform = ComputedStyle::TransformCapitalize;
        } else {
            s.textTransform = ComputedStyle::TransformNone;
        }
        s.specified |= ComputedStyle::SpecifiedTextTransform;
    } else if (property == "vertical-align") {
        if (value == "top") {
            s.verticalAlign = ComputedStyle::VAlignTop;
        } else if (value == "bottom") {
            s.verticalAlign = ComputedStyle::VAlignBottom;
        } else if (value == "middle") {
            s.verticalAlign = ComputedStyle::VAlignMiddle;
        } else if (value == "sub") {
            s.verticalAlign = ComputedStyle::VAlignSub;
        } else if (value == "super") {
            s.verticalAlign = ComputedStyle::VAlignSuper;
        } else if (value == "text-top") {
            s.verticalAlign = ComputedStyle::VAlignTextTop;
        } else if (value == "text-bottom") {
            s.verticalAlign = ComputedStyle::VAlignTextBottom;
        } else {
            s.verticalAlign = ComputedStyle::VAlignBaseline;
        }
        s.specified |= ComputedStyle::SpecifiedVerticalAlign;
    } else if (property == "transform") {
        s.transform = std::string(value);
        s.specified |= ComputedStyle::SpecifiedTransform;
    } else if (property == "transform-origin") {
        applyTransformOrigin(value, s);
    } else if (property == "perspective") {
        // `none` (or garbage) means no viewer distance; a positive length sets
        // one, anything else keeps the previous cascade winner.
        if (value == "none") {
            s.perspective = 0.0f;
        } else {
            const Length distance = parseLength(value);
            if (!distance.isAuto() && distance.unit != Length::Percent &&
                distance.resolve(0.0f, s.fontSize, rootFontSize) > 0.0f) {
                s.perspective = distance.resolve(0.0f, s.fontSize, rootFontSize);
            }
        }
    } else if (property == "transform-style") {
        // Parsed and stored; the flat compositor cannot share 3D space, so
        // `preserve-3d` still flattens.
        if (value == "preserve-3d") {
            s.transformStyle = ComputedStyle::TransformStyle::Preserve3d;
        } else if (value == "flat") {
            s.transformStyle = ComputedStyle::TransformStyle::Flat;
        }
    } else if (property == "backface-visibility") {
        if (value == "hidden") {
            s.backfaceHidden = true;
        } else if (value == "visible") {
            s.backfaceHidden = false;
        }
    } else if (property == "scroll-snap-type") {
        // `none`, or an axis with an optional strictness (proximity when the
        // strictness is absent). Anything else keeps the previous winner.
        using Axis = ComputedStyle::ScrollSnapAxis;
        using Strict = ComputedStyle::ScrollSnapStrictness;
        std::vector<std::string_view> tokens;
        for (std::string_view token : splitTokens(value)) {
            if (!token.empty()) {
                tokens.push_back(token);
            }
        }
        if (tokens.size() == 1 && tokens[0] == "none") {
            s.snapAxis = Axis::None;
        } else if (!tokens.empty() && tokens.size() <= 2) {
            Axis axis = Axis::None;
            if (tokens[0] == "x" || tokens[0] == "inline") {
                axis = Axis::X;
            } else if (tokens[0] == "y" || tokens[0] == "block") {
                axis = Axis::Y;
            } else if (tokens[0] == "both") {
                axis = Axis::Both;
            }
            Strict strict = Strict::Proximity;
            bool ok = axis != Axis::None;
            if (tokens.size() == 2) {
                if (tokens[1] == "mandatory") {
                    strict = Strict::Mandatory;
                } else if (tokens[1] == "proximity") {
                    strict = Strict::Proximity;
                } else {
                    ok = false;
                }
            }
            if (ok) {
                s.snapAxis = axis;
                s.snapStrictness = strict;
            }
        }
    } else if (property == "color-scheme") {
        // `normal` is the absence of a preference; a list takes its first
        // recognized scheme, and `only` refuses the other one outright.
        ComputedStyle::ColorScheme scheme = ComputedStyle::ColorScheme::Normal;
        bool only = false;
        bool saw = false;
        for (std::string_view token : splitTokens(value)) {
            if (!saw && (token == "normal" || token == "only" || token == "light" ||
                         token == "dark")) {
                saw = true;
            }
            if (token == "only") {
                only = true;
                continue;
            }
            if (token == "light" && scheme == ComputedStyle::ColorScheme::Normal) {
                scheme = ComputedStyle::ColorScheme::Light;
            } else if (token == "dark" && scheme == ComputedStyle::ColorScheme::Normal) {
                scheme = ComputedStyle::ColorScheme::Dark;
            }
        }
        if (saw) {
            s.colorScheme = scheme;
            s.colorSchemeOnly = only;
            s.specified |= ComputedStyle::SpecifiedColorScheme;
        }
    } else if (property == "scroll-behavior") {
        // Not inherited: it describes how this box's own scrolling moves.
        if (value == "smooth") {
            s.scrollBehavior = ComputedStyle::ScrollBehavior::Smooth;
            s.specified |= ComputedStyle::SpecifiedScrollBehavior;
        } else if (value == "auto") {
            s.scrollBehavior = ComputedStyle::ScrollBehavior::Auto;
            s.specified |= ComputedStyle::SpecifiedScrollBehavior;
        }
    } else if (property == "scroll-padding" || property == "scroll-padding-top" ||
               property == "scroll-padding-right" || property == "scroll-padding-bottom" ||
               property == "scroll-padding-left") {
        // One to four lengths (longhand takes exactly one); `auto` insets
        // nothing. Percentages resolve against the snapport at use.
        std::vector<Length> parts;
        bool ok = true;
        for (std::string_view token : splitTokens(value)) {
            if (token.empty()) {
                continue;
            }
            if (token == "auto") {
                parts.push_back(Length{});
                continue;
            }
            const Length length = parseLength(token);
            if (length.isAuto() || length.unit == Length::Number) {
                ok = false;
                break;
            }
            parts.push_back(length);
        }
        if (ok && !parts.empty() && parts.size() <= 4) {
            ComputedStyle::ScrollPadding padding = s.scrollPadding;
            if (property == "scroll-padding-top") {
                padding.top = parts[0];
            } else if (property == "scroll-padding-right") {
                padding.right = parts[0];
            } else if (property == "scroll-padding-bottom") {
                padding.bottom = parts[0];
            } else if (property == "scroll-padding-left") {
                padding.left = parts[0];
            } else {
                expandSides(parts, padding.top, padding.right, padding.bottom,
                            padding.left);
            }
            s.scrollPadding = padding;
        }
    } else if (property == "scroll-margin" || property == "scroll-margin-top" ||
               property == "scroll-margin-right" || property == "scroll-margin-bottom" ||
               property == "scroll-margin-left") {
        // One to four lengths (longhand takes exactly one); `auto` insets
        // nothing. Unlike padding, negative lengths are honoured.
        std::vector<Length> parts;
        bool ok = true;
        for (std::string_view token : splitTokens(value)) {
            if (token.empty()) {
                continue;
            }
            if (token == "auto") {
                parts.push_back(Length{});
                continue;
            }
            const Length length = parseLength(token);
            if (length.isAuto() || length.unit == Length::Number) {
                ok = false;
                break;
            }
            parts.push_back(length);
        }
        if (ok && !parts.empty() && parts.size() <= 4) {
            ComputedStyle::ScrollPadding margin = s.scrollMargin;
            if (property == "scroll-margin-top") {
                margin.top = parts[0];
            } else if (property == "scroll-margin-right") {
                margin.right = parts[0];
            } else if (property == "scroll-margin-bottom") {
                margin.bottom = parts[0];
            } else if (property == "scroll-margin-left") {
                margin.left = parts[0];
            } else {
                expandSides(parts, margin.top, margin.right, margin.bottom,
                            margin.left);
            }
            s.scrollMargin = margin;
        }
    } else if (property == "scroll-snap-align") {
        // One value sets both axes; two set block (y) then inline (x).
        using Align = ComputedStyle::ScrollSnapAlign;
        const auto word = [](std::string_view token, Align& out) {
            if (token == "none") {
                out = Align::None;
            } else if (token == "start") {
                out = Align::Start;
            } else if (token == "end") {
                out = Align::End;
            } else if (token == "center") {
                out = Align::Center;
            } else {
                return false;
            }
            return true;
        };
        std::vector<std::string_view> tokens;
        for (std::string_view token : splitTokens(value)) {
            if (!token.empty()) {
                tokens.push_back(token);
            }
        }
        if (tokens.size() == 1) {
            Align align = Align::None;
            if (word(tokens[0], align)) {
                s.snapAlignX = align;
                s.snapAlignY = align;
            }
        } else if (tokens.size() == 2) {
            Align vertical = Align::None;
            Align horizontal = Align::None;
            if (word(tokens[0], vertical) && word(tokens[1], horizontal)) {
                s.snapAlignY = vertical;
                s.snapAlignX = horizontal;
            }
        }
    } else if (property == "filter") {
        s.filter = std::string(value);
        s.specified |= ComputedStyle::SpecifiedFilter;
    } else if (property == "backdrop-filter") {
        s.backdropFilter = std::string(value);
    } else if (property == "quotes") {
        // Pairs of strings, or `none`. An odd trailing string, a bare word, or
        // garbage drops the whole declaration.
        if (trim(value) == "none") {
            s.quotes.clear();
            s.specified |= ComputedStyle::SpecifiedQuotes;
        } else {
            std::vector<std::string> pairs;
            bool ok = true;
            std::size_t i = 0;
            while (i < value.size() && ok) {
                while (i < value.size() &&
                       std::isspace(static_cast<unsigned char>(value[i]))) {
                    ++i;
                }
                if (i >= value.size()) {
                    break;
                }
                const char mark = value[i];
                if (mark != '"' && mark != '\'') {
                    ok = false;
                    break;
                }
                const std::size_t start = ++i;
                while (i < value.size() && value[i] != mark) {
                    ++i;
                }
                if (i >= value.size()) {
                    ok = false;
                    break;
                }
                pairs.emplace_back(value.substr(start, i - start));
                ++i;
            }
            if (ok && pairs.size() >= 2 && pairs.size() % 2 == 0) {
                s.quotes = std::move(pairs);
                s.specified |= ComputedStyle::SpecifiedQuotes;
            }
        }
    } else if (property == "text-align-last") {
        // Garbage keeps the previous cascade winner, like every other keyword
        // property.
        if (value == "left") {
            s.textAlignLast = ComputedStyle::TextAlignLast::Left;
            s.specified |= ComputedStyle::SpecifiedTextAlignLast;
        } else if (value == "center" || isCentre(value)) {
            s.textAlignLast = ComputedStyle::TextAlignLast::Center;
            s.specified |= ComputedStyle::SpecifiedTextAlignLast;
        } else if (value == "right") {
            s.textAlignLast = ComputedStyle::TextAlignLast::Right;
            s.specified |= ComputedStyle::SpecifiedTextAlignLast;
        } else if (value == "justify") {
            s.textAlignLast = ComputedStyle::TextAlignLast::Justify;
            s.specified |= ComputedStyle::SpecifiedTextAlignLast;
        } else if (value == "start") {
            s.textAlignLast = ComputedStyle::TextAlignLast::Start;
            s.specified |= ComputedStyle::SpecifiedTextAlignLast;
        } else if (value == "end") {
            s.textAlignLast = ComputedStyle::TextAlignLast::End;
            s.specified |= ComputedStyle::SpecifiedTextAlignLast;
        } else if (value == "auto") {
            s.textAlignLast = ComputedStyle::TextAlignLast::Auto;
            s.specified |= ComputedStyle::SpecifiedTextAlignLast;
        }
    } else if (property == "text-align") {
        if (isCentre(value)) {
            s.textAlign = ComputedStyle::Center;
        } else if (value == "right") {
            s.textAlign = ComputedStyle::Right;
        } else if (value == "justify") {
            s.textAlign = ComputedStyle::Justify;
        } else if (value == "start") {
            s.textAlign = ComputedStyle::Start;
        } else if (value == "end") {
            s.textAlign = ComputedStyle::End;
        } else {
            s.textAlign = ComputedStyle::Left;
        }
    } else if (property == "direction") {
        if (value == "ltr") {
            s.direction = ComputedStyle::Direction::LTR;
            s.specified |= ComputedStyle::SpecifiedDirection;
        } else if (value == "rtl") {
            s.direction = ComputedStyle::Direction::RTL;
            s.specified |= ComputedStyle::SpecifiedDirection;
        }
    } else if (property == "visibility") {
        s.visible = parseBool(value);
    } else if (property == "transition") {
        // The shorthand is a full reset of the four parallel lists, like CSS's
        // declaration-level reset rather than a list append; later longhands in
        // the same cascade then overwrite individual lists.
        applyTransitionShorthand(decl.value, s);
    } else if (property == "transition-property") {
        s.transitionProperties.clear();
        if (value != "none") {
            s.transitionProperties = splitCommaList(decl.value);
        }
    } else if (property == "transition-duration") {
        s.transitionDurationsMs.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            float ms = 0.0f;
            if (!parseTimeMs(part, ms)) {
                ms = 0.0f;  // an invalid time is a null duration, per CSS
            }
            s.transitionDurationsMs.push_back(ms);
        }
    } else if (property == "transition-timing-function") {
        s.transitionEasings.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            s.transitionEasings.push_back(Easing::parse(part));
        }
    } else if (property == "transition-delay") {
        s.transitionDelaysMs.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            float ms = 0.0f;
            if (!parseTimeMs(part, ms)) {
                ms = 0.0f;
            }
            s.transitionDelaysMs.push_back(ms);
        }
    } else if (property == "transition-behavior") {
        // Accepted and ignored: the engine animates style values only, so there
        // is no discrete/animate distinction for it to switch.
    } else if (property == "animation") {
        applyAnimationShorthand(decl.value, s);
    } else if (property == "animation-name") {
        s.animationNames.clear();
        if (value != "none") {
            s.animationNames = splitCommaList(decl.value);
        }
    } else if (property == "animation-duration") {
        s.animationDurationsMs.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            float ms = 0.0f;
            if (!parseTimeMs(part, ms)) {
                ms = 0.0f;
            }
            s.animationDurationsMs.push_back(ms);
        }
    } else if (property == "animation-timing-function") {
        s.animationEasings.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            s.animationEasings.push_back(Easing::parse(part));
        }
    } else if (property == "animation-delay") {
        s.animationDelaysMs.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            float ms = 0.0f;
            if (!parseTimeMs(part, ms)) {
                ms = 0.0f;
            }
            s.animationDelaysMs.push_back(ms);
        }
    } else if (property == "animation-iteration-count") {
        s.animationIterations.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            if (part == "infinite") {
                s.animationIterations.push_back(-1.0f);
                continue;
            }
            char* end = nullptr;
            const float count = std::strtof(part.c_str(), &end);
            s.animationIterations.push_back(end != part.c_str() && *end == '\0'
                                                ? (count < 0.0f ? 1.0f : count)
                                                : 1.0f);
        }
    } else if (property == "animation-direction") {
        s.animationDirections.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            const bool known = part == "normal" || part == "reverse" ||
                               part == "alternate" || part == "alternate-reverse";
            s.animationDirections.push_back(known ? part : "normal");
        }
    } else if (property == "animation-fill-mode") {
        s.animationFills.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            const bool known = part == "none" || part == "forwards" ||
                               part == "backwards" || part == "both";
            s.animationFills.push_back(known ? part : "none");
        }
    } else if (property == "animation-composition") {
        s.animationCompositions.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            const bool known =
                part == "replace" || part == "add" || part == "accumulate";
            s.animationCompositions.push_back(known ? part : "replace");
        }
    } else if (property == "animation-play-state") {
        s.animationPaused.clear();
        for (const std::string& part : splitCommaList(decl.value)) {
            s.animationPaused.push_back(part == "paused");
        }
    } else if (property == "object-fit") {
        if (value == "cover") {
            s.objectFit = ComputedStyle::ObjCover;
        } else if (value == "fill") {
            s.objectFit = ComputedStyle::ObjFill;
        } else if (value == "none") {
            s.objectFit = ComputedStyle::ObjNone;
        } else if (value == "scale-down") {
            s.objectFit = ComputedStyle::ObjScaleDown;
        } else {
            s.objectFit = ComputedStyle::ObjContain;
        }
    } else if (property == "object-position") {
        // One value takes the horizontal slot unless it names a vertical edge;
        // `center` takes whichever slot is still free. Two same-axis keywords
        // invalidate the whole declaration, as does a length with nowhere to
        // go; anything else keeps the previous cascade winner. (`left 10px`
        // leniently lands the length vertically rather than dropping.)
        const std::vector<std::string_view> parts = splitTokens(value);
        Length x{Length::Percent, 50.0f};
        Length y{Length::Percent, 50.0f};
        bool xTaken = false;
        bool yTaken = false;
        bool xKeyword = false;
        bool ok = parts.size() == 1 || parts.size() == 2;
        for (std::string_view token : parts) {
            if (!ok) {
                break;
            }
            if (token == "left" || token == "right") {
                ok = !xTaken;
                x = Length{Length::Percent, token == "left" ? 0.0f : 100.0f};
                xTaken = true;
                xKeyword = true;
            } else if (token == "top" || token == "bottom") {
                ok = !yTaken;
                y = Length{Length::Percent, token == "top" ? 0.0f : 100.0f};
                yTaken = true;
            } else if (token == "center") {
                if (!xTaken) {
                    x = Length{Length::Percent, 50.0f};
                    xTaken = true;
                } else if (!yTaken) {
                    y = Length{Length::Percent, 50.0f};
                    yTaken = true;
                } else {
                    ok = false;
                }
            } else {
                const Length length = parseLength(token);
                ok = !length.isAuto() && length.unit != Length::Number;
                if (ok && !xTaken) {
                    x = length;
                    xTaken = true;
                } else if (ok && !yTaken && xKeyword) {
                    y = length;
                    yTaken = true;
                } else {
                    ok = false;
                }
            }
        }
        if (ok) {
            s.objectPositionX = x;
            s.objectPositionY = y;
        }
    } else if (property == "list-style-type") {
        s.specified |= ComputedStyle::SpecifiedListStyleType;
        if (!parseCounterStyleName(value, s.listStyleType)) {
            s.listStyleType = ComputedStyle::LSTDisc;
        }
    } else if (property == "list-style") {
        s.specified |= ComputedStyle::SpecifiedListStyleType;
        // Shorthand: type || position || image. Only the type keyword is stored.
        // A `none` token clears the type whatever else rides along.
        bool hasNone = false;
        for (std::string_view token : splitTokens(value)) {
            if (token == "none") {
                hasNone = true;
            }
        }
        if (hasNone) {
            s.listStyleType = ComputedStyle::LSTNone;
        } else if (value.find("upper-roman") != std::string_view::npos) {
            s.listStyleType = ComputedStyle::LSTUpperRoman;
        } else if (value.find("lower-roman") != std::string_view::npos) {
            s.listStyleType = ComputedStyle::LSTLowerRoman;
        } else if (value.find("upper-alpha") != std::string_view::npos ||
                   value.find("upper-latin") != std::string_view::npos) {
            s.listStyleType = ComputedStyle::LSTUpperAlpha;
        } else if (value.find("lower-alpha") != std::string_view::npos ||
                   value.find("lower-latin") != std::string_view::npos) {
            s.listStyleType = ComputedStyle::LSTLowerAlpha;
        } else if (value.find("circle") != std::string_view::npos) {
            s.listStyleType = ComputedStyle::LSTCircle;
        } else if (value.find("square") != std::string_view::npos) {
            s.listStyleType = ComputedStyle::LSTSquare;
        } else if (value.find("decimal") != std::string_view::npos) {
            s.listStyleType = ComputedStyle::LSTDecimal;
        } else if (value.find("disc") != std::string_view::npos) {
            s.listStyleType = ComputedStyle::LSTDisc;
        }
        if (value.find("inside") != std::string_view::npos) {
            s.listStylePosition = ComputedStyle::LSPInside;
            s.specified |= ComputedStyle::SpecifiedListStylePosition;
        } else if (value.find("outside") != std::string_view::npos) {
            s.listStylePosition = ComputedStyle::LSPOutside;
            s.specified |= ComputedStyle::SpecifiedListStylePosition;
        }
        // The image rides along in the shorthand: a `url()` sets it, and the
        // `none` that also clears the type clears the image.
        s.specified |= ComputedStyle::SpecifiedListStyleImage;
        s.listStyleImage.clear();
        for (std::string_view token : splitTokens(value)) {
            if (const std::string_view href = unwrapUrl(token); !href.empty()) {
                s.listStyleImage = std::string(href);
                break;
            }
        }
    } else if (property == "list-style-image") {
        // `none` clears; a `url()` stores its href for the marker paint.
        // Anything else keeps the previous cascade winner.
        if (trim(value) == "none") {
            s.listStyleImage.clear();
            s.specified |= ComputedStyle::SpecifiedListStyleImage;
        } else if (const std::string_view href = unwrapUrl(trim(value));
                   !href.empty()) {
            s.listStyleImage = std::string(href);
            s.specified |= ComputedStyle::SpecifiedListStyleImage;
        }
    } else if (property == "pointer-events") {
        std::string_view val = trim(value);
        if (val == "none") {
            s.pointerEventsNone = true;
            s.specified |= ComputedStyle::SpecifiedPointerEvents;
        } else if (val == "auto" || val == "initial") {
            s.pointerEventsNone = false;
            s.specified |= ComputedStyle::SpecifiedPointerEvents;
        }
    } else if (property == "list-style-position") {
        std::string_view val = trim(value);
        if (val == "inside") {
            s.listStylePosition = ComputedStyle::LSPInside;
            s.specified |= ComputedStyle::SpecifiedListStylePosition;
        } else if (val == "outside") {
            s.listStylePosition = ComputedStyle::LSPOutside;
            s.specified |= ComputedStyle::SpecifiedListStylePosition;
        }
    } else if (property == "cursor") {
        // `cursor` is a comma-separated fallback list: any number of `url()`
        // entries (which this engine cannot load) may precede the keyword that is
        // actually used. The last recognised keyword wins, so `url(x.png), pointer`
        // resolves to the pointer. A list with no keyword is invalid and dropped.
        ComputedStyle::Cursor chosen = s.cursor;
        bool matched = false;
        std::size_t i = 0;
        while (i < value.size()) {
            while (i < value.size() &&
                   (value[i] == ',' || std::isspace(static_cast<unsigned char>(value[i])))) {
                ++i;
            }
            if (i >= value.size()) {
                break;
            }
            const std::size_t start = i;
            while (i < value.size() && value[i] != ',' &&
                   !std::isspace(static_cast<unsigned char>(value[i]))) {
                ++i;
            }
            ComputedStyle::Cursor parsed = ComputedStyle::CursorAuto;
            if (cursorKeyword(value.substr(start, i - start), parsed)) {
                chosen = parsed;
                matched = true;
            }
        }
        if (matched) {
            s.cursor = chosen;
            s.specified |= ComputedStyle::SpecifiedCursor;
        }
    } else if (property == "float") {
        if (value == "left") s.floatType = ComputedStyle::FloatLeft;
        else if (value == "right") s.floatType = ComputedStyle::FloatRight;
        else s.floatType = ComputedStyle::FloatNone;
    } else if (property == "clear") {
        if (value == "left") s.clearType = ComputedStyle::ClearLeft;
        else if (value == "right") s.clearType = ComputedStyle::ClearRight;
        else if (value == "both") s.clearType = ComputedStyle::ClearBoth;
        else s.clearType = ComputedStyle::ClearNone;
    } else if (property == "border-collapse") {
        s.borderCollapse = value == "collapse" ? ComputedStyle::CollapseCollapse
                                                : ComputedStyle::CollapseSeparate;
    } else if (property == "border-spacing") {
        // One length sets both axes; two set horizontal then vertical.
        const std::string_view text = trim(value);
        const std::size_t space = text.find(' ');
        const auto toPx = [&](std::string_view v) {
            return parseLength(v).resolve(0.0f, s.fontSize, rootFontSize);
        };
        if (space == std::string_view::npos) {
            const float px = toPx(text);
            s.borderSpacingH = px;
            s.borderSpacingV = px;
        } else {
            s.borderSpacingH = toPx(text.substr(0, space));
            s.borderSpacingV = toPx(trim(text.substr(space + 1)));
        }
    } else if (property == "table-layout") {
        s.tableLayout = value == "fixed" ? ComputedStyle::TableLayoutFixed
                                         : ComputedStyle::TableLayoutAuto;
    } else if (property == "line-clamp" || property == "-webkit-line-clamp") {
        // `none` lifts the cap; otherwise a bare integer of at least 1. Zero,
        // negatives, and anything but digits drop the declaration outright.
        if (trim(value) == "none") {
            s.lineClamp = 0;
            s.specified |= ComputedStyle::SpecifiedLineClamp;
        } else {
            char* stop = nullptr;
            const long parsed = std::strtol(std::string(trim(value)).c_str(), &stop, 10);
            if (stop && *stop == '\0' && parsed >= 1 && parsed <= 1000) {
                s.lineClamp = static_cast<int>(parsed);
                s.specified |= ComputedStyle::SpecifiedLineClamp;
            }
        }
    } else if (property == "empty-cells") {
        if (value == "show" || value == "hide") {
            s.emptyCells = value == "hide" ? ComputedStyle::EmptyCellsHide
                                           : ComputedStyle::EmptyCellsShow;
            s.specified |= ComputedStyle::SpecifiedEmptyCells;
        }
    } else if (property == "caption-side") {
        s.captionSide = value == "bottom" ? ComputedStyle::CaptionBottom
                                          : ComputedStyle::CaptionTop;
    } else if (property == "counter-reset") {
        s.counterReset = parseCounterList(value, 0);
    } else if (property == "counter-increment") {
        s.counterIncrement = parseCounterList(value, 1);
    } else if (property == "counter-set") {
        s.counterSet = parseCounterList(value, 0);
    } else if (property == "resize") {
        // `block` and `inline` name the vertical and horizontal axes in this
        // engine's horizontal writing mode. Anything else keeps the cascade.
        if (value == "none") {
            s.resize = ComputedStyle::ResizeNone;
            s.specified |= ComputedStyle::SpecifiedResize;
        } else if (value == "both") {
            s.resize = ComputedStyle::ResizeBoth;
            s.specified |= ComputedStyle::SpecifiedResize;
        } else if (value == "horizontal" || value == "inline") {
            s.resize = ComputedStyle::ResizeHorizontal;
            s.specified |= ComputedStyle::SpecifiedResize;
        } else if (value == "vertical" || value == "block") {
            s.resize = ComputedStyle::ResizeVertical;
            s.specified |= ComputedStyle::SpecifiedResize;
        }
    } else if (property == "appearance" || property == "-webkit-appearance") {
        // Accepted; these control browser chrome that this engine does not draw.
    } else if (property == "isolation") {
        // `isolate` opens the element's own group; `auto` is the initial
        // value, and garbage keeps the previous cascade winner.
        if (value == "isolate") {
            s.isolate = true;
        } else if (value == "auto") {
            s.isolate = false;
        }
    } else if (property == "will-change" || property == "contain") {
        // Performance / compositing hints: accepted and ignored.
    } else if (property == "mask" || property == "-webkit-mask") {
        // Shorthand: every layer expands into all eight longhands, resetting
        // whatever they set. One bad token drops the whole declaration.
        parseMask(value, s);
    } else if (property == "mask-image" || property == "-webkit-mask-image") {
        // Every layer parses; one bad layer drops the whole declaration, like
        // any failed parse.
        std::vector<ComputedStyle::MaskImage> layers;
        bool ok = false;
        for (std::string_view layer : splitTopLevel(value)) {
            ComputedStyle::MaskImage mask;
            if (!parseMaskImage(layer, mask)) {
                ok = false;
                break;
            }
            ok = true;
            layers.push_back(std::move(mask));
        }
        if (ok && !layers.empty()) {
            s.maskImages = std::move(layers);
        }
    } else if (property == "mask-mode") {
        std::vector<ComputedStyle::MaskMode> modes;
        bool ok = false;
        for (std::string_view entry : splitTopLevel(value)) {
            const std::string_view mode = trim(entry);
            using Mode = ComputedStyle::MaskMode;
            if (mode == "match-source") {
                modes.push_back(Mode::MatchSource);
            } else if (mode == "alpha") {
                modes.push_back(Mode::Alpha);
            } else if (mode == "luminance") {
                modes.push_back(Mode::Luminance);
            } else {
                ok = false;
                break;
            }
            ok = true;
        }
        if (ok && !modes.empty()) {
            s.maskModes = std::move(modes);
        }
    } else if (property == "mask-composite") {
        std::vector<ComputedStyle::MaskComposite> ops;
        bool ok = false;
        for (std::string_view entry : splitTopLevel(value)) {
            const std::string_view op = trim(entry);
            using Composite = ComputedStyle::MaskComposite;
            if (op == "add") {
                ops.push_back(Composite::Add);
            } else if (op == "subtract") {
                ops.push_back(Composite::Subtract);
            } else if (op == "intersect") {
                ops.push_back(Composite::Intersect);
            } else if (op == "exclude") {
                ops.push_back(Composite::Exclude);
            } else {
                ok = false;
                break;
            }
            ok = true;
        }
        if (ok && !ops.empty()) {
            s.maskComposites = std::move(ops);
        }
    } else if (property == "mask-size" || property == "-webkit-mask-size" ||
               property == "mask-position" || property == "-webkit-mask-position" ||
               property == "mask-repeat" || property == "-webkit-mask-repeat" ||
               property == "mask-origin" || property == "-webkit-mask-origin" ||
               property == "mask-clip" || property == "-webkit-mask-clip") {
        // Parallel lists matched to the image layers by position, like the
        // background longhands: an entry that does not parse becomes the
        // initial value so later layers stay lined up with their images.
        const bool isSize = property == "mask-size" || property == "-webkit-mask-size";
        const bool isPosition =
            property == "mask-position" || property == "-webkit-mask-position";
        const bool isRepeat =
            property == "mask-repeat" || property == "-webkit-mask-repeat";
        const bool isOrigin =
            property == "mask-origin" || property == "-webkit-mask-origin";
        for (std::string_view entry : splitTopLevel(value)) {
            if (isSize) {
                ComputedStyle::BackgroundSize size;
                if (!parseBackgroundSize(entry, size)) {
                    size = ComputedStyle::BackgroundSize{};
                }
                s.maskSizes.push_back(size);
            } else if (isPosition) {
                ComputedStyle::BackgroundPosition position;
                if (!parsePositionPair(entry, position)) {
                    position = ComputedStyle::BackgroundPosition{};
                }
                s.maskPositions.push_back(position);
            } else if (isRepeat) {
                ComputedStyle::RepeatPair repeat;
                if (!parseRepeatValue(entry, repeat)) {
                    repeat = ComputedStyle::RepeatPair{};
                }
                s.maskRepeats.push_back(repeat);
            } else {
                ComputedStyle::BackgroundBox box = ComputedStyle::BackgroundBorderBox;
                if (!parseBackgroundBox(entry, box)) {
                    box = ComputedStyle::BackgroundBorderBox;
                }
                if (isOrigin) {
                    s.maskOrigins.push_back(box);
                } else {
                    s.maskClips.push_back(box);
                }
            }
        }
    } else if (property == "mask-type") {
        // Only `<mask>` elements read this, and SVG does not render yet, but
        // the value is stored and serialized all the same.
        if (value == "luminance") {
            s.maskType = ComputedStyle::MaskType::Luminance;
        } else if (value == "alpha") {
            s.maskType = ComputedStyle::MaskType::Alpha;
        }
    } else if (property == "mask-border-source") {
        // A single image; a list is not a source.
        if (splitTopLevel(value).size() == 1) {
            ComputedStyle::MaskImage source;
            if (parseMaskImage(value, source)) {
                s.maskBorder.source = source;
            }
        }
    } else if (property == "mask-border-mode") {
        if (value == "luminance") {
            s.maskBorder.mode = ComputedStyle::MaskBorder::Mode::Luminance;
        } else if (value == "alpha") {
            s.maskBorder.mode = ComputedStyle::MaskBorder::Mode::Alpha;
        }
    } else if (property == "mask-border-slice") {
        ComputedStyle::MaskBorder::Slice slice;
        if (parseMaskBorderSlice(value, slice)) {
            s.maskBorder.slice = slice;
        }
    } else if (property == "mask-border-width") {
        ComputedStyle::MaskBorder::Width width;
        if (parseMaskBorderWidth(value, width)) {
            s.maskBorder.width = width;
        }
    } else if (property == "mask-border-outset") {
        ComputedStyle::MaskBorder::Outset outset;
        if (parseMaskBorderOutset(value, outset)) {
            s.maskBorder.outset = outset;
        }
    } else if (property == "mask-border-repeat") {
        parseMaskBorderRepeat(value, s.maskBorder);
    } else if (property == "mask-border") {
        // Shorthand: source, slice, width, outset, repeat, and mode in one
        // declaration. One bad token drops it all.
        parseMaskBorder(value, s);
    } else if (property == "border-image-source") {
        // A single image; a list is not a source.
        if (splitTopLevel(value).size() == 1) {
            ComputedStyle::BackgroundImage source;
            if (parseBorderImageSource(value, source)) {
                s.borderImage.source = source;
            }
        }
    } else if (property == "border-image-slice") {
        ComputedStyle::BorderImage::Slice slice;
        if (parseBorderImageSlice(value, slice)) {
            s.borderImage.slice = slice;
        }
    } else if (property == "border-image-width") {
        ComputedStyle::BorderImage::Width width;
        if (parseBorderImageWidth(value, width)) {
            s.borderImage.width = width;
        }
    } else if (property == "border-image-outset") {
        ComputedStyle::BorderImage::Outset outset;
        if (parseBorderImageOutset(value, outset)) {
            s.borderImage.outset = outset;
        }
    } else if (property == "border-image-repeat") {
        parseBorderImageRepeat(value, s.borderImage);
    } else if (property == "border-image") {
        // Shorthand: source, slice, width, outset, and repeat in one
        // declaration. One bad token drops it all.
        parseBorderImage(value, s);
    } else if (property == "clip-path") {
        // Shapes clip the element and its subtree; anything else keeps the
        // previous cascade winner, like any failed parse.
        ComputedStyle::ClipPath parsed;
        if (parseClipPath(value, parsed)) {
            s.clipPath = parsed;
        }
    } else if (property == "word-break") {
        // `keep-all` behaves as normal here: only spaces break lines either way.
        // Garbage keeps the previous cascade winner.
        if (value == "normal" || value == "keep-all") {
            s.wordBreak = value == "normal" ? ComputedStyle::WordBreak::Normal
                                            : ComputedStyle::WordBreak::KeepAll;
            s.specified |= ComputedStyle::SpecifiedWordBreak;
        } else if (value == "break-all") {
            s.wordBreak = ComputedStyle::WordBreak::BreakAll;
            s.specified |= ComputedStyle::SpecifiedWordBreak;
        }
    } else if (property == "overflow-wrap" || property == "word-wrap") {
        // `anywhere` breaks like `break-word` for wrapping purposes; the two
        // only differ in intrinsic sizing, which never breaks lines here.
        if (value == "normal") {
            s.overflowWrap = ComputedStyle::OverflowWrap::Normal;
            s.specified |= ComputedStyle::SpecifiedOverflowWrap;
        } else if (value == "break-word" || value == "anywhere") {
            s.overflowWrap = ComputedStyle::OverflowWrap::BreakWord;
            s.specified |= ComputedStyle::SpecifiedOverflowWrap;
        }
    } else if (property == "text-size-adjust") {
        // Accepted and carried, never applied: see the field comment.
        // Garbage keeps the previous cascade winner.
        if (value == "auto") {
            s.textSizeAdjust = ComputedStyle::TextSizeAdjust::Auto;
            s.textSizeAdjustPercent = 100.0f;
            s.specified |= ComputedStyle::SpecifiedTextSizeAdjust;
        } else if (value == "none") {
            s.textSizeAdjust = ComputedStyle::TextSizeAdjust::None;
            s.textSizeAdjustPercent = 100.0f;
            s.specified |= ComputedStyle::SpecifiedTextSizeAdjust;
        } else {
            const Length length = parseLength(value);
            if (length.unit == Length::Percent && length.value >= 0.0f) {
                s.textSizeAdjust = ComputedStyle::TextSizeAdjust::Percent;
                s.textSizeAdjustPercent = length.value;
                s.specified |= ComputedStyle::SpecifiedTextSizeAdjust;
            }
        }
    } else if (property == "tab-size") {
        // A number counts space widths, a length is absolute. Percentages and
        // negative values are invalid, and an invalid value keeps the cascade's
        // previous winner, like any failed parse.
        std::string_view val = trim(value);
        if (!val.empty() && val[0] == '+') {
            val = val.substr(1);
        }
        std::size_t digits = 0;
        while (digits < val.size() &&
               (std::isdigit(static_cast<unsigned char>(val[digits])) || val[digits] == '.')) {
            ++digits;
        }
        if (digits > 0 && digits == val.size()) {
            s.tabSize = Length{Length::Number,
                               std::strtof(std::string(val).c_str(), nullptr)};
            s.specified |= ComputedStyle::SpecifiedTabSize;
        } else {
            const Length len = parseLength(val);
            if (!len.isAuto() && len.unit != Length::Percent && len.value >= 0.0f) {
                s.tabSize = len;
                s.specified |= ComputedStyle::SpecifiedTabSize;
            }
        }
    } else if (property == "hyphens") {
        // The engine has no hyphenation dictionary, so `auto` gets no breaks of
        // its own and behaves like `manual`: both take the soft hyphens the
        // author wrote. `none` leaves them invisible and never breaks at one.
        if (value == "manual") {
            s.hyphens = ComputedStyle::Hyphens::Manual;
            s.specified |= ComputedStyle::SpecifiedHyphens;
        } else if (value == "none") {
            s.hyphens = ComputedStyle::Hyphens::None;
            s.specified |= ComputedStyle::SpecifiedHyphens;
        } else if (value == "auto") {
            s.hyphens = ComputedStyle::Hyphens::Auto;
            s.specified |= ComputedStyle::SpecifiedHyphens;
        }
    } else if (property == "text-overflow") {
        std::string_view val = trim(value);
        if (val == "ellipsis") {
            s.textOverflow = ComputedStyle::TextOverflowEllipsis;
        } else if (val == "clip") {
            s.textOverflow = ComputedStyle::TextOverflowClip;
        }
    } else if (property == "aspect-ratio") {
        std::string_view val = trim(value);
        if (val == "auto" || val == "initial" || val == "unset") {
            s.aspectRatioW = 0.0f;
            s.aspectRatioH = 0.0f;
        } else {
            if (val.rfind("auto", 0) == 0) {
                val = trim(val.substr(4));
            }
            const std::size_t slash = val.find('/');
            if (slash != std::string_view::npos) {
                const float w = parseNumber(val.substr(0, slash));
                const float h = parseNumber(val.substr(slash + 1));
                if (w > 0.0f && h > 0.0f) {
                    s.aspectRatioW = w;
                    s.aspectRatioH = h;
                }
            } else {
                const float r = parseNumber(val);
                if (r > 0.0f) {
                    s.aspectRatioW = r;
                    s.aspectRatioH = 1.0f;
                }
            }
        }
    } else if (property == "user-select" || property == "-webkit-user-select") {
        // Selection control: accepted; non-editable elements already ignore selection.
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

bool StyleResolver::containerMatches(const Rule::ContainerQuery& query,
                                     Element& element) const {
    // The nearest ancestor that establishes a query container — named when the
    // query names names — read at its laid-out content width. Only widths are
    // queried: heights settle bottom-up, so they are never final while a pass
    // is still resolving. An ancestor without a box (`display:none`) is not a
    // container; with no container at all the query never matches.
    for (Element* ancestor = element.parent(); ancestor;
         ancestor = ancestor->parent()) {
        const ComputedStyle cs = resolve(*ancestor);
        if (cs.display == ComputedStyle::None) {
            continue;
        }
        if (cs.containerType == ComputedStyle::ContainerNormal) {
            continue;
        }
        if (!query.names.empty()) {
            bool named = false;
            for (const std::string& want : query.names) {
                if (std::find(cs.containerNames.begin(), cs.containerNames.end(), want) !=
                    cs.containerNames.end()) {
                    named = true;
                    break;
                }
            }
            if (!named) {
                continue;
            }
        }
        const float width = ancestor->contentWidth();
        if (query.minWidth >= 0.0f && width < query.minWidth) {
            return false;
        }
        if (query.maxWidth >= 0.0f && width > query.maxWidth) {
            return false;
        }
        return true;
    }
    return false;
}

void StyleResolver::noteLayer(const std::string& name) {
    if (name.empty()) {
        return;
    }
    if (std::find(layerOrder_.begin(), layerOrder_.end(), name) == layerOrder_.end()) {
        layerOrder_.push_back(name);
    }
}

int StyleResolver::layerIndex(const std::string& name) const {
    for (std::size_t i = 0; i < layerOrder_.size(); ++i) {
        if (layerOrder_[i] == name) {
            return static_cast<int>(i);
        }
    }
    // Unreachable after registration; sorting unknown layers last is saner
    // than crashing.
    return static_cast<int>(layerOrder_.size());
}

// One integer that orders layers for a cascade pass. Normal declarations run
// first-declared-layer first with unlayered last; `!important` runs unlayered
// first with later-declared layers first.
int StyleResolver::layerRank(const std::string& layer, bool important) const {
    if (layer.empty()) {
        return important ? std::numeric_limits<int>::min()
                         : std::numeric_limits<int>::max();
    }
    const int index = layerIndex(layer);
    return important ? -index : index;
}

void StyleResolver::sortRulesNormal() {
    std::stable_sort(rules_.begin(), rules_.end(),
                     [this](const Rule& a, const Rule& b) {
                         const int la = layerRank(a.layer, false);
                         const int lb = layerRank(b.layer, false);
                         if (la != lb) {
                             return la < lb;
                         }
                         return selectorRank(a.selector) < selectorRank(b.selector);
                     });
}

void StyleResolver::rebuildImportantOrder() {
    importantOrder_.resize(rules_.size());
    for (std::size_t i = 0; i < rules_.size(); ++i) {
        importantOrder_[i] = i;
    }
    std::stable_sort(importantOrder_.begin(), importantOrder_.end(),
                     [this](std::size_t a, std::size_t b) {
                         const int la = layerRank(rules_[a].layer, true);
                         const int lb = layerRank(rules_[b].layer, true);
                         if (la != lb) {
                             return la < lb;
                         }
                         return selectorRank(rules_[a].selector) <
                                selectorRank(rules_[b].selector);
                     });
}

void StyleResolver::setRules(std::vector<Rule> rules) {
    for (Rule& rule : rules) {
        noteCustomUsage(rule);
    }
    rules_ = std::move(rules);
    // A fresh rule set re-declares its layers: statements and first use both
    // register in stream order.
    layerOrder_.clear();
    anyContainerRules_ = false;
    hasFirstLineRules_ = false;
    hasFirstLetterRules_ = false;
    for (const Rule& rule : rules_) {
        noteLayer(rule.layer);
        if (rule.container.active) {
            anyContainerRules_ = true;
        }
        for (const Selector& part : rule.selector.parts) {
            if (part.kind == Selector::Kind::PseudoElement) {
                if (part.value == "::first-line") {
                    hasFirstLineRules_ = true;
                } else if (part.value == "::first-letter") {
                    hasFirstLetterRules_ = true;
                }
            }
        }
    }
    sortRulesNormal();
    rebuildImportantOrder();
    ++generation_;
}

void StyleResolver::addRule(Rule rule) {
    noteCustomUsage(rule);
    noteLayer(rule.layer);
    if (rule.container.active) {
        anyContainerRules_ = true;
    }
    rules_.push_back(std::move(rule));
    sortRulesNormal();
    rebuildImportantOrder();
    ++generation_;
}

void StyleResolver::eraseRule(std::size_t index) {
    if (index >= rules_.size()) {
        return;
    }
    rules_.erase(rules_.begin() + static_cast<std::ptrdiff_t>(index));
    anyContainerRules_ = false;
    for (const Rule& rule : rules_) {
        if (rule.container.active) {
            anyContainerRules_ = true;
            break;
        }
    }
    rebuildImportantOrder();
    ++generation_;
}

std::string StyleResolver::propertyValue(const ComputedStyle& style,
                                         std::string_view property) {
    if (isCustomProperty(property)) {
        const auto found = style.custom.find(std::string(property));
        return found == style.custom.end() ? std::string() : found->second;
    }
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
        if (l.unit == Length::Calc) {
            return serializeMathNode(l.expr.get());
        }
        if (l.unit == Length::Number) {
            return unitless(l.value);
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
            case ComputedStyle::Grid: return "grid";
            case ComputedStyle::InlineGrid: return "inline-grid";
            case ComputedStyle::Table: return "table";
            case ComputedStyle::InlineTable: return "inline-table";
            case ComputedStyle::TableRow: return "table-row";
            case ComputedStyle::TableCell: return "table-cell";
            case ComputedStyle::TableRowGroup: return "table-row-group";
            case ComputedStyle::TableHeaderGroup: return "table-header-group";
            case ComputedStyle::TableFooterGroup: return "table-footer-group";
            case ComputedStyle::TableColumn: return "table-column";
            case ComputedStyle::TableColumnGroup: return "table-column-group";
            case ComputedStyle::TableCaption: return "table-caption";
            case ComputedStyle::None: return "none";
        }
        return {};
    }
    if (property == "grid-template-columns" || property == "grid-template-rows") {
        const bool columns = property == "grid-template-columns";
        const auto& tracks = columns ? style.gridColumns : style.gridRows;
        const bool autoFill = columns ? style.gridColumnsAutoFill : style.gridRowsAutoFill;
        const bool autoFit = columns ? style.gridColumnsAutoFit : style.gridRowsAutoFit;
        std::string out;
        for (const ComputedStyle::GridTrack& track : tracks) {
            if (!out.empty()) {
                out += ' ';
            }
            std::string max;
            if (track.isAuto) {
                max = "auto";
            } else if (track.isFr) {
                max = unitless(track.fr) + "fr";
            } else {
                max = length(track.size, "0px");
            }
            if (track.hasMin) {
                out += "minmax(" + length(track.minSize, "0px") + ", " + max + ")";
            } else {
                out += max;
            }
        }
        if (autoFill) {
            out = "repeat(" + std::string(autoFit ? "auto-fit" : "auto-fill") + ", " + out +
                  ")";
        }
        return out;
    }
    if (property == "grid-column") {
        if (style.gridColumnStart > 0 && style.gridColumnEnd > 0) {
            return std::to_string(style.gridColumnStart) + " / " +
                   std::to_string(style.gridColumnEnd);
        }
        if (style.gridColumnStart > 0) {
            return std::to_string(style.gridColumnStart) + " / span " +
                   std::to_string(style.gridColumnSpan);
        }
        if (style.gridColumnSpan > 1) {
            return "span " + std::to_string(style.gridColumnSpan);
        }
        return "auto";
    }
    if (property == "grid-column-start") {
        return style.gridColumnStart > 0 ? std::to_string(style.gridColumnStart) : "auto";
    }
    if (property == "grid-column-end") {
        return style.gridColumnEnd > 0 ? std::to_string(style.gridColumnEnd) : "auto";
    }
    if (property == "grid-row") {
        if (style.gridRowStart > 0 && style.gridRowEnd > 0) {
            return std::to_string(style.gridRowStart) + " / " +
                   std::to_string(style.gridRowEnd);
        }
        if (style.gridRowStart > 0) {
            return std::to_string(style.gridRowStart) + " / span " +
                   std::to_string(style.gridRowSpan);
        }
        if (style.gridRowSpan > 1) {
            return "span " + std::to_string(style.gridRowSpan);
        }
        return "auto";
    }
    if (property == "grid-row-start") {
        return style.gridRowStart > 0 ? std::to_string(style.gridRowStart) : "auto";
    }
    if (property == "grid-row-end") {
        return style.gridRowEnd > 0 ? std::to_string(style.gridRowEnd) : "auto";
    }
    if (property == "grid-template-areas") {
        if (!style.gridTemplateAreasRaw.empty()) {
            return style.gridTemplateAreasRaw;
        }
        return "none";
    }
    if (property == "grid-area") {
        if (!style.gridAreaName.empty()) {
            return style.gridAreaName;
        }
        if (style.gridRowStart > 0 || style.gridColumnStart > 0 ||
            style.gridRowEnd > 0 || style.gridColumnEnd > 0) {
            return std::to_string(std::max(1, style.gridRowStart)) + " / " +
                   std::to_string(std::max(1, style.gridColumnStart)) + " / " +
                   std::to_string(std::max(1, style.gridRowEnd)) + " / " +
                   std::to_string(std::max(1, style.gridColumnEnd));
        }
        return "auto";
    }
    if (property == "grid-auto-rows") {
        if (!style.hasGridAutoRows) return "auto";
        const auto& track = style.gridAutoRows;
        std::string max;
        if (track.isAuto) max = "auto";
        else if (track.isFr) max = unitless(track.fr) + "fr";
        else max = length(track.size, "0px");
        return track.hasMin ? ("minmax(" + length(track.minSize, "0px") + ", " + max + ")") : max;
    }
    if (property == "grid-auto-columns") {
        if (!style.hasGridAutoColumns) return "auto";
        const auto& track = style.gridAutoColumns;
        std::string max;
        if (track.isAuto) max = "auto";
        else if (track.isFr) max = unitless(track.fr) + "fr";
        else max = length(track.size, "0px");
        return track.hasMin ? ("minmax(" + length(track.minSize, "0px") + ", " + max + ")") : max;
    }
    if (property == "grid-auto-flow") {
        switch (style.gridAutoFlow) {
            case ComputedStyle::AutoFlowColumn: return "column";
            case ComputedStyle::AutoFlowRowDense: return "row dense";
            case ComputedStyle::AutoFlowColumnDense: return "column dense";
            default: return "row";
        }
    }
    if (property == "color") return color(style.color);
    if (property == "font-kerning") return style.fontKerning ? "normal" : "none";
    if (property == "font-variant") {
        return style.fontVariant == ComputedStyle::FontVariant::SmallCaps ? "small-caps"
                                                                          : "normal";
    }
    if (property == "scroll-behavior") {
        return style.scrollBehavior == ComputedStyle::ScrollBehavior::Smooth ? "smooth"
                                                                            : "auto";
    }
    if (property == "color-scheme") {
        const char* name = style.colorScheme == ComputedStyle::ColorScheme::Dark  ? "dark"
                            : style.colorScheme == ComputedStyle::ColorScheme::Light ? "light"
                                                                                     : "normal";
        if (style.colorSchemeOnly && style.colorScheme != ComputedStyle::ColorScheme::Normal) {
            return std::string("only ") + name;
        }
        return name;
    }
    if (property == "font-stretch") {
        for (const auto& step : {std::pair<float, const char*>{0.5f, "ultra-condensed"},
                                 {0.625f, "extra-condensed"},
                                 {0.75f, "condensed"},
                                 {0.875f, "semi-condensed"},
                                 {1.0f, "normal"},
                                 {1.125f, "semi-expanded"},
                                 {1.25f, "expanded"},
                                 {1.5f, "extra-expanded"},
                                 {2.0f, "ultra-expanded"}}) {
            if (style.fontStretch == step.first) {
                return step.second;
            }
        }
        return std::to_string(static_cast<int>(style.fontStretch * 100.0f)) + "%";
    }
    if (property == "list-style-image") {
        return style.listStyleImage.empty() ? "none" : "url(" + style.listStyleImage + ")";
    }
    if (property == "accent-color") {
        return style.hasAccentColor ? color(style.accentColor) : "auto";
    }
    if (property == "caret-color") {
        return style.hasCaretColor ? color(style.caretColor) : "auto";
    }
    if (property == "mix-blend-mode") {
        switch (style.mixBlendMode) {
            case ComputedStyle::MixBlendMode::Multiply: return "multiply";
            case ComputedStyle::MixBlendMode::Screen: return "screen";
            case ComputedStyle::MixBlendMode::Overlay: return "overlay";
            case ComputedStyle::MixBlendMode::Darken: return "darken";
            case ComputedStyle::MixBlendMode::Lighten: return "lighten";
            case ComputedStyle::MixBlendMode::ColorDodge: return "color-dodge";
            case ComputedStyle::MixBlendMode::ColorBurn: return "color-burn";
            case ComputedStyle::MixBlendMode::HardLight: return "hard-light";
            case ComputedStyle::MixBlendMode::SoftLight: return "soft-light";
            case ComputedStyle::MixBlendMode::Difference: return "difference";
            case ComputedStyle::MixBlendMode::Exclusion: return "exclusion";
            case ComputedStyle::MixBlendMode::Hue: return "hue";
            case ComputedStyle::MixBlendMode::Saturation: return "saturation";
            case ComputedStyle::MixBlendMode::Color: return "color";
            case ComputedStyle::MixBlendMode::Luminosity: return "luminosity";
            default: return "normal";
        }
    }
    if (property == "background-color") return color(style.backgroundColor);
    if (property == "background-blend-mode") {
        if (style.backgroundBlendModes.empty()) {
            return "normal";
        }
        std::string out;
        for (std::size_t i = 0; i < style.backgroundBlendModes.size(); ++i) {
            if (i > 0) {
                out += ", ";
            }
            out += blendModeName(style.backgroundBlendModes[i]);
        }
        return out;
    }
    if (property == "border-color") return color(style.borderColor);
    if (property == "font-size") return number(style.fontSize);
    // line-height is kept as a multiplier, so it serialises without a unit.
    if (property == "line-height") return unitless(style.lineHeight);
    if (property == "font-weight") return std::to_string(style.fontWeight);
    if (property == "font-family") return style.fontFamily;
    if (property == "font-style") {
        switch (style.fontStyle) {
            case ComputedStyle::FontItalic: return "italic";
            case ComputedStyle::FontOblique: return "oblique";
            case ComputedStyle::FontNormal: return "normal";
        }
        return "normal";
    }
    if (property == "text-decoration-line") {
        switch (style.textDecorationLine) {
            case ComputedStyle::DecUnderline: return "underline";
            case ComputedStyle::DecOverline: return "overline";
            case ComputedStyle::DecLineThrough: return "line-through";
            case ComputedStyle::DecNone: return "none";
        }
        return "none";
    }
    if (property == "text-decoration") {
        switch (style.textDecorationLine) {
            case ComputedStyle::DecUnderline: return "underline";
            case ComputedStyle::DecOverline: return "overline";
            case ComputedStyle::DecLineThrough: return "line-through";
            case ComputedStyle::DecNone: return "none";
        }
        return "none";
    }
    if (property == "text-decoration-color") {
        return style.hasTextDecorationColor ? color(style.textDecorationColor)
                                            : color(style.color);
    }
    if (property == "letter-spacing") return length(style.letterSpacing, "normal");
    if (property == "word-spacing") return length(style.wordSpacing, "normal");
    if (property == "tab-size") return length(style.tabSize);
    if (property == "text-indent") return length(style.textIndent, "0px");
    if (property == "text-transform") {
        switch (style.textTransform) {
            case ComputedStyle::TransformUppercase: return "uppercase";
            case ComputedStyle::TransformLowercase: return "lowercase";
            case ComputedStyle::TransformCapitalize: return "capitalize";
            case ComputedStyle::TransformNone: return "none";
        }
        return "none";
    }
    if (property == "white-space") {
        switch (style.whiteSpace) {
            case ComputedStyle::WrapNone: return "nowrap";
            case ComputedStyle::WrapPre: return "pre";
            case ComputedStyle::WrapPreWrap: return "pre-wrap";
            case ComputedStyle::WrapBreakSpaces: return "break-spaces";
            case ComputedStyle::WrapText: return "normal";
        }
        return "normal";
    }
    if (property == "text-wrap-mode") {
        return style.textWrapMode == ComputedStyle::TextWrapMode::Nowrap ? "nowrap" : "wrap";
    }
    if (property == "text-wrap-style") {
        switch (style.textWrapStyle) {
            case ComputedStyle::TextWrapStyle::Balance: return "balance";
            case ComputedStyle::TextWrapStyle::Pretty: return "pretty";
            case ComputedStyle::TextWrapStyle::Auto: break;
        }
        return "auto";
    }
    if (property == "text-wrap") {
        const bool nowrap = style.textWrapMode == ComputedStyle::TextWrapMode::Nowrap;
        switch (style.textWrapStyle) {
            case ComputedStyle::TextWrapStyle::Balance:
                return nowrap ? "nowrap balance" : "balance";
            case ComputedStyle::TextWrapStyle::Pretty:
                return nowrap ? "nowrap pretty" : "pretty";
            case ComputedStyle::TextWrapStyle::Auto: break;
        }
        return nowrap ? "nowrap" : "wrap";
    }
    if (property == "isolation") return style.isolate ? "isolate" : "auto";
    if (property == "vertical-align") {
        switch (style.verticalAlign) {
            case ComputedStyle::VAlignTop: return "top";
            case ComputedStyle::VAlignBottom: return "bottom";
            case ComputedStyle::VAlignMiddle: return "middle";
            case ComputedStyle::VAlignSub: return "sub";
            case ComputedStyle::VAlignSuper: return "super";
            case ComputedStyle::VAlignTextTop: return "text-top";
            case ComputedStyle::VAlignTextBottom: return "text-bottom";
            case ComputedStyle::VAlignBaseline: return "baseline";
        }
        return "baseline";
    }
    if (property == "transform") return style.transform;
    if (property == "filter") return style.filter;
    if (property == "backdrop-filter") return style.backdropFilter;
    if (property == "mask-type") {
        return style.maskType == ComputedStyle::MaskType::Alpha ? "alpha" : "luminance";
    }
    if (property == "transform-origin") {
        std::string result = length(style.transformOrigin.x, "50%");
        const std::string second = length(style.transformOrigin.y, "50%");
        if (second != "50%") {
            result += " " + second;
        }
        if (!style.transformOrigin.z.isAuto() && style.transformOrigin.z.value != 0.0f) {
            result += " " + length(style.transformOrigin.z, "0px");
        }
        return result;
    }
    if (property == "perspective") {
        if (style.perspective <= 0.0f) {
            return "none";
        }
        return number(style.perspective);
    }
    if (property == "transform-style") {
        return style.transformStyle == ComputedStyle::TransformStyle::Preserve3d
                   ? "preserve-3d"
                   : "flat";
    }
    if (property == "backface-visibility") {
        return style.backfaceHidden ? "hidden" : "visible";
    }
    if (property == "text-align") {
        switch (style.textAlign) {
            case ComputedStyle::Left: return "left";
            case ComputedStyle::Center: return "center";
            case ComputedStyle::Right: return "right";
            case ComputedStyle::Justify: return "justify";
            case ComputedStyle::Start: return "start";
            case ComputedStyle::End: return "end";
        }
        return {};
    }
    if (property == "direction") {
        return style.direction == ComputedStyle::Direction::RTL ? "rtl" : "ltr";
    }
    if (property == "quotes") {
        if (style.quotes.empty()) {
            return "none";
        }
        std::string out;
        for (const std::string& mark : style.quotes) {
            if (!out.empty()) {
                out += ' ';
            }
            out += '"';
            out += mark;
            out += '"';
        }
        return out;
    }
    if (property == "text-align-last") {
        switch (style.textAlignLast) {
            case ComputedStyle::TextAlignLast::Left: return "left";
            case ComputedStyle::TextAlignLast::Center: return "center";
            case ComputedStyle::TextAlignLast::Right: return "right";
            case ComputedStyle::TextAlignLast::Justify: return "justify";
            case ComputedStyle::TextAlignLast::Start: return "start";
            case ComputedStyle::TextAlignLast::End: return "end";
            default: return "auto";
        }
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
    if (property == "overflow") {
        const auto name = [](ComputedStyle::Overflow o) -> const char* {
            switch (o) {
                case ComputedStyle::Hidden: return "hidden";
                case ComputedStyle::Scroll: return "scroll";
                case ComputedStyle::Auto: return "auto";
                case ComputedStyle::Visible: return "visible";
            }
            return "visible";
        };
        if (style.overflowX == style.overflowY) {
            return name(style.overflowX);
        }
        return std::string(name(style.overflowX)) + " " + name(style.overflowY);
    }
    if (property == "overflow-x") {
        switch (style.overflowX) {
            case ComputedStyle::Hidden: return "hidden";
            case ComputedStyle::Scroll: return "scroll";
            case ComputedStyle::Auto: return "auto";
            case ComputedStyle::Visible: return "visible";
        }
        return "visible";
    }
    if (property == "overflow-y") {
        switch (style.overflowY) {
            case ComputedStyle::Hidden: return "hidden";
            case ComputedStyle::Scroll: return "scroll";
            case ComputedStyle::Auto: return "auto";
            case ComputedStyle::Visible: return "visible";
        }
        return "visible";
    }
    if (property == "clip-path") {
        using Clip = ComputedStyle::ClipPath;
        const auto& clip = style.clipPath;
        if (clip.shape == Clip::Shape::None) {
            return "none";
        }
        const auto boxName = [&] {
            switch (clip.box) {
                case Clip::Box::Margin: return " margin-box";
                case Clip::Box::Padding: return " padding-box";
                case Clip::Box::Content: return " content-box";
                case Clip::Box::Border: return "";
            }
            return "";
        };
        const auto radialName = [&](Clip::Radial radial, const Length& size) {
            switch (radial) {
                case Clip::Radial::ClosestSide: return std::string("closest-side");
                case Clip::Radial::FarthestSide: return std::string("farthest-side");
                case Clip::Radial::ClosestCorner: return std::string("closest-corner");
                case Clip::Radial::FarthestCorner: return std::string("farthest-corner");
                case Clip::Radial::Explicit: return length(size);
            }
            return std::string();
        };
        if (clip.shape == Clip::Shape::Inset) {
            std::string out = "inset(" + length(clip.inset[0]) + " " +
                              length(clip.inset[1]) + " " +
                              length(clip.inset[2]) + " " +
                              length(clip.inset[3]);
            if (clip.insetRadius.unit != Length::Auto) {
                out += " round " + length(clip.insetRadius);
            }
            return out + ")" + boxName();
        }
        if (clip.shape == Clip::Shape::Circle) {
            return "circle(" + radialName(clip.radius, clip.radiusLength) + " at " +
                   length(clip.centerX) + " " + length(clip.centerY) + ")" +
                   boxName();
        }
        if (clip.shape == Clip::Shape::Ellipse) {
            return "ellipse(" + radialName(clip.radiusX, clip.radiusXLength) + " " +
                   radialName(clip.radiusY, clip.radiusYLength) + " at " +
                   length(clip.centerX) + " " + length(clip.centerY) + ")" +
                   boxName();
        }
        std::string out = "polygon(";
        if (clip.evenOdd) {
            out += "evenodd, ";
        }
        bool first = true;
        for (const auto& point : clip.points) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += length(point.x) + " " + length(point.y);
        }
        return out + ")" + boxName();
    }
    if (property == "container-type") {
        switch (style.containerType) {
            case ComputedStyle::ContainerInlineSize: return "inline-size";
            case ComputedStyle::ContainerSize: return "size";
            case ComputedStyle::ContainerNormal: return "normal";
        }
        return "normal";
    }
    if (property == "container-name") {
        if (style.containerNames.empty()) {
            return "none";
        }
        std::string names;
        for (const std::string& name : style.containerNames) {
            if (!names.empty()) {
                names += ' ';
            }
            names += name;
        }
        return names;
    }
    if (property == "margin-top") return length(style.margin.top);
    if (property == "margin-right") return length(style.margin.right);
    if (property == "margin-bottom") return length(style.margin.bottom);
    if (property == "margin-left") return length(style.margin.left);
    if (property == "margin-inline-start") return length(style.margin.left);
    if (property == "margin-inline-end") return length(style.margin.right);
    if (property == "margin-block-start") return length(style.margin.top);
    if (property == "margin-block-end") return length(style.margin.bottom);
    if (property == "padding-top") return length(style.padding.top);
    if (property == "padding-right") return length(style.padding.right);
    if (property == "padding-bottom") return length(style.padding.bottom);
    if (property == "padding-left") return length(style.padding.left);
    if (property == "padding-inline-start") return length(style.padding.left);
    if (property == "padding-inline-end") return length(style.padding.right);
    if (property == "padding-block-start") return length(style.padding.top);
    if (property == "padding-block-end") return length(style.padding.bottom);
    if (property == "top") return length(style.top, "auto");
    if (property == "right") return length(style.right, "auto");
    if (property == "bottom") return length(style.bottom, "auto");
    if (property == "left") return length(style.left, "auto");
    if (property == "inset-inline-start") return length(style.left, "auto");
    if (property == "inset-inline-end") return length(style.right, "auto");
    if (property == "inset-block-start") return length(style.top, "auto");
    if (property == "inset-block-end") return length(style.bottom, "auto");
    // The box shorthands read back in the form the cascade writes them: one
    // value while both sides agree, `start end` otherwise, and for the
    // four-sided ones a one-to-four value list collapsed the way `margin`
    // distributes it. Reading them back is also what lets their CSS-wide
    // keywords round-trip, since `inherit`, `unset` and `initial` re-apply this
    // serialisation rather than carrying a style object around.
    const auto collapsed = [](const std::string& top, const std::string& right,
                              const std::string& bottom, const std::string& left) {
        if (top == right && right == bottom && bottom == left) {
            return top;
        }
        if (top == bottom && left == right) {
            return top + " " + right;
        }
        if (left == right) {
            return top + " " + right + " " + bottom;
        }
        return top + " " + right + " " + bottom + " " + left;
    };
    const auto twoSides = [&](const Length& start, const Length& end,
                              const char* autoKeyword) {
        const std::string first = length(start, autoKeyword);
        const std::string second = length(end, autoKeyword);
        return first == second ? first : first + " " + second;
    };
    const auto fourSides = [&](const BoxEdge& edges, const char* autoKeyword) {
        return collapsed(length(edges.top, autoKeyword), length(edges.right, autoKeyword),
                         length(edges.bottom, autoKeyword), length(edges.left, autoKeyword));
    };
    if (property == "margin") return fourSides(style.margin, "0px");
    if (property == "margin-inline")
        return twoSides(style.margin.left, style.margin.right, "0px");
    if (property == "margin-block")
        return twoSides(style.margin.top, style.margin.bottom, "0px");
    if (property == "padding") return fourSides(style.padding, "0px");
    if (property == "padding-inline")
        return twoSides(style.padding.left, style.padding.right, "0px");
    if (property == "padding-block")
        return twoSides(style.padding.top, style.padding.bottom, "0px");
    if (property == "inset") {
        return fourSides(BoxEdge{style.top, style.right, style.bottom, style.left}, "auto");
    }
    if (property == "inset-inline") return twoSides(style.left, style.right, "auto");
    if (property == "inset-block") return twoSides(style.top, style.bottom, "auto");
    if (property == "border-width-top" || property == "border-top-width")
        return length(style.borderWidth.top);
    if (property == "border-width-right" || property == "border-right-width")
        return length(style.borderWidth.right);
    if (property == "border-width-bottom" || property == "border-bottom-width")
        return length(style.borderWidth.bottom);
    if (property == "border-width-left" || property == "border-left-width")
        return length(style.borderWidth.left);
    // A border side reads back as `<width> <style> <colour>`, the form the
    // shorthands themselves accept, so getComputedStyle answers what an author
    // could have written. The inline and block pairs cover two sides each and
    // can only read back as one value while both sides agree; a shorthand that
    // cannot represent its sides has no serialisation, as in CSSOM.
    const auto borderLine = [&](const Length& width, ComputedStyle::BorderStyle borderStyle) {
        return length(width) + " " + std::string(borderStyleName(borderStyle)) + " " +
               color(style.borderColor);
    };
    const auto borderLines = [&](const Length& widthA, ComputedStyle::BorderStyle styleA,
                                 const Length& widthB, ComputedStyle::BorderStyle styleB) {
        if (length(widthA) != length(widthB) || styleA != styleB) {
            return std::string();
        }
        return borderLine(widthA, styleA);
    };
    if (property == "border-top" || property == "border-block-start")
        return borderLine(style.borderWidth.top, style.borderStyles.top);
    if (property == "border-right" || property == "border-inline-end")
        return borderLine(style.borderWidth.right, style.borderStyles.right);
    if (property == "border-bottom" || property == "border-block-end")
        return borderLine(style.borderWidth.bottom, style.borderStyles.bottom);
    if (property == "border-left" || property == "border-inline-start")
        return borderLine(style.borderWidth.left, style.borderStyles.left);
    if (property == "border-inline")
        return borderLines(style.borderWidth.left, style.borderStyles.left,
                           style.borderWidth.right, style.borderStyles.right);
    if (property == "border-block")
        return borderLines(style.borderWidth.top, style.borderStyles.top,
                           style.borderWidth.bottom, style.borderStyles.bottom);
    // `border-width` and `border-style` are four-sided shorthands distributed
    // like `margin`, while `border` writes all three slots at once and therefore
    // reads back only while all four of its sides agree, as CSSOM asks.
    if (property == "border-width") return fourSides(style.borderWidth, "0px");
    if (property == "border-style") {
        return collapsed(std::string(borderStyleName(style.borderStyles.top)),
                         std::string(borderStyleName(style.borderStyles.right)),
                         std::string(borderStyleName(style.borderStyles.bottom)),
                         std::string(borderStyleName(style.borderStyles.left)));
    }
    if (property == "border") {
        const std::string top = borderLine(style.borderWidth.top, style.borderStyles.top);
        const std::string right = borderLine(style.borderWidth.right, style.borderStyles.right);
        const std::string bottom =
            borderLine(style.borderWidth.bottom, style.borderStyles.bottom);
        const std::string left = borderLine(style.borderWidth.left, style.borderStyles.left);
        return top == right && right == bottom && bottom == left ? top : std::string();
    }
    // Outlines are the border family's single-sided counterpart: the same
    // `<width> <style> <colour>` triple, plus an offset that is a plain length.
    if (property == "outline") {
        return length(style.outline.width) + " " +
               std::string(borderStyleName(style.outline.style)) + " " +
               color(style.outline.color);
    }
    if (property == "outline-width") return length(style.outline.width);
    if (property == "outline-style") return std::string(borderStyleName(style.outline.style));
    if (property == "outline-color") return color(style.outline.color);
    if (property == "outline-offset") return length(style.outline.offset);
    // Placement, stacking, and compositing values: they have always driven
    // layout and paint, but the cascade had no way to read them back, so their
    // CSS-wide keywords were dropped as well.
    if (property == "position") {
        switch (style.position) {
            case ComputedStyle::Static: return "static";
            case ComputedStyle::Relative: return "relative";
            case ComputedStyle::Absolute: return "absolute";
            case ComputedStyle::Fixed: return "fixed";
            case ComputedStyle::Sticky: return "sticky";
        }
        return std::string();
    }
    if (property == "z-index") {
        // `auto` and `0` share one slot (both order as zero), so `auto` is the
        // reading that round-trips.
        return style.zIndex == 0 ? std::string("auto") : std::to_string(style.zIndex);
    }
    if (property == "opacity") return unitless(style.opacity);
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
    if (property == "justify-items") return alignName(style.justifyItems);
    if (property == "justify-self") return alignName(style.justifySelf);
    if (property == "flex") {
        return unitless(style.flex.grow) + " " + unitless(style.flex.shrink) + " " +
               length(style.flex.basis, "auto");
    }
    if (property == "flex-grow") return unitless(style.flex.grow);
    if (property == "flex-shrink") return unitless(style.flex.shrink);
    if (property == "flex-basis") return length(style.flex.basis, "auto");
    // Gaps resolve to px at parse time; `normal` is the unauthored state, which
    // the two longhands record as a negative number.
    if (property == "gap" || property == "grid-gap") {
        const auto side = [&](float px) {
            return px < 0.0f ? std::string("normal") : number(px);
        };
        const std::string row = side(style.rowGap);
        const std::string column = side(style.columnGap);
        return row == column ? row : row + " " + column;
    }
    if (property == "row-gap") {
        return style.rowGap < 0.0f ? std::string("normal") : number(style.rowGap);
    }
    if (property == "column-gap") {
        return style.columnGap < 0.0f ? std::string("normal") : number(style.columnGap);
    }
    if (property == "transition" || property == "transition-property" ||
        property == "transition-duration" || property == "transition-delay" ||
        property == "transition-timing-function") {
        return serializeTransition(property, style);
    }
    if (property == "animation" || property == "animation-name" ||
        property == "animation-duration" || property == "animation-delay" ||
        property == "animation-timing-function" ||
        property == "animation-iteration-count" || property == "animation-direction" ||
        property == "animation-fill-mode" || property == "animation-play-state" ||
        property == "animation-composition") {
        return serializeAnimation(property, style);
    }
    if (property == "object-fit") {
        switch (style.objectFit) {
            case ComputedStyle::ObjCover: return "cover";
            case ComputedStyle::ObjFill: return "fill";
            case ComputedStyle::ObjNone: return "none";
            case ComputedStyle::ObjScaleDown: return "scale-down";
            case ComputedStyle::ObjContain: return "contain";
        }
        return "contain";
    }
    if (property == "list-style-type") {
        switch (style.listStyleType) {
            case ComputedStyle::LSTCircle: return "circle";
            case ComputedStyle::LSTSquare: return "square";
            case ComputedStyle::LSTDecimal: return "decimal";
            case ComputedStyle::LSTUpperRoman: return "upper-roman";
            case ComputedStyle::LSTLowerRoman: return "lower-roman";
            case ComputedStyle::LSTUpperAlpha: return "upper-alpha";
            case ComputedStyle::LSTLowerAlpha: return "lower-alpha";
            case ComputedStyle::LSTNone: return "none";
            case ComputedStyle::LSTDisc: return "disc";
        }
        return "disc";
    }
    if (property == "list-style-position") {
        return style.listStylePosition == ComputedStyle::LSPInside ? "inside" : "outside";
    }
    if (property == "float") {
        switch (style.floatType) {
            case ComputedStyle::FloatLeft: return "left";
            case ComputedStyle::FloatRight: return "right";
            default: return "none";
        }
    }
    if (property == "clear") {
        switch (style.clearType) {
            case ComputedStyle::ClearLeft: return "left";
            case ComputedStyle::ClearRight: return "right";
            case ComputedStyle::ClearBoth: return "both";
            default: return "none";
        }
    }
    if (property == "pointer-events") {
        return style.pointerEventsNone ? "none" : "auto";
    }
    if (property == "cursor") {
        return std::string(cursorName(style.cursor));
    }
    if (property == "counter-reset" || property == "counter-increment" ||
        property == "counter-set") {
        const std::vector<ComputedStyle::CounterOp>& ops =
            property == "counter-reset"     ? style.counterReset
            : property == "counter-increment" ? style.counterIncrement
                                              : style.counterSet;
        std::string out;
        for (const auto& op : ops) {
            if (!out.empty()) {
                out += ' ';
            }
            out += op.name;
            out += ' ';
            out += std::to_string(op.value);
        }
        return out.empty() ? std::string("none") : out;
    }
    if (property == "list-style-position") {
        return style.listStylePosition == ComputedStyle::LSPInside ? "inside" : "outside";
    }
    if (property == "float") {
        switch (style.floatType) {
            case ComputedStyle::FloatLeft: return "left";
            case ComputedStyle::FloatRight: return "right";
            default: return "none";
        }
    }
    if (property == "clear") {
        switch (style.clearType) {
            case ComputedStyle::ClearLeft: return "left";
            case ComputedStyle::ClearRight: return "right";
            case ComputedStyle::ClearBoth: return "both";
            default: return "none";
        }
    }
    if (property == "border-collapse") {
        return style.borderCollapse == ComputedStyle::CollapseCollapse ? "collapse"
                                                                      : "separate";
    }
    if (property == "border-spacing") {
        if (style.borderSpacingH == style.borderSpacingV) {
            return number(style.borderSpacingH);
        }
        return number(style.borderSpacingH) + " " + number(style.borderSpacingV);
    }
    if (property == "table-layout") {
        return style.tableLayout == ComputedStyle::TableLayoutFixed ? "fixed" : "auto";
    }
    if (property == "line-clamp") {
        return style.lineClamp > 0 ? std::to_string(style.lineClamp) : "none";
    }
    if (property == "resize") {
        switch (style.resize) {
            case ComputedStyle::ResizeBoth: return "both";
            case ComputedStyle::ResizeHorizontal: return "horizontal";
            case ComputedStyle::ResizeVertical: return "vertical";
            case ComputedStyle::ResizeNone: return "none";
        }
        return "none";
    }
    if (property == "empty-cells") {
        return style.emptyCells == ComputedStyle::EmptyCellsHide ? "hide" : "show";
    }
    if (property == "caption-side") {
        return style.captionSide == ComputedStyle::CaptionBottom ? "bottom" : "top";
    }
    if (property == "text-overflow") {
        return style.textOverflow == ComputedStyle::TextOverflowEllipsis ? "ellipsis" : "clip";
    }
    if (property == "word-break") {
        switch (style.wordBreak) {
            case ComputedStyle::WordBreak::BreakAll: return "break-all";
            case ComputedStyle::WordBreak::KeepAll: return "keep-all";
            default: return "normal";
        }
    }
    if (property == "overflow-wrap" || property == "word-wrap") {
        return style.overflowWrap == ComputedStyle::OverflowWrap::BreakWord ? "break-word"
                                                                            : "normal";
    }
    if (property == "hyphens") {
        switch (style.hyphens) {
            case ComputedStyle::Hyphens::None: return "none";
            case ComputedStyle::Hyphens::Auto: return "auto";
            case ComputedStyle::Hyphens::Manual: return "manual";
        }
        return "manual";
    }
    if (property == "object-position") {
        const auto length = [](const Length& l) {
            switch (l.unit) {
                case Length::Percent: return unitless(l.value) + "%";
                case Length::Px: return unitless(l.value) + "px";
                case Length::Em: return unitless(l.value) + "em";
                case Length::Rem: return unitless(l.value) + "rem";
                default: return unitless(l.value);
            }
        };
        return length(style.objectPositionX) + " " + length(style.objectPositionY);
    }
    if (property == "text-size-adjust") {
        switch (style.textSizeAdjust) {
            case ComputedStyle::TextSizeAdjust::None: return "none";
            case ComputedStyle::TextSizeAdjust::Percent:
                return unitless(style.textSizeAdjustPercent) + "%";
            default: return "auto";
        }
    }
    if (property == "aspect-ratio") {
        if (style.aspectRatioW <= 0.0f || style.aspectRatioH <= 0.0f) {
            return "auto";
        }
        if (style.aspectRatioH == 1.0f) {
            return unitless(style.aspectRatioW);
        }
        return unitless(style.aspectRatioW) + " / " + unitless(style.aspectRatioH);
    }
    return {};
}

// Splits an `:is(...)` / `:not(...)` argument on top-level commas and parses each
// branch. Commas nested inside another functional pseudo are ignored so that
// `:not(:is(a, b))` stays one branch.
std::vector<SimpleSelector> parseSelectorList(std::string_view text) {
    std::vector<SimpleSelector> out;
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const bool atEnd = i == text.size();
        const char c = atEnd ? '\0' : text[i];
        if (!atEnd && c == '(') {
            ++depth;
        } else if (!atEnd && c == ')') {
            --depth;
        }
        if (atEnd || (c == ',' && depth == 0)) {
            std::string_view piece = text.substr(start, i - start);
            while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front()))) {
                piece.remove_prefix(1);
            }
            while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back()))) {
                piece.remove_suffix(1);
            }
            if (!piece.empty()) {
                out.push_back(CSSParser::parseSelector(piece));
            }
            start = i + 1;
        }
    }
    return out;
}

// Evaluates the An+B microsyntax `:nth-child()` accepts, against a 1-based index.
bool nthChildMatches(std::string_view arg, int index) {
    std::string s;
    for (char c : arg) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (s.empty()) {
        return false;
    }
    if (s == "odd") {
        return index % 2 == 1;
    }
    if (s == "even") {
        return index % 2 == 0;
    }
    const std::size_t n = s.find('n');
    if (n == std::string::npos) {
        return std::atoi(s.c_str()) == index;
    }
    // An+B: the part before `n` is A (empty means 1, "-" means -1), the part after
    // is a signed B offset that may be absent.
    const std::string_view aPart = std::string_view(s).substr(0, n);
    int a = 1;
    if (aPart == "-") {
        a = -1;
    } else if (!aPart.empty()) {
        a = std::atoi(std::string(aPart).c_str());
    }
    int b = 0;
    const std::string_view bPart = std::string_view(s).substr(n + 1);
    if (!bPart.empty()) {
        b = std::atoi(std::string(bPart).c_str());
    }
    if (a == 0) {
        return index == b;
    }
    const int diff = index - b;
    return diff % a == 0 && diff / a >= 0;
}

// --- `:has()` -------------------------------------------------------------

// How a `:has()` argument is anchored to the element being matched.
enum class RelativeCombinator { Descendant, Child, NextSibling, LaterSibling };

// True when any descendant of `element` (skipping text nodes and generated
// boxes) matches `branch`.
bool anyDescendantMatches(const SimpleSelector& branch, const Element& element,
                          const PseudoState* state) {
    for (const auto& child : element.children()) {
        if (!child || child->isTextNode() || child->isPseudoElement()) {
            continue;
        }
        if (selectorMatches(branch, *child, state)) {
            return true;
        }
        if (anyDescendantMatches(branch, *child, state)) {
            return true;
        }
    }
    return false;
}

// The next element sibling after `element`, ignoring text nodes.
const Element* nextElementSibling(const Element& element) {
    const Element* parent = element.parent();
    if (!parent) {
        return nullptr;
    }
    bool seen = false;
    for (const auto& sibling : parent->children()) {
        if (!sibling) {
            continue;
        }
        if (!seen) {
            seen = sibling.get() == &element;
            continue;
        }
        if (!sibling->isTextNode()) {
            return sibling.get();
        }
    }
    return nullptr;
}

// Evaluates one relative selector against `element`: the leading combinator
// picks the candidates, and the branch is matched against each with the ordinary
// matcher.
bool relativeMatches(const SimpleSelector& branch, const Element& element,
                     RelativeCombinator leading, const PseudoState* state) {
    switch (leading) {
        case RelativeCombinator::Child:
            for (const auto& child : element.children()) {
                if (child && !child->isTextNode() && !child->isPseudoElement() &&
                    selectorMatches(branch, *child, state)) {
                    return true;
                }
            }
            return false;
        case RelativeCombinator::Descendant:
            return anyDescendantMatches(branch, element, state);
        case RelativeCombinator::NextSibling: {
            const Element* next = nextElementSibling(element);
            return next && selectorMatches(branch, *next, state);
        }
        case RelativeCombinator::LaterSibling:
            for (const Element* sibling = nextElementSibling(element); sibling;
                 sibling = nextElementSibling(*sibling)) {
                if (selectorMatches(branch, *sibling, state)) {
                    return true;
                }
            }
            return false;
    }
    return false;
}

// `:has(rs)` matches when an element described by the relative selector list `rs`
// exists relative to `element`. The list is split on top-level commas; a leading
// `>`, `+`, or `~` anchors each branch, and no combinator means a descendant. The
// rest of the branch is parsed as an ordinary selector. A branch the parser cannot
// understand is skipped rather than failing the whole list.
bool hasMatches(std::string_view argument, const Element& element,
                const PseudoState* state) {
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i <= argument.size(); ++i) {
        const bool atEnd = i == argument.size();
        const char c = atEnd ? '\0' : argument[i];
        if (!atEnd && c == '(') {
            ++depth;
        } else if (!atEnd && c == ')') {
            --depth;
        }
        if (!atEnd && !(c == ',' && depth == 0)) {
            continue;
        }
        std::string_view piece = trim(argument.substr(start, i - start));
        start = i + 1;
        if (piece.empty()) {
            continue;
        }
        RelativeCombinator leading = RelativeCombinator::Descendant;
        if (piece.front() == '>') {
            leading = RelativeCombinator::Child;
            piece.remove_prefix(1);
        } else if (piece.front() == '+') {
            leading = RelativeCombinator::NextSibling;
            piece.remove_prefix(1);
        } else if (piece.front() == '~') {
            leading = RelativeCombinator::LaterSibling;
            piece.remove_prefix(1);
        }
        piece = trim(piece);
        if (piece.empty()) {
            continue;
        }
        const SimpleSelector branch = CSSParser::parseSelector(piece);
        if (!branch.unsupported && relativeMatches(branch, element, leading, state)) {
            return true;
        }
    }
    return false;
}

// Element siblings for the structural pseudo-classes: text nodes and
// generated boxes never count, so mixed content and `::before`/`::after`
// leave indices alone. Empty when the element has no parent.
std::vector<const Element*> elementSiblings(const Element& element) {
    std::vector<const Element*> siblings;
    const Element* parent = element.parent();
    if (!parent) {
        return siblings;
    }
    for (const auto& child : parent->children()) {
        if (child && !child->isTextNode() && !child->isPseudoElement()) {
            siblings.push_back(child.get());
        }
    }
    return siblings;
}

bool selectorMatches(const SimpleSelector& selector, const Element& element,
                     const PseudoState* state) {
    if (selector.unsupported) {
        return false;
    }
    // Interactive and structural pseudo-classes
    for (const PseudoClass pseudo : selector.pseudoClasses) {
        if (pseudo == PseudoClass::Hover || pseudo == PseudoClass::Active ||
            pseudo == PseudoClass::Focus || pseudo == PseudoClass::FocusWithin) {
            const Element* target = nullptr;
            switch (pseudo) {
                case PseudoClass::Hover: target = state ? state->hovered : nullptr; break;
                case PseudoClass::Active: target = state ? state->active : nullptr; break;
                case PseudoClass::Focus:
                case PseudoClass::FocusWithin: target = state ? state->focused : nullptr; break;
                default: break;
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
            continue;
        }
        if (pseudo == PseudoClass::FocusVisible) {
            // The focused element, but only when it wants visible focus UI:
            // keyboard-driven focus, or a control that takes text entry.
            if (!state || state->focused != &element || !state->focusVisible) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::Root) {
            if (element.parent()) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::Checked) {
            // `:checked` is a form-control state: a checkable <input> (checkbox
            // or radio) carrying the boolean `checked` attribute.
            const std::string_view type = element.attribute("type");
            const bool checkable =
                element.tag() == "input" && (type == "checkbox" || type == "radio");
            if (!checkable || !element.hasAttribute("checked")) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::Disabled || pseudo == PseudoClass::Enabled) {
            // Only form controls take `disabled`: anything else matches
            // neither. An option in a disabled optgroup counts as disabled.
            const std::string_view tag = element.tag();
            const bool disableable =
                tag == "input" || tag == "button" || tag == "select" ||
                tag == "textarea" || tag == "optgroup" || tag == "option";
            if (!disableable) {
                return false;
            }
            bool disabled = element.hasAttribute("disabled");
            if (!disabled && tag == "option") {
                if (const Element* parent = element.parent()) {
                    disabled = parent->tag() == "optgroup" &&
                               parent->hasAttribute("disabled");
                }
            }
            if ((pseudo == PseudoClass::Disabled) != disabled) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::Required || pseudo == PseudoClass::Optional) {
            // Only inputs, selects, and textareas take `required`: anything
            // else matches neither.
            const std::string_view tag = element.tag();
            if (tag != "input" && tag != "select" && tag != "textarea") {
                return false;
            }
            if ((pseudo == PseudoClass::Required) != element.hasAttribute("required")) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::Valid || pseudo == PseudoClass::Invalid) {
            // Candidates that satisfy their constraints match `:valid` and
            // the rest match `:invalid`; barred controls match neither. The
            // engine lives on the element so scripts share it.
            if (!element.willValidate()) {
                return false;
            }
            if ((pseudo == PseudoClass::Valid) != element.checkValidity()) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::FirstChild || pseudo == PseudoClass::LastChild ||
            pseudo == PseudoClass::OnlyChild) {
            const std::vector<const Element*> siblings = elementSiblings(element);
            if (siblings.empty()) {
                return false;
            }
            if (pseudo == PseudoClass::FirstChild) {
                if (siblings.front() != &element) {
                    return false;
                }
            } else if (pseudo == PseudoClass::LastChild) {
                if (siblings.back() != &element) {
                    return false;
                }
            } else if (siblings.size() != 1 || siblings.front() != &element) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::NthChild || pseudo == PseudoClass::NthLastChild) {
            const std::vector<const Element*> siblings = elementSiblings(element);
            std::size_t at = siblings.size();
            for (std::size_t i = 0; i < siblings.size(); ++i) {
                if (siblings[i] == &element) {
                    at = i;
                    break;
                }
            }
            if (at >= siblings.size()) {
                return false;
            }
            const int index = pseudo == PseudoClass::NthChild
                                  ? static_cast<int>(at) + 1
                                  : static_cast<int>(siblings.size() - at);
            if (!nthChildMatches(selector.pseudoArgument, index)) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::NthOfType) {
            int index = 0;
            int seen = 0;
            for (const Element* sibling : elementSiblings(element)) {
                if (sibling->tag() == element.tag()) {
                    ++seen;
                    if (sibling == &element) {
                        index = seen;
                    }
                }
            }
            if (index == 0) {
                return false;
            }
            if (!nthChildMatches(selector.pseudoArgument, index)) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::NthLastOfType) {
            int index = 0;
            int seen = 0;
            const std::vector<const Element*> siblings = elementSiblings(element);
            for (std::size_t i = siblings.size(); i-- > 0;) {
                if (siblings[i]->tag() == element.tag()) {
                    ++seen;
                    if (siblings[i] == &element) {
                        index = seen;
                    }
                }
            }
            if (index == 0) {
                return false;
            }
            if (!nthChildMatches(selector.pseudoArgument, index)) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::OnlyOfType) {
            int count = 0;
            bool self = false;
            for (const Element* sibling : elementSiblings(element)) {
                if (sibling->tag() == element.tag()) {
                    ++count;
                    if (sibling == &element) {
                        self = true;
                    }
                }
            }
            if (count != 1 || !self) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::Has) {
            // `:has()` looks at the element's subtree and following siblings, so it
            // is evaluated against the element itself rather than an ancestor.
            if (!hasMatches(selector.pseudoArgument, element, state)) {
                return false;
            }
            continue;
        }
        if (pseudo == PseudoClass::Not || pseudo == PseudoClass::Is) {
            // `:not()` and `:is()` take a full selector list. Each branch is parsed
            // and tested against the element with the same state, and the compound
            // matches when every `:not` fails and any `:is` hits.
            const std::vector<SimpleSelector> branches = parseSelectorList(selector.pseudoArgument);
            bool any = false;
            for (const SimpleSelector& branch : branches) {
                if (selectorMatches(branch, element, state)) {
                    any = true;
                    break;
                }
            }
            if (pseudo == PseudoClass::Not ? any : !any) {
                return false;
            }
            continue;
        }
        return false;
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
    return value == "inherit" || value == "unset" || value == "initial" ||
           value == "revert";
}

// The properties `unset` inherits when no other value applies. Mirrors the set
// inheritInto() fills in.
bool isInheritable(std::string_view property) {
    return property == "color" || property == "font-size" ||
           property == "font-family" || property == "font-weight" ||
           property == "line-height" || property == "text-align" ||
           property == "list-style-type" || property == "list-style" ||
           property == "text-decoration-color" || property == "pointer-events" ||
           property == "cursor" || property == "list-style-position" ||
           property == "quotes" || property == "caret-color" || property == "direction" ||
           property == "font-variant" || property == "text-align-last" ||
           property == "empty-cells" || property == "tab-size" ||
           property == "white-space" || property == "text-wrap" ||
           property == "text-wrap-mode" || property == "text-wrap-style" ||
           property == "hyphens";
}

// `inherit` and `unset` are the two CSS-wide keywords the engine can answer
// exactly: the first always takes the parent's value, the second takes it for an
// inheritable property and is otherwise left to the property's own initial value,
// which is what a default-constructed `ComputedStyle` already holds.
void applyCssWideKeyword(ComputedStyle& style, Element* element,
                          const Declaration& decl, const ComputedStyle& parent) {
    const std::string_view value = trim(decl.value);
    // `unset` is `inherit` for inherited properties and `initial` for the rest,
    // so it shares both paths instead of doing nothing on the non-inherited half.
    ComputedStyle source;
    if (value == "revert") {
        source = element ? defaultStyleFor(element->tag()) : ComputedStyle();
        if (element && element->tag() == "input") {
            applyInputTypeStyle(source, element->attribute("type"));
        }
    } else if (value == "initial" || (value == "unset" && !isInheritable(decl.property))) {
        source = ComputedStyle();
    } else if (value == "inherit" || value == "unset") {
        source = parent;
    } else {
        return;
    }
    applyDeclaration(style,
                     Declaration{decl.property,
                                 StyleResolver::propertyValue(source, decl.property),
                                 decl.important},
                     0.0f);
}

}  // namespace

// `currentColor` asks for the element's own `color`, which is only settled
// once inheritance and the cascade are done. Every parked placeholder is
// swapped out here, before the style leaves the resolver.
void substituteCurrentColor(ComputedStyle& style) {
    const auto swap = [&](SDL_Color& color) {
        if (isCurrentColor(color)) {
            color = style.color;
        }
    };
    const auto swapStops = [&](std::vector<ComputedStyle::GradientStop>& stops) {
        for (auto& stop : stops) {
            swap(stop.color);
        }
    };
    swap(style.backgroundColor);
    swap(style.borderColor);
    swap(style.accentColor);
    swap(style.textDecorationColor);
    if (style.hasCaretColor) {
        swap(style.caretColor);
    }
    swap(style.outline.color);
    for (auto& shadow : style.boxShadows) {
        swap(shadow.color);
    }
    for (auto& shadow : style.textShadows) {
        swap(shadow.color);
    }
    for (auto& image : style.backgroundImages) {
        swapStops(image.linear.stops);
        swapStops(image.radial.stops);
        swapStops(image.conical.stops);
    }
}

// The first <summary> child, or null when there is none.
const Element* disclosureSummary(const Element& details) {
    for (const auto& child : details.children()) {
        if (child && !child->isTextNode() && child->tag() == "summary") {
            return child.get();
        }
    }
    return nullptr;
}

// Whether a <dialog> without `open` takes no box, the user-agent rule
// `dialog:not([open])`. Positioned before inheritance so author declarations
// still win below, the way closed details stay hidden.
bool hiddenByClosedDialog(const Element& element) {
    return element.tag() == "dialog" && !element.hasAttribute("open");
}

// Whether the element is hidden by a closed <details>: anything but its first
// <summary> (and with no summary, everything) takes no box.
bool hiddenByClosedDetails(const Element& element) {
    const Element* parent = element.parent();
    if (!parent || parent->tag() != "details" || parent->hasAttribute("open")) {
        return false;
    }
    return &element != disclosureSummary(*parent);
}

ComputedStyle StyleResolver::resolveWithParent(Element& element,
                                               const ComputedStyle& parent) const {
    ComputedStyle style = defaultStyleFor(element.tag());
    // A closed <details> shows only its first <summary>, like the UA rule
    // `details:not([open]) > :not(summary)`. Positioned before inheritance so
    // author declarations still win below.
    if (hiddenByClosedDetails(element)) {
        style.display = ComputedStyle::None;
    }
    if (hiddenByClosedDialog(element)) {
        style.display = ComputedStyle::None;
    }
    if (element.tag() == "input") {
        applyInputTypeStyle(style, element.attribute("type"));
    }
    if (element.tag() == "button" && element.hasAttribute("disabled")) {
        // A disabled button dims; an author opacity still overrides below.
        style.opacity = 0.5f;
    }
    if (element.tag() == "select" && element.listboxRows() > 0) {
        // An open list scrolls its rows: the UA default clips and scrolls,
        // which author overflow still overrides below.
        style.overflowX = ComputedStyle::Auto;
        style.overflowY = ComputedStyle::Auto;
    }
    if ((element.tag() == "option" || element.tag() == "optgroup") &&
        element.owningSelect() && element.owningSelect()->listboxRows() > 0) {
        // Options lay out as rows only inside an open list; a dropdown's
        // options paint from the popup instead and take no space.
        style.display = ComputedStyle::Block;
    }
    inheritInto(style, parent);
    applyPhrasingStyle(style, element.tag(), parent);

    // `!important` is a cascade level of its own, not a tiebreak: it outranks a
    // stronger selector at an ordinary weight. So the declarations are applied in
    // two passes, each already sorted weakest-first.
    const std::vector<Declaration> inlineDecls =
        element.hasAttribute("style")
            ? parseDeclarations(element.attribute("style"))
            : std::vector<Declaration>{};

    // A custom property is a value, not a style, and `var()` has to be substituted
    // against the *final* map, not the map as it stood when the consuming rule was
    // reached. So `div { --x: 1; width: var(--x) } div { --x: 2 }` must give width
    // 2. When anything in reach refers to a custom property, the matched
    // declarations are therefore walked twice: once to settle the custom map, then
    // once to apply. The apply pass keeps its order, so a `var()` that loses its
    // cascade position loses it exactly as a literal value would.
    bool customInPlay = false;
    for (const auto& rule : rules_) {
        if (rule.usesCustom) {
            customInPlay = true;
            break;
        }
    }
    if (!customInPlay) {
        for (const auto& decl : inlineDecls) {
            if (isCustomProperty(decl.property) || containsVars(decl.value)) {
                customInPlay = true;
                break;
            }
        }
    }

    const auto collectCustom = [&](bool important) {
        const auto visit = [&](const Rule& rule) {
            if (rule.layerStatement || !rule.usesCustom) {
                return;
            }
            if (!rule.media.empty() &&
                !mediaMatches(rule.media, viewportWidth_, viewportHeight_, systemDark_)) {
                return;
            }
            if (rule.container.active && !containerMatches(rule.container, element)) {
                return;
            }
            if (!selectorMatches(rule.selector, element, &state_)) {
                return;
            }
            for (const auto& decl : rule.declarations) {
                if (decl.important != important || !isCustomProperty(decl.property)) {
                    continue;
                }
                style.custom[decl.property] = decl.value;
            }
        };
        if (!important) {
            for (const auto& rule : rules_) {
                visit(rule);
            }
        } else {
            for (std::size_t index : importantOrder_) {
                visit(rules_[index]);
            }
        }
        for (const auto& decl : inlineDecls) {
            if (decl.important != important || !isCustomProperty(decl.property)) {
                continue;
            }
            style.custom[decl.property] = decl.value;
        }
    };

    // Applies one already-important-filtered declaration so the rules and inline
    // loops share the exact same custom-property and substitution handling.
    const auto applyOne = [&](const Declaration& decl) {
        if (isCustomProperty(decl.property)) {
            return;  // settled by collectCustom, above
        }
        if (isCssWideKeyword(decl.value)) {
            applyCssWideKeyword(style, &element, decl, parent);
            return;
        }
        if (containsVars(decl.value)) {
            std::string filled;
            if (resolveVars(decl.value, style.custom, filled, 0)) {
                Declaration applied = decl;
                applied.value = std::move(filled);
                applyDeclaration(style, applied, rootFontSize_);
            }
            // Unresolved: invalid at computed-value time, dropped. resolveVars
            // already logged which name (if any) caused it.
            return;
        }
        applyDeclaration(style, decl, rootFontSize_);
    };

    const auto applyLevel = [&](bool important) {
        const auto visit = [&](const Rule& rule) {
            if (rule.layerStatement) {
                return;
            }
            // Only a rule under an `@media` pays for evaluating one, which keeps
            // an ordinary stylesheet at the cost it had before.
            if (!rule.media.empty() &&
                !mediaMatches(rule.media, viewportWidth_, viewportHeight_, systemDark_)) {
                return;
            }
            if (rule.container.active && !containerMatches(rule.container, element)) {
                return;
            }
            if (!selectorMatches(rule.selector, element, &state_)) {
                return;
            }
            for (const auto& decl : rule.declarations) {
                if (decl.important == important) {
                    applyOne(decl);
                }
            }
        };
        if (!important) {
            for (const auto& rule : rules_) {
                visit(rule);
            }
        } else {
            for (std::size_t index : importantOrder_) {
                visit(rules_[index]);
            }
        }
        for (const auto& decl : inlineDecls) {
            if (decl.important == important) {
                applyOne(decl);
            }
        }
    };

    if (customInPlay) {
        collectCustom(false);
        collectCustom(true);
    }
    applyLevel(false);
    applyLevel(true);

    applyDimensionAttributes(style, element);
    substituteCurrentColor(style);
    // The document root still wears the user-agent canvas colours, which
    // `color-scheme` re-picks. Either pair an author set survives, and the UA
    // colors carry no `specified` bit of their own, so both are recognized by
    // still holding the dark pair they shipped with.
    if ((style.specified & ComputedStyle::SpecifiedUACanvas) &&
        style.colorScheme != ComputedStyle::ColorScheme::Normal) {
        const UaCanvas& was = uaCanvas(true);
        const UaCanvas& now =
            uaCanvas(style.colorScheme == ComputedStyle::ColorScheme::Dark);
        if (style.color.r == was.text.r && style.color.g == was.text.g &&
            style.color.b == was.text.b) {
            style.color = now.text;
        }
        if (style.backgroundColor.r == was.background.r &&
            style.backgroundColor.g == was.background.g &&
            style.backgroundColor.b == was.background.b) {
            style.backgroundColor = now.background;
        }
    }
    if (element.dialogModal()) {
        // The top layer is not overridable: a modal dialog fixes to the
        // viewport above every z-index, centered by the frame after layout.
        // Author position and z-index lose here the way they lose to the
        // browser's top layer.
        style.position = ComputedStyle::Fixed;
        style.zIndex = 1000000;
    }
    animateStyle(element, style);
    return style;
}

// --- Animation runtime ---

namespace {

// Two snapshots name-or-value equal? The transition diff compares author values,
// so a changed target restarts a run and an untouched one lets it finish.
bool sameAnimatable(const AnimatableValue& a, const AnimatableValue& b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
        case AnimatableValue::Kind::None:
            return true;
        case AnimatableValue::Kind::Length:
            return a.length.unit == b.length.unit && a.length.value == b.length.value;
        case AnimatableValue::Kind::Color:
            return a.color.r == b.color.r && a.color.g == b.color.g &&
                   a.color.b == b.color.b && a.color.a == b.color.a;
        case AnimatableValue::Kind::Number:
            return a.number == b.number;
    }
    return false;
}

bool easingEqual(const Easing& a, const Easing& b) {
    if (a.type != b.type) {
        return false;
    }
    if (a.type == Easing::Type::CubicBezier) {
        return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
    }
    if (a.type == Easing::Type::Steps) {
        return a.steps == b.steps && a.stepStart == b.stepStart;
    }
    return true;
}

// Applies every declaration of a keyframe onto the style, snapping. This is the
// fill phase (before or after playback), where there is no progress to blend.
void applyFrame(ComputedStyle& style, const KeyframeBlock::Keyframe& frame,
                float rootFontSize) {
    for (const Declaration& decl : frame.declarations) {
        if (isCustomProperty(decl.property)) {
            continue;  // custom properties have no animated form
        }
        applyDeclaration(style, decl, rootFontSize);
    }
}

// Interpolates every property the two bracketing keyframes share. A property
// declared in only one of them animates nothing and falls back to the element's
// own value, which is the CSS rule; the writer only touches shared properties.
void applyKeyframeSegment(ComputedStyle& style, const ElementAnim::KeyframeRun& run,
                          const std::vector<KeyframeBlock::Keyframe>& frames, float t,
                          float rootFontSize) {
    const std::size_t count = frames.size();
    if (count < 2) {
        return;
    }
    if (t < frames.front().offset || t >= frames.back().offset) {
        return;  // outside the declared span: the author value stands
    }
    std::size_t pair = 0;
    while (pair + 1 < count && frames[pair + 1].offset <= t) {
        ++pair;
    }
    const KeyframeBlock::Keyframe& a = frames[pair];
    const KeyframeBlock::Keyframe& b = frames[pair + 1];
    const float span = b.offset - a.offset;
    const float segT = span > 0.0f ? (t - a.offset) / span : 0.0f;
    const float eased = run.easing.apply(std::clamp(segT, 0.0f, 1.0f));

    std::set<std::string, std::less<>> properties;
    for (const Declaration& decl : a.declarations) {
        if (!isCustomProperty(decl.property)) {
            properties.insert(decl.property);
        }
    }
    for (const Declaration& decl : b.declarations) {
        if (!isCustomProperty(decl.property)) {
            properties.insert(decl.property);
        }
    }
    // The run's composite operation, pairing with its animation name: an
    // empty entry reads as the initial `replace`.
    const std::string& composite = run.composite;
    const bool additive = composite == "add" || composite == "accumulate";
    for (const std::string& property : properties) {
        // Apply the frame declarations over a copy of the current style and read
        // the property back, so keyframes parse through the same code as rules.
        ComputedStyle nearA = style;
        for (const Declaration& decl : a.declarations) {
            if (!isCustomProperty(decl.property)) {
                applyDeclaration(nearA, decl, rootFontSize);
            }
        }
        const AnimatableValue from = animatableValue(property, nearA);
        ComputedStyle nearB = style;
        for (const Declaration& decl : b.declarations) {
            if (!isCustomProperty(decl.property)) {
                applyDeclaration(nearB, decl, rootFontSize);
            }
        }
        const AnimatableValue to = animatableValue(property, nearB);
        if (additive && canBlend(from, to)) {
            // `add` builds the effect onto the underlying value; `accumulate`
            // builds the effect's delta from its start instead. Either falls
            // back to replace when the values are not additive together.
            const AnimatableValue effect = blendValues(from, to, eased);
            const AnimatableValue underlying = animatableValue(property, style);
            AnimatableValue combined;
            if (composite == "add") {
                combined = addValues(underlying, effect);
            } else {
                const AnimatableValue delta = subValues(effect, from);
                if (delta.kind != AnimatableValue::Kind::None) {
                    combined = addValues(underlying, delta);
                }
            }
            if (combined.kind != AnimatableValue::Kind::None) {
                writeValue(style, property, combined);
                continue;
            }
        }
        if (canBlend(from, to)) {
            blendInto(style, property, from, to, eased);
        } else if (to.kind != AnimatableValue::Kind::None) {
            blendInto(style, property, to, to, 1.0f);  // discrete: snap to the far frame
        }
    }
}

}  // namespace

void StyleResolver::animateStyle(Element& element, ComputedStyle& style) const {
    ElementAnim& anim = animations_[&element];
    const double now = currentTimeMs();
    // The raw author-computed values, captured before any blend: transitions diff
    // against this so an interpolated output can never restart its own run.
    const ComputedStyle raw = style;

    if (!anim.hasTarget) {
        anim.target = raw;
        anim.hasTarget = true;
        reconcileKeyframes(style, anim, now);
        applyKeyframes(style, anim, now);
        return;
    }

    const auto atDistance = [&](const ElementAnim::Transition& t, double time) {
        const double start = t.startMs + t.delayMs;
        const double span = time - start;
        const float p = t.durationMs > 0.0
                            ? static_cast<float>(std::clamp(span / t.durationMs, 0.0, 1.0))
                            : 1.0f;
        return t.easing.apply(p);
    };

    // Diff the author values of every transitionable property against the last
    // resolve's raw target and start or restart the matching run. An in-flight
    // run restarts from where the eye currently is; a fresh one from the old
    // target value, so a hover that flips a property twice never jumps.
    const std::size_t declared = style.transitionProperties.size();
    for (std::size_t i = 0; i < declared; ++i) {
        const float durationMs = listAt(style.transitionDurationsMs, i);
        const float delayMs = listAt(style.transitionDelaysMs, i);
        const Easing& easing = listAt(style.transitionEasings, i);
        for (const std::string& property : expandTransitionProperty(style.transitionProperties[i])) {
            const AnimatableValue to = animatableValue(property, style);
            if (to.kind == AnimatableValue::Kind::None) {
                continue;
            }
            const AnimatableValue previous = animatableValue(property, anim.target);
            auto run = std::find_if(anim.transitions.begin(), anim.transitions.end(),
                                    [&](const ElementAnim::Transition& t) {
                                        return t.property == property;
                                    });
            if (!sameAnimatable(previous, to)) {
                AnimatableValue from = previous;
                if (run != anim.transitions.end() &&
                    now < run->startMs + run->delayMs + run->durationMs &&
                    run->durationMs > 0.0) {
                    from = blendValues(run->from, run->to, atDistance(*run, now));
                }
                ElementAnim::Transition next;
                next.property = property;
                next.startMs = now;
                next.durationMs = durationMs;
                next.delayMs = delayMs;
                next.from = from;
                next.to = to;
                next.easing = easing;
                if (run == anim.transitions.end()) {
                    anim.transitions.push_back(std::move(next));
                } else {
                    *run = std::move(next);
                }
            }
            // Unchanged author value: an in-flight run keeps going to its target.
        }
    }

    // Overwrite the computed values with each running transition's blend, then
    // drop the ones that finished. A finished run blends to exactly the target
    // the style already holds, so erasing it changes nothing on screen.
    for (const ElementAnim::Transition& t : anim.transitions) {
        if (t.durationMs > 0.0 && now < t.startMs + t.delayMs + t.durationMs) {
            blendInto(style, t.property, t.from, t.to, atDistance(t, now));
        }
    }
    anim.transitions.erase(
        std::remove_if(anim.transitions.begin(), anim.transitions.end(),
                       [&](const ElementAnim::Transition& t) {
                           return !(t.durationMs > 0.0 &&
                                    now < t.startMs + t.delayMs + t.durationMs);
                       }),
        anim.transitions.end());

    // Keyframe animations render after transitions, so an animation on the same
    // property wins the frame, matching CSS.
    reconcileKeyframes(style, anim, now);
    applyKeyframes(style, anim, now);

    anim.target = raw;  // the raw style, not the interpolated one
}

void StyleResolver::reconcileKeyframes(ComputedStyle& style, ElementAnim& anim,
                                       double now) const {
    std::vector<ElementAnim::KeyframeRun> next;
    const std::size_t count = style.animationNames.size();
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& name = style.animationNames[i];
        if (name.empty()) {
            continue;
        }
        const float durationMs = listAt(style.animationDurationsMs, i);
        const float delayMs = listAt(style.animationDelaysMs, i);
        const float iterations = listAt(style.animationIterations, i);
        const std::string& direction = listAt(style.animationDirections, i);
        const std::string& fill = listAt(style.animationFills, i);
        const std::string& composite = listAt(style.animationCompositions, i);
        const bool paused = listAt(style.animationPaused, i);
        const Easing& easing = listAt(style.animationEasings, i);
        auto existing = std::find_if(
            anim.keyframeRuns.begin(), anim.keyframeRuns.end(),
            [&](const ElementAnim::KeyframeRun& r) { return r.name == name; });
        if (existing != anim.keyframeRuns.end() && existing->durationMs == durationMs &&
            existing->delayMs == delayMs && existing->iterations == iterations &&
            existing->direction == direction && existing->fill == fill &&
            existing->paused == paused && easingEqual(existing->easing, easing)) {
            // Composition is not timing: a changed operation applies to the
            // running timeline instead of restarting it.
            existing->composite = composite;
            next.push_back(std::move(*existing));
            continue;
        }
        ElementAnim::KeyframeRun run;
        run.name = name;
        run.composite = composite;
        run.startMs = now;  // a new configuration starts the timeline here
        run.durationMs = durationMs;
        run.delayMs = delayMs;
        run.iterations = iterations;
        run.direction = direction;
        run.fill = fill;
        run.paused = paused;
        run.easing = easing;
        next.push_back(std::move(run));
    }
    anim.keyframeRuns = std::move(next);
}

void StyleResolver::applyKeyframes(ComputedStyle& style, ElementAnim& anim,
                                   double now) const {
    for (const ElementAnim::KeyframeRun& run : anim.keyframeRuns) {
        if (run.paused) {
            continue;
        }
        const auto found = keyframes_.find(run.name);
        if (found == keyframes_.end() || found->second.empty()) {
            continue;
        }
        const std::vector<KeyframeBlock::Keyframe>& frames = found->second;
        const double start = run.startMs + run.delayMs;
        const double elapsed = now - start;
        if (elapsed < 0.0) {
            if (run.fill == "backwards" || run.fill == "both") {
                applyFrame(style, frames.front(), rootFontSize_);
            }
            continue;
        }
        const bool infinite = run.iterations < 0.0f;
        const double total =
            infinite ? 0.0 : static_cast<double>(run.durationMs) * run.iterations;
        if (!infinite && elapsed >= total) {
            if (run.fill == "forwards" || run.fill == "both") {
                applyFrame(style, frames.back(), rootFontSize_);
            }
            continue;
        }
        const double durationMs = run.durationMs;
        if (durationMs <= 0.0) {
            continue;  // instantaneous, already past end: fill decides the picture
        }
        const std::int64_t iteration =
            static_cast<std::int64_t>(std::floor(elapsed / durationMs));
        double t = std::fmod(elapsed, durationMs) / durationMs;
        if ((run.direction == "reverse") ||
            (run.direction == "alternate" && (iteration % 2) == 1) ||
            (run.direction == "alternate-reverse" && (iteration % 2) == 0)) {
            t = 1.0 - t;
        }
        applyKeyframeSegment(style, run, frames, static_cast<float>(t), rootFontSize_);
    }
}

double StyleResolver::currentTimeMs() const {
    return timeSource_ ? timeSource_() : sui::script::now();
}

bool StyleResolver::hasRunningAnimations() const {
    const double now = currentTimeMs();
    for (const auto& [element, anim] : animations_) {
        for (const auto& run : anim.transitions) {
            // The delay counts as running too: nothing paints new pixels then,
            // but the movement that follows needs the loop to keep ticking.
            if (now < run.startMs + run.delayMs + run.durationMs) {
                return true;
            }
        }
        for (const auto& run : anim.keyframeRuns) {
            if (run.paused || run.durationMs <= 0.0f) {
                continue;
            }
            if (run.iterations < 0.0f) {
                return true;
            }
            if (now < run.startMs + run.delayMs +
                          static_cast<double>(run.durationMs) * run.iterations) {
                return true;
            }
        }
    }
    return false;
}

void StyleResolver::setKeyframes(std::vector<KeyframeBlock> keyframes) {
    keyframes_.clear();
    addKeyframes(std::move(keyframes));
}

void StyleResolver::addKeyframes(std::vector<KeyframeBlock> keyframes) {
    for (auto& block : keyframes) {
        keyframes_[std::move(block.name)] = std::move(block.frames);
    }
}

void StyleResolver::clearStyles() {
    rules_.clear();
    importantOrder_.clear();
    layerOrder_.clear();
    keyframes_.clear();
    animations_.clear();
    anyContainerRules_ = false;
    hasFirstLineRules_ = false;
    hasFirstLetterRules_ = false;
    ++generation_;
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

bool StyleResolver::setSystemColorSchemeDark(bool dark) {
    if (dark == systemDark_) {
        return false;
    }
    systemDark_ = dark;
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

bool StyleResolver::setFocusVisible(bool visible) {
    SUI_SET_PSEUDO(focusVisible, visible);
}

SelectionColors StyleResolver::selectionColors(Element& element) const {
    return pseudoColors(element, "::selection");
}

SelectionColors StyleResolver::firstLineColors(Element& element) const {
    return pseudoColors(element, "::first-line");
}

SelectionColors StyleResolver::firstLetterColors(Element& element) const {
    return pseudoColors(element, "::first-letter");
}

SelectionColors StyleResolver::markerColors(Element& element) const {
    return pseudoColors(element, "::marker");
}

SelectionColors StyleResolver::pseudoColors(Element& element,
                                            std::string_view pseudo) const {
    SelectionColors out;
    // Best (important, specificity, order) per property so far.
    struct Bid {
        bool important = false;
        int specificity = -1;
        std::size_t order = 0;
        bool set = false;
    };
    Bid fgBid, bgBid;
    for (const auto& rule : rules_) {
        if (rule.layerStatement || rule.selector.parts.empty()) {
            continue;
        }
        const Selector::Kind last = rule.selector.parts.back().kind;
        if (last != Selector::Kind::PseudoElement ||
            rule.selector.parts.back().value != pseudo) {
            continue;
        }
        if (!rule.media.empty() &&
            !mediaMatches(rule.media, viewportWidth_, viewportHeight_, systemDark_)) {
            continue;
        }
        if (rule.container.active && !containerMatches(rule.container, element)) {
            continue;
        }
        // The prefix selects the highlight's owner; the pseudo-element part
        // only named the highlight itself. A bare `::selection` owns every
        // element's text.
        SimpleSelector prefix = rule.selector;
        prefix.parts.pop_back();
        if (!prefix.combinators.empty()) {
            prefix.combinators.pop_back();
        }
        const bool ownsAll =
            prefix.parts.empty() && prefix.pseudoClasses.empty();
        if (!ownsAll && !selectorMatches(prefix, element, &state_)) {
            continue;
        }
        const int specificity = specificityOf(prefix);
        for (const auto& decl : rule.declarations) {
            const bool isFg = decl.property == "color";
            const bool isBg = decl.property == "background-color" ||
                              decl.property == "background";
            if (!isFg && !isBg) {
                continue;
            }
            Bid& bid = isFg ? fgBid : bgBid;
            if (bid.set &&
                (decl.important != bid.important
                     ? bid.important > decl.important
                     : (specificity != bid.specificity
                            ? specificity < bid.specificity
                            : rule.order < bid.order))) {
                continue;
            }
            SDL_Color color{};
            if (isBg && decl.property == "background") {
                // A bare `background` shorthand carries the color first.
                std::string_view first = trim(decl.value);
                const std::size_t space = first.find(' ');
                if (space != std::string_view::npos) {
                    first = trim(first.substr(0, space));
                }
                if (!parseColor(first, color)) {
                    continue;
                }
            } else if (!parseColor(decl.value, color)) {
                continue;
            }
            bid = Bid{decl.important, specificity, rule.order, true};
            if (isFg) {
                out.fgSet = true;
                out.fg = color;
            } else {
                out.bgSet = true;
                out.bg = color;
            }
        }
    }
    return out;
}

#undef SUI_SET_PSEUDO

}  // namespace sui

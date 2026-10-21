#include "CSS/Parser.h"

#include "CSS/StyleResolver.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

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

bool isIdentChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '-' || c == '_' || u >= 0x80;
}

bool isSpace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// Splits a `@layer` statement prelude on commas: `@layer a, b.c;` declares
// two names in order. Anything non-empty survives; validation beyond that
// would only rename a map key.
std::vector<std::string> splitLayerNames(std::string_view prelude) {
    std::vector<std::string> names;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= prelude.size(); ++i) {
        if (i < prelude.size() && prelude[i] != ',') {
            continue;
        }
        const std::string_view name = trim(prelude.substr(start, i - start));
        start = i + 1;
        if (!name.empty()) {
            names.emplace_back(name);
        }
    }
    return names;
}

// A layer nested inside another is named by joining with a dot, so
// `@layer a { @layer b { } }` orders `a` before `a.b`.
std::string composeLayerName(const std::string& outer, std::string_view inner) {
    if (outer.empty()) {
        return std::string(inner);
    }
    std::string composed;
    composed.reserve(outer.size() + 1 + inner.size());
    composed += outer;
    composed += '.';
    composed += inner;
    return composed;
}

// Anonymous layers (`@layer { }`) are each their own layer, ordered by
// appearance. The counter is process-wide so names stay unique across
// stylesheets, the way separate <style> elements stay separate layers.
std::string anonymousLayerName() {
    static unsigned count = 0;
    return "@anonymous-" + std::to_string(++count);
}

// --- @supports conditions ---------------------------------------------------

// Index of the `)` matching the `(` at `open`, skipping quoted strings. Returns
// npos when the group is not closed.
std::size_t findClosingParen(std::string_view source, std::size_t open) {
    if (open >= source.size() || source[open] != '(') {
        return std::string_view::npos;
    }
    int depth = 0;
    char quote = 0;
    for (std::size_t i = open; i < source.size(); ++i) {
        const char c = source[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < source.size()) {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

void skipConditionSpace(std::string_view source, std::size_t& i) {
    while (i < source.size()) {
        if (isSpace(source[i])) {
            ++i;
        } else if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < source.size() &&
                   !(source[i] == '*' && source[i + 1] == '/')) {
                ++i;
            }
            i = std::min(i + 2, source.size());
        } else {
            break;
        }
    }
}

bool conditionWordAt(std::string_view source, std::size_t at, std::string_view word) {
    if (at > source.size() || source.size() - at < word.size() ||
        source.compare(at, word.size(), word) != 0) {
        return false;
    }
    if (at > 0 && isIdentChar(source[at - 1])) {
        return false;
    }
    const std::size_t after = at + word.size();
    return after >= source.size() || !isIdentChar(source[after]);
}

bool validSupportsProperty(std::string_view property) {
    if (property.empty()) {
        return false;
    }
    for (const char c : property) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) == 0 && c != '-' && c != '_' && u < 0x80) {
            return false;
        }
    }
    return true;
}

// Parses the condition grammar used by `@supports`: parenthesised declarations
// or nested conditions joined by `and`/`or`, with `not` in front, plus
// `selector(...)`. General-enclosed functions are syntactically valid but never
// supported. `and` binds tighter than `or`; parentheses remain the way to mix
// them unambiguously.
class SupportsConditionParser {
public:
    bool parse(std::string_view source) {
        source_ = source;
        pos_ = 0;
        const bool value = parseOr();
        skip();
        return ok_ && pos_ == source_.size() && value;
    }

private:
    bool parseOr() {
        bool value = parseAnd();
        while (ok_) {
            skip();
            if (!consumeWord("or")) {
                break;
            }
            const bool right = parseAnd();
            value = right || value;
        }
        return value;
    }

    bool parseAnd() {
        bool value = parseNot();
        while (ok_) {
            skip();
            if (!consumeWord("and")) {
                break;
            }
            const bool right = parseNot();
            value = right && value;
        }
        return value;
    }

    bool parseNot() {
        skip();
        if (consumeWord("not")) {
            return !parseInParens();
        }
        return parseInParens();
    }

    bool parseInParens() {
        skip();
        if (pos_ >= source_.size()) {
            ok_ = false;
            return false;
        }
        if (source_[pos_] == '(') {
            const std::size_t close = findClosingParen(source_, pos_);
            if (close == std::string_view::npos) {
                ok_ = false;
                return false;
            }
            const std::string_view inner =
                trim(source_.substr(pos_ + 1, close - pos_ - 1));
            pos_ = close + 1;
            return evaluateParens(inner);
        }
        if (conditionWordAt(source_, pos_, "selector")) {
            const std::size_t open = pos_ + 8;
            if (open >= source_.size() || source_[open] != '(') {
                ok_ = false;
                return false;
            }
            const std::size_t close = findClosingParen(source_, open);
            if (close == std::string_view::npos) {
                ok_ = false;
                return false;
            }
            const std::string_view inner =
                trim(source_.substr(open + 1, close - open - 1));
            pos_ = close + 1;
            if (inner.empty()) {
                return false;
            }
            // A bare pseudo-class has no parts of its own, so an empty part
            // list is valid as long as the selector parser understood it.
            const SimpleSelector selector = CSSParser::parseSelector(inner);
            return !selector.unsupported;
        }
        // General-enclosed: `name(...)` parses but is unknown, so it is false.
        if (const auto u = static_cast<unsigned char>(source_[pos_]);
            std::isalpha(u) != 0 || source_[pos_] == '_' || source_[pos_] == '-') {
            while (pos_ < source_.size() && isIdentChar(source_[pos_])) {
                ++pos_;
            }
            if (pos_ < source_.size() && source_[pos_] == '(') {
                const std::size_t close = findClosingParen(source_, pos_);
                if (close == std::string_view::npos) {
                    ok_ = false;
                    return false;
                }
                pos_ = close + 1;
                return false;
            }
        }
        ok_ = false;
        return false;
    }

    bool evaluateParens(std::string_view inner) {
        if (inner.empty()) {
            return false;
        }
        // A parenthesised condition, `not (...)`, or `selector(...)` is a
        // condition, not a declaration.
        if (inner.front() == '(' || conditionWordAt(inner, 0, "not") ||
            conditionWordAt(inner, 0, "selector")) {
            SupportsConditionParser nested;
            return nested.parse(inner);
        }
        const std::size_t colon = inner.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        const std::string_view property = trim(inner.substr(0, colon));
        const std::string_view value = trim(inner.substr(colon + 1));
        return validSupportsProperty(property) &&
               StyleResolver::supportsDeclaration(property, value);
    }

    void skip() { skipConditionSpace(source_, pos_); }

    bool consumeWord(std::string_view word) {
        skip();
        if (!conditionWordAt(source_, pos_, word)) {
            return false;
        }
        pos_ += word.size();
        return true;
    }

    std::string_view source_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

bool supportsConditionMatches(std::string_view source) {
    SupportsConditionParser parser;
    return parser.parse(source);
}

// `@import` wraps its condition in a `supports(...)` function, whose argument may
// be a bare declaration (`supports(display: grid)`) or a full condition.
bool supportsImportConditionMatches(std::string_view source) {
    source = trim(source);
    if (source.empty()) {
        return false;
    }
    if (source.front() == '(' || conditionWordAt(source, 0, "not") ||
        conditionWordAt(source, 0, "selector")) {
        return supportsConditionMatches(source);
    }
    const std::size_t colon = source.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const std::string_view property = trim(source.substr(0, colon));
    const std::string_view value = trim(source.substr(colon + 1));
    return validSupportsProperty(property) &&
           StyleResolver::supportsDeclaration(property, value);
}

// Threaded through nested blocks so `@import` resolves hrefs, dedupes sheets,
// and stays in order wherever it nests. Copies are cheap (one function plus
// one path); the seen set is shared.
struct ImportContext {
    CSSParser::FetchStylesheet fetch;
    std::string baseHref;
    std::set<std::string>* seen = nullptr;
};

// One `@import` prelude: the sheet, an optional media condition, and an
// optional layer to wrap the imported rules in.
struct ImportSpec {
    std::string href;
    std::vector<MediaQuery> media;
    bool hasMedia = false;
    std::string layerName;  // empty for a bare `layer`
    bool hasLayer = false;
    bool hasSupports = false;
};

// Resolves an imported href against the importing sheet: absolute paths and
// URLs pass through, anything else joins onto the importer's directory.
std::string joinHref(std::string_view base, std::string_view href) {
    if (href.find("://") != std::string_view::npos ||
        (!href.empty() && href.front() == '/')) {
        return std::string(href);
    }
    const std::size_t slash = base.find_last_of('/');
    std::string out;
    if (slash != std::string_view::npos) {
        out.append(base.substr(0, slash + 1));
    }
    out.append(href);
    return out;
}

// Reads `url(...)` or a quoted string into `href`. Returns false for anything
// else, including an empty reference.
bool readImportHref(std::string_view& rest, std::string& href) {
    std::string_view text = trim(rest);
    std::string_view body;
    if (text.size() > 5 && text.compare(0, 4, "url(") == 0) {
        const std::size_t close = text.find_last_of(')');
        if (close == std::string_view::npos || close <= 4) {
            return false;
        }
        body = trim(text.substr(4, close - 4));
    } else if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'')) {
        const char quote = text.front();
        const std::size_t close = text.find(quote, 1);
        if (close == std::string_view::npos) {
            return false;
        }
        body = text.substr(1, close - 1);
        rest = text.substr(close + 1);
        if (body.empty()) {
            return false;
        }
        href = std::string(body);
        return true;
    } else {
        return false;
    }
    if (body.size() >= 2 && (body.front() == '"' || body.front() == '\'') &&
        body.back() == body.front()) {
        body = body.substr(1, body.size() - 2);
    }
    if (body.empty()) {
        return false;
    }
    // The prelude continues after the closing paren.
    const std::size_t close = text.find_last_of(')');
    rest = text.substr(close + 1);
    href = std::string(body);
    return true;
}

// Parses `@import <url> [layer | layer(name)] [supports(condition)] [media]`.
// The layer and supports conditions come before the media query, as the grammar
// requires. A false supports condition drops the import; unparsable media drops
// it too, the way `@media` itself drops.
bool parseImportPrelude(std::string_view prelude, ImportSpec& spec) {
    std::string_view rest = trim(prelude);
    if (!readImportHref(rest, spec.href)) {
        return false;
    }
    rest = trim(rest);

    while (!rest.empty()) {
        if (conditionWordAt(rest, 0, "layer")) {
            if (spec.hasLayer) {
                return false;
            }
            if (rest.size() == 5 || isSpace(rest[5])) {
                spec.hasLayer = true;
                rest = trim(rest.substr(5));
                continue;
            }
            if (rest[5] == '(') {
                const std::size_t close = findClosingParen(rest, 5);
                if (close == std::string_view::npos) {
                    return false;
                }
                const std::string_view name = trim(rest.substr(6, close - 6));
                if (name.empty() || name.find(',') != std::string_view::npos) {
                    return false;
                }
                spec.layerName = std::string(name);
                spec.hasLayer = true;
                rest = trim(rest.substr(close + 1));
                continue;
            }
            return false;
        }
        if (conditionWordAt(rest, 0, "supports") && rest.size() > 8 &&
            rest[8] == '(') {
            if (spec.hasSupports) {
                return false;
            }
            const std::size_t close = findClosingParen(rest, 8);
            if (close == std::string_view::npos) {
                return false;
            }
            const std::string_view condition = trim(rest.substr(9, close - 9));
            if (!supportsImportConditionMatches(condition)) {
                return false;
            }
            spec.hasSupports = true;
            rest = trim(rest.substr(close + 1));
            continue;
        }
        break;
    }

    if (!rest.empty()) {
        spec.media = parseMediaList(rest);
        if (spec.media.empty()) {
            return false;
        }
        spec.hasMedia = true;
    }
    return true;
}

// True when `source` starts with the `style(` function at `at`.
bool styleFunctionAt(std::string_view source, std::size_t at) {
    return conditionWordAt(source, at, "style") && at + 5 < source.size() &&
           source[at + 5] == '(';
}

// A small recursive-descent parser for one boolean `@container` condition.
// Size features keep their comparison, unit, and axis; `style()` features keep
// the property and value for the resolver to compare against a container.
// `and` binds tighter than `or`; `not` wraps the next parenthesised group.
class ContainerConditionParser {
public:
    using Query = Rule::ContainerQuery;
    using Node = Query::Node;

    ContainerConditionParser(std::string_view source, bool styleContext)
        : source_(source), style_(styleContext) {}

    bool parse(Node& out) {
        skip();
        if (pos_ >= source_.size()) {
            return false;
        }
        out = parseOr();
        skip();
        return ok_ && pos_ == source_.size();
    }

private:
    Node parseOr() {
        Node left = parseAnd();
        while (ok_) {
            skip();
            if (!consumeWord("or")) {
                break;
            }
            Node right = parseAnd();
            if (!ok_) {
                break;
            }
            left = combine(Node::Kind::Or, std::move(left), std::move(right));
        }
        return left;
    }

    Node parseAnd() {
        Node left = parseNot();
        while (ok_) {
            skip();
            if (!consumeWord("and")) {
                break;
            }
            Node right = parseNot();
            if (!ok_) {
                break;
            }
            left = combine(Node::Kind::And, std::move(left), std::move(right));
        }
        return left;
    }

    Node parseNot() {
        skip();
        if (conditionWordAt(source_, pos_, "not")) {
            pos_ += 3;
            Node node;
            node.kind = Node::Kind::Not;
            node.children.push_back(parsePrimary());
            return node;
        }
        return parsePrimary();
    }

    Node parsePrimary() {
        skip();
        if (pos_ >= source_.size()) {
            ok_ = false;
            return {};
        }
        if (source_[pos_] == '(') {
            const std::size_t close = findClosingParen(source_, pos_);
            if (close == std::string_view::npos) {
                ok_ = false;
                return {};
            }
            const std::string_view inner =
                trim(source_.substr(pos_ + 1, close - pos_ - 1));
            pos_ = close + 1;
            if (startsCondition(inner)) {
                return nested(inner, style_);
            }
            return feature(inner);
        }
        if (styleFunctionAt(source_, pos_)) {
            const std::size_t open = pos_ + 5;
            const std::size_t close = findClosingParen(source_, open);
            if (close == std::string_view::npos) {
                ok_ = false;
                return {};
            }
            const std::string_view inner =
                trim(source_.substr(open + 1, close - open - 1));
            pos_ = close + 1;
            if (startsCondition(inner)) {
                return nested(inner, true);
            }
            return styleFeature(inner);
        }
        ok_ = false;
        return {};
    }

    static bool startsCondition(std::string_view source) {
        source = trim(source);
        if (source.empty()) {
            return false;
        }
        return source.front() == '(' || conditionWordAt(source, 0, "not") ||
               styleFunctionAt(source, 0);
    }

    Node nested(std::string_view source, bool styleContext) {
        Node node;
        ContainerConditionParser parser(source, styleContext);
        if (!parser.parse(node)) {
            ok_ = false;
            return {};
        }
        return node;
    }

    Node feature(std::string_view source) {
        if (style_) {
            return styleFeature(source);
        }
        Node node;
        if (!parseSizeExpression(source, node)) {
            ok_ = false;
            return {};
        }
        return node;
    }

    Node styleFeature(std::string_view source) {
        Query::Feature parsed;
        if (!parseStyleFeature(source, parsed)) {
            ok_ = false;
            return {};
        }
        Node node;
        node.feature = std::move(parsed);
        return node;
    }

    static Node combine(Node::Kind kind, Node left, Node right) {
        if (left.kind == kind) {
            left.children.push_back(std::move(right));
            return left;
        }
        Node node;
        node.kind = kind;
        node.children.push_back(std::move(left));
        node.children.push_back(std::move(right));
        return node;
    }

    static Query::Comparison reverse(Query::Comparison comparison) {
        switch (comparison) {
            case Query::Comparison::Less: return Query::Comparison::Greater;
            case Query::Comparison::LessEqual: return Query::Comparison::GreaterEqual;
            case Query::Comparison::Greater: return Query::Comparison::Less;
            case Query::Comparison::GreaterEqual: return Query::Comparison::LessEqual;
            case Query::Comparison::Equal: return Query::Comparison::Equal;
        }
        return Query::Comparison::Equal;
    }

    static bool lessComparison(Query::Comparison comparison) {
        return comparison == Query::Comparison::Less ||
               comparison == Query::Comparison::LessEqual;
    }

    static bool greaterComparison(Query::Comparison comparison) {
        return comparison == Query::Comparison::Greater ||
               comparison == Query::Comparison::GreaterEqual;
    }

    // Finds the first top-level `:`, ignoring quoted strings and nested groups.
    static std::size_t findTopLevelColon(std::string_view source) {
        int depth = 0;
        char quote = 0;
        for (std::size_t i = 0; i < source.size(); ++i) {
            const char c = source[i];
            if (quote != 0) {
                if (c == '\\' && i + 1 < source.size()) {
                    ++i;
                } else if (c == quote) {
                    quote = 0;
                }
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
            } else if (c == ':' && depth == 0) {
                return i;
            }
        }
        return std::string_view::npos;
    }

    // Finds the first top-level comparison operator, returning its index and
    // length (`1` for `<`, `>`, `=` and `2` for `<=`, `>=`).
    static std::size_t findTopLevelComparison(std::string_view source,
                                              std::size_t from,
                                              std::size_t& length) {
        int depth = 0;
        char quote = 0;
        for (std::size_t i = from; i < source.size(); ++i) {
            const char c = source[i];
            if (quote != 0) {
                if (c == '\\' && i + 1 < source.size()) {
                    ++i;
                } else if (c == quote) {
                    quote = 0;
                }
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
            } else if (depth == 0 && (c == '<' || c == '>' || c == '=')) {
                length = (i + 1 < source.size() && source[i + 1] == '=') ? 2 : 1;
                return i;
            }
        }
        return std::string_view::npos;
    }

    static bool comparisonAt(std::string_view source, std::size_t at,
                             std::size_t length, Query::Comparison& out) {
        if (at + length > source.size()) {
            return false;
        }
        const char c = source[at];
        const bool equal = length == 2 && source[at + 1] == '=';
        if (c == '<') {
            out = equal ? Query::Comparison::LessEqual : Query::Comparison::Less;
            return true;
        }
        if (c == '>') {
            out = equal ? Query::Comparison::GreaterEqual
                        : Query::Comparison::Greater;
            return true;
        }
        if (c == '=' && length == 1) {
            out = Query::Comparison::Equal;
            return true;
        }
        return false;
    }

    static bool parseBaseSizeProperty(std::string_view source, bool& height) {
        if (source == "width" || source == "inline-size") {
            height = false;
            return true;
        }
        if (source == "height" || source == "block-size") {
            height = true;
            return true;
        }
        return false;
    }

    static bool parseColonSizeProperty(std::string_view source, bool& height,
                                       Query::Comparison& comparison) {
        if (source == "min-width") {
            height = false;
            comparison = Query::Comparison::GreaterEqual;
            return true;
        }
        if (source == "max-width") {
            height = false;
            comparison = Query::Comparison::LessEqual;
            return true;
        }
        if (source == "min-height") {
            height = true;
            comparison = Query::Comparison::GreaterEqual;
            return true;
        }
        if (source == "max-height") {
            height = true;
            comparison = Query::Comparison::LessEqual;
            return true;
        }
        if (parseBaseSizeProperty(source, height)) {
            comparison = Query::Comparison::Equal;
            return true;
        }
        return false;
    }

    static bool parseLength(std::string_view source, Query::Unit& unit,
                            float& amount) {
        source = trim(source);
        if (source.empty()) {
            return false;
        }
        const std::string storage(source);
        char* end = nullptr;
        const float value = std::strtof(storage.c_str(), &end);
        if (end == storage.c_str() || !std::isfinite(value) || value < 0.0f) {
            return false;
        }
        std::string suffix(end);
        for (char& c : suffix) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const std::string_view unitName = trim(suffix);
        if (unitName.empty()) {
            if (value != 0.0f) {
                return false;
            }
            unit = Query::Unit::Px;
            amount = 0.0f;
            return true;
        }
        if (unitName == "px") unit = Query::Unit::Px;
        else if (unitName == "em") unit = Query::Unit::Em;
        else if (unitName == "rem") unit = Query::Unit::Rem;
        else if (unitName == "vw") unit = Query::Unit::Vw;
        else if (unitName == "vh") unit = Query::Unit::Vh;
        else if (unitName == "cqw") unit = Query::Unit::Cqw;
        else if (unitName == "cqh") unit = Query::Unit::Cqh;
        else if (unitName == "cqi") unit = Query::Unit::Cqi;
        else if (unitName == "cqb") unit = Query::Unit::Cqb;
        else if (unitName == "cqmin") unit = Query::Unit::Cqmin;
        else if (unitName == "cqmax") unit = Query::Unit::Cqmax;
        else return false;
        amount = value;
        return true;
    }

    static Node sizeFeature(bool height, Query::Comparison comparison,
                            Query::Unit unit, float amount) {
        Node node;
        node.feature.height = height;
        node.feature.comparison = comparison;
        node.feature.unit = unit;
        node.feature.amount = amount;
        return node;
    }

    bool parseSizeExpression(std::string_view source, Node& out) {
        source = trim(source);
        if (source.empty()) {
            return false;
        }
        const std::size_t colon = findTopLevelColon(source);
        if (colon != std::string_view::npos) {
            std::size_t ignored = 0;
            if (findTopLevelComparison(source, 0, ignored) !=
                std::string_view::npos) {
                return false;
            }
            const std::string_view property = trim(source.substr(0, colon));
            const std::string_view value = trim(source.substr(colon + 1));
            bool height = false;
            Query::Comparison comparison = Query::Comparison::Equal;
            Query::Unit unit = Query::Unit::Px;
            float amount = 0.0f;
            if (!parseColonSizeProperty(property, height, comparison) ||
                !parseLength(value, unit, amount)) {
                return false;
            }
            out = sizeFeature(height, comparison, unit, amount);
            return true;
        }

        std::size_t firstLength = 0;
        const std::size_t first = findTopLevelComparison(source, 0, firstLength);
        if (first == std::string_view::npos) {
            return false;
        }
        const std::string_view left = trim(source.substr(0, first));
        const std::string_view right = trim(source.substr(first + firstLength));
        if (left.empty() || right.empty()) {
            return false;
        }
        std::size_t secondLength = 0;
        const std::size_t second =
            findTopLevelComparison(right, 0, secondLength);
        if (second != std::string_view::npos) {
            const std::string_view middle = trim(right.substr(0, second));
            const std::string_view last = trim(right.substr(second + secondLength));
            bool height = false;
            if (!parseBaseSizeProperty(middle, height) || last.empty()) {
                return false;
            }
            Query::Comparison firstComparison = Query::Comparison::Equal;
            Query::Comparison secondComparison = Query::Comparison::Equal;
            if (!comparisonAt(source, first, firstLength, firstComparison) ||
                !comparisonAt(right, second, secondLength, secondComparison) ||
                firstComparison == Query::Comparison::Equal ||
                secondComparison == Query::Comparison::Equal ||
                (lessComparison(firstComparison) !=
                 lessComparison(secondComparison)) ||
                (greaterComparison(firstComparison) !=
                 greaterComparison(secondComparison))) {
                return false;
            }
            Query::Unit leftUnit = Query::Unit::Px;
            Query::Unit rightUnit = Query::Unit::Px;
            float leftAmount = 0.0f;
            float rightAmount = 0.0f;
            if (!parseLength(left, leftUnit, leftAmount) ||
                !parseLength(last, rightUnit, rightAmount)) {
                return false;
            }
            Node low = sizeFeature(height, reverse(firstComparison), leftUnit,
                                   leftAmount);
            Node high = sizeFeature(height, secondComparison, rightUnit,
                                    rightAmount);
            out.kind = Node::Kind::And;
            out.children.push_back(std::move(low));
            out.children.push_back(std::move(high));
            return true;
        }

        bool height = false;
        Query::Comparison comparison = Query::Comparison::Equal;
        Query::Unit unit = Query::Unit::Px;
        float amount = 0.0f;
        if (parseBaseSizeProperty(left, height)) {
            if (!comparisonAt(source, first, firstLength, comparison) ||
                !parseLength(right, unit, amount)) {
                return false;
            }
            out = sizeFeature(height, comparison, unit, amount);
            return true;
        }
        if (parseBaseSizeProperty(right, height)) {
            if (!comparisonAt(source, first, firstLength, comparison) ||
                !parseLength(left, unit, amount)) {
                return false;
            }
            out = sizeFeature(height, reverse(comparison), unit, amount);
            return true;
        }
        return false;
    }

    static bool validStyleProperty(std::string_view property) {
        if (property.empty()) {
            return false;
        }
        const char first = property.front();
        if (!(std::isalpha(static_cast<unsigned char>(first)) != 0 ||
              first == '_' || first == '-')) {
            return false;
        }
        for (const char c : property) {
            if (!isIdentChar(c)) {
                return false;
            }
        }
        return true;
    }

    static bool parseStyleFeature(std::string_view source,
                                  Query::Feature& feature) {
        source = trim(source);
        if (source.empty()) {
            return false;
        }
        feature = {};
        feature.style = true;
        const std::size_t colon = findTopLevelColon(source);
        if (colon == std::string_view::npos) {
            feature.property = std::string(trim(source));
            return validStyleProperty(feature.property);
        }
        feature.property = std::string(trim(source.substr(0, colon)));
        feature.value = std::string(trim(source.substr(colon + 1)));
        return validStyleProperty(feature.property) && !feature.value.empty();
    }

    void skip() { skipConditionSpace(source_, pos_); }

    bool consumeWord(std::string_view word) {
        skip();
        if (!conditionWordAt(source_, pos_, word)) {
            return false;
        }
        pos_ += word.size();
        return true;
    }

    std::string_view source_;
    std::size_t pos_ = 0;
    bool style_ = false;
    bool ok_ = true;
};

// Parses an `@container` prelude: an optional name list followed by an
// optional boolean condition. Size features accept width/height, inline-size/
// block-size, min/max aliases, range comparisons, and px/em/rem/viewport/
// container units. `style()` features compare computed declarations, and
// `and`, `or`, `not`, and nested parentheses compose features. Anything the
// grammar cannot express drops the block, the way an unparsable `@media` does.
bool parseContainerPrelude(std::string_view prelude, Rule::ContainerQuery& query) {
    query = {};
    std::size_t i = 0;
    skipConditionSpace(prelude, i);
    while (i < prelude.size()) {
        if (prelude[i] == '(' || styleFunctionAt(prelude, i) ||
            conditionWordAt(prelude, i, "not")) {
            break;
        }
        if (conditionWordAt(prelude, i, "and") ||
            conditionWordAt(prelude, i, "or")) {
            return false;
        }
        const std::size_t start = i;
        while (i < prelude.size() &&
               (isIdentChar(prelude[i]) || prelude[i] == '.')) {
            ++i;
        }
        const std::string_view name = trim(prelude.substr(start, i - start));
        if (name.empty()) {
            return false;
        }
        query.names.emplace_back(name);
        skipConditionSpace(prelude, i);
    }
    if (i < prelude.size()) {
        ContainerConditionParser parser(prelude.substr(i), false);
        if (!parser.parse(query.condition)) {
            return false;
        }
        query.hasCondition = true;
    } else if (query.names.empty()) {
        return false;
    }
    query.active = true;
    return true;
}

// Advances past a `/* ... */` comment starting at `i`, or returns `i` unchanged
// when there is no comment there.
std::size_t skipComment(std::string_view source, std::size_t i) {
    if (i + 1 >= source.size() || source[i] != '/' || source[i + 1] != '*') {
        return i;
    }
    i += 2;
    while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) {
        ++i;
    }
    return std::min(i + 2, source.size());
}

// Removes comments so a declaration block can be scanned as plain text. Comments
// are legal anywhere in a block, and without this the text before one becomes
// part of the property name.
std::string stripComments(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size();) {
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            i = skipComment(source, i);
            // A comment separates tokens, so it becomes a space rather than
            // nothing: `red/*x*/blue` must not collapse into `redblue`.
            out += ' ';
            continue;
        }
        out += source[i];
        ++i;
    }
    return out;
}

// Reads a single compound selector (no combinators) starting at `i`. Returns
// the number of characters consumed, or 0 if there is no selector there.
std::size_t readCompound(std::string_view src, std::size_t i, SimpleSelector& out) {
    const std::size_t start = i;
    while (i < src.size()) {
        const char c = src[i];
        if (c == '.' || c == '#') {
            const Selector::Kind kind =
                c == '.' ? Selector::Kind::Class : Selector::Kind::Id;
            ++i;
            const std::size_t nameStart = i;
            while (i < src.size() && isIdentChar(src[i])) {
                ++i;
            }
            out.parts.push_back({kind, std::string(src.substr(nameStart, i - nameStart)),
                                 '\0', {}});
            continue;
        }
        if (c == ':' && i + 1 < src.size() && src[i + 1] != ':') {
            ++i;
            const std::size_t nameStart = i;
            while (i < src.size() && isIdentChar(src[i])) {
                ++i;
            }
            const std::string_view name = src.substr(nameStart, i - nameStart);
            // The interactive states plus the structural pseudo-classes the engine
            // can evaluate. Anything else leaves the selector matching nothing rather
            // than silently matching every element the rest of it describes.
            if (name == "hover") {
                out.pseudoClasses.push_back(PseudoClass::Hover);
            } else if (name == "active") {
                out.pseudoClasses.push_back(PseudoClass::Active);
            } else if (name == "focus") {
                out.pseudoClasses.push_back(PseudoClass::Focus);
            } else if (name == "focus-within") {
                out.pseudoClasses.push_back(PseudoClass::FocusWithin);
            } else if (name == "focus-visible") {
                out.pseudoClasses.push_back(PseudoClass::FocusVisible);
            } else if (name == "checked") {
                out.pseudoClasses.push_back(PseudoClass::Checked);
            } else if (name == "disabled") {
                out.pseudoClasses.push_back(PseudoClass::Disabled);
            } else if (name == "enabled") {
                out.pseudoClasses.push_back(PseudoClass::Enabled);
            } else if (name == "required") {
                out.pseudoClasses.push_back(PseudoClass::Required);
            } else if (name == "optional") {
                out.pseudoClasses.push_back(PseudoClass::Optional);
            } else if (name == "valid") {
                out.pseudoClasses.push_back(PseudoClass::Valid);
            } else if (name == "invalid") {
                out.pseudoClasses.push_back(PseudoClass::Invalid);
            } else if (name == "first-child") {
                out.pseudoClasses.push_back(PseudoClass::FirstChild);
            } else if (name == "last-child") {
                out.pseudoClasses.push_back(PseudoClass::LastChild);
            } else if (name == "only-child") {
                out.pseudoClasses.push_back(PseudoClass::OnlyChild);
            } else if (name == "only-of-type") {
                out.pseudoClasses.push_back(PseudoClass::OnlyOfType);
            } else if (name == "root") {
                out.pseudoClasses.push_back(PseudoClass::Root);
            } else if (name == "nth-child") {
                out.pseudoClasses.push_back(PseudoClass::NthChild);
                // skip args
            } else if (name == "nth-of-type") {
                out.pseudoClasses.push_back(PseudoClass::NthOfType);
                // skip args
            } else if (name == "nth-last-child") {
                out.pseudoClasses.push_back(PseudoClass::NthLastChild);
                // skip args
            } else if (name == "nth-last-of-type") {
                out.pseudoClasses.push_back(PseudoClass::NthLastOfType);
                // skip args
            } else if (name == "not") {
                out.pseudoClasses.push_back(PseudoClass::Not);
            } else if (name == "is") {
                out.pseudoClasses.push_back(PseudoClass::Is);
            } else if (name == "has") {
                out.pseudoClasses.push_back(PseudoClass::Has);
            } else {
                out.unsupported = true;
            }
            if (i < src.size() && src[i] == '(') {
                int depth = 1;
                ++i;
                const std::size_t argStart = i;
                while (i < src.size() && depth > 0) {
                    if (src[i] == '(') ++depth;
                    if (src[i] == ')') --depth;
                    ++i;
                }
                const std::size_t argEnd = depth == 0 ? i - 1 : i;
                if (argEnd > argStart) {
                    out.pseudoArgument =
                        std::string(src.substr(argStart, argEnd - argStart));
                }
            }
            continue;
        }
        if (c == ':' && i + 1 < src.size() && src[i + 1] == ':') {
            i += 2;
            const std::size_t nameStart = i;
            while (i < src.size() && isIdentChar(src[i])) {
                ++i;
            }
            const std::string_view name = src.substr(nameStart, i - nameStart);
            // `::before` and `::after` become real boxes in the tree, so they can
            // select those boxes and carry their own declarations. `::placeholder`
            // works the same way: its box carries the `placeholder` text.
            // `::selection`, `::first-line`, `::first-letter`, and `::marker`
            // never become boxes; the resolver matches them separately for
            // their colors. No other pseudo-element is generated.
            if (name == "before" || name == "after" || name == "placeholder" ||
                name == "selection" || name == "first-line" || name == "first-letter" ||
                name == "marker") {
                Selector part;
                part.kind = Selector::Kind::PseudoElement;
                part.value = std::string("::") + std::string(name);
                out.parts.push_back(std::move(part));
            } else if (name == "first-child") {
                out.pseudoClasses.push_back(PseudoClass::FirstChild);
            } else if (name == "last-child") {
                out.pseudoClasses.push_back(PseudoClass::LastChild);
            } else if (name == "only-child") {
                out.pseudoClasses.push_back(PseudoClass::OnlyChild);
            } else if (name == "only-of-type") {
                out.pseudoClasses.push_back(PseudoClass::OnlyOfType);
            } else if (name == "root") {
                out.pseudoClasses.push_back(PseudoClass::Root);
            } else if (name == "nth-child") {
                out.pseudoClasses.push_back(PseudoClass::NthChild);
                // skip args
            } else if (name == "nth-of-type") {
                out.pseudoClasses.push_back(PseudoClass::NthOfType);
                // skip args
            } else if (name == "nth-last-child") {
                out.pseudoClasses.push_back(PseudoClass::NthLastChild);
                // skip args
            } else if (name == "nth-last-of-type") {
                out.pseudoClasses.push_back(PseudoClass::NthLastOfType);
                // skip args
            } else if (name == "focus-within") {
                out.pseudoClasses.push_back(PseudoClass::FocusWithin);
            } else if (name == "focus-visible") {
                out.pseudoClasses.push_back(PseudoClass::FocusVisible);
            } else if (name == "disabled") {
                out.pseudoClasses.push_back(PseudoClass::Disabled);
            } else if (name == "enabled") {
                out.pseudoClasses.push_back(PseudoClass::Enabled);
            } else if (name == "required") {
                out.pseudoClasses.push_back(PseudoClass::Required);
            } else if (name == "optional") {
                out.pseudoClasses.push_back(PseudoClass::Optional);
            } else if (name == "valid") {
                out.pseudoClasses.push_back(PseudoClass::Valid);
            } else if (name == "invalid") {
                out.pseudoClasses.push_back(PseudoClass::Invalid);
            } else if (name == "not") {
                out.pseudoClasses.push_back(PseudoClass::Not);
            } else if (name == "is") {
                out.pseudoClasses.push_back(PseudoClass::Is);
            } else if (name == "has") {
                out.pseudoClasses.push_back(PseudoClass::Has);
            } else {
                out.unsupported = true;
            }
            continue;
        }
        if (isIdentChar(c)) {
            const std::size_t nameStart = i;
            while (i < src.size() && isIdentChar(src[i])) {
                ++i;
            }
            out.parts.push_back({Selector::Kind::Type,
                                 std::string(src.substr(nameStart, i - nameStart)), '\0', {}});
            continue;
        }
        if (c == '*') {
            ++i;
            out.parts.push_back({Selector::Kind::Universal, std::string(), '\0', {}});
            continue;
        }
        if (c == '[') {
            ++i;
            const std::size_t nameStart = i;
            while (i < src.size() && isIdentChar(src[i])) {
                ++i;
            }
            const std::string name(src.substr(nameStart, i - nameStart));
            std::string value;
            // `[attr]` is the presence test and stops here. Anything else compares,
            // and the operator is read rather than skipped: skipping it is what made
            // `[href^="http"]` an exact match on the three letters `http`, which the
            // engine reported as understood because nothing looked malformed.
            char op = '\0';
            while (i < src.size() && isSpace(src[i])) {
                ++i;
            }
            if (i < src.size() && src[i] != ']') {
                if (src[i] == '~' || src[i] == '|' || src[i] == '^' || src[i] == '$' ||
                    src[i] == '*') {
                    op = src[i++];
                    while (i < src.size() && isSpace(src[i])) {
                        ++i;
                    }
                }
                if (i < src.size() && src[i] == '=') {
                    ++i;
                    if (op == '\0') {
                        op = '=';
                    }
                } else {
                    // `[attr ~ v]` and `[attr v]` are both malformed. Matching the
                    // wrong way round would be worse than matching nothing.
                    out.unsupported = true;
                }
            }
            if (i < src.size() && (src[i] == '"' || src[i] == '\'')) {
                const char quote = src[i++];
                const std::size_t vStart = i;
                while (i < src.size() && src[i] != quote) {
                    ++i;
                }
                value.assign(src.substr(vStart, i - vStart));
                if (i < src.size()) {
                    ++i;  // the closing quote
                }
                while (i < src.size() && src[i] != ']') {
                    ++i;
                }
            } else {
                const std::size_t vStart = i;
                while (i < src.size() && src[i] != ']' && !isSpace(src[i])) {
                    ++i;
                }
                value.assign(src.substr(vStart, i - vStart));
            }
            if (i < src.size()) {
                ++i;  // the closing bracket
            }
            Selector part;
            part.kind = Selector::Kind::Attribute;
            part.value = name;
            part.attributeOp = op;
            part.attributeValue = value;
            out.parts.push_back(std::move(part));
            continue;
        }
        break;
    }
    return i - start;
}

// Parses one selector in a comma-separated list. combinators[i] is the link
// between parts[i] and parts[i + 1].
SimpleSelector readSelector(std::string_view src) {
    SimpleSelector sel;
    std::size_t i = 0;
    bool sawSpace = false;
    Combinator pending = Combinator::None;

    while (i < src.size()) {
        while (i < src.size() && isSpace(src[i])) {
            ++i;
            sawSpace = true;
        }
        if (i >= src.size()) {
            break;
        }
        if (src[i] == '>') {
            ++i;
            pending = Combinator::Child;
            sawSpace = false;
            continue;
        }
        const std::size_t before = sel.parts.size();
        const std::size_t consumed = readCompound(src, i, sel);
        if (consumed == 0) {
            ++i;  // unsupported token
            sawSpace = false;
            continue;
        }
        i += consumed;
        if (before > 0) {
            sel.combinators.push_back(sawSpace ? pending : Combinator::Descendant);
        }
        // Extra parts inside one compound have no combinator between them.
        while (sel.combinators.size() + 1 < sel.parts.size()) {
            sel.combinators.push_back(Combinator::None);
        }
        pending = Combinator::Descendant;
        sawSpace = false;
    }

    sel.combinators.resize(sel.parts.size() > 0 ? sel.parts.size() - 1 : 0);
    return sel;
}

}  // namespace

SimpleSelector CSSParser::parseSelector(std::string_view source) {
    return readSelector(source);
}

std::vector<Declaration> parseDeclarations(std::string_view rawSource) {
    // `stripped` owns the text and `source` only views it: substr on a std::string
    // returns a temporary, so a string_view taken from one would dangle.
    const std::string stripped = stripComments(rawSource);
    const std::string_view source(stripped);
    std::vector<Declaration> out;
    std::size_t i = 0;
    while (i < source.size()) {
        while (i < source.size() && (isSpace(source[i]) || source[i] == ';')) {
            ++i;
        }
        const std::size_t propStart = i;
        while (i < source.size() && source[i] != ':' && source[i] != ';') {
            ++i;
        }
        if (i >= source.size() || source[i] != ':') {
            break;  // no colon: skip to the next declaration
        }
        const std::string property(trim(source.substr(propStart, i - propStart)));
        ++i;  // ':'
        const std::size_t valueStart = i;
        int depth = 0;
        while (i < source.size()) {
            const char c = source[i];
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
            } else if (c == ';' && depth == 0) {
                break;
            }
            ++i;
        }
        std::string_view value = trim(source.substr(valueStart, i - valueStart));
        if (i < source.size() && source[i] == ';') {
            ++i;
        }
        if (property.empty() || value.empty()) {
            continue;
        }
        Declaration decl;
        decl.property = property;
        constexpr std::string_view kImportant = " !important";
        if (value.ends_with(kImportant)) {
            decl.value = std::string(trim(value.substr(0, value.size() - kImportant.size())));
            decl.important = true;
        } else {
            decl.value = std::string(value);
        }
        out.push_back(std::move(decl));
    }
    return out;
}

namespace {

// ---------------------------------------------------------------------------
// Media queries
// ---------------------------------------------------------------------------

// An unsupported feature makes its query match nothing, which is what CSS says
// of a feature the device does not have. Saying nothing on top of that would
// leave a stylesheet wondering why its breakpoint never fires, so each name is
// reported once for the life of the process.
void warnUnsupportedMedia(std::string_view what) {
    static std::set<std::string, std::less<>> reported;
    if (reported.emplace(what).second) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "ignoring unsupported media query '%.*s'",
                    static_cast<int>(what.size()), what.data());
    }
}

bool startsWithWord(std::string_view s, std::string_view word) {
    return s.size() >= word.size() && s.compare(0, word.size(), word) == 0;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Splits on commas that are not inside parentheses, for the alternative list of
// an `@media` prelude.
std::vector<std::string_view> splitAlternatives(std::string_view s) {
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t j = 0; j < s.size(); ++j) {
        if (s[j] == '(') {
            ++depth;
        } else if (s[j] == ')') {
            --depth;
        } else if (s[j] == ',' && depth == 0) {
            out.push_back(s.substr(start, j - start));
            start = j + 1;
        }
    }
    out.push_back(s.substr(start));
    return out;
}

// Splits on the `and` keyword at paren depth zero, which is what joins a media
// type to its features and one feature to the next.
std::vector<std::string_view> splitOnAnd(std::string_view s) {
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t j = 0; j < s.size(); ++j) {
        const char c = s[j];
        if (c == '(') {
            ++depth;
            continue;
        }
        if (c == ')') {
            --depth;
            continue;
        }
        if (depth != 0 || !isSpace(c)) {
            continue;
        }
        std::size_t wordStart = j;
        while (wordStart < s.size() && isSpace(s[wordStart])) {
            ++wordStart;
        }
        if (wordStart + 3 > s.size() || s.compare(wordStart, 3, "and") != 0) {
            continue;
        }
        const char after = wordStart + 3 < s.size() ? s[wordStart + 3] : '\0';
        if (after != '\0' && !isSpace(after) && after != '(') {
            continue;
        }
        out.push_back(s.substr(start, j - start));
        start = wordStart + 3;
        j = wordStart + 2;  // the loop's ++ lands just past "and"
    }
    out.push_back(s.substr(start));
    return out;
}

// Reads `600`, `600px`, `50%`, `50vw`, `50vh`, `50vmin`, `50vmax`. Returns false
// for anything else, including `em` and `rem`, which have no defined meaning in a
// media query once the initial font size is not the page's.
bool parseMediaNumber(std::string_view value, MediaFeature& out) {
    std::string_view s = trim(value);
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        ++i;
    }
    const std::size_t digits = i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) != 0 || s[i] == '.')) {
        ++i;
    }
    if (i == digits) {
        return false;
    }
    out.number = std::strtof(std::string(s.substr(0, i)).c_str(), nullptr);
    const std::string_view unit = trim(s.substr(i));
    if (unit.empty() || unit == "px") {
        out.unit = MediaFeature::Unit::Px;
        return true;
    }
    if (unit == "%") {
        out.unit = MediaFeature::Unit::Percent;
        return true;
    }
    if (unit == "vw") {
        out.unit = MediaFeature::Unit::Vw;
        return true;
    }
    if (unit == "vh") {
        out.unit = MediaFeature::Unit::Vh;
        return true;
    }
    if (unit == "vmin") {
        out.unit = MediaFeature::Unit::Vmin;
        return true;
    }
    if (unit == "vmax") {
        out.unit = MediaFeature::Unit::Vmax;
        return true;
    }
    return false;
}

// `16/9` or a bare number, for `aspect-ratio`.
bool parseAspectRatio(std::string_view value, MediaFeature& out) {
    out.unit = MediaFeature::Unit::Number;
    const std::size_t slash = value.find('/');
    if (slash == std::string_view::npos) {
        return parseMediaNumber(value, out);
    }
    MediaFeature numerator;
    MediaFeature denominator;
    if (!parseMediaNumber(value.substr(0, slash), numerator) ||
        !parseMediaNumber(value.substr(slash + 1), denominator) || denominator.number == 0.0f) {
        return false;
    }
    out.number = numerator.number / denominator.number;
    return true;
}

// Turns the inside of one `( ... )` into a comparable feature. An unknown name,
// a missing colon, or an unparseable value all come back as `Kind::Unknown`,
// which never matches but is still reported so the query is not a mystery.
MediaFeature parseMediaFeature(std::string_view text) {
    MediaFeature f;
    const std::string_view s = trim(text);
    const std::size_t colon = s.find(':');
    if (colon == std::string_view::npos) {
        return f;
    }
    std::string_view name = trim(s.substr(0, colon));
    const std::string_view value = trim(s.substr(colon + 1));
    if (name.empty() || value.empty()) {
        return f;
    }

    // Media Queries Level 4 range syntax, `(width >= 600px)`, is written without
    // a colon and so already returned above.
    bool ranged = false;
    if (startsWithWord(name, "min-")) {
        f.op = MediaFeature::Op::Ge;
        name = name.substr(4);
        ranged = true;
    } else if (startsWithWord(name, "max-")) {
        f.op = MediaFeature::Op::Le;
        name = name.substr(4);
        ranged = true;
    }
    name = trim(name);

    if (name == "width" || name == "height") {
        f.kind = name == "width" ? MediaFeature::Kind::Width : MediaFeature::Kind::Height;
        if (!parseMediaNumber(value, f)) {
            f.kind = MediaFeature::Kind::Unknown;
        }
    } else if (name == "aspect-ratio") {
        f.kind = MediaFeature::Kind::AspectRatio;
        if (ranged || !parseAspectRatio(value, f)) {
            f.kind = MediaFeature::Kind::Unknown;
        }
    } else if (name == "orientation") {
        f.kind = MediaFeature::Kind::Orientation;
        f.keyword = lower(value);
        if (ranged || (f.keyword != "landscape" && f.keyword != "portrait")) {
            f.kind = MediaFeature::Kind::Unknown;
        }
    } else if (name == "prefers-color-scheme") {
        // A bare range test against the host preference; `min-`/`max-` spellings
        // do not exist for it.
        f.kind = MediaFeature::Kind::ColorScheme;
        f.keyword = lower(value);
        if (ranged || (f.keyword != "light" && f.keyword != "dark")) {
            f.kind = MediaFeature::Kind::Unknown;
        }
    }
    return f;
}

MediaQuery parseMediaQuery(std::string_view raw) {
    MediaQuery q;
    std::string_view s = trim(raw);
    // `not` negates the whole query, type and features together. `only` is a
    // legacy hint that told old browsers to ignore the rule; it means nothing here.
    if (startsWithWord(s, "not") && (s.size() == 3 || isSpace(s[3]))) {
        q.negate = true;
        s = trim(s.substr(3));
    } else if (startsWithWord(s, "only") && (s.size() == 4 || isSpace(s[4]))) {
        s = trim(s.substr(4));
    }

    for (std::string_view segment : splitOnAnd(s)) {
        segment = trim(segment);
        if (segment.empty() || (segment.size() == 3 && segment == "and")) {
            continue;
        }
        if (segment.front() == '(') {
            std::string_view inner = segment;
            if (inner.size() >= 2 && inner.back() == ')') {
                inner = trim(inner.substr(1, inner.size() - 2));
            }
            MediaFeature f = parseMediaFeature(inner);
            if (f.kind == MediaFeature::Kind::Unknown) {
                warnUnsupportedMedia(inner);
            }
            q.features.push_back(std::move(f));
            continue;
        }
        q.type = lower(segment);
        if (q.type != "all" && q.type != "screen" && q.type != "print" && q.type != "speech") {
            warnUnsupportedMedia(q.type);
        }
    }
    return q;
}

// A nested `@media` has to hold at both levels, so an inner alternative is
// folded into its outer one: features concatenate and the two media types have
// to be compatible. `not` cannot be expressed that way, and a pair of types that
// can never both hold (`screen` and `print`) produces a query that never matches
// rather than one that silently matches everything.
bool mergeMedia(const MediaQuery& outer, const MediaQuery& inner, MediaQuery& out) {
    if (outer.negate || inner.negate) {
        return false;
    }
    out = outer;
    if (outer.type == "all") {
        out.type = inner.type;
    } else if (inner.type != "all" && inner.type != outer.type) {
        out.type = "never";
        out.negate = false;
        out.features.clear();
        return true;
    }
    out.features.insert(out.features.end(), inner.features.begin(), inner.features.end());
    return true;
}

// Resolves one feature's threshold against the viewport, in CSS pixels.
float mediaThreshold(const MediaFeature& f, float width, float height) {
    switch (f.unit) {
        case MediaFeature::Unit::Percent:
            // `%` is relative to the dimension the feature itself measures.
            return f.number * 0.01f * (f.kind == MediaFeature::Kind::Height ? height : width);
        case MediaFeature::Unit::Vw:
            return f.number * 0.01f * width;
        case MediaFeature::Unit::Vh:
            return f.number * 0.01f * height;
        case MediaFeature::Unit::Vmin:
            return f.number * 0.01f * std::min(width, height);
        case MediaFeature::Unit::Vmax:
            return f.number * 0.01f * std::max(width, height);
        case MediaFeature::Unit::Number:
        case MediaFeature::Unit::Px:
            return f.number;
    }
    return f.number;
}

bool featureMatches(const MediaFeature& f, float width, float height, bool systemDark) {
    if (f.kind == MediaFeature::Kind::Unknown) {
        return false;
    }
    if (f.kind == MediaFeature::Kind::Orientation) {
        return f.keyword == (width >= height ? "landscape" : "portrait");
    }
    if (f.kind == MediaFeature::Kind::ColorScheme) {
        return f.keyword == (systemDark ? "dark" : "light");
    }
    const float threshold = mediaThreshold(f, width, height);
    // A zero-height viewport would divide by zero before the first layout; the
    // engine has not been told its size yet, and no aspect-ratio query should
    // match on that alone.
    const float actual = f.kind == MediaFeature::Kind::Width    ? width
                         : f.kind == MediaFeature::Kind::Height ? height
                                                                : (height > 0.0f ? width / height : 0.0f);
    switch (f.op) {
        case MediaFeature::Op::Eq:
            return std::fabs(actual - threshold) < 0.001f;
        case MediaFeature::Op::Lt:
            return actual < threshold;
        case MediaFeature::Op::Gt:
            return actual > threshold;
        case MediaFeature::Op::Le:
            return actual <= threshold;
        case MediaFeature::Op::Ge:
            return actual >= threshold;
    }
    return false;
}

// The one rule that matters for `print`: the engine only ever draws to a screen.
bool typeMatches(std::string_view type) {
    return type == "all" || type == "screen";
}

}  // namespace

std::vector<MediaQuery> parseMediaList(std::string_view source) {
    std::vector<MediaQuery> out;
    for (const std::string_view alternative : splitAlternatives(source)) {
        if (trim(alternative).empty()) {
            continue;
        }
        out.push_back(parseMediaQuery(alternative));
    }
    return out;
}

bool mediaQueryMatches(const MediaQuery& query, float viewportWidth, float viewportHeight,
                       bool systemDark) {
    bool base = typeMatches(query.type);
    if (base) {
        for (const MediaFeature& feature : query.features) {
            if (!featureMatches(feature, viewportWidth, viewportHeight, systemDark)) {
                base = false;
                break;
            }
        }
    }
    return query.negate ? !base : base;
}

bool mediaMatches(const std::vector<MediaQuery>& media, float viewportWidth,
                  float viewportHeight, bool systemDark) {
    if (media.empty()) {
        return true;
    }
    for (const MediaQuery& query : media) {
        if (mediaQueryMatches(query, viewportWidth, viewportHeight, systemDark)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

namespace {

// Reads a rule list, appending to `out`. `media` is what the caller is already
// nested inside, and `order` is shared so source order stays one sequence
// across nested `@media` blocks.
// `from`/"0%" map to 0, `to`/"100%" to 1, `N%` to N/100. -1 means the token is
// not a keyframe selector, so the caller skips it.
float keyframeOffset(std::string_view token) {
    if (token == "from") {
        return 0.0f;
    }
    if (token == "to") {
        return 1.0f;
    }
    if (token.size() > 1 && token.back() == '%') {
        const std::string number =
            std::string(token.substr(0, token.size() - 1));
        char* end = nullptr;
        const float value = std::strtof(number.c_str(), &end);
        if (end != number.c_str() && *end == '\0' && value >= 0.0f && value <= 100.0f) {
            return value * 0.01f;
        }
    }
    return -1.0f;
}

// Parses the body of one `@keyframes` block: a list of `from`/`50%`/`to`
// selector blocks that carry the declarations. A comma list shares its block
// across offsets, and a repeated offset overrides the earlier one, which is
// what CSS does. The result is sorted ascending by offset.
void parseKeyframesBody(std::string_view source,
                        std::vector<KeyframeBlock::Keyframe>& frames) {
    std::map<float, std::vector<Declaration>> byOffset;
    std::size_t i = 0;
    while (i < source.size()) {
        if (isSpace(source[i])) {
            ++i;
            continue;
        }
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            i = skipComment(source, i);
            continue;
        }
        const std::size_t selStart = i;
        while (i < source.size() && source[i] != '{') {
            if (source[i] == '}') {
                return;  // stray close brace
            }
            ++i;
        }
        if (i >= source.size()) {
            break;
        }
        const std::string_view selector = trim(source.substr(selStart, i - selStart));
        ++i;  // '{'

        const std::size_t declStart = i;
        int depth = 1;
        while (i < source.size() && depth > 0) {
            if (source[i] == '{') {
                ++depth;
            } else if (source[i] == '}') {
                --depth;
            }
            ++i;
        }
        const std::size_t declEnd = i > declStart && source[i - 1] == '}' ? i - 1 : i;
        auto declarations = parseDeclarations(source.substr(declStart, declEnd - declStart));
        if (declarations.empty() || selector.empty()) {
            continue;
        }
        // Split the selector on commas; every part is a timeline offset.
        std::size_t partStart = 0;
        for (std::size_t j = 0; j <= selector.size(); ++j) {
            const bool last = j == selector.size();
            const char c = last ? ',' : selector[j];
            if (!last && c != ',') {
                continue;
            }
            const float offset = keyframeOffset(trim(selector.substr(partStart, j - partStart)));
            partStart = j + 1;
            if (offset < 0.0f) {
                continue;
            }
            byOffset[offset] = declarations;  // later source order wins
        }
    }
    for (const auto& [offset, declarations] : byOffset) {
        KeyframeBlock::Keyframe frame;
        frame.offset = offset;
        frame.declarations = declarations;
        frames.push_back(std::move(frame));
    }
}

void parseRules(std::string_view source, const std::vector<MediaQuery>& media,
                std::size_t& order, std::vector<Rule>& out,
                std::vector<KeyframeBlock>& keyframes, const std::string& layer,
                const std::vector<Rule::ContainerQuery>& containers,
                const ImportContext& imports) {
    std::size_t i = 0;
    // An `@import` only counts before any other rule in its block (layer
    // statements excepted); anything later is invalid and ignored.
    bool importAllowed = true;

    while (i < source.size()) {
        if (isSpace(source[i])) {
            ++i;
            continue;
        }
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            i = skipComment(source, i);
            continue;
        }
        if (source[i] == '@') {
            ++i;
            const std::size_t nameStart = i;
            while (i < source.size() && isIdentChar(source[i])) {
                ++i;
            }
            const std::string_view name = source.substr(nameStart, i - nameStart);
            const std::size_t preludeStart = i;
            while (i < source.size() && source[i] != '{' && source[i] != ';') {
                ++i;
            }

            if (name == "media" && i < source.size() && source[i] == '{') {
                const std::vector<MediaQuery> inner =
                    parseMediaList(source.substr(preludeStart, i - preludeStart));
                if (inner.empty()) {
                    warnUnsupportedMedia("an empty or unparsable @media condition");
                    continue;
                }
                ++i;  // '{'
                const std::size_t bodyStart = i;
                int depth = 1;
                while (i < source.size() && depth > 0) {
                    if (source[i] == '{') {
                        ++depth;
                    } else if (source[i] == '}') {
                        --depth;
                    }
                    ++i;
                }
                const std::size_t bodyEnd = (i > bodyStart && source[i - 1] == '}') ? i - 1 : i;

                std::vector<MediaQuery> combined;
                if (media.empty()) {
                    combined = inner;
                } else {
                    for (const MediaQuery& outer : media) {
                        for (const MediaQuery& one : inner) {
                            MediaQuery merged;
                            if (mergeMedia(outer, one, merged)) {
                                combined.push_back(std::move(merged));
                            }
                        }
                    }
                    if (combined.empty()) {
                        // Nothing can satisfy both levels; keep one query that
                        // never matches so the block still parses as CSS.
                        MediaQuery impossible;
                        impossible.type = "never";
                        combined.push_back(std::move(impossible));
                    }
                }
                parseRules(source.substr(bodyStart, bodyEnd - bodyStart), combined, order, out,
                           keyframes, layer, containers, imports);
                importAllowed = false;
                continue;
            }

            if (name == "supports" && i < source.size() && source[i] == '{') {
                const std::string_view prelude =
                    trim(source.substr(preludeStart, i - preludeStart));
                ++i;  // '{'
                const std::size_t bodyStart = i;
                int depth = 1;
                while (i < source.size() && depth > 0) {
                    if (source[i] == '{') {
                        ++depth;
                    } else if (source[i] == '}') {
                        --depth;
                    }
                    ++i;
                }
                const std::size_t bodyEnd =
                    (i > bodyStart && source[i - 1] == '}') ? i - 1 : i;
                // A false or unparsable condition drops the block. Nested
                // supports blocks are evaluated independently as they parse.
                if (supportsConditionMatches(prelude)) {
                    parseRules(source.substr(bodyStart, bodyEnd - bodyStart), media, order,
                               out, keyframes, layer, containers, imports);
                }
                importAllowed = false;
                continue;
            }

            if (name == "import" && i < source.size() && source[i] == ';') {
                ++i;
                // Inlines the sheet where the statement stands: relative hrefs
                // resolve against the importer, repeats and cycles are dropped,
                // and the import's media and layer wrap the imported rules.
                // Misplaced imports and unfetchable sheets are skipped.
                if (importAllowed && imports.fetch && imports.seen) {
                    ImportSpec spec;
                    if (parseImportPrelude(
                            trim(source.substr(preludeStart, i - 1 - preludeStart)), spec)) {
                        const std::string href = joinHref(imports.baseHref, spec.href);
                        if (imports.seen->insert(href).second) {
                            if (std::optional<std::string> text = imports.fetch(href)) {
                                const std::vector<MediaQuery>& outerMedia =
                                    spec.hasMedia ? spec.media : media;
                                const std::string layerName =
                                    spec.hasLayer
                                        ? (spec.layerName.empty()
                                               ? anonymousLayerName()
                                               : composeLayerName(layer, spec.layerName))
                                        : layer;
                                const ImportContext nested{imports.fetch, href, imports.seen};
                                parseRules(*text, outerMedia, order, out, keyframes, layerName,
                                           containers, nested);
                            }
                        }
                    }
                }
                continue;
            }

            if (name == "container" && i < source.size() && source[i] == '{') {
                const std::string_view prelude =
                    trim(source.substr(preludeStart, i - preludeStart));
                ++i;  // '{'
                const std::size_t bodyStart = i;
                int depth = 1;
                while (i < source.size() && depth > 0) {
                    if (source[i] == '{') {
                        ++depth;
                    } else if (source[i] == '}') {
                        --depth;
                    }
                    ++i;
                }
                const std::size_t bodyEnd =
                    (i > bodyStart && source[i - 1] == '}') ? i - 1 : i;
                // Anything the query grammar cannot express drops the block,
                // the way an unparsable `@media` does. Nested queries stay
                // separate: each resolves its own nearest query container.
                Rule::ContainerQuery inner;
                if (parseContainerPrelude(prelude, inner)) {
                    std::vector<Rule::ContainerQuery> nested = containers;
                    nested.push_back(std::move(inner));
                    parseRules(source.substr(bodyStart, bodyEnd - bodyStart), media, order,
                               out, keyframes, layer, nested, imports);
                    importAllowed = false;
                }
                continue;
            }

            if (name == "layer") {
                const std::string_view prelude =
                    trim(source.substr(preludeStart, i - preludeStart));
                if (i < source.size() && source[i] == ';') {
                    // A statement: `@layer a, b;` declares the order up front.
                    // Each name becomes a marker rule so the order survives in
                    // the rule stream itself.
                    ++i;
                    for (std::string layerName : splitLayerNames(prelude)) {
                        Rule marker;
                        marker.layer = composeLayerName(layer, layerName);
                        marker.layerStatement = true;
                        marker.media = media;
                        marker.order = order++;
                        out.push_back(std::move(marker));
                    }
                    continue;
                }
                if (i < source.size() && source[i] == '{') {
                    ++i;  // '{'
                    const std::size_t bodyStart = i;
                    int depth = 1;
                    while (i < source.size() && depth > 0) {
                        if (source[i] == '{') {
                            ++depth;
                        } else if (source[i] == '}') {
                            --depth;
                        }
                        ++i;
                    }
                    const std::size_t bodyEnd =
                        (i > bodyStart && source[i - 1] == '}') ? i - 1 : i;
                    // A block holds one layer; an empty prelude is an anonymous
                    // one. A comma list here is invalid CSS and is ignored,
                    // like any rule the engine cannot parse.
                    const bool singleName =
                        !prelude.empty() && prelude.find(',') == std::string_view::npos;
                    if (singleName || prelude.empty()) {
                        importAllowed = false;
                        parseRules(source.substr(bodyStart, bodyEnd - bodyStart), media, order,
                                   out, keyframes,
                                   singleName ? composeLayerName(layer, prelude)
                                              : anonymousLayerName(),
                                   containers, imports);
                    }
                    continue;
                }
                // Malformed (`@layer` with neither `;` nor `{`): fall through
                // to the generic skip below.
            }

            if (name == "keyframes" && i < source.size() && source[i] == '{') {
                const std::string_view name_view =
                    trim(source.substr(preludeStart, i - preludeStart));
                ++i;  // '{'
                const std::size_t bodyStart = i;
                int depth = 1;
                while (i < source.size() && depth > 0) {
                    if (source[i] == '{') {
                        ++depth;
                    } else if (source[i] == '}') {
                        --depth;
                    }
                    ++i;
                }
                const std::size_t bodyEnd = (i > bodyStart && source[i - 1] == '}') ? i - 1 : i;
                if (!name_view.empty()) {
                    KeyframeBlock block;
                    block.name = std::string(name_view);
                    parseKeyframesBody(source.substr(bodyStart, bodyEnd - bodyStart), block.frames);
                    if (!block.frames.empty()) {
                        keyframes.push_back(std::move(block));
                    }
                }
                importAllowed = false;
                continue;
            }

            // Any other at-rule: skip the prelude, then its block if it has one.
            if (i < source.size() && source[i] == ';') {
                ++i;
            } else if (i < source.size() && source[i] == '{') {
                int depth = 1;
                ++i;
                while (i < source.size() && depth > 0) {
                    if (source[i] == '{') {
                        ++depth;
                    } else if (source[i] == '}') {
                        --depth;
                    }
                    ++i;
                }
            }
            importAllowed = false;
            continue;
        }

        // Read the selector list up to '{'.
        const std::size_t selStart = i;
        while (i < source.size() && source[i] != '{') {
            if (source[i] == '}') {
                return;  // stray close brace: stop
            }
            ++i;
        }
        if (i >= source.size()) {
            break;
        }
        const std::string_view prelude = trim(source.substr(selStart, i - selStart));
        ++i;  // '{'

        const std::size_t declStart = i;
        int depth = 1;
        while (i < source.size() && depth > 0) {
            if (source[i] == '{') {
                ++depth;
            } else if (source[i] == '}') {
                --depth;
            }
            ++i;
        }
        const std::size_t declEnd = i > declStart && source[i - 1] == '}' ? i - 1 : i;
        const std::string_view block = source.substr(declStart, declEnd - declStart);

        auto declarations = parseDeclarations(block);
        if (!declarations.empty()) {
            // Split the prelude on commas at depth zero.
            std::size_t partStart = 0;
            int parenDepth = 0;
            for (std::size_t j = 0; j <= prelude.size(); ++j) {
                const bool last = j == prelude.size();
                const char c = last ? ',' : prelude[j];
                if (c == '(') ++parenDepth;
                if (c == ')') --parenDepth;
                if (!last && (c != ',' || parenDepth > 0)) {
                    continue;
                }
                const std::string_view part = trim(prelude.substr(partStart, j - partStart));
                partStart = j + 1;
                if (part.empty()) {
                    continue;
                }
                Rule rule;
                rule.selector = readSelector(part);
                rule.declarations = declarations;
                rule.order = order++;
                rule.media = media;
                rule.layer = layer;
                rule.containers = containers;
                if (!rule.selector.parts.empty()) {
                    out.push_back(std::move(rule));
                    importAllowed = false;
                }
            }
        }
    }
}

}  // namespace

void CSSParser::parseWithKeyframes(std::string_view source, std::vector<Rule>& rules,
                                   std::vector<KeyframeBlock>& keyframes,
                                   FetchStylesheet fetch, std::string_view baseHref) {
    std::size_t order = 0;
    std::set<std::string> seen;
    const ImportContext imports{std::move(fetch), std::string(baseHref), &seen};
    parseRules(source, {}, order, rules, keyframes, std::string(), {}, imports);
}

std::vector<Rule> CSSParser::parse(std::string_view source) {
    std::vector<Rule> rules;
    std::vector<KeyframeBlock> keyframes;
    parseWithKeyframes(source, rules, keyframes);
    return rules;
}

// One @font-face descriptor value: dequoted and trimmed, or empty.
std::string fontFaceString(const Declaration& decl) {
    std::string_view value = trim(decl.value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = trim(value.substr(1, value.size() - 2));
    }
    return std::string(value);
}

// Splits a `src` value on top-level commas, so commas that belong to a
// url() or format() argument do not cut a candidate in two.
std::vector<std::string_view> splitFontSources(std::string_view value) {
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

// Reads the target of a leading `url(...)` in one `src` candidate. Anything
// trailing (a `format(...)` hint) is ignored; `url ()` with a space is
// accepted the way the url tokenizer does. False when there is no usable
// target, which also drops `local(...)` candidates.
bool readFontUrl(std::string_view candidate, std::string_view& out) {
    candidate = trim(candidate);
    if (candidate.size() <= 5 || lower(candidate.substr(0, 4)) != "url(") {
        return false;
    }
    const std::size_t close = candidate.find(')', 4);
    if (close == std::string_view::npos || close <= 4) {
        return false;
    }
    out = trim(candidate.substr(4, close - 4));
    if (out.size() >= 2 && (out.front() == '"' || out.front() == '\'') &&
        out.back() == out.front()) {
        out = trim(out.substr(1, out.size() - 2));
    }
    return !out.empty();
}

std::vector<FontFaceRule> CSSParser::parseFontFaces(std::string_view source,
                                                    std::string_view baseHref) {
    std::vector<FontFaceRule> faces;
    std::size_t i = 0;
    while (i < source.size()) {
        if (isSpace(source[i])) {
            ++i;
            continue;
        }
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            i = skipComment(source, i);
            continue;
        }
        if (source[i] != '@') {
            // Skip anything that is not an at-rule: a rule body or stray text.
            if (source[i] == '{') {
                int depth = 1;
                ++i;
                while (i < source.size() && depth > 0) {
                    if (source[i] == '{') {
                        ++depth;
                    } else if (source[i] == '}') {
                        --depth;
                    }
                    ++i;
                }
            } else {
                ++i;
            }
            continue;
        }
        ++i;
        const std::size_t nameStart = i;
        while (i < source.size() && isIdentChar(source[i])) {
            ++i;
        }
        const std::string_view name = source.substr(nameStart, i - nameStart);
        while (i < source.size() && source[i] != '{' && source[i] != ';') {
            ++i;
        }
        if (name != "font-face" || i >= source.size() || source[i] != '{') {
            if (i < source.size() && source[i] == ';') {
                ++i;
            } else if (i < source.size() && source[i] == '{') {
                int depth = 1;
                ++i;
                while (i < source.size() && depth > 0) {
                    if (source[i] == '{') {
                        ++depth;
                    } else if (source[i] == '}') {
                        --depth;
                    }
                    ++i;
                }
            }
            continue;
        }
        ++i;  // '{'
        const std::size_t bodyStart = i;
        int depth = 1;
        while (i < source.size() && depth > 0) {
            if (source[i] == '{') {
                ++depth;
            } else if (source[i] == '}') {
                --depth;
            }
            ++i;
        }
        const std::size_t bodyEnd = i > bodyStart && source[i - 1] == '}' ? i - 1 : i;
        FontFaceRule face;
        for (const Declaration& decl :
             parseDeclarations(source.substr(bodyStart, bodyEnd - bodyStart))) {
            if (decl.property == "font-family") {
                face.family = fontFaceString(decl);
            } else if (decl.property == "src") {
                // Candidate sources in order: url() targets load, local()
                // names need system matching and are skipped. A trailing
                // format(...) hint is ignored, the target decides.
                for (std::string_view part : splitFontSources(decl.value)) {
                    std::string_view target;
                    if (readFontUrl(part, target)) {
                        face.src.push_back(joinHref(baseHref, target));
                    }
                }
            } else if (decl.property == "font-weight") {
                const std::string text = fontFaceString(decl);
                if (text == "bold") {
                    face.weight = 700;
                } else if (text == "normal") {
                    face.weight = 400;
                } else {
                    char* stop = nullptr;
                    const long weight = std::strtol(text.c_str(), &stop, 10);
                    face.weight = (stop && *stop == '\0' && weight >= 1 && weight <= 1000)
                                      ? static_cast<int>(weight)
                                      : 400;
                }
            } else if (decl.property == "font-style") {
                const std::string text = fontFaceString(decl);
                face.italic = text == "italic" || text == "oblique";
            }
        }
        if (!face.family.empty() && !face.src.empty()) {
            faces.push_back(std::move(face));
        }
    }
    return faces;
}

}  // namespace sui

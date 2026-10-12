#include "CSS/Parser.h"

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

// Parses `@import <url> [media] [layer | layer(name)]`. A `supports(...)`
// condition cannot be evaluated, so like an `@supports` block it drops the
// import. Unparsable media drops it too, the way `@media` itself drops.
bool parseImportPrelude(std::string_view prelude, ImportSpec& spec) {
    std::string_view rest = trim(prelude);
    if (!readImportHref(rest, spec.href)) {
        return false;
    }
    rest = trim(rest);
    if (rest.empty()) {
        return true;
    }
    if (rest.size() >= 8 && rest.compare(0, 8, "supports") == 0 &&
        (rest.size() == 8 || !isIdentChar(rest[8]))) {
        return false;
    }
    // A depth-zero `layer` word splits media from layer. Neither may nest
    // inside the other's parens here.
    std::size_t depth = 0;
    std::size_t split = std::string_view::npos;
    for (std::size_t i = 0; i + 5 <= rest.size(); ++i) {
        const char c = rest[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            depth = depth > 0 ? depth - 1 : 0;
        } else if (depth == 0 && rest.compare(i, 5, "layer") == 0 &&
                   (i + 5 >= rest.size() || !isIdentChar(rest[i + 5]))) {
            split = i;
            break;
        }
    }
    std::string_view mediaPart = trim(split == std::string_view::npos
                                          ? rest
                                          : rest.substr(0, split));
    if (!mediaPart.empty()) {
        spec.media = parseMediaList(mediaPart);
        if (spec.media.empty()) {
            return false;
        }
        spec.hasMedia = true;
    }
    if (split != std::string_view::npos) {
        std::string_view layerPart = trim(rest.substr(split + 5));
        if (layerPart.empty()) {
            spec.hasLayer = true;
        } else if (layerPart.front() == '(' && layerPart.back() == ')') {
            const std::string_view name = trim(layerPart.substr(1, layerPart.size() - 2));
            if (name.empty() || name.find(',') != std::string_view::npos) {
                return false;
            }
            spec.layerName = std::string(name);
            spec.hasLayer = true;
        } else {
            return false;
        }
    }
    return true;
}

// Parses an `@container` prelude: an optional name list followed by one
// `(min-width: Npx)` / `(max-width: Npx)` feature, optionally `and`-chained.
// Anything else (style queries, other units, garbage) fails, and the block is
// dropped, the way an unparsable `@media` is.
bool parseContainerPrelude(std::string_view prelude, Rule::ContainerQuery& query) {
    std::size_t i = 0;
    const auto skipSpaces = [&] {
        while (i < prelude.size() && isSpace(prelude[i])) {
            ++i;
        }
    };
    skipSpaces();
    // Leading idents name the container; the first '(' starts the features.
    while (i < prelude.size() && prelude[i] != '(') {
        const std::size_t start = i;
        while (i < prelude.size() && (isIdentChar(prelude[i]) || prelude[i] == '.')) {
            ++i;
        }
        const std::string_view name = trim(prelude.substr(start, i - start));
        if (!name.empty() && name != "and" && name != "not" && name != "or") {
            query.names.emplace_back(name);
        }
        skipSpaces();
        if (i < prelude.size() && prelude[i] != '(' && !isIdentChar(prelude[i]) &&
            !isSpace(prelude[i])) {
            return false;
        }
    }
    bool anyFeature = false;
    while (true) {
        skipSpaces();
        if (i >= prelude.size() || prelude[i] != '(') {
            return false;
        }
        ++i;
        const std::size_t close = prelude.find(')', i);
        if (close == std::string_view::npos) {
            return false;
        }
        const std::string_view feature = prelude.substr(i, close - i);
        i = close + 1;
        const std::size_t colon = feature.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        const std::string_view property = trim(feature.substr(0, colon));
        const std::string_view amount = trim(feature.substr(colon + 1));
        if (amount.size() < 3 || amount.substr(amount.size() - 2) != "px") {
            return false;
        }
        const float value =
            std::strtof(std::string(amount.substr(0, amount.size() - 2)).c_str(), nullptr);
        if (!std::isfinite(value) || value < 0.0f) {
            return false;
        }
        if (property == "min-width") {
            query.minWidth = anyFeature && query.minWidth >= 0.0f
                                 ? std::max(query.minWidth, value)
                                 : value;
        } else if (property == "max-width") {
            query.maxWidth = anyFeature && query.maxWidth >= 0.0f
                                 ? std::min(query.maxWidth, value)
                                 : value;
        } else {
            return false;
        }
        anyFeature = true;
        skipSpaces();
        if (i >= prelude.size()) {
            break;
        }
        // Only `and` chains; `or`/`not` stay unsupported and drop the block.
        if ((prelude.substr(i, 3) == "and" &&
             (i + 3 >= prelude.size() || !isIdentChar(prelude[i + 3])))) {
            i += 3;
            continue;
        }
        return false;
    }
    if (!anyFeature) {
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
            } else if (name == "checked") {
                out.pseudoClasses.push_back(PseudoClass::Checked);
            } else if (name == "disabled") {
                out.pseudoClasses.push_back(PseudoClass::Disabled);
            } else if (name == "enabled") {
                out.pseudoClasses.push_back(PseudoClass::Enabled);
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
            // select those boxes and carry their own declarations. No other
            // pseudo-element is generated.
            if (name == "before" || name == "after") {
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
            } else if (name == "disabled") {
                out.pseudoClasses.push_back(PseudoClass::Disabled);
            } else if (name == "enabled") {
                out.pseudoClasses.push_back(PseudoClass::Enabled);
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

bool featureMatches(const MediaFeature& f, float width, float height) {
    if (f.kind == MediaFeature::Kind::Unknown) {
        return false;
    }
    if (f.kind == MediaFeature::Kind::Orientation) {
        return f.keyword == (width >= height ? "landscape" : "portrait");
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

bool mediaQueryMatches(const MediaQuery& query, float viewportWidth, float viewportHeight) {
    bool base = typeMatches(query.type);
    if (base) {
        for (const MediaFeature& feature : query.features) {
            if (!featureMatches(feature, viewportWidth, viewportHeight)) {
                base = false;
                break;
            }
        }
    }
    return query.negate ? !base : base;
}

bool mediaMatches(const std::vector<MediaQuery>& media, float viewportWidth,
                  float viewportHeight) {
    if (media.empty()) {
        return true;
    }
    for (const MediaQuery& query : media) {
        if (mediaQueryMatches(query, viewportWidth, viewportHeight)) {
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
                Rule::ContainerQuery container, const ImportContext& imports) {
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
                           keyframes, layer, container, imports);
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
                                           container, nested);
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
                // the way an unparsable `@media` does. A nested query
                // intersects the outer one: bounds tighten, names union.
                Rule::ContainerQuery inner;
                if (parseContainerPrelude(prelude, inner)) {
                    Rule::ContainerQuery merged = container;
                    merged.names.insert(merged.names.end(), inner.names.begin(),
                                        inner.names.end());
                    if (inner.minWidth >= 0.0f) {
                        merged.minWidth = merged.minWidth >= 0.0f
                                              ? std::max(merged.minWidth, inner.minWidth)
                                              : inner.minWidth;
                    }
                    if (inner.maxWidth >= 0.0f) {
                        merged.maxWidth = merged.maxWidth >= 0.0f
                                              ? std::min(merged.maxWidth, inner.maxWidth)
                                              : inner.maxWidth;
                    }
                    merged.active = true;
                    parseRules(source.substr(bodyStart, bodyEnd - bodyStart), media, order,
                               out, keyframes, layer, merged, imports);
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
                                   container, imports);
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
                rule.container = container;
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
    parseRules(source, {}, order, rules, keyframes, std::string(), Rule::ContainerQuery(),
               imports);
}

std::vector<Rule> CSSParser::parse(std::string_view source) {
    std::vector<Rule> rules;
    std::vector<KeyframeBlock> keyframes;
    parseWithKeyframes(source, rules, keyframes);
    return rules;
}

}  // namespace sui

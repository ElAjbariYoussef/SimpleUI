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
            // The three interactive states are the only pseudo-classes the engine
            // evaluates. Anything else leaves the selector matching nothing rather
            // than silently matching every element the rest of it describes.
            if (name == "hover") {
                out.pseudoClasses.push_back(PseudoClass::Hover);
            } else if (name == "active") {
                out.pseudoClasses.push_back(PseudoClass::Active);
            } else if (name == "focus") {
                out.pseudoClasses.push_back(PseudoClass::Focus);
            } else if (name == "first-child") {
                out.pseudoClasses.push_back(PseudoClass::FirstChild);
            } else if (name == "last-child") {
                out.pseudoClasses.push_back(PseudoClass::LastChild);
            } else if (name == "root") {
                out.pseudoClasses.push_back(PseudoClass::Root);
            } else if (name == "nth-child") {
                out.pseudoClasses.push_back(PseudoClass::NthChild);
                // skip args
            } else if (name == "not") {
                out.pseudoClasses.push_back(PseudoClass::Not);
            } else if (name == "is") {
                out.pseudoClasses.push_back(PseudoClass::Is);
            } else {
                out.unsupported = true;
            }
            if (i < src.size() && src[i] == '(') {
                int depth = 1;
                ++i;
                while (i < src.size() && depth > 0) {
                    if (src[i] == '(') ++depth;
                    if (src[i] == ')') --depth;
                    ++i;
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
            } else if (name == "root") {
                out.pseudoClasses.push_back(PseudoClass::Root);
            } else if (name == "nth-child") {
                out.pseudoClasses.push_back(PseudoClass::NthChild);
                // skip args
            } else if (name == "not") {
                out.pseudoClasses.push_back(PseudoClass::Not);
            } else if (name == "is") {
                out.pseudoClasses.push_back(PseudoClass::Is);
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
                std::vector<KeyframeBlock>& keyframes) {
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
                           keyframes);
                continue;
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
                if (!rule.selector.parts.empty()) {
                    out.push_back(std::move(rule));
                }
            }
        }
    }
}

}  // namespace

void CSSParser::parseWithKeyframes(std::string_view source, std::vector<Rule>& rules,
                                   std::vector<KeyframeBlock>& keyframes) {
    std::size_t order = 0;
    parseRules(source, {}, order, rules, keyframes);
}

std::vector<Rule> CSSParser::parse(std::string_view source) {
    std::vector<Rule> rules;
    std::vector<KeyframeBlock> keyframes;
    parseWithKeyframes(source, rules, keyframes);
    return rules;
}

}  // namespace sui

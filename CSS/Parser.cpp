#include "CSS/Parser.h"

#include <algorithm>
#include <cctype>

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

std::vector<Rule> CSSParser::parse(std::string_view source) {
    std::vector<Rule> rules;
    std::size_t i = 0;
    std::size_t order = 0;

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
            // Skip the at-rule prelude, then its block if it has one.
            while (i < source.size() && source[i] != '{' && source[i] != ';') {
                ++i;
            }
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
                return rules;  // stray close brace: stop
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
                if (!rule.selector.parts.empty()) {
                    rules.push_back(std::move(rule));
                }
            }
        }
    }
    return rules;
}

}  // namespace sui

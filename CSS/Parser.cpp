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
            out.parts.push_back({kind, std::string(src.substr(nameStart, i - nameStart))});
            continue;
        }
        if (c == ':' && i + 1 < src.size() && src[i + 1] != ':') {
            // Pseudo-classes/elements are not supported; skip them so the rest
            // of the selector still parses.
            ++i;
            while (i < src.size() && isIdentChar(src[i])) {
                ++i;
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
        if (isIdentChar(c)) {
            const std::size_t nameStart = i;
            while (i < src.size() && isIdentChar(src[i])) {
                ++i;
            }
            out.parts.push_back(
                {Selector::Kind::Type, std::string(src.substr(nameStart, i - nameStart))});
            continue;
        }
        if (c == '*') {
            ++i;
            out.parts.push_back({Selector::Kind::Universal, std::string()});
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

std::vector<Declaration> parseDeclarations(std::string_view source) {
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
            i += 2;
            while (i + 1 < source.size() &&
                   !(source[i] == '*' && source[i + 1] == '/')) {
                ++i;
            }
            i = std::min(i + 2, source.size());
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

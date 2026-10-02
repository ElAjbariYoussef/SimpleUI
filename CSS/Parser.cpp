#include "CSS/Parser.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace sui {
namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

bool isNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
           c == ':';
}

}  // namespace

std::vector<Rule> CSSParser::parse(std::string_view source) {
    std::vector<Rule> rules;
    std::size_t i = 0;
    const std::size_t n = source.size();
    while (i < n) {
        const char c = source[i];
        if (c == '/' && i + 1 < n && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(source[i] == '*' && source[i + 1] == '/')) {
                ++i;
            }
            if (i + 1 < n) i += 2;
            continue;
        }
        if (c == '@') {
            // skip at-rule until ';' or '{'
            while (i < n && source[i] != ';' && source[i] != '{') {
                ++i;
            }
            if (i < n && source[i] == '{') {
                // skip block
                int depth = 1;
                ++i;
                while (i < n && depth > 0) {
                    if (source[i] == '{') {
                        ++depth;
                    } else if (source[i] == '}') {
                        --depth;
                    }
                    ++i;
                }
            } else if (i < n) {
                ++i;
            }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        // read selector
        const std::size_t selStart = i;
        while (i < n && source[i] != '{' && source[i] != '}') {
            ++i;
        }
        if (i >= n || source[i] == '}') {
            break;
        }
        std::string selStr = trim(source.substr(selStart, i - selStart));
        ++i;  // '{'
        const std::size_t declStart = i;
        int depth = 1;
        while (i < n && depth > 0) {
            if (source[i] == '{') {
                ++depth;
            } else if (source[i] == '}') {
                --depth;
            }
            ++i;
        }
        if (depth == 0) {
            --i;  // before '}'
        }
        std::string declBlock(source.substr(declStart, i - declStart));
        if (i < n && source[i] == '}') {
            ++i;
        }
        // parse selector simple
        SimpleSelector ss;
        // naive tokenization
        std::size_t t = 0;
        while (t < selStr.size()) {
            char ch = selStr[t];
            if (std::isspace(static_cast<unsigned char>(ch))) {
                ++t;
                continue;
            }
            if (ch == '#') {
                ++t;
                std::size_t s2 = t;
                while (t < selStr.size() && isNameChar(selStr[t])) ++t;
                Selector sel{Selector::Id, std::string(selStr.substr(s2, t - s2))};
                ss.parts.push_back(sel);
            } else if (ch == '.') {
                ++t;
                std::size_t s2 = t;
                while (t < selStr.size() && isNameChar(selStr[t])) ++t;
                Selector sel{Selector::Class, std::string(selStr.substr(s2, t - s2))};
                ss.parts.push_back(sel);
            } else if (isNameChar(ch)) {
                std::size_t s2 = t;
                while (t < selStr.size() && isNameChar(selStr[t])) ++t;
                Selector sel{Selector::Type, std::string(selStr.substr(s2, t - s2))};
                ss.parts.push_back(sel);
            } else {
                ++t;
            }
        }
        // parse declarations
        Rule r;
        r.selector = ss;
        std::size_t d = 0;
        while (d < declBlock.size()) {
            const char dc = declBlock[d];
            if (dc == '/' && d + 1 < declBlock.size() && declBlock[d + 1] == '*') {
                d += 2;
                while (d + 1 < declBlock.size() &&
                       !(declBlock[d] == '*' && declBlock[d + 1] == '/')) {
                    ++d;
                }
                if (d + 1 < declBlock.size()) d += 2;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(dc)) || dc == ';') {
                ++d;
                continue;
            }
            std::size_t ds = d;
            while (d < declBlock.size() && declBlock[d] != ':' && declBlock[d] != '}') {
                ++d;
            }
            if (d >= declBlock.size() || declBlock[d] != ':') {
                break;
            }
            std::string prop = trim(declBlock.substr(ds, d - ds));
            ++d;  // ':'
            std::size_t vs = d;
            while (d < declBlock.size() && declBlock[d] != ';' && declBlock[d] != '}') {
                ++d;
            }
            std::string val = trim(declBlock.substr(vs, d - vs));
            if (!prop.empty()) {
                r.declarations.emplace_back(prop, val);
            }
            if (d < declBlock.size() && declBlock[d] == ';') {
                ++d;
            }
        }
        if (!r.declarations.empty() || !r.selector.parts.empty()) {
            rules.push_back(std::move(r));
        }
    }
    return rules;
}

}  // namespace sui
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sui {

// A single simple selector: `div`, `.sidebar`, `#counter`, or `*`.
struct Selector {
    enum class Kind { Type, Class, Id, Universal };
    Kind kind = Kind::Universal;
    std::string value;
};

// One comma-separated selector in a rule's prelude. Descendant and child
// combinators are kept as separate parts, tagged with their combinator.
enum class Combinator { None, Descendant, Child };

struct SimpleSelector {
    std::vector<Selector> parts;
    // Combinator linking parts[i] to parts[i + 1].
    std::vector<Combinator> combinators;
};

struct Declaration {
    std::string property;
    std::string value;
    bool important = false;
};

struct Rule {
    SimpleSelector selector;
    std::vector<Declaration> declarations;
    std::size_t order = 0;  // source position, used as the final tiebreak
};

class CSSParser {
public:
    // Parses CSS source into rules, skipping comments and at-rules.
    static std::vector<Rule> parse(std::string_view source);

    // Parses a single selector (no comma list, no declarations).
    static SimpleSelector parseSelector(std::string_view source);
};

// Parses a `style="..."` attribute value into declarations.
std::vector<Declaration> parseDeclarations(std::string_view source);

}  // namespace sui

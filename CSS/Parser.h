#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

// A single simple selector: `div`, `.sidebar`, `#counter`, or `*`.
struct Selector {
    enum class Kind { Type, Class, Id, Universal, Attribute };
    Kind kind = Kind::Universal;
    std::string value;
    // Only for Kind::Attribute: empty means "the attribute is present".
    std::string attributeValue;
};

// One comma-separated selector in a rule's prelude. Descendant and child
// combinators are kept as separate parts, tagged with their combinator.
enum class Combinator { None, Descendant, Child };

// The interactive states a selector can match against. Everything else the
// engine cannot evaluate leaves `unsupported` set on the selector.
enum class PseudoClass : std::uint8_t { Hover, Active, Focus };

struct SimpleSelector {
    std::vector<Selector> parts;
    // Combinator linking parts[i] to parts[i + 1].
    std::vector<Combinator> combinators;
    std::vector<PseudoClass> pseudoClasses;
    // Set when the selector used something the engine cannot evaluate, such as a
    // pseudo-element or an unknown pseudo-class. It then matches nothing at all:
    // dropping the unknown part and matching the rest would make `#t:hover` apply
    // to every `#t`.
    bool unsupported = false;
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

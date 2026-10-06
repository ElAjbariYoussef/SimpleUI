#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

// A single simple selector: `div`, `.sidebar`, `#counter`, or `*`.
struct Selector {
    enum class Kind { Type, Class, Id, Universal, Attribute, PseudoElement };
    Kind kind = Kind::Universal;
    std::string value;
    // Only for Kind::Attribute: the operator read between the name and the value,
    // one of '=', '~', '|', '^', '$', '*'. '\0' means there was none, which is
    // the presence test `[data-role]`; an empty value with '=' is `[data-role=""]`.
    char attributeOp = '\0';
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

// One feature inside an `@media` query, already reduced to something that can be
// compared against the viewport. `min-width: 768px` is Width, >=, 768.
struct MediaFeature {
    enum class Kind : std::uint8_t { Width, Height, AspectRatio, Orientation, Unknown };
    // `min-` and `max-` collapse onto the ordered operators; a bare `name: value`
    // is Eq.
    enum class Op : std::uint8_t { Eq, Lt, Gt, Le, Ge };
    // How the number is scaled before comparison. `%` is relative to the
    // viewport width for `width` and to its height for `height`, as CSS says.
    // The viewport units have no meaning outside a media query, so unlike the
    // engine's own lengths they are resolved here rather than during layout.
    enum class Unit : std::uint8_t { Px, Percent, Number, Vw, Vh, Vmin, Vmax };

    Kind kind = Kind::Unknown;
    Op op = Op::Eq;
    Unit unit = Unit::Px;
    float number = 0.0f;
    // Filled for keyword features such as `orientation: landscape`; empty when
    // the feature compares numbers.
    std::string keyword;
};

// One comma-separated alternative of an `@media` list. Every entry in `features`
// must hold (they are AND'd); an empty list means the query named only a type.
struct MediaQuery {
    // `all`, `screen`, `print`, `speech`, or a type the engine does not know.
    // The engine renders to a screen, so `print` and `speech` are supported and
    // simply never match.
    std::string type = "all";
    // `not` negates the whole query, type and features together.
    bool negate = false;
    std::vector<MediaFeature> features;
};

// One rule, plus the `@media` alternatives it was nested under. Empty means the
// rule is unconditional.
struct Rule {
    SimpleSelector selector;
    std::vector<Declaration> declarations;
    std::size_t order = 0;  // source position, used as the final tiebreak
    std::vector<MediaQuery> media;
    // True once any declaration is a custom property (`--name`) or references one
    // (`var(...)`). Computed by the resolver when the rule is registered, so a
    // rule built by hand does not silently miss the custom-property path.
    bool usesCustom = false;
};

class CSSParser {
public:
    // Parses CSS source into rules, skipping comments and at-rules.
    static std::vector<Rule> parse(std::string_view source);

    // Parses a single selector (no comma list, no declarations).
    static SimpleSelector parseSelector(std::string_view source);
};

// Parses an `@media` query list: alternatives separated by commas, each an
// optional `not`/`only` flag, an optional media type, and `and`-joined
// `(feature: value)` tests. A feature the engine cannot evaluate is kept with
// `Unknown` so it matches nothing and can be reported once, rather than being
// dropped in a way that would make the whole query match.
std::vector<MediaQuery> parseMediaList(std::string_view source);

// Evaluates one alternative, or a whole list as an OR of alternatives. An empty
// list is unconditional and matches.
bool mediaQueryMatches(const MediaQuery& query, float viewportWidth,
                       float viewportHeight);
bool mediaMatches(const std::vector<MediaQuery>& media, float viewportWidth,
                  float viewportHeight);

// Parses a `style="..."` attribute value into declarations.
std::vector<Declaration> parseDeclarations(std::string_view source);

}  // namespace sui

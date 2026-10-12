#pragma once

#include <cstdint>
#include <functional>
#include <optional>
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
enum class PseudoClass : std::uint8_t { Hover, Active, Focus, Checked, FirstChild, LastChild, Root, NthChild, Not, Is, Has };

struct SimpleSelector {
    std::vector<Selector> parts;
    // Combinator linking parts[i] to parts[i + 1].
    std::vector<Combinator> combinators;
    std::vector<PseudoClass> pseudoClasses;
    // The argument of a functional pseudo-class (`:nth-child(2)`, `:not(.x)`,
    // `:is(a, b)`), stored verbatim without the surrounding parentheses. Only the
    // last one is kept, which is enough for the single functional pseudo a compound
    // selector carries in practice.
    std::string pseudoArgument;
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
    // A `@container` size query the rule was nested under. Inactive means none:
    // only `min-width`/`max-width` in px (joined by `and`, with an optional
    // name list) become rules; anything else is dropped at parse time.
    struct ContainerQuery {
        std::vector<std::string> names;
        float minWidth = -1.0f;  // px, negative when absent
        float maxWidth = -1.0f;  // px, negative when absent
        bool active = false;
    };
    ContainerQuery container;
    // True once any declaration is a custom property (`--name`) or references one
    // (`var(...)`). Computed by the resolver when the rule is registered, so a
    // rule built by hand does not silently miss the custom-property path.
    bool usesCustom = false;
    // The `@layer` this rule was written in, dotted for nesting (`outer.inner`),
    // empty when unlayered. Unlayered author rules beat layered ones; among
    // layers the later-declared wins (reversed for `!important`).
    std::string layer;
    // A `@layer a, b;` statement arrives as one marker rule per name: it orders
    // the layers but styles nothing, so the resolver skips it when applying.
    bool layerStatement = false;
};

// One `@keyframes` block: a name plus the stops on the animation timeline,
// each carrying declarations. Stops are stored sorted by offset — `from` is 0,
// `to` is 1, and a percentage maps to its fraction.
struct KeyframeBlock {
    struct Keyframe {
        float offset = 0.0f;  // 0..1
        std::vector<Declaration> declarations;
    };
    std::string name;
    std::vector<Keyframe> frames;
};

class CSSParser {
public:
    // Parses CSS source into rules, skipping comments and at-rules.
    static std::vector<Rule> parse(std::string_view source);

    // Fetches an imported stylesheet by its resolved href. The application
    // hands its resources over; tests hand canned sheets. Absent means the
    // sheet cannot be read and the import is skipped.
    using FetchStylesheet =
        std::function<std::optional<std::string>(std::string_view href)>;

    // Like parse(), but additionally captures `@keyframes` blocks. Both are
    // returned so one stylesheet can carry rules and keyframes separately;
    // parse() is this minus the keyframes.
    //
    // With a fetcher, `@import` inlines the imported sheet where the statement
    // stands: relative hrefs resolve against `baseHref`, cycles are dropped,
    // and the import's media condition and layer wrap the imported rules. An
    // `@import` after any other rule is invalid and ignored, and without a
    // fetcher every import is skipped, which is what parse() does.
    static void parseWithKeyframes(std::string_view source, std::vector<Rule>& rules,
                                   std::vector<KeyframeBlock>& keyframes,
                                   FetchStylesheet fetch = {},
                                   std::string_view baseHref = {});

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

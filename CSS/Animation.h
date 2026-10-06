// What a transition or a keyframe animation is allowed to move, and how.
//
// The engine animates the subset of computed values that have a numeric sense:
// lengths, colours, and plain numbers. A property outside the set, or a pair of
// values that cannot blend (different kinds, or lengths in different units),
// snaps to the target instead of animating, which is also what CSS does for the
// discrete cases. All parsing helpers the resolver needs to turn the
// `transition`/`animation` declarations into the parallel lists in ComputedStyle
// live here too, so applyDeclaration stays a table of property names.
#pragma once

#include "CSS/Style.h"

#include <string>
#include <string_view>
#include <vector>

namespace sui {

// One property snapshot that can be animated. `kind` decides whether and how it
// blends; `None` means the property is not animatable at all (or the value has
// no numeric sense, like `width: auto`).
struct AnimatableValue {
    enum class Kind : std::uint8_t { None, Length, Color, Number };

    Kind kind = Kind::None;
    Length length;
    SDL_Color color{0, 0, 0, 0};
    float number = 0.0f;
};

// Reads one computed property as an animatable snapshot.
AnimatableValue animatableValue(std::string_view property, const ComputedStyle& style);

// Whether `from` can slide to `to`: both sides exist, agree on a kind, and (for
// lengths) share a unit.
bool canBlend(const AnimatableValue& from, const AnimatableValue& to) noexcept;

// The blend of from→to at eased progress t, without writing it anywhere. Used by
// a restarted transition to pick up from "where the eye is".
AnimatableValue blendValues(const AnimatableValue& from, const AnimatableValue& to, float t);

// Writes style[property] as the blend of from→to at progress t.
void blendInto(ComputedStyle& style, std::string_view property,
               const AnimatableValue& from, const AnimatableValue& to, float t);

// `transition-property`/`animation-name` tokens are split on top-level commas
// (a `)`-depth that treats `var(...)` and function arguments as one unit) and
// then trimmed, like every other comma list in the parser.
std::vector<std::string> splitCommaList(std::string_view source);

// Parses one `<time>` token (a number plus `s` or `ms`) into milliseconds.
// Returns false for anything else; a unitless 0 is a valid zero time.
bool parseTimeMs(std::string_view token, float& outMs);

// Expands one transition-property token into the longhand names it covers:
// `all` is the whole animatable set, `margin` the four edges, and so on. A
// token that is not a shorthand is returned as itself, so unknown names simply
// expand to nothing the resolver animates.
std::vector<std::string> expandTransitionProperty(std::string_view property);

// The tokens `transition: all` stands for.
const std::vector<std::string>& allAnimatableProperties() noexcept;

// Fills the transition parallel lists in a style from one `transition` value
// (a comma list of `property duration easing delay` words).
void applyTransitionShorthand(const std::string& value, ComputedStyle& style);

// Fills the animation parallel lists in a style from one `animation` value (a
// comma list of `name duration easing delay iteration direction fill play`
// words, order-tolerant on the times).
void applyAnimationShorthand(const std::string& value, ComputedStyle& style);

// One entry of a parallel list, repeating the last value so the model matches
// CSS's "shorter lists repeat" pairing rule. `empty` stays empty.
template <typename T>
const T& listAt(const std::vector<T>& list, std::size_t index) noexcept {
    if (list.empty()) {
        static const T empty{};
        return empty;
    }
    return list[index < list.size() ? index : list.size() - 1];
}

// Serialises a time in milliseconds as a CSS `<time>` (`0s`, `500ms`, `1.5s`).
std::string serializeTime(float ms);

// Serialises one easing back to CSS text.
std::string serializeEasing(const Easing& easing);

// Serialises the transition parallel lists as one property's CSS text. `property`
// is the shorthand name or one of the longhands; the shorthand folds every
// parallel list into `property duration easing delay` words. Empty when nothing
// is authored.
std::string serializeTransition(std::string_view property, const ComputedStyle& style);

// Same for the animation parallel lists and their longhands.
std::string serializeAnimation(std::string_view property, const ComputedStyle& style);

}  // namespace sui
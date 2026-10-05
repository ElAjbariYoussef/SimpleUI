#pragma once

#include "CSS/DefaultStyle.h"
#include "CSS/Parser.h"
#include "CSS/Style.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Element;

// Which elements are currently hovered, pressed, and focused. A selector's
// `:hover`/`:active`/`:focus` parts are matched against this, so passing a null
// pointer is the same as saying nothing is in any state.
struct PseudoState {
    const Element* hovered = nullptr;
    const Element* active = nullptr;
    const Element* focused = nullptr;
};

// Resolves the computed style for an element: user-agent defaults, then
// matching stylesheet rules by specificity, then the inline style attribute.
class StyleResolver {
public:
    ComputedStyle resolve(Element& element) const;

    void setRules(std::vector<Rule> rules);
    void addRule(Rule rule);
    void eraseRule(std::size_t index);
    void clearRules() {
        rules_.clear();
        ++generation_;
    }

    const std::vector<Rule>& rules() const noexcept { return rules_; }
    bool empty() const noexcept { return rules_.empty(); }

    // Serialises one property of a resolved style back to CSS text, so a script
    // can read what it sees. Returns an empty string for an unsupported name.
    static std::string propertyValue(const ComputedStyle& style, std::string_view property);

    // Bumped by every change to the rule set, so the frame loop can notice that
    // resolution may differ even when the DOM did not move.
    std::uint64_t generation() const noexcept { return generation_; }

    // What `rem` and initial `font-size: <percentage>` resolve against. The
    // application sets this from the document root once it is known.
    float rootFontSize() const noexcept { return rootFontSize_; }
    void setRootFontSize(float size);

    // Interactive state the pseudo-classes are matched against. Each setter
    // returns whether anything changed, so the caller only re-lays-out on a real
    // transition rather than on every mouse move.
    bool setHovered(const Element* element);
    bool setActive(const Element* element);
    bool setFocused(const Element* element);
    const PseudoState& pseudoState() const noexcept { return state_; }

private:
    ComputedStyle resolveWithParent(Element& element, const ComputedStyle& parent) const;

    // Kept sorted weakest-first so resolve() can apply rules in order.
    std::vector<Rule> rules_;
    std::uint64_t generation_ = 0;
    float rootFontSize_ = 16.0f;
    PseudoState state_;
};

// True when `selector` matches `element`, treating a compound selector as
// requiring all of its simple parts on the same element. Pass `state` to
// evaluate `:hover`, `:active`, and `:focus`.
bool selectorMatches(const SimpleSelector& selector, const Element& element,
                     const PseudoState* state = nullptr);

}  // namespace sui

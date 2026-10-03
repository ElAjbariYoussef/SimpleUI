#pragma once

#include "CSS/DefaultStyle.h"
#include "CSS/Parser.h"
#include "CSS/Style.h"

#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Element;

// Resolves the computed style for an element: user-agent defaults, then
// matching stylesheet rules by specificity, then the inline style attribute.
class StyleResolver {
public:
    ComputedStyle resolve(Element& element) const;

    void setRules(std::vector<Rule> rules);
    void addRule(Rule rule);
    void clearRules() { rules_.clear(); }

    const std::vector<Rule>& rules() const noexcept { return rules_; }
    bool empty() const noexcept { return rules_.empty(); }

private:
    ComputedStyle resolveWithParent(Element& element, const ComputedStyle& parent) const;

    // Kept sorted weakest-first so resolve() can apply rules in order.
    std::vector<Rule> rules_;
};

// True when `selector` matches `element`, treating a compound selector as
// requiring all of its simple parts on the same element.
bool selectorMatches(const SimpleSelector& selector, const Element& element);

}  // namespace sui

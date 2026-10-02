#pragma once

#include "CSS/DefaultStyle.h"
#include "CSS/Style.h"

#include <memory>
#include <string_view>
#include <unordered_map>

namespace sui {

class Element;

class StyleResolver {
public:
    // Resolves computed style for an element by merging defaults + inline style
    // attributes (very small subset for now). CSS rules from stylesheets come
    // later (Goal 4).
    ComputedStyle resolve(Element& element) const;

    // Mark a rule dirty if stylesheets change later
    void invalidate() {}
};

}  // namespace sui
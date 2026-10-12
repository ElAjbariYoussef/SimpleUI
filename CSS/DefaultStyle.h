// Default style lookup by element tag. Provides UA-like defaults.
#pragma once

#include "CSS/Style.h"

#include <string_view>

namespace sui {

ComputedStyle defaultStyleFor(std::string_view tag);

// Layers the type-specific form-control defaults over the generic `<input>`
// UA style: a checkbox/radio is a control-sized square or circle with no
// text padding, and a range is a fixed-size groove with no border, rather
// than the text-field chrome `defaultStyleFor` builds for `<input>`.
void applyInputTypeStyle(ComputedStyle& style, std::string_view type);

}  // namespace sui
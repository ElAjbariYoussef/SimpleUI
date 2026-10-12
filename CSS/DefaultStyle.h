// Default style lookup by element tag. Provides UA-like defaults.
#pragma once

#include "CSS/Style.h"

#include <string_view>

namespace sui {

ComputedStyle defaultStyleFor(std::string_view tag);

// Layers the type-specific form-control defaults over the generic `<input>`
// UA style: a checkbox/radio is a control-sized square or circle with no
// text padding, a range is a fixed-size groove with no border, and
// date/time/color/file keep the text-field chrome with wider padding where
// the box paints an indicator (calendar/clock glyph, swatch, Browse button).
void applyInputTypeStyle(ComputedStyle& style, std::string_view type);

}  // namespace sui
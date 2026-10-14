// Default style lookup by element tag. Provides UA-like defaults.
#pragma once

#include "CSS/Style.h"

#include <string_view>

namespace sui {

ComputedStyle defaultStyleFor(std::string_view tag);

// The user-agent canvas colours: the two pairs `color-scheme` switches between
// when the document root never chose its own. The engine's own default is the
// dark pair.
struct UaCanvas {
    SDL_Color background;
    SDL_Color text;
};
const UaCanvas& uaCanvas(bool dark) noexcept;

// Layers the type-specific form-control defaults over the generic `<input>`
// UA style: a checkbox/radio is a control-sized square or circle with no
// text padding, a range is a fixed-size groove with no border, and
// date/time/color/file keep the text-field chrome with wider padding where
// the box paints an indicator (calendar/clock glyph, swatch, Browse button).
void applyInputTypeStyle(ComputedStyle& style, std::string_view type);

}  // namespace sui
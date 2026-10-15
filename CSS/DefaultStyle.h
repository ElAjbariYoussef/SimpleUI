// Default style lookup by element tag. Provides UA-like defaults.
#pragma once

#include "CSS/Style.h"

#include <string_view>

namespace sui {

ComputedStyle defaultStyleFor(std::string_view tag);

// Sizes phrasing elements relative to their parent, which a static user-agent
// default cannot do: `small`, `sub` and `sup` shrink, `big` grows, and `sub`
// and `sup` shift the baseline. Only fills in what the cascade left alone, so
// an author `font-size` or `vertical-align` still wins. Runs after inheritance
// and before author declarations, like the input-type style.
void applyPhrasingStyle(ComputedStyle& style, std::string_view tag,
                        const ComputedStyle& parent);

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
// Loading a stylesheet's `@font-face` blocks into the application font.
//
// The blocks are a per-page concern, but the faces land in the process-wide
// TextFont and stay for its lifetime, so loading is deduplicated by the
// identity a face declares: family, weight, and style. A page that restates
// the same identity keeps the face that loaded first.
#pragma once

#include <cstddef>
#include <string_view>

namespace sui {

class Resources;
class TextFont;

// Loads every `@font-face` of one stylesheet into `font`, resolving url()
// targets through `resources`. `sheetHref` is the sheet's own href, the base
// relative targets resolve against (the same base @import uses). Returns how
// many faces were added; identities already loaded add nothing.
std::size_t loadFontFaces(TextFont& font, const Resources& resources, std::string_view css,
                          std::string_view sheetHref);

}  // namespace sui

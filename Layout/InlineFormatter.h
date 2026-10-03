#pragma once

#include "CSS/Style.h"

#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Element;
class TextFont;

struct TextRun {
    std::string text;
    float width = 0.0f;
    float height = 0.0f;
    // Distance from the top of the line to the text baseline.
    float baseline = 0.0f;
};

struct InlineLine {
    std::vector<TextRun> runs;
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
};

// Breaks text into lines within maxWidth using real glyph advances. A '\n' in
// the input forces a line break (used for <br>).
class InlineFormatter {
public:
    InlineFormatter() = default;

    // Explicit API so callers can supply text gathered from several nodes.
    std::vector<InlineLine> formatText(const TextFont& font,
                                       const ComputedStyle& style, float maxWidth,
                                       std::string_view text);

    std::vector<InlineLine> format(const TextFont& font, Element& element,
                                   const ComputedStyle& style, float maxWidth);

    // Width of text without wrapping; used for shrink-to-fit sizing.
    static float measure(const TextFont& font, const ComputedStyle& style,
                         std::string_view text);
};

}  // namespace sui

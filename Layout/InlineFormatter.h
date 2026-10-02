#pragma once

#include "CSS/Style.h"

#include <functional>
#include <string_view>

namespace sui {

class Element;

struct TextRun {
    std::string text;
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
};

struct InlineLine {
    std::vector<TextRun> runs;
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
    float y = 0.0f;
};

// Simple inline formatter: breaks text into lines within maxWidth. Currently
// ASCII-only and whitespace-based wrapping.
class InlineFormatter {
public:
    InlineFormatter() = default;

    // Format a text node's content into lines given style and container width.
    std::vector<InlineLine> format(Element& element, const ComputedStyle& style,
                                   float maxWidth);
};

}  // namespace sui
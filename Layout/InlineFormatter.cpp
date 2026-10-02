#include "Layout/InlineFormatter.h"

#include "Rendering/Text.h"
#include "SUI/DOM/Element.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <vector>

namespace sui {
namespace {

float ascentFor(TextFont& /*font*/, float size) {
    return size * 0.8f;
}

}  // namespace

std::vector<InlineLine> InlineFormatter::format(Element& element,
                                               const ComputedStyle& style,
                                               float maxWidth) {
    std::vector<InlineLine> lines;
    if (maxWidth <= 0.0f) {
        return lines;
    }
    std::string text = element.textContent();
    if (text.empty()) {
        return lines;
    }

    // Whitespace split
    std::vector<std::string> words;
    std::string cur;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) {
                words.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        words.push_back(cur);
    }

    InlineLine line;
    float lineW = 0.0f;
    const float lineH = style.fontSize * style.lineHeight;
    const float base = style.fontSize;  // rough baseline

    for (const auto& w : words) {
        const float wordW = style.fontSize * static_cast<float>(w.size()) * 0.6f;  // rough
        if (lineW + wordW <= maxWidth) {
            TextRun r;
            r.text = w;
            r.width = wordW;
            r.height = lineH;
            r.baseline = base;
            line.runs.push_back(std::move(r));
            lineW += wordW + style.fontSize * 0.2f;  // space
        } else {
            if (!line.runs.empty()) {
                line.width = lineW;
                line.height = lineH;
                line.baseline = base;
                lines.push_back(std::move(line));
                line = InlineLine();
                lineW = 0.0f;
            }
            // word longer than line
            TextRun r;
            r.text = w;
            r.width = wordW;
            r.height = lineH;
            r.baseline = base;
            line.runs.push_back(std::move(r));
            lineW = wordW;
        }
    }
    if (!line.runs.empty()) {
        line.width = lineW;
        line.height = lineH;
        line.baseline = base;
        lines.push_back(std::move(line));
    }
    if (lines.empty()) {
        InlineLine l;
        l.height = lineH;
        l.baseline = base;
        lines.push_back(l);
    }
    return lines;
}

}  // namespace sui
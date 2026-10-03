#include "Layout/InlineFormatter.h"

#include "Rendering/Text.h"
#include "SUI/DOM/Element.h"

#include <algorithm>
#include <cctype>

namespace sui {
namespace {

// Block-level children force a line break on both sides, mirroring how a
// browser separates `<div>` siblings inside an inline formatting context.
bool isBlockTag(std::string_view tag) {
    return tag == "div" || tag == "p" || tag == "section" || tag == "article" ||
           tag == "header" || tag == "footer" || tag == "main" || tag == "nav" ||
           tag == "aside" || tag == "ul" || tag == "ol" || tag == "li" ||
           tag == "pre" || tag == "blockquote" || tag == "table" || tag == "h1" ||
           tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6";
}

void collect(Element& el, std::string& out, bool& pendingBreak) {
    if (!el.isVisual()) {
        return;
    }
    if (el.tag() == "br") {
        out.push_back('\n');
        pendingBreak = true;
        return;
    }
    if (pendingBreak && !out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    pendingBreak = false;

    const std::string_view own = el.text();
    for (char c : own) {
        if (c == '\n' || c == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }

    for (const auto& child : el.children()) {
        if (!child) {
            continue;
        }
        if (isBlockTag(child->tag()) && !out.empty() && out.back() != '\n') {
            out.push_back('\n');
        }
        collect(*child, out, pendingBreak);
    }
}

struct Word {
    std::string_view text;
    float width = 0.0f;
    bool forcedBreak = false;
};

}  // namespace

float InlineFormatter::measure(const TextFont& font, const ComputedStyle& style,
                               std::string_view text) {
    if (text.empty() || style.fontSize <= 0.0f) {
        return 0.0f;
    }
    return font.measureText(text, style.fontSize);
}

std::vector<InlineLine> InlineFormatter::formatText(const TextFont& font,
                                                    const ComputedStyle& style,
                                                    float maxWidth,
                                                    std::string_view text) {
    std::vector<InlineLine> lines;
    if (text.empty() || style.fontSize <= 0.0f) {
        return lines;
    }

    const TextFont::VerticalMetrics vm = font.verticalMetrics(style.fontSize);
    const float lineHeight =
        std::max(style.fontSize * style.lineHeight, vm.height);
    const float spaceWidth = font.measureText(" ", style.fontSize);

    // Split into words; newlines become forced-break markers.
    std::vector<Word> words;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        const std::size_t end = nl == std::string_view::npos ? text.size() : nl;
        std::size_t cursor = start;
        while (cursor < end) {
            while (cursor < end && std::isspace(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
            const std::size_t wordStart = cursor;
            while (cursor < end && !std::isspace(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
            if (cursor > wordStart) {
                const std::string_view word = text.substr(wordStart, cursor - wordStart);
                words.push_back({word, font.measureText(word, style.fontSize), false});
            }
        }
        if (nl == std::string_view::npos) {
            break;
        }
        words.push_back({"", 0.0f, true});
        start = nl + 1;
    }

    if (words.empty()) {
        return lines;
    }

    auto finishLine = [&](InlineLine& line, float width) {
        line.width = width;
        line.height = lineHeight;
        line.baseline = vm.ascent;
        lines.push_back(std::move(line));
        line = InlineLine();
    };

    InlineLine line;
    float lineWidth = 0.0f;
    bool needsSpace = false;

    for (const auto& word : words) {
        if (word.forcedBreak) {
            finishLine(line, lineWidth);
            lineWidth = 0.0f;
            needsSpace = false;
            continue;
        }
        const float advance = (needsSpace ? spaceWidth : 0.0f) + word.width;
        if (!line.runs.empty() && lineWidth + advance > maxWidth) {
            finishLine(line, lineWidth);
            lineWidth = 0.0f;
            needsSpace = false;
        }
        if (needsSpace) {
            TextRun space;
            space.text = " ";
            space.width = spaceWidth;
            space.height = lineHeight;
            space.baseline = vm.ascent;
            line.runs.push_back(std::move(space));
            lineWidth += spaceWidth;
        }
        TextRun run;
        run.text = std::string(word.text);
        run.width = word.width;
        run.height = lineHeight;
        run.baseline = vm.ascent;
        line.runs.push_back(std::move(run));
        lineWidth += word.width;
        needsSpace = true;
    }
    if (!line.runs.empty()) {
        finishLine(line, lineWidth);
    }

    return lines;
}

std::vector<InlineLine> InlineFormatter::format(const TextFont& font, Element& element,
                                                const ComputedStyle& style,
                                                float maxWidth) {
    std::string text;
    bool pendingBreak = false;
    collect(element, text, pendingBreak);
    return formatText(font, style, maxWidth, text);
}

}  // namespace sui

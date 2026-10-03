#include "CSS/DefaultStyle.h"

namespace sui {
namespace {

using Style = ComputedStyle;

// Mirrors a browser user-agent stylesheet: sizes, weights, and margins come
// from the UA, everything else is inherited. Inheritable values that are set
// here are flagged so StyleResolver::inheritInto leaves them alone.
ComputedStyle block(std::uint16_t specified = 0) {
    Style s;
    s.display = Style::Block;
    s.specified = specified;
    return s;
}

}  // namespace

ComputedStyle defaultStyleFor(std::string_view tag) {
    if (tag == "body") {
        Style s = block(Style::SpecifiedColor);
        s.backgroundColor = {24, 24, 27, 255};
        s.color = {228, 228, 231, 255};
        s.margin = {8.0f, 8.0f, 8.0f, 8.0f};
        return s;
    }

    if (tag == "div" || tag == "section" || tag == "article" ||
        tag == "header" || tag == "footer" || tag == "main" ||
        tag == "nav" || tag == "aside") {
        return block();
    }

    if (tag == "p") {
        Style s = block();
        s.margin = {8.0f, 0.0f, 8.0f, 0.0f};
        return s;
    }

    if (tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" ||
        tag == "h5" || tag == "h6") {
        Style s = block(Style::SpecifiedFontSize | Style::SpecifiedFontBold);
        s.fontBold = true;
        switch (tag[1]) {
            case '1': s.fontSize = 32.0f; s.margin = {16.0f, 0.0f, 8.0f, 0.0f}; break;
            case '2': s.fontSize = 24.0f; s.margin = {12.0f, 0.0f, 6.0f, 0.0f}; break;
            case '3': s.fontSize = 20.0f; s.margin = {10.0f, 0.0f, 6.0f, 0.0f}; break;
            case '4': s.fontSize = 18.0f; s.margin = {8.0f, 0.0f, 4.0f, 0.0f}; break;
            case '5': s.fontSize = 16.0f; s.margin = {6.0f, 0.0f, 4.0f, 0.0f}; break;
            default: s.fontSize = 14.0f; s.margin = {4.0f, 0.0f, 2.0f, 0.0f}; break;
        }
        return s;
    }

    if (tag == "a") {
        Style s;
        s.display = Style::Inline;
        s.specified = Style::SpecifiedColor;
        s.color = {96, 165, 250, 255};
        return s;
    }

    if (tag == "span" || tag == "label" || tag == "br" || tag == "code" ||
        tag == "em" || tag == "i" || tag == "small" || tag == "sub" ||
        tag == "sup") {
        Style s;
        s.display = Style::Inline;
        return s;
    }

    if (tag == "strong" || tag == "b") {
        Style s;
        s.display = Style::Inline;
        s.specified = Style::SpecifiedFontBold;
        s.fontBold = true;
        return s;
    }

    if (tag == "button") {
        Style s;
        s.display = Style::InlineBlock;
        s.specified = Style::SpecifiedColor | Style::SpecifiedLineHeight;
        s.padding = {6.0f, 12.0f, 6.0f, 12.0f};
        s.borderWidth = {1.0f, 1.0f, 1.0f, 1.0f};
        s.borderColor = {63, 63, 70, 255};
        s.backgroundColor = {39, 39, 42, 255};
        s.color = {228, 228, 231, 255};
        s.lineHeight = 1.0f;
        return s;
    }

    if (tag == "input") {
        Style s;
        s.display = Style::InlineBlock;
        s.specified = Style::SpecifiedColor | Style::SpecifiedLineHeight;
        s.padding = {4.0f, 8.0f, 4.0f, 8.0f};
        s.borderWidth = {1.0f, 1.0f, 1.0f, 1.0f};
        s.borderColor = {82, 82, 91, 255};
        s.backgroundColor = {24, 24, 27, 255};
        s.color = {228, 228, 231, 255};
        s.lineHeight = 1.0f;
        return s;
    }

    if (tag == "ul" || tag == "ol") {
        Style s = block();
        s.margin = {8.0f, 0.0f, 8.0f, 0.0f};
        s.padding = {0.0f, 0.0f, 0.0f, 32.0f};
        return s;
    }

    if (tag == "li") {
        Style s = block();
        s.margin = {0.0f, 0.0f, 0.0f, 0.0f};
        return s;
    }

    if (tag == "table") {
        Style s = block();
        s.margin = {8.0f, 0.0f, 8.0f, 0.0f};
        return s;
    }
    if (tag == "tr") {
        return block();
    }
    if (tag == "th" || tag == "td") {
        Style s;
        s.display = Style::InlineBlock;
        s.padding = {4.0f, 8.0f, 4.0f, 8.0f};
        s.borderWidth = {1.0f, 1.0f, 1.0f, 1.0f};
        s.borderColor = {63, 63, 70, 255};
        return s;
    }

    // Media and replaced elements keep their author-specified size.
    if (tag == "img" || tag == "video" || tag == "canvas" || tag == "svg" ||
        tag == "iframe" || tag == "picture" || tag == "audio" ||
        tag == "source" || tag == "track") {
        Style s;
        s.display = Style::InlineBlock;
        return s;
    }

    return block();
}

}  // namespace sui
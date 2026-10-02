#include "CSS/DefaultStyle.h"

#include <functional>

namespace sui {

ComputedStyle defaultStyleFor(std::string_view tag) {
    ComputedStyle s;
    // Body baseline
    if (tag == "body") {
        s.display = ComputedStyle::Block;
        s.backgroundColor = {24, 24, 27, 255};
        s.color = {228, 228, 231, 255};
        s.fontSize = 14.0f;
        s.lineHeight = 1.5f;
        s.margin = {8.0f, 8.0f, 8.0f, 8.0f};
        return s;
    }

    if (tag == "div" || tag == "section" || tag == "article" ||
        tag == "header" || tag == "footer" || tag == "main" ||
        tag == "nav" || tag == "aside") {
        s.display = ComputedStyle::Block;
        s.fontSize = 14.0f;
        s.lineHeight = 1.4f;
        return s;
    }

    if (tag == "p") {
        s.display = ComputedStyle::Block;
        s.margin = {8.0f, 0.0f, 8.0f, 0.0f};
        s.fontSize = 14.0f;
        s.lineHeight = 1.5f;
        return s;
    }

    if (tag == "h1") {
        s.display = ComputedStyle::Block;
        s.fontSize = 32.0f;
        s.fontBold = true;
        s.lineHeight = 1.15f;
        s.margin = {16.0f, 0.0f, 8.0f, 0.0f};
        return s;
    }
    if (tag == "h2") {
        s.display = ComputedStyle::Block;
        s.fontSize = 24.0f;
        s.fontBold = true;
        s.lineHeight = 1.2f;
        s.margin = {12.0f, 0.0f, 6.0f, 0.0f};
        return s;
    }
    if (tag == "h3") {
        s.display = ComputedStyle::Block;
        s.fontSize = 20.0f;
        s.fontBold = true;
        s.lineHeight = 1.25f;
        s.margin = {10.0f, 0.0f, 6.0f, 0.0f};
        return s;
    }
    if (tag == "h4") {
        s.display = ComputedStyle::Block;
        s.fontSize = 18.0f;
        s.fontBold = true;
        s.margin = {8.0f, 0.0f, 4.0f, 0.0f};
        return s;
    }
    if (tag == "h5") {
        s.display = ComputedStyle::Block;
        s.fontSize = 16.0f;
        s.fontBold = true;
        s.margin = {6.0f, 0.0f, 4.0f, 0.0f};
        return s;
    }
    if (tag == "h6") {
        s.display = ComputedStyle::Block;
        s.fontSize = 14.0f;
        s.fontBold = true;
        s.margin = {4.0f, 0.0f, 2.0f, 0.0f};
        return s;
    }

    if (tag == "a") {
        s.display = ComputedStyle::Inline;
        s.color = {96, 165, 250, 255};
        return s;
    }

    if (tag == "span" || tag == "label") {
        s.display = ComputedStyle::Inline;
        return s;
    }

    if (tag == "button") {
        s.display = ComputedStyle::InlineBlock;
        s.padding = {6.0f, 12.0f, 6.0f, 12.0f};
        s.borderWidth = {1.0f, 1.0f, 1.0f, 1.0f};
        s.borderColor = {63, 63, 70, 255};
        s.backgroundColor = {39, 39, 42, 255};
        s.color = {228, 228, 231, 255};
        s.fontSize = 14.0f;
        s.lineHeight = 1.0f;
        return s;
    }

    if (tag == "input") {
        s.display = ComputedStyle::InlineBlock;
        s.padding = {4.0f, 8.0f, 4.0f, 8.0f};
        s.borderWidth = {1.0f, 1.0f, 1.0f, 1.0f};
        s.borderColor = {82, 82, 91, 255};
        s.backgroundColor = {24, 24, 27, 255};
        s.color = {228, 228, 231, 255};
        s.fontSize = 14.0f;
        s.lineHeight = 1.0f;
        return s;
    }

    if (tag == "br") {
        s.display = ComputedStyle::Block;
        return s;
    }

    if (tag == "table") {
        s.display = ComputedStyle::Block;
        s.margin = {8.0f, 0.0f, 8.0f, 0.0f};
        return s;
    }
    if (tag == "tr") {
        s.display = ComputedStyle::Block;
        return s;
    }
    if (tag == "th" || tag == "td") {
        s.display = ComputedStyle::InlineBlock;
        s.padding = {4.0f, 8.0f, 4.0f, 8.0f};
        s.borderWidth = {1.0f, 1.0f, 1.0f, 1.0f};
        s.borderColor = {63, 63, 70, 255};
        return s;
    }

    if (tag == "img" || tag == "video" || tag == "canvas") {
        s.display = ComputedStyle::InlineBlock;
        return s;
    }
    if (tag == "svg" || tag == "iframe" || tag == "picture" || tag == "audio" ||
        tag == "source" || tag == "track") {
        s.display = ComputedStyle::InlineBlock;
        return s;
    }

    s.display = ComputedStyle::Block;
    s.fontSize = 14.0f;
    return s;
}

}  // namespace sui
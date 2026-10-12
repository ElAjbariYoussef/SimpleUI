#include "CSS/DefaultStyle.h"

namespace sui {
namespace {

using Style = ComputedStyle;

// Mirrors a browser user-agent stylesheet: sizes, weights, and margins come
// from the UA, everything else is inherited. Inheritable values that are set
// here are flagged so StyleResolver::inheritInto leaves them alone.
ComputedStyle block(std::uint32_t specified = 0) {
    Style s;
    s.display = Style::Block;
    s.specified = specified;
    return s;
}

}  // namespace

ComputedStyle defaultStyleFor(std::string_view tag) {
    // A generated box joins the inline flow of the element it hangs off, which is
    // what `content` on `::before`/`::after` is for.
    if (tag == "::before" || tag == "::after") {
        Style s;
        s.display = Style::Inline;
        return s;
    }

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
        s.specified = Style::SpecifiedColor | Style::SpecifiedCursor;
        s.color = {96, 165, 250, 255};
        // A link is the one element whose role is unambiguous from the tag, so the
        // UA gives it the pointer shape the way a browser does.
        s.cursor = Style::CursorPointer;
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

    // Phrasing tags. They have to stay inline: defaulted to block, each one forces a
    // line break and the text either side of it lays out into no run at all, so it is
    // never drawn. That made these the one gap here that damages correct markup
    // rather than merely failing to decorate it.
    //
    // Deliberately only `mark`: `text-decoration` and `font-style` are unimplemented,
    // and `font-family` is stored but never used to choose a font, so UA rules for
    // underline, italic and monospace would be declarations that do nothing -- which
    // is the failure mode this milestone exists to remove.
    if (tag == "u" || tag == "s" || tag == "big" || tag == "kbd" || tag == "samp" ||
        tag == "var" || tag == "q" || tag == "cite" || tag == "abbr" || tag == "mark" ||
        tag == "del" || tag == "ins" || tag == "time" || tag == "data" || tag == "bdi" ||
        tag == "bdo" || tag == "wbr") {
        Style s;
        s.display = Style::Inline;
        if (tag == "mark") {
            s.backgroundColor = {255, 255, 0, 255};
            s.color = {0, 0, 0, 255};
            s.specified = Style::SpecifiedColor;
        }
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

    if (tag == "textarea") {
        Style s;
        s.display = Style::InlineBlock;
        s.specified = Style::SpecifiedColor | Style::SpecifiedLineHeight;
        s.padding = {2.0f, 2.0f, 2.0f, 2.0f};
        s.borderWidth = {1.0f, 1.0f, 1.0f, 1.0f};
        s.borderColor = {82, 82, 91, 255};
        s.backgroundColor = {24, 24, 27, 255};
        s.color = {228, 228, 231, 255};
        s.lineHeight = 1.0f;
        // Lines keep their newlines and wrap instead of overflowing; its layout
        // gives it a default size from the `rows` / `cols` attributes.
        s.whiteSpace = Style::WrapPreWrap;
        s.overflowX = Style::Auto;
        s.overflowY = Style::Auto;
        return s;
    }

    if (tag == "select") {
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

    // An option never lays out as a box of its own: the closed select paints
    // the selected text itself and the open popup is an overlay, so option and
    // optgroup children take no space.
    if (tag == "option" || tag == "optgroup") {
        Style s;
        s.display = Style::None;
        return s;
    }

    if (tag == "ul") {
        Style s = block(Style::SpecifiedListStyleType);
        s.margin = {8.0f, 0.0f, 8.0f, 0.0f};
        s.padding = {0.0f, 0.0f, 0.0f, 32.0f};
        s.listStyleType = Style::LSTDisc;
        return s;
    }

    if (tag == "ol") {
        Style s = block(Style::SpecifiedListStyleType);
        s.margin = {8.0f, 0.0f, 8.0f, 0.0f};
        s.padding = {0.0f, 0.0f, 0.0f, 32.0f};
        s.listStyleType = Style::LSTDecimal;
        return s;
    }

    if (tag == "li") {
        Style s = block();
        s.margin = {0.0f, 0.0f, 0.0f, 0.0f};
        return s;
    }

    if (tag == "table") {
        Style s = block();
        s.display = Style::Table;
        s.margin = {8.0f, 0.0f, 8.0f, 0.0f};
        // The HTML default: the separate border model with a 2px gap.
        s.borderSpacingH = 2.0f;
        s.borderSpacingV = 2.0f;
        return s;
    }

    if (tag == "caption") {
        Style s = block();
        s.display = Style::TableCaption;
        return s;
    }

    if (tag == "tr") {
        Style s;
        s.display = Style::TableRow;
        return s;
    }
    if (tag == "td" || tag == "th") {
        Style s;
        s.display = Style::TableCell;
        s.padding = {4.0f, 8.0f, 4.0f, 8.0f};
        if (tag == "th") {
            s.specified = Style::SpecifiedFontBold;
            s.fontBold = true;
        }
        return s;
    }
    if (tag == "thead") {
        Style s;
        s.display = Style::TableHeaderGroup;
        return s;
    }
    if (tag == "tbody") {
        Style s;
        s.display = Style::TableRowGroup;
        return s;
    }
    if (tag == "tfoot") {
        Style s;
        s.display = Style::TableFooterGroup;
        return s;
    }

    // Media and replaced elements keep their author-specified size, or HTML standard defaults.
    if (tag == "video" || tag == "canvas") {
        Style s;
        s.display = Style::InlineBlock;
        s.width = {Length::Px, 300.0f};
        s.height = {Length::Px, 150.0f};
        return s;
    }
    if (tag == "audio") {
        Style s;
        s.display = Style::InlineBlock;
        s.width = {Length::Px, 300.0f};
        s.height = {Length::Px, 32.0f};
        return s;
    }
    if (tag == "img" || tag == "svg" ||
        tag == "iframe" || tag == "picture" ||
        tag == "source" || tag == "track") {
        Style s;
        s.display = Style::InlineBlock;
        return s;
    }

    return block();
}

void applyInputTypeStyle(ComputedStyle& style, std::string_view type) {
    if (type == "range") {
        // A slider is a fixed-size groove with no text-field chrome: no padding,
        // no border, and a transparent background so the page shows through.
        style.padding = {0.0f, 0.0f, 0.0f, 0.0f};
        style.width = Length(129.0f);
        style.height = Length(15.0f);
        style.borderWidth = {0.0f, 0.0f, 0.0f, 0.0f};
        style.backgroundColor = {0, 0, 0, 0};
        return;
    }
    if (type != "checkbox" && type != "radio") {
        return;
    }
    // A checkbox or radio is a control-sized box rather than a text field: no
    // padding, a fixed square, and (for the radio) a fully round corner.
    constexpr float kSize = 13.0f;
    style.padding = {0.0f, 0.0f, 0.0f, 0.0f};
    style.width = Length(kSize);
    style.height = Length(kSize);
    const Length radius = type == "radio" ? Length(Length::Percent, 50.0f) : Length(2.0f);
    style.borderRadii.topLeft = {radius, radius};
    style.borderRadii.topRight = {radius, radius};
    style.borderRadii.bottomRight = {radius, radius};
    style.borderRadii.bottomLeft = {radius, radius};
}

}  // namespace sui
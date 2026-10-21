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

const UaCanvas& uaCanvas(bool dark) noexcept {
    static const UaCanvas kDark{{24, 24, 27, 255}, {228, 228, 231, 255}};
    static const UaCanvas kLight{{255, 255, 255, 255}, {24, 24, 27, 255}};
    return dark ? kDark : kLight;
}

const UaWidgets& uaWidgets(bool dark) noexcept {
    static const UaWidgets kDark{
        {24, 24, 27, 255},   // surface
        {39, 39, 42, 255},   // raised
        {82, 82, 91, 255},   // border
        {63, 63, 70, 255},   // raisedBorder
        {228, 228, 231, 255},// text
        {244, 244, 245, 255},// icon
        {39, 39, 42, 255},   // track
        {113, 113, 122, 255},// thumb
        {63, 63, 70, 255},   // selected
    };
    static const UaWidgets kLight{
        {255, 255, 255, 255},
        {239, 239, 244, 255},
        {181, 181, 190, 255},
        {212, 212, 216, 255},
        {24, 24, 27, 255},
        {24, 24, 27, 255},
        {241, 241, 245, 255},
        {160, 160, 170, 255},
        {229, 229, 234, 255},
    };
    return dark ? kDark : kLight;
}

void applyPhrasingStyle(ComputedStyle& style, std::string_view tag,
                        const ComputedStyle& parent) {
    const bool smaller = tag == "small" || tag == "sub" || tag == "sup";
    if ((smaller || tag == "big") && !(style.specified & ComputedStyle::SpecifiedFontSize) &&
        parent.fontSize > 0.0f) {
        style.fontSize = parent.fontSize * (tag == "big" ? 1.2f : 0.8f);
    }
    if ((tag == "sub" || tag == "sup") &&
        !(style.specified & ComputedStyle::SpecifiedVerticalAlign)) {
        style.verticalAlign =
            tag == "sub" ? ComputedStyle::VAlignSub : ComputedStyle::VAlignSuper;
    }
}

ComputedStyle defaultStyleFor(std::string_view tag) {
    // A generated box joins the inline flow of the element it hangs off, which is
    // what `content` on `::before`/`::after` is for.
    if (tag == "::before" || tag == "::after") {
        Style s;
        s.display = Style::Inline;
        return s;
    }

    if (tag == "::backdrop") {
        // The modal veil: fixed over the viewport, under the dialog, dimming
        // the page. Author `dialog::backdrop` rules restyle it from here.
        Style s = block();
        s.position = Style::Fixed;
        s.top = Length(0.0f);
        s.left = Length(0.0f);
        s.width = Length(Length::Percent, 100.0f);
        s.height = Length(Length::Percent, 100.0f);
        s.zIndex = 999999;
        s.backgroundColor = {0, 0, 0, 128};
        return s;
    }

    if (tag == "body") {
        // The document root paints the canvas, so it is the one element whose
        // user-agent colours `color-scheme` gets to re-pick.
        Style s = block(Style::SpecifiedColor | Style::SpecifiedUACanvas);
        const UaCanvas& canvas = uaCanvas(true);
        s.backgroundColor = canvas.background;
        s.color = canvas.text;
        s.margin = {8.0f, 8.0f, 8.0f, 8.0f};
        return s;
    }

    // Disclosure widgets and dialogs lay out as plain blocks; collapsing
    // is resolved, not defaulted, and the marker triangle paints.
    if (tag == "details" || tag == "summary" || tag == "dialog" || tag == "div" || tag == "section" || tag == "article" ||
        tag == "header" || tag == "footer" || tag == "main" ||
        tag == "nav" || tag == "aside") {
        return block();
    }

    if (tag == "p") {
        Style s = block();
        s.margin = {8.0f, 0.0f, 8.0f, 0.0f};
        return s;
    }

    if (tag == "blockquote" || tag == "figure") {
        // The browser's 1em/40px block: a quoted or illustrated passage is
        // inset from both sides and separated vertically.
        Style s = block();
        s.margin = {16.0f, 40.0f, 16.0f, 40.0f};
        return s;
    }

    if (tag == "figcaption") {
        // A caption is an ordinary block immediately after its figure.
        return block();
    }

    if (tag == "dl") {
        Style s = block();
        s.margin = {16.0f, 0.0f, 16.0f, 0.0f};
        return s;
    }

    if (tag == "dt") {
        return block();
    }

    if (tag == "dd") {
        // Browsers indent the description by 40px, so the term and definition
        // read as a pair rather than two flush-left paragraphs.
        Style s = block();
        s.margin = {0.0f, 0.0f, 0.0f, 40.0f};
        return s;
    }

    if (tag == "hr") {
        // A horizontal rule is a two-pixel inset line at browser defaults: the
        // 1px top and bottom borders make the visible rule, and the block has
        // no content height of its own. Author `border: none; height: ...`
        // rules still take over cleanly.
        Style s = block();
        s.margin = {8.0f, Length(), 8.0f, Length()};
        s.borderWidth = {1.0f, 1.0f, 1.0f, 1.0f};
        s.borderStyles = {Style::BorderInset, Style::BorderInset,
                          Style::BorderInset, Style::BorderInset};
        return s;
    }

    if (tag == "fieldset") {
        // Form grouping chrome: the browser's 2px border and roomy padding.
        // The legend does not notch the border yet, so it lays out as the first
        // block inside the fieldset.
        Style s = block();
        s.margin = {0.0f, 2.0f, 0.0f, 2.0f};
        s.padding = {6.0f, 12.0f, 10.0f, 12.0f};
        s.borderWidth = {2.0f, 2.0f, 2.0f, 2.0f};
        s.borderStyles = {Style::BorderGroove, Style::BorderGroove,
                          Style::BorderGroove, Style::BorderGroove};
        return s;
    }

    if (tag == "legend") {
        Style s = block();
        s.padding = {0.0f, 2.0f, 0.0f, 2.0f};
        return s;
    }

    if (tag == "center") {
        Style s = block(Style::SpecifiedTextAlign);
        s.textAlign = Style::Center;
        return s;
    }

    if (tag == "address") {
        Style s = block(Style::SpecifiedFontStyle);
        s.fontStyle = Style::FontItalic;
        return s;
    }

    if (tag == "pre") {
        // A code block: whitespace and newlines survive, tabs stop at
        // `tab-size`, and the face is the generic monospace family (the app
        // loads one; the default face answers when the machine has none).
        Style s = block();
        s.fontFamily = "monospace";
        s.whiteSpace = Style::WrapPre;
        s.margin = {16.0f, 0.0f, 16.0f, 0.0f};
        s.specified = Style::SpecifiedFontFamily | Style::SpecifiedWhiteSpace;
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

    if (tag == "span" || tag == "label" || tag == "br") {
        // Neutral phrasing: inline, no decoration of its own.
        Style s;
        s.display = Style::Inline;
        return s;
    }

    if (tag == "em" || tag == "i" || tag == "var" || tag == "cite" || tag == "dfn") {
        // Emphasis is italic. Sizes and shifts need the parent font, so they
        // land in applyPhrasingStyle instead of here.
        Style s;
        s.display = Style::Inline;
        s.fontStyle = Style::FontItalic;
        s.specified = Style::SpecifiedFontStyle;
        return s;
    }

    if (tag == "code" || tag == "kbd" || tag == "samp") {
        // Computer text is monospaced and upright: a browser never italicizes
        // it, whatever the surrounding emphasis does.
        Style s;
        s.display = Style::Inline;
        s.fontFamily = "monospace";
        s.specified = Style::SpecifiedFontFamily;
        return s;
    }

    if (tag == "small" || tag == "sub" || tag == "sup" || tag == "big") {
        // Sized relative to the parent in applyPhrasingStyle; sub and sup also
        // shift the baseline there.
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
    // never drawn.
    if (tag == "u" || tag == "ins") {
        Style s;
        s.display = Style::Inline;
        s.textDecorationLine = Style::DecUnderline;
        s.specified = Style::SpecifiedTextDecorationLine;
        return s;
    }

    if (tag == "s" || tag == "strike" || tag == "del") {
        Style s;
        s.display = Style::Inline;
        s.textDecorationLine = Style::DecLineThrough;
        s.specified = Style::SpecifiedTextDecorationLine;
        return s;
    }

    if (tag == "kbd" || tag == "samp" || tag == "q" || tag == "abbr" ||
        tag == "time" || tag == "data" || tag == "bdi" || tag == "bdo" || tag == "wbr") {
        Style s;
        s.display = Style::Inline;
        return s;
    }

    if (tag == "mark") {
        Style s;
        s.display = Style::Inline;
        s.backgroundColor = {255, 255, 0, 255};
        s.color = {0, 0, 0, 255};
        s.specified = Style::SpecifiedColor;
        return s;
    }

    // A template's contents are inert: they never lay out, paint, or match.
    if (tag == "template") {
        Style s;
        s.display = Style::None;
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
        s.specified = Style::SpecifiedColor | Style::SpecifiedLineHeight |
                      Style::SpecifiedWhiteSpace;
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
        // Textareas resize both ways out of the box, like a browser's.
        s.resize = Style::ResizeBoth;
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

    // A gauge is a bar, not a text field: no padding, a thin border, and a
    // dark track. Layout sizes it from the font when the author does not.
    if (tag == "meter" || tag == "progress") {
        Style s;
        s.display = Style::InlineBlock;
        s.specified = Style::SpecifiedColor | Style::SpecifiedLineHeight;
        s.padding = {0.0f, 0.0f, 0.0f, 0.0f};
        s.borderWidth = {1.0f, 1.0f, 1.0f, 1.0f};
        s.borderColor = {82, 82, 91, 255};
        s.backgroundColor = {39, 39, 42, 255};
        s.color = {228, 228, 231, 255};
        s.lineHeight = 1.0f;
        return s;
    }

    // An option never lays out as a box of its own: the closed select paints
    // the selected text itself and the open popup is an overlay, so option and
    // optgroup children take no space. A datalist only feeds suggestions to
    // its inputs, so it takes no space either.
    if (tag == "option" || tag == "optgroup" || tag == "datalist") {
        Style s;
        s.display = Style::None;
        return s;
    }

    // SVG shapes never lay out as boxes of their own: the svg box paints them
    // from their attributes, so they take no space (a nested svg stays
    // unpainted).
    if (tag == "rect" || tag == "circle" || tag == "ellipse" || tag == "line" ||
        tag == "polyline" || tag == "polygon" || tag == "path" || tag == "text" ||
        tag == "g" || tag == "use" || tag == "image") {
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

    if (tag == "colgroup") {
        Style s;
        s.display = Style::TableColumnGroup;
        return s;
    }
    if (tag == "col") {
        Style s;
        s.display = Style::TableColumn;
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
    if (tag == "video" || tag == "canvas" || tag == "iframe") {
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
        tag == "picture" ||
        tag == "source" || tag == "track") {
        Style s;
        s.display = Style::InlineBlock;
        return s;
    }

    // Placeholder hints read dimmer than values: a muted gray over whatever
    // the field inherits, overridable by an author `::placeholder` rule.
    if (tag == "::placeholder") {
        Style s;
        s.display = Style::Inline;
        s.specified = Style::SpecifiedColor;
        s.color = {130, 130, 140, 255};
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
    // Date/time pickers keep the text-field chrome and edit as text; the wider
    // right padding leaves room for the calendar or clock glyph the box paints.
    if (type == "date" || type == "time" || type == "datetime-local" ||
        type == "month" || type == "week") {
        style.padding.right = Length(24.0f);
        return;
    }
    // A color swatch edits as hex text; the padding leaves room for the swatch
    // the box paints from the value.
    if (type == "color") {
        style.padding.right = Length(32.0f);
        return;
    }
    // A file control shows a Browse button on the left of the file name; the
    // padding leaves room for the button the box paints.
    if (type == "file") {
        style.padding.left = Length(80.0f);
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
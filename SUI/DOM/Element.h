// DOM node. Owns its children; a Document owns the root.
//
// Text is stored as child nodes with the reserved tag "#text" so that mixed
// content (text, inline elements, <br>) keeps its document order, which layout
// and script-driven DOM mutation both depend on.
#pragma once

#include "Core/Object/Object.h"
#include "CSS/Style.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Document;
class Element;

// Reserved tag for text nodes.
inline constexpr std::string_view kTextTag = "#text";

// The live inline declaration for an element, mirroring `element.style` in the
// DOM. The `style` attribute stays the single source of truth, so a write here
// round-trips through the same cascade path the parser produces and the value is
// still visible in the markup.
class InlineStyle {
public:
    explicit InlineStyle(Element& owner) noexcept;

    void setProperty(std::string property, std::string value, bool important = false);
    std::string getPropertyValue(std::string_view property) const;
    bool hasProperty(std::string_view property) const;

    // Drops the property and returns whatever it was set to.
    std::string removeProperty(std::string_view property);

private:
    Element& owner_;
};

class Element final : public Object {
public:
    Element(std::string tag, Document* document);
    ~Element() override;

    // Builds a text node holding `data`.
    static std::unique_ptr<Element> createText(std::string data, Document* document);

    bool isTextNode() const noexcept { return tag_ == kTextTag; }

    std::string_view tag() const noexcept { return tag_; }
    Document* document() const noexcept { return document_; }

    Element* parent() const noexcept { return parent_; }

    void appendChild(std::unique_ptr<Element> child);
    void insertChild(std::unique_ptr<Element> child, std::size_t index);
    std::unique_ptr<Element> removeChild(std::size_t index);
    const std::vector<std::unique_ptr<Element>>& children() const noexcept {
        return children_;
    }

    // Hands ownership of the children to the caller; used by the parser to lift
    // the <sui> wrapper's children into the document root.
    std::vector<std::unique_ptr<Element>> releaseChildren();

    bool hasAttribute(std::string_view key) const;
    std::string_view attribute(std::string_view key) const;
    void setAttribute(std::string key, std::string value);
    void removeAttribute(std::string_view key);

    std::string_view id() const;
    std::string_view classList() const;

    // classList manipulation, spelled the way a web developer expects.
    bool hasClass(std::string_view name) const;
    void addClass(std::string_view name);
    void removeClass(std::string_view name);
    void toggleClass(std::string_view name, bool enabled);

    // Text belonging to this node. For a text node this is its data; for an
    // element it is empty, because text lives in child nodes.
    std::string_view text() const noexcept { return text_; }
    void setText(std::string text);

    // Concatenated text of this node and all descendants.
    std::string textContent() const;

    // Replaces all children with a single text node (innerText = "...").
    void setTextContent(std::string text);

    // Web-named aliases used by the script API.
    std::string innerText() const { return textContent(); }
    void setInnerText(std::string text) { setTextContent(std::move(text)); }

    InlineStyle style() noexcept { return InlineStyle(*this); }

    // <input> value, stored as an attribute.
    std::string value() const;
    void setValue(std::string text);

    // A <select>'s options in document order, recursing through <optgroup>.
    // Options never lay out as boxes of their own; the closed select paints the
    // selected text itself and the open popup is an overlay.
    std::vector<Element*> selectOptions() const;
    // The option carrying `selected`, else the first option, else null.
    Element* selectedOption() const;
    // An option's display text: its text content stripped of surrounding
    // whitespace with internal runs collapsed to one space, the way a browser
    // reads it (so indented markup still measures and shows cleanly).
    std::string optionLabel() const;
    // A file input shows only the file name, not the path: the text after the
    // last slash or backslash, or the whole value when neither appears.
    std::string fileName() const;
    // Rows visible without scrolling: 0 for a dropdown (no `multiple` and
    // `size` of 1 or less), else `size`, or 4 for a bare `multiple`.
    int listboxRows() const;
    // A <meter> or <progress> fill fraction from value/min/max, clamped to
    // 0..1. An indeterminate progress (no `value`) reports 0; test
    // `gaugeIndeterminate()` for the animated case.
    float gaugeFraction() const;
    // A <dialog> opens non-modally with show() (no-op when already open) and
    // modally with showModal() (no-op when already open, demoting any other
    // modal dialog). close() shuts either and fires `onclose` when there is
    // one. Modal dialogs paint above the page, centered, with a backdrop.
    void show();
    void showModal();
    void close();
    bool dialogOpen() const;
    bool dialogModal() const;
    // A valueless <progress> shows indeterminate instead of a fraction.
    bool gaugeIndeterminate() const;
    // A <meter> colour zone from low/high/optimum: 0 beside the optimum, 1
    // inside the low/high band but off the optimum side, 2 outside the band.
    // Only meaningful for meters.
    int gaugeZone() const;
    // Constraint validation: whether this control is a candidate, its
    // per-constraint state, and the script-facing API. Custom errors come
    // from setCustomValidity; reportValidity focuses the first failure.
    struct ValidityState {
        bool valueMissing = false;
        bool typeMismatch = false;
        bool patternMismatch = false;
        bool tooShort = false;
        bool rangeUnderflow = false;
        bool rangeOverflow = false;
        bool customError = false;
        bool valid() const noexcept {
            return !valueMissing && !typeMismatch && !patternMismatch && !tooShort &&
                   !rangeUnderflow && !rangeOverflow && !customError;
        }
    };
    bool willValidate() const;
    ValidityState validity() const;
    bool checkValidity() const;
    bool reportValidity();
    std::string validationMessage() const;
    void setCustomValidity(std::string message);
    // The select this option/optgroup nests in: the parent, or the grandparent
    // through an <optgroup>. Null for anything else.
    Element* owningSelect() const;
    // The datalist a text-like input names with `list`, or null when the
    // attribute is missing, empty, or names nothing (or nothing list-like).
    Element* datalistFor() const;

    // Tags outside the visual set (meta, title, script, link, #text) are
    // retained so they stay reachable by the linker, but layout and paint skip
    // them.
    bool isVisual() const noexcept { return visual_; }

    // A box the engine generated from a `::before`/`::after` rule rather than from
    // markup. Its tag is the pseudo-element name, so it takes part in layout and
    // paint but is never reachable from script or from a plain type selector.
    bool isPseudoElement() const noexcept { return tag_.rfind("::", 0) == 0; }

    // Whether the element's own children give it visible content: text with a
    // non-space character, or a real child box. Generated boxes count only when
    // they carry such text. `empty-cells` reads this to find blank cells.
    bool hasVisibleContent() const;
    void setVisual(bool visual) noexcept { visual_ = visual; }

    // Depth-first walk over this node and its descendants.
    void visit(const std::function<void(Element&)>& fn);

    // A collapsed table border edge is a used value, not the element's CSS:
    // adjacent cells share one winning edge and the layout pass overwrites the
    // losing side. Paint reads this instead of resolving the cell again.
    struct LayoutBorderSide {
        float width = 0.0f;
        SDL_Color color = {0, 0, 0, 0};
        ComputedStyle::BorderStyle style = ComputedStyle::BorderStyleNone;
    };
    struct LayoutBorder {
        LayoutBorderSide top;
        LayoutBorderSide right;
        LayoutBorderSide bottom;
        LayoutBorderSide left;
    };
    void setLayoutBorder(const LayoutBorder& border) noexcept {
        layoutBorder_ = border;
        hasLayoutBorder_ = true;
    }
    void clearLayoutBorder() noexcept { hasLayoutBorder_ = false; }
    bool hasLayoutBorder() const noexcept { return hasLayoutBorder_; }
    const LayoutBorder& layoutBorder() const noexcept { return layoutBorder_; }

    // Layout data (owned by element for now). X/Y are the border-box origin in
    // window space; Width/Height are border-box dimensions.
    void setLayoutX(float x) noexcept { layoutX_ = x; }
    void setLayoutY(float y) noexcept { layoutY_ = y; }
    void setLayoutWidth(float w) noexcept { layoutW_ = w; }
    void setLayoutHeight(float h) noexcept { layoutH_ = h; }
    float layoutX() const noexcept { return layoutX_; }
    float layoutY() const noexcept { return layoutY_; }
    float layoutWidth() const noexcept { return layoutW_; }
    float layoutHeight() const noexcept { return layoutH_; }

    // Used multi-column geometry from the latest layout pass: how many column
    // boxes the container produced, so paint can draw `column-rule` between
    // them. Zero on everything that is not a multi-column container.
    void setLayoutColumnCount(int count) noexcept { layoutColumnCount_ = count; }
    int layoutColumnCount() const noexcept { return layoutColumnCount_; }

    // Drops the text runs and geometry of this node and every descendant, used
    // when `content-visibility: hidden` skips a subtree so stale runs from a
    // previous layout pass cannot paint at old positions.
    void clearLayoutSubtree();

    // The padding box, which is what `overflow` clips to, and the content box, which is
    // what `background-clip: content-box` is relative to. Layout records both because
    // padding percentages were already resolved against the right containing block and
    // re-resolving them here would give a different answer.
    void setPaddingX(float x) noexcept { paddingX_ = x; }
    void setPaddingY(float y) noexcept { paddingY_ = y; }
    void setPaddingWidth(float w) noexcept { paddingW_ = w; }
    void setPaddingHeight(float h) noexcept { paddingH_ = h; }
    float paddingX() const noexcept { return paddingX_; }
    float paddingY() const noexcept { return paddingY_; }
    float paddingWidth() const noexcept { return paddingW_; }
    float paddingHeight() const noexcept { return paddingH_; }

    void setContentX(float x) noexcept { contentX_ = x; }
    void setContentY(float y) noexcept { contentY_ = y; }
    void setContentWidth(float w) noexcept { contentW_ = w; }
    void setContentHeight(float h) noexcept { contentH_ = h; }
    float contentX() const noexcept { return contentX_; }
    float contentY() const noexcept { return contentY_; }
    float contentWidth() const noexcept { return contentW_; }
    float contentHeight() const noexcept { return contentH_; }

    // Scrolling. `client` is the padding box, `scroll` the furthest edge of any
    // descendant, both in window pixels; the layout pass refreshes them and clamps
    // the offsets. Scripts and wheel events write `scrollTop`/`scrollLeft`, which
    // clamp as far as the current extents know; an axis that does not clip cannot
    // hold an offset at all, so writing one is a no-op there.
    void setScrollExtents(float clientW, float clientH, float scrollW, float scrollH,
                          bool scrollsX, bool scrollsY) noexcept;
    float scrollLeft() const noexcept;
    void setScrollLeft(float v);
    float scrollTop() const noexcept;
    void setScrollTop(float v);
    float clientWidth() const noexcept { return clientW_; }
    float clientHeight() const noexcept { return clientH_; }
    float scrollWidth() const noexcept { return scrollW_; }
    float scrollHeight() const noexcept { return scrollH_; }

    // Text the layout pass placed inside this element's content box. The
    // painter reads these instead of re-running the formatter.
    struct TextRun {
        std::string text;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float baseline = 0.0f;
        float fontSize = 0.0f;
        std::uint8_t r = 255, g = 255, b = 255, a = 255;
        // Synthetic emphasis: bold is a one-pixel smear and italic is a shear
        // applied while the glyph rasterises, on top of whatever face the
        // family resolved to.
        bool bold = false;
        bool italic = false;
        // The first `font-family` in effect where the run was laid out, so the
        // painter rasters with the face it measured with.
        std::string fontFamily;
        // Clockwise rotation in degrees for vertical writing modes. Zero for
        // ordinary horizontal text.
        float rotation = 0.0f;
    };
    const std::vector<TextRun>& textRuns() const noexcept { return textRuns_; }
    std::vector<TextRun>& textRuns() noexcept { return textRuns_; }
    void addTextRun(TextRun run) { textRuns_.push_back(std::move(run)); }
    void clearTextRuns() { textRuns_.clear(); }

private:
    void collectText(std::string& out) const;
    void collectOptions(std::vector<Element*>& out) const;
    void touch();

    std::string tag_;
    Document* document_ = nullptr;
    Element* parent_ = nullptr;
    std::vector<std::unique_ptr<Element>> children_;
    std::map<std::string, std::string, std::less<>> attributes_;
    std::string text_;
    bool visual_ = true;
    // A custom validation error from setCustomValidity; empty means none.
    std::string customValidity_;

    float layoutX_ = 0.0f;
    float layoutY_ = 0.0f;
    float layoutW_ = 0.0f;
    float layoutH_ = 0.0f;
    int layoutColumnCount_ = 0;
    LayoutBorder layoutBorder_;
    bool hasLayoutBorder_ = false;
    float paddingX_ = 0.0f;
    float paddingY_ = 0.0f;
    float paddingW_ = 0.0f;
    float paddingH_ = 0.0f;
    float contentX_ = 0.0f;
    float contentY_ = 0.0f;
    float contentW_ = 0.0f;
    float contentH_ = 0.0f;
    float scrollLeft_ = 0.0f;
    float scrollTop_ = 0.0f;
    bool scrollsX_ = false;
    bool scrollsY_ = false;
    // A <dialog> showing modally: painted above the page, centered, with a
    // backdrop. Only the topmost dialog holds this; opening another demotes
    // the first back to a plain open dialog.
    bool modalShown_ = false;
    float clientW_ = 0.0f;
    float clientH_ = 0.0f;
    float scrollW_ = 0.0f;
    float scrollH_ = 0.0f;
    std::vector<TextRun> textRuns_;
};

}  // namespace sui

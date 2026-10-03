// DOM node. Owns its children; a Document owns the root.
//
// Text is stored as child nodes with the reserved tag "#text" so that mixed
// content (text, inline elements, <br>) keeps its document order, which layout
// and script-driven DOM mutation both depend on.
#pragma once

#include "Core/Object/Object.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Document;

// Reserved tag for text nodes.
inline constexpr std::string_view kTextTag = "#text";

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

    // Text belonging to this node. For a text node this is its data; for an
    // element it is empty, because text lives in child nodes.
    std::string_view text() const noexcept { return text_; }
    void setText(std::string text);

    // Concatenated text of this node and all descendants.
    std::string textContent() const;

    // Replaces all children with a single text node (innerText = "...").
    void setTextContent(std::string text);

    // Tags outside the visual set (meta, title, script, link, #text) are
    // retained so they stay reachable by the linker, but layout and paint skip
    // them.
    bool isVisual() const noexcept { return visual_; }
    void setVisual(bool visual) noexcept { visual_ = visual; }

    // Depth-first walk over this node and its descendants.
    void visit(const std::function<void(Element&)>& fn);

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
    };
    const std::vector<TextRun>& textRuns() const noexcept { return textRuns_; }
    void addTextRun(TextRun run) { textRuns_.push_back(std::move(run)); }
    void clearTextRuns() { textRuns_.clear(); }

private:
    void collectText(std::string& out) const;

    std::string tag_;
    Document* document_ = nullptr;
    Element* parent_ = nullptr;
    std::vector<std::unique_ptr<Element>> children_;
    std::map<std::string, std::string, std::less<>> attributes_;
    std::string text_;
    bool visual_ = true;

    float layoutX_ = 0.0f;
    float layoutY_ = 0.0f;
    float layoutW_ = 0.0f;
    float layoutH_ = 0.0f;
    std::vector<TextRun> textRuns_;
};

}  // namespace sui

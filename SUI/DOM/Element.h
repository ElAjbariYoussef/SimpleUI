// DOM node. Owns its children; a Document owns the root.
#pragma once

#include "Core/Object/Object.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Document;

class Element final : public Object {
public:
    Element(std::string tag, Document* document);

    std::string_view tag() const noexcept { return tag_; }
    Document* document() const noexcept { return document_; }

    Element* parent() const noexcept { return parent_; }

    void appendChild(std::unique_ptr<Element> child);
    const std::vector<std::unique_ptr<Element>>& children() const noexcept {
        return children_;
    }

    // Hands ownership of the children to the caller; used by the parser to lift
    // the <sui> wrapper's children into the document root.
    std::vector<std::unique_ptr<Element>> releaseChildren();

    bool hasAttribute(std::string_view key) const;
    std::string_view attribute(std::string_view key) const;
    void setAttribute(std::string key, std::string value);

    std::string_view id() const;
    std::string_view classList() const;

    // Direct text held by this node, not including descendant text.
    std::string_view text() const noexcept { return text_; }
    void setText(std::string text);

    // Concatenated text of this node and all descendants.
    std::string textContent() const;

    // Tags outside the visual set (meta, title, script, link...) are retained so
    // they stay reachable by the linker, but layout and paint skip them.
    bool isVisual() const noexcept { return visual_; }
    void setVisual(bool visual) noexcept { visual_ = visual; }

    // Depth-first walk over this node and its descendants.
    void visit(const std::function<void(Element&)>& fn);

private:
    void collectText(std::string& out) const;

    std::string tag_;
    Document* document_ = nullptr;
    Element* parent_ = nullptr;
    std::vector<std::unique_ptr<Element>> children_;
    std::map<std::string, std::string, std::less<>> attributes_;
    std::string text_;
    bool visual_ = true;
};

}  // namespace sui
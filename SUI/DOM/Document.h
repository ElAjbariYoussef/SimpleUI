// Owns the element tree and provides the web-style lookup surface that scripts
// and inline handlers will use.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Element;

struct StylesheetLink {
    std::string href;
};

// A <script> reference. Where it sits relative to <body> decides when it runs,
// exactly as it does in a web page.
struct ScriptReference {
    std::string href;
    bool afterBody = false;
};

class Document {
public:
    Document();

    Element* root() const noexcept { return root_.get(); }
    void setRoot(std::unique_ptr<Element> root);

    // document.getElementById
    Element* getElementById(std::string_view id);

    // First element with a matching tag, in document order.
    Element* querySelector(std::string_view tag);

    // The <window> element declared in <head>, if any.
    Element* windowElement();

    // <link rel="stylesheet"> hrefs in document order.
    const std::vector<StylesheetLink>& stylesheets() const noexcept {
        return stylesheets_;
    }

    // <script href="..."> entries in document order, each tagged with the phase
    // its position in the markup implies.
    const std::vector<ScriptReference>& scripts() const noexcept {
        return scripts_;
    }

    // Walks the tree once, collecting the link and script references.
    void index();

    // Bumped by every mutation, so the layout/paint passes can skip work when a
    // frame draws exactly what the previous one drew.
    std::uint64_t mutationCount() const noexcept { return mutations_; }
    void noteMutation() noexcept { ++mutations_; }

    // The element that last had focus, so focus-driven restyles can be diffed.
    Element* focused() const noexcept { return focused_; }
    void setFocused(Element* element) noexcept { focused_ = element; }

private:
    std::unique_ptr<Element> root_;
    std::vector<StylesheetLink> stylesheets_;
    std::vector<ScriptReference> scripts_;
    std::uint64_t mutations_ = 0;
    Element* focused_ = nullptr;
};

}  // namespace sui
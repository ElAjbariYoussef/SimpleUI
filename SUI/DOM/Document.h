// Owns the element tree and provides the web-style lookup surface that scripts
// and inline handlers will use.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Element;

struct StylesheetLink {
    std::string href;
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

    // <script href="..."> entries in document order.
    const std::vector<std::string>& scriptHrefs() const noexcept {
        return scriptHrefs_;
    }

    // Walks the tree once, collecting the link and script references.
    void index();

private:
    std::unique_ptr<Element> root_;
    std::vector<StylesheetLink> stylesheets_;
    std::vector<std::string> scriptHrefs_;
};

}  // namespace sui
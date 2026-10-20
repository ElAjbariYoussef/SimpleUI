// Owns the element tree and provides the web-style lookup surface that scripts
// and inline handlers will use.
#pragma once

#include "SUI/DOM/MutationObserver.h"

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

    // Delivers every queued MutationObserver batch, including records queued by
    // a callback during delivery. Returns true when at least one callback ran,
    // so the frame loop can treat the frame as stale. Application calls this
    // once per pumped frame; tests can call it directly.
    bool deliverMutationObservers();

    // The element that last had focus, so focus-driven restyles can be diffed.
    Element* focused() const noexcept { return focused_; }
    void setFocused(Element* element) noexcept {
        focused_ = element;
        ++mutations_;
    }

private:
    friend class Element;
    friend class MutationObserver;

    std::shared_ptr<MutationObserverState> newMutationObserver(MutationCallback callback);
    void attachMutationObserver(const std::shared_ptr<MutationObserverState>& observer,
                                Element* target, MutationObserverInit options);
    void detachMutationObserver(const std::shared_ptr<MutationObserverState>& observer);
    std::vector<MutationRecord> takeMutationRecords(
        const std::shared_ptr<MutationObserverState>& observer);
    void recordChildList(Element* target, const std::vector<Element*>& added,
                         const std::vector<Element*>& removed,
                         Element* previousSibling, Element* nextSibling);
    void recordAttribute(Element* target, std::string_view name, std::string oldValue,
                         bool hadOldValue);
    void recordCharacterData(Element* target, std::string oldValue);
    void queueMutation(MutationRecord record);
    bool observerMatches(const MutationObserverRegistration& registration,
                         const MutationRecord& record) const;
    static bool isWithin(const Element* node, const Element* root);

    std::unique_ptr<Element> root_;
    std::vector<StylesheetLink> stylesheets_;
    std::vector<ScriptReference> scripts_;
    std::uint64_t mutations_ = 0;
    Element* focused_ = nullptr;
    std::vector<std::weak_ptr<MutationObserverState>> mutationObservers_;
};

}  // namespace sui
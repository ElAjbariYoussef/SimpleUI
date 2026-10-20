// DOM mutation notification: a script-facing MutationObserver over the
// engine's element tree. Records are queued per observer and delivered when
// the application pumps a frame, or by an embedding test that calls
// Document::deliverMutationObservers. The callback is a plain function pointer
// for the same reason timers are: scripts are statically linked C++.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sui {

class Document;
class Element;

enum class MutationType {
    ChildList,
    Attributes,
    CharacterData,
};

struct MutationRecord {
    MutationType type = MutationType::ChildList;
    Element* target = nullptr;
    std::string attributeName;
    std::string oldValue;
    bool hadOldValue = false;
    std::vector<Element*> addedNodes;
    std::vector<Element*> removedNodes;
    Element* previousSibling = nullptr;
    Element* nextSibling = nullptr;
};

// The web option bag, flattened into C++ so a script can write
// `observer.observe(node, {true, false, false, true});`.
struct MutationObserverInit {
    bool childList = false;
    bool attributes = false;
    bool characterData = false;
    bool subtree = false;
    bool attributeOldValue = false;
    bool characterDataOldValue = false;
    std::vector<std::string> attributeFilter;
};

using MutationCallback = void (*)(const std::vector<MutationRecord>&);

// One observer shared by the script handle and the document's weak registry.
// The document owns the registrations; the handle keeps the state alive, so a
// document can safely drop expired observers.
struct MutationObserverRegistration {
    Element* target = nullptr;
    MutationObserverInit options;
};

struct MutationObserverState {
    MutationCallback callback = nullptr;
    std::vector<MutationObserverRegistration> registrations;
    std::vector<MutationRecord> queue;
    bool connected = true;
};

class MutationObserver {
public:
    explicit MutationObserver(MutationCallback callback = nullptr);
    ~MutationObserver();

    MutationObserver(const MutationObserver&) = default;
    MutationObserver& operator=(const MutationObserver&) = default;
    MutationObserver(MutationObserver&&) noexcept = default;
    MutationObserver& operator=(MutationObserver&&) noexcept = default;

    // Observes `target`. Calling observe again for the same target replaces
    // that target's options, matching the DOM's one-registration-per-node rule.
    void observe(Element* target, MutationObserverInit options = {});

    void disconnect();
    std::vector<MutationRecord> takeRecords();

    bool valid() const noexcept { return state_ && state_->connected; }

private:
    Document* document_ = nullptr;
    std::shared_ptr<MutationObserverState> state_;
};

}  // namespace sui

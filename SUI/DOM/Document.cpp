#include "SUI/DOM/Document.h"

#include "SUI/DOM/Element.h"

#include <algorithm>
#include <utility>

namespace sui {

Document::Document() : root_(std::make_unique<Element>("document", this)) {}

void Document::setRoot(std::unique_ptr<Element> root) {
    // A replacement root is a new document tree; observers belong to the tree
    // they were watching rather than surviving into an unrelated one.
    mutationObservers_.clear();
    root_ = std::move(root);
    index();
}

Element* Document::getElementById(std::string_view id) {
    if (!root_ || id.empty()) {
        return nullptr;
    }
    Element* found = nullptr;
    root_->visit([&](Element& el) {
        if (!found && el.id() == id) {
            found = &el;
        }
    });
    return found;
}

Element* Document::querySelector(std::string_view tag) {
    if (!root_) {
        return nullptr;
    }
    Element* found = nullptr;
    root_->visit([&](Element& el) {
        if (!found && el.tag() == tag) {
            found = &el;
        }
    });
    return found;
}

Element* Document::windowElement() {
    if (!root_) {
        return nullptr;
    }
    Element* found = nullptr;
    root_->visit([&](Element& el) {
        if (!found && el.tag() == "window") {
            found = &el;
        }
    });
    return found;
}

void Document::index() {
    stylesheets_.clear();
    scripts_.clear();
    if (!root_) {
        return;
    }

    // <body> is the dividing line: a <script> before it runs before the first
    // paint, one after it runs once the frame is up.
    bool seenBody = false;
    root_->visit([&](Element& el) {
        if (el.tag() == "body") {
            seenBody = true;
        }
        if (el.tag() == "link" && el.attribute("rel") == "stylesheet") {
            stylesheets_.push_back(StylesheetLink{std::string(el.attribute("href"))});
        } else if (el.tag() == "script" && !el.attribute("href").empty()) {
            scripts_.push_back(ScriptReference{std::string(el.attribute("href")), seenBody});
        }
    });
}


std::shared_ptr<MutationObserverState> Document::newMutationObserver(
    MutationCallback callback) {
    auto observer = std::make_shared<MutationObserverState>();
    observer->callback = callback;
    mutationObservers_.push_back(observer);
    return observer;
}

void Document::attachMutationObserver(
    const std::shared_ptr<MutationObserverState>& observer, Element* target,
    MutationObserverInit options) {
    if (!observer || !target || !observer->connected) {
        return;
    }
    for (MutationObserverRegistration& registration : observer->registrations) {
        if (registration.target == target) {
            registration.options = std::move(options);
            return;
        }
    }
    observer->registrations.push_back(
        MutationObserverRegistration{target, std::move(options)});
    // A handle that was default-copied has the same state; the weak registry
    // needs one entry, not one per registration.
    for (const auto& existing : mutationObservers_) {
        if (existing.lock() == observer) {
            return;
        }
    }
    mutationObservers_.push_back(observer);
}

void Document::detachMutationObserver(
    const std::shared_ptr<MutationObserverState>& observer) {
    if (!observer) {
        return;
    }
    observer->connected = false;
    observer->registrations.clear();
    observer->queue.clear();
    mutationObservers_.erase(
        std::remove_if(mutationObservers_.begin(), mutationObservers_.end(),
                       [&](const std::weak_ptr<MutationObserverState>& weak) {
                           const auto state = weak.lock();
                           return !state || state == observer;
                       }),
        mutationObservers_.end());
}

std::vector<MutationRecord> Document::takeMutationRecords(
    const std::shared_ptr<MutationObserverState>& observer) {
    if (!observer || !observer->connected) {
        return {};
    }
    std::vector<MutationRecord> records = std::move(observer->queue);
    observer->queue.clear();
    return records;
}

bool Document::isWithin(const Element* node, const Element* root) {
    for (const Element* current = node; current; current = current->parent()) {
        if (current == root) {
            return true;
        }
    }
    return false;
}

bool Document::observerMatches(const MutationObserverRegistration& registration,
                               const MutationRecord& record) const {
    if (!registration.target || !record.target) {
        return false;
    }
    if (record.target != registration.target &&
        !(registration.options.subtree &&
          isWithin(record.target, registration.target))) {
        return false;
    }
    switch (record.type) {
        case MutationType::ChildList:
            return registration.options.childList;
        case MutationType::Attributes:
            if (!registration.options.attributes) {
                return false;
            }
            if (!registration.options.attributeFilter.empty() &&
                std::find(registration.options.attributeFilter.begin(),
                          registration.options.attributeFilter.end(),
                          record.attributeName) ==
                    registration.options.attributeFilter.end()) {
                return false;
            }
            return true;
        case MutationType::CharacterData:
            return registration.options.characterData;
    }
    return false;
}

void Document::queueMutation(MutationRecord record) {
    if (mutationObservers_.empty()) {
        return;
    }
    for (auto it = mutationObservers_.begin(); it != mutationObservers_.end();) {
        const auto observer = it->lock();
        if (!observer) {
            it = mutationObservers_.erase(it);
            continue;
        }
        ++it;
        if (!observer->connected) {
            continue;
        }
        for (const MutationObserverRegistration& registration : observer->registrations) {
            if (!observerMatches(registration, record)) {
                continue;
            }
            MutationRecord copy = record;
            if (copy.type == MutationType::Attributes &&
                !registration.options.attributeOldValue) {
                copy.oldValue.clear();
                copy.hadOldValue = false;
            }
            if (copy.type == MutationType::CharacterData &&
                !registration.options.characterDataOldValue) {
                copy.oldValue.clear();
                copy.hadOldValue = false;
            }
            observer->queue.push_back(std::move(copy));
            // One observer sees one record even when two of its registrations
            // cover the same node through a subtree overlap.
            break;
        }
    }
}

void Document::recordChildList(Element* target, const std::vector<Element*>& added,
                               const std::vector<Element*>& removed,
                               Element* previousSibling, Element* nextSibling) {
    MutationRecord record;
    record.type = MutationType::ChildList;
    record.target = target;
    record.addedNodes = added;
    record.removedNodes = removed;
    record.previousSibling = previousSibling;
    record.nextSibling = nextSibling;
    queueMutation(std::move(record));
}

void Document::recordAttribute(Element* target, std::string_view name,
                               std::string oldValue, bool hadOldValue) {
    MutationRecord record;
    record.type = MutationType::Attributes;
    record.target = target;
    record.attributeName = std::string(name);
    record.oldValue = std::move(oldValue);
    record.hadOldValue = hadOldValue;
    queueMutation(std::move(record));
}

void Document::recordCharacterData(Element* target, std::string oldValue) {
    MutationRecord record;
    record.type = MutationType::CharacterData;
    record.target = target;
    record.oldValue = std::move(oldValue);
    record.hadOldValue = true;
    queueMutation(std::move(record));
}

bool Document::deliverMutationObservers() {
    bool delivered = false;
    // A callback is allowed to mutate the tree and queue more records; keep
    // draining until quiet. The pass cap keeps a pathological observer that
    // mutates on every delivery from hanging the frame.
    for (int pass = 0; pass < 32; ++pass) {
        std::vector<std::pair<std::shared_ptr<MutationObserverState>,
                              std::vector<MutationRecord>>>
            batches;
        for (auto it = mutationObservers_.begin(); it != mutationObservers_.end();) {
            const auto observer = it->lock();
            if (!observer) {
                it = mutationObservers_.erase(it);
                continue;
            }
            ++it;
            if (!observer->connected || observer->queue.empty()) {
                continue;
            }
            batches.emplace_back(observer, std::move(observer->queue));
            observer->queue.clear();
        }
        if (batches.empty()) {
            break;
        }
        delivered = true;
        for (auto& [observer, records] : batches) {
            if (observer->connected && observer->callback) {
                observer->callback(records);
            }
        }
    }
    return delivered;
}

}  // namespace sui

#include "SUI/DOM/MutationObserver.h"

#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"

#include <utility>

namespace sui {

MutationObserver::MutationObserver(MutationCallback callback)
    : state_(std::make_shared<MutationObserverState>()) {
    state_->callback = callback;
}

MutationObserver::~MutationObserver() = default;

void MutationObserver::observe(Element* target, MutationObserverInit options) {
    if (!target) {
        return;
    }
    Document* document = target->document();
    if (!document) {
        return;
    }
    // A handle belongs to one document, because delivery and weak registration
    // live there. Cross-document observation would need separate state per
    // document, which the web API's single observer does not expose.
    if (document_ && document_ != document) {
        return;
    }
    document_ = document;
    if (!state_) {
        state_ = document->newMutationObserver(nullptr);
    }
    // A disconnected observer may be re-armed, as the web API allows.
    state_->connected = true;
    document->attachMutationObserver(state_, target, std::move(options));
}

void MutationObserver::disconnect() {
    if (state_ && document_) {
        document_->detachMutationObserver(state_);
    }
    if (state_) {
        state_->connected = false;
        state_->registrations.clear();
        state_->queue.clear();
    }
}

std::vector<MutationRecord> MutationObserver::takeRecords() {
    if (!state_ || !document_) {
        return {};
    }
    return document_->takeMutationRecords(state_);
}

}  // namespace sui

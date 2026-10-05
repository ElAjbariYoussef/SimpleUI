#include "CSS/PseudoElements.h"

#include "CSS/StyleResolver.h"
#include "SUI/DOM/Element.h"

#include <vector>

namespace sui {
namespace {

// Every generated box currently in the tree. They are collected before any are
// removed, because removing one shifts the indices the walk is holding.
void collectGenerated(Element& element, std::vector<Element*>& out) {
    for (const auto& child : element.children()) {
        if (!child || child->isTextNode()) {
            continue;
        }
        if (child->isPseudoElement()) {
            out.push_back(child.get());
            continue;
        }
        collectGenerated(*child, out);
    }
}

// `::before` goes in front of the element's own content and `::after` behind it,
// so the two are added in that order and each picks its slot from the current child
// count.
void addBox(Element& element, const StyleResolver& resolver, const char* tag,
            bool first) {
    Document* document = element.document();
    const std::size_t index = first ? 0 : element.children().size();
    element.insertChild(std::make_unique<Element>(std::string(tag), document), index);

    // The box has to be in the tree before it can be resolved: `content` comes from
    // the rule that matched, and every other declaration is inherited from the
    // element the box hangs off.
    Element* box = element.children()[index].get();
    const std::string content = resolver.resolve(*box).content;
    if (content.empty()) {
        element.removeChild(index);
        return;
    }
    box->appendChild(Element::createText(content, document));
}

void build(Element& element, const StyleResolver& resolver) {
    for (const auto& child : element.children()) {
        if (!child || child->isTextNode() || child->isPseudoElement()) {
            continue;
        }
        addBox(*child, resolver, "::before", true);
        addBox(*child, resolver, "::after", false);
        // Recursing after the boxes exist is safe: they are skipped above, and
        // `::before::before` is not a thing CSS can express.
        build(*child, resolver);
    }
}

}  // namespace

void generatePseudoElements(Document& document, const StyleResolver& resolver) {
    if (!document.root()) {
        return;
    }
    std::vector<Element*> stale;
    collectGenerated(*document.root(), stale);
    for (Element* box : stale) {
        Element* parent = box->parent();
        if (!parent) {
            continue;
        }
        for (std::size_t i = 0; i < parent->children().size(); ++i) {
            if (parent->children()[i].get() == box) {
                parent->removeChild(i);
                break;
            }
        }
    }
    build(*document.root(), resolver);
}

}  // namespace sui
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
    const ComputedStyle s = resolver.resolve(*box);
    if (!s.hasContent) {
        element.removeChild(index);
        return;
    }
    std::string text;
    const std::string& raw = s.content;
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '\x01') {
            const std::size_t end = raw.find('\x02', i + 1);
            if (end != std::string::npos) {
                const std::string attrName = raw.substr(i + 1, end - (i + 1));
                text.append(element.attribute(attrName));
                i = end + 1;
                continue;
            }
        } else if (raw[i] == '\x03') {
            const std::size_t end = raw.find('\x04', i + 1);
            if (end != std::string::npos) {
                i = end + 1;
                continue;
            }
        } else if (raw[i] == '\x05') {
            const std::size_t end = raw.find('\x06', i + 1);
            if (end != std::string::npos) {
                i = end + 1;
                continue;
            }
        }
        text.push_back(raw[i]);
        ++i;
    }
    box->appendChild(Element::createText(text, document));
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
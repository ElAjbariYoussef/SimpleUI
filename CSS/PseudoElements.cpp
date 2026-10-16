#include "CSS/PseudoElements.h"

#include "CSS/Style.h"
#include "CSS/StyleResolver.h"
#include "SUI/DOM/Element.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sui {
namespace {

// --- CSS counter engine --------------------------------------------------
//
// Counters are resolved for the real elements only, in document order, while
// the generated boxes are built. Each element opens a scope holding the
// counters its own `counter-reset` created; lookups walk the open scopes from
// the innermost out, so an element sees its own counters, then each ancestor's.
//
// A counter is written into its *parent's* scope rather than the element's own:
// CSS scopes a counter to the element, its descendants, and its following
// siblings, and the parent scope is exactly the set of later siblings still
// being walked. Resets that nest therefore land in progressively deeper scopes,
// which is what lets `counters()` list them outermost-first.

struct CounterScope {
    std::vector<std::pair<std::string, int>> values;
};

struct CounterStack {
    std::vector<CounterScope> scopes;
};

int* findCounter(CounterStack& stack, std::string_view name) {
    for (auto scope = stack.scopes.rbegin(); scope != stack.scopes.rend(); ++scope) {
        for (auto& entry : scope->values) {
            if (entry.first == name) {
                return &entry.second;
            }
        }
    }
    return nullptr;
}

int counterValue(CounterStack& stack, std::string_view name) {
    const int* value = findCounter(stack, name);
    return value ? *value : 0;
}

std::string countersValue(CounterStack& stack, std::string_view name,
                          std::string_view separator) {
    std::string out;
    bool first = true;
    for (const CounterScope& scope : stack.scopes) {  // outermost first
        for (const auto& entry : scope.values) {
            if (entry.first != name) {
                continue;
            }
            if (!first) {
                out.append(separator);
            }
            out += std::to_string(entry.second);
            first = false;
        }
    }
    return out;
}

// Applies one element's `counter-reset`, then `counter-increment`, then
// `counter-set`, in that order, as CSS specifies. Resets and sets create their
// counter in the parent scope; an increment that finds an existing counter
// updates it where it lives so the value carries on to the following siblings.
void applyCounters(const ComputedStyle& style, CounterStack& stack) {
    if (stack.scopes.size() < 2) {
        return;
    }
    CounterScope& scope = stack.scopes[stack.scopes.size() - 2];
    for (const auto& op : style.counterReset) {
        bool found = false;
        for (auto& entry : scope.values) {
            if (entry.first == op.name) {
                entry.second = op.value;
                found = true;
                break;
            }
        }
        if (!found) {
            scope.values.push_back({op.name, op.value});
        }
    }
    for (const auto& op : style.counterIncrement) {
        if (int* value = findCounter(stack, op.name)) {
            *value += op.value;
        } else {
            scope.values.push_back({op.name, op.value});
        }
    }
    for (const auto& op : style.counterSet) {
        bool found = false;
        for (auto& entry : scope.values) {
            if (entry.first == op.name) {
                entry.second = op.value;
                found = true;
                break;
            }
        }
        if (!found) {
            scope.values.push_back({op.name, op.value});
        }
    }
}

// --- generated content ---------------------------------------------------

// The open or close mark at the current quote depth: the pair for the depth,
// or the last pair when nested deeper. Empty with `quotes: none`.
std::string quoteMark(const std::vector<std::string>& quotes, int depth, bool open) {
    if (quotes.size() < 2) {
        return {};
    }
    const std::size_t pairs = quotes.size() / 2;
    const std::size_t at =
        static_cast<std::size_t>(std::max(0, std::min(depth, static_cast<int>(pairs) - 1)));
    return quotes[at * 2 + (open ? 0 : 1)];
}

// Whether `element` already carries a generated box of the named kind.
bool hasPseudoBox(const Element& element, std::string_view tag) {
    for (const auto& child : element.children()) {
        if (child && child->isPseudoElement() && child->tag() == tag) {
            return true;
        }
    }
    return false;
}

// Appends a generated box carrying exactly `mark`, for quotes the element
// implies rather than declares.
void addMarkBox(Element& element, const char* tag, bool first, const std::string& mark) {
    Document* document = element.document();
    const std::size_t index = first ? 0 : element.children().size();
    element.insertChild(std::make_unique<Element>(std::string(tag), document), index);
    element.children()[index]->appendChild(Element::createText(mark, document));
}

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
void addBox(Element& element, const StyleResolver& resolver, const char* tag, bool first,
            CounterStack& counters, const std::vector<std::string>& quotes, int& depth) {
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
                const std::string name = raw.substr(i + 1, end - (i + 1));
                text += std::to_string(counterValue(counters, name));
                i = end + 1;
                continue;
            }
        } else if (raw[i] == '\x07' || raw[i] == '\x08' || raw[i] == '\x09' ||
                   raw[i] == '\x0A') {
            // Quotation marks read the originating element's `quotes` at the
            // current depth; the no- forms move the depth without emitting.
            // A `none` list emits nothing and leaves the depth alone.
            // An open emits at its level and descends; a close ascends
            // first so it answers the level being closed.
            const bool open = raw[i] == '\x07' || raw[i] == '\x09';
            const bool emit = raw[i] == '\x07' || raw[i] == '\x08';
            if (!quotes.empty()) {
                if (open) {
                    if (emit) {
                        text += quoteMark(quotes, depth, true);
                    }
                    ++depth;
                } else {
                    if (depth > 0) {
                        --depth;
                    }
                    if (emit) {
                        text += quoteMark(quotes, depth, false);
                    }
                }
            }
            ++i;
            continue;
        } else if (raw[i] == '\x05') {
            const std::size_t end = raw.find('\x06', i + 1);
            if (end != std::string::npos) {
                const std::string body = raw.substr(i + 1, end - (i + 1));
                const std::size_t split = body.find('\x1f');
                const std::string name =
                    split == std::string::npos ? body : body.substr(0, split);
                const std::string separator =
                    split == std::string::npos ? std::string() : body.substr(split + 1);
                text += countersValue(counters, name, separator);
                i = end + 1;
                continue;
            }
        }
        text.push_back(raw[i]);
        ++i;
    }
    box->appendChild(Element::createText(text, document));
}

// A `::placeholder` box carries the `placeholder` attribute as its text, but
// only while the field is empty. Stale boxes are swept with the rest, so a
// field that gains a value loses its box on the next rebuild.
void addPlaceholderBox(Element& element, const StyleResolver& resolver) {
    if (element.tag() != "input" && element.tag() != "textarea") {
        return;
    }
    const std::string_view hint = element.attribute("placeholder");
    if (hint.empty() || !element.value().empty()) {
        return;
    }
    Document* document = element.document();
    element.insertChild(std::make_unique<Element>("::placeholder", document), 0);
    Element* box = element.children()[0].get();
    box->appendChild(Element::createText(std::string(hint), document));
    // The box resolves for its own declarations (`input::placeholder`);
    // everything else inherits from the field. It lays out no box of its own:
    // the field flows the hint text itself.
    (void)resolver.resolve(*box);
}

void build(Element& element, const StyleResolver& resolver, CounterStack& counters,
           int& quoteDepth) {
    for (const auto& child : element.children()) {
        if (!child || child->isTextNode() || child->isPseudoElement()) {
            continue;
        }
        // Resolve the element for its counter declarations before its generated
        // boxes are built, so `::before`/`::after` can read the counters it set.
        counters.scopes.push_back({});
        const ComputedStyle style = resolver.resolve(*child);
        applyCounters(style, counters);

        addBox(*child, resolver, "::before", true, counters, style.quotes, quoteDepth);
        addBox(*child, resolver, "::after", false, counters, style.quotes, quoteDepth);
        // A <q> quotes itself the way the browser user-agent rule would, unless
        // the author already gave it generated boxes.
        if (child->tag() == "q") {
            // The pair is self-contained: the close answers the depth the open
            // saw, whatever unbalanced author content did between them. When
            // only one side is implicit it neither opens nor closes the depth
            // — there is no matching half to balance.
            int openedAt = -1;
            if (!hasPseudoBox(*child, "::before")) {
                const std::string mark = quoteMark(style.quotes, quoteDepth, true);
                if (!mark.empty()) {
                    addMarkBox(*child, "::before", true, mark);
                    openedAt = quoteDepth;
                    ++quoteDepth;
                }
            }
            addPlaceholderBox(*child, resolver);
            build(*child, resolver, counters, quoteDepth);
            if (!hasPseudoBox(*child, "::after")) {
                const int level = openedAt >= 0 ? openedAt : quoteDepth;
                const std::string mark = quoteMark(style.quotes, level, false);
                if (!mark.empty()) {
                    addMarkBox(*child, "::after", false, mark);
                }
                if (openedAt >= 0) {
                    quoteDepth = openedAt;
                }
            }
        } else {
            addPlaceholderBox(*child, resolver);
            // Recursing after the boxes exist is safe: they are skipped above, and
            // `::before::before` is not a thing CSS can express.
            build(*child, resolver, counters, quoteDepth);
        }
        counters.scopes.pop_back();
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
    CounterStack counters;
    counters.scopes.push_back({});  // the document root's scope
    int quoteDepth = 0;
    build(*document.root(), resolver, counters, quoteDepth);
}

}  // namespace sui

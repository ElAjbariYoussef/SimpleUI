#include "SUI/Markup/Parser.h"

#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"

#include <cctype>
#include <utility>

namespace sui {
namespace {

class Scanner {
public:
    explicit Scanner(std::string_view source) : src_(source) {}

    [[nodiscard]] bool done() const { return pos_ >= src_.size(); }
    [[nodiscard]] std::size_t line() const { return line_; }
    [[nodiscard]] std::size_t pos() const { return pos_; }

    bool startsWith(std::string_view what) const {
        return src_.compare(pos_, what.size(), what) == 0;
    }

    // Whitespace plus // and /* */ comments, which the .sui sample relies on.
    void skipInsignificant() {
        while (!done()) {
            const char c = src_[pos_];
            if (c == '\n') {
                ++line_;
                ++pos_;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                ++pos_;
            } else if (startsWith("//")) {
                skipToLineEnd();
            } else if (startsWith("/*")) {
                pos_ += 2;
                while (!done() && !startsWith("*/")) {
                    if (src_[pos_] == '\n') {
                        ++line_;
                    }
                    ++pos_;
                }
                if (!done()) {
                    pos_ += 2;
                }
            } else {
                return;
            }
        }
    }

    char peek() const { return done() ? '\0' : src_[pos_]; }

    char take() {
        const char c = src_[pos_++];
        if (c == '\n') {
            ++line_;
        }
        return c;
    }

    std::string_view rest() const { return src_.substr(pos_); }

    std::string_view next(std::size_t count) {
        const auto end = std::min(pos_ + count, src_.size());
        std::string_view chunk = src_.substr(pos_, end - pos_);
        pos_ = end;
        return chunk;
    }

    void advanceTo(std::size_t target) {
        while (pos_ < target && pos_ < src_.size()) {
            take();
        }
    }

    void skipToLineEnd() {
        while (!done() && src_[pos_] != '\n') {
            ++pos_;
        }
    }

private:
    std::string_view src_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
};

bool isNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
           c == ':';
}

std::string_view readName(Scanner& sc) {
    sc.skipInsignificant();
    const std::size_t start = sc.pos();
    while (!sc.done() && isNameChar(sc.peek())) {
        sc.take();
    }
    const std::size_t len = sc.pos() - start;
    if (len == 0) {
        return std::string_view{};
    }
    return std::string_view{sc.rest().data() - (sc.pos() - start), len};
}

std::string readQuotedValue(Scanner& sc) {
    const char quote = sc.take();
    std::string value;
    while (!sc.done() && sc.peek() != quote) {
        value.push_back(sc.take());
    }
    if (!sc.done()) {
        sc.take();  // closing quote
    }
    return value;
}

// Attribute values may be quoted or bare; bare values end at whitespace or '>'.
std::string readAttributeValue(Scanner& sc) {
    sc.skipInsignificant();
    if (sc.peek() == '"' || sc.peek() == '\'') {
        return readQuotedValue(sc);
    }
    std::string value;
    while (!sc.done()) {
        const char c = sc.peek();
        if (std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '/') {
            break;
        }
        value.push_back(sc.take());
    }
    return value;
}

struct OpenTag {
    std::string name;
    bool selfClosing = false;
};

bool readAttributes(Scanner& sc, Element& element, bool& selfClosing,
                    std::string& error) {
    while (true) {
        sc.skipInsignificant();
        if (sc.done()) {
            error = "unexpected end of file inside a tag";
            return false;
        }
        if (sc.startsWith("/>")) {
            sc.next(2);
            selfClosing = true;
            return true;
        }
        if (sc.peek() == '>') {
            sc.take();
            selfClosing = false;
            return true;
        }

        const std::string_view name = readName(sc);
        if (name.empty()) {
            error = "expected an attribute name";
            return false;
        }

        sc.skipInsignificant();
        if (sc.peek() != '=') {
            // Valueless attribute, e.g. disabled.
            element.setAttribute(std::string(name), "");
            continue;
        }
        sc.take();  // '='
        element.setAttribute(std::string(name), readAttributeValue(sc));
    }
}

bool parseChildren(Scanner& sc, Element& parent, ParseError& error);

bool parseElement(Scanner& sc, Element& parent, ParseError& error) {
    sc.skipInsignificant();
    if (!sc.startsWith("<")) {
        error = {sc.line(), "expected '<'"};
        return false;
    }
    sc.take();  // '<'

    const std::string_view tagName = readName(sc);
    if (tagName.empty()) {
        error = {sc.line(), "expected a tag name after '<'"};
        return false;
    }

    auto element = std::make_unique<Element>(std::string(tagName), parent.document());
    bool selfClosing = false;
    std::string attrError;
    if (!readAttributes(sc, *element, selfClosing, attrError)) {
        error = {sc.line(), "<" + std::string(tagName) + ">: " + attrError};
        return false;
    }

    Element& ref = *element;
    parent.appendChild(std::move(element));

    if (!selfClosing) {
        if (!parseChildren(sc, ref, error)) {
            return false;
        }
    }
    return true;
}

bool parseChildren(Scanner& sc, Element& parent, ParseError& error) {
    std::string text;
    while (true) {
        if (sc.done()) {
            error = {sc.line(),
                     "unclosed <" + std::string(parent.tag()) + "> at end of file"};
            return false;
        }
        if (sc.startsWith("</")) {
            sc.next(2);
            const std::string_view closing = readName(sc);
            sc.skipInsignificant();
            if (sc.peek() == '>') {
                sc.take();
            }
            if (closing != parent.tag()) {
                error = {sc.line(), "closing tag </" + std::string(closing) +
                                        "> does not match <" +
                                        std::string(parent.tag()) + ">"};
                return false;
            }
            parent.setText(text);
            return true;
        }
        if (sc.peek() == '<') {
            if (sc.startsWith("<!--")) {
                sc.advanceTo(sc.pos() + 4);
                while (!sc.done() && !sc.startsWith("-->")) {
                    sc.take();
                }
                if (!sc.done()) {
                    sc.advanceTo(sc.pos() + 3);
                }
                continue;
            }
            if (!parent.children().empty() || !text.empty()) {
                parent.setText(text);
                text.clear();
            }
            if (!parseElement(sc, parent, error)) {
                return false;
            }
            continue;
        }
        text.push_back(sc.take());
    }
}

}  // namespace

std::unique_ptr<Document> MarkupParser::parse(std::string_view source,
                                              ParseError& error) {
    auto document = std::make_unique<Document>();

    Scanner sc(source);
    sc.skipInsignificant();
    if (!sc.startsWith("<")) {
        error = {sc.line(), "document must start with a tag (expected <sui>)"};
        return nullptr;
    }

    // Parse into a scratch holder so ownership is settled before we re-parent.
    Element scratch("document", document.get());
    if (!parseElement(sc, scratch, error)) {
        return nullptr;
    }
    if (scratch.children().empty()) {
        error = {sc.line(), "<sui> has no content"};
        return nullptr;
    }

    // <sui> is a document wrapper, not a layout node, so its children become the
    // tree root. Ordering is preserved.
    auto root = std::make_unique<Element>("root", document.get());
    for (auto& child : scratch.releaseChildren()) {
        root->appendChild(std::move(child));
    }

    document->setRoot(std::move(root));
    return document;
}

}  // namespace sui
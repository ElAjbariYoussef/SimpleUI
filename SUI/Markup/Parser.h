#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace sui {

class Document;

// Diagnostics carry a 1-based line number so a bad .sui file points at the
// offending markup instead of failing silently.
struct ParseError {
    std::size_t line = 0;
    std::string message;
};

class MarkupParser {
public:
    // Returns nullptr and fills `error` on failure.
    static std::unique_ptr<Document> parse(std::string_view source,
                                          ParseError& error);
};

}  // namespace sui
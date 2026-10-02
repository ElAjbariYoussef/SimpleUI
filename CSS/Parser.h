#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Element;

struct Selector {
    enum Type { Type, Class, Id, Universal } type = Universal;
    std::string value;
};

struct SimpleSelector {
    std::vector<Selector> parts;
};

struct Rule {
    SimpleSelector selector;
    // minimal declarations stored as raw key/value pairs for now
    std::vector<std::pair<std::string, std::string>> declarations;
};

class CSSParser {
public:
    // Parse CSS source; ignores @rules and comments.
    static std::vector<Rule> parse(std::string_view source);
};

}  // namespace sui
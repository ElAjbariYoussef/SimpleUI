#include "CSS/StyleResolver.h"

#include "SUI/DOM/Element.h"

#include <charconv>
#include <cctype>
#include <string>
#include <sstream>
#include <string_view>
#include <vector>

namespace sui {
namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

float parseFloat(std::string_view v) {
    float out = 0.0f;
    auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    (void)r;
    return out;
}

// Very naive inline style parser: supports width:10px; height:20; color:#fff;
// background-color:#111827; margin:4 0; padding:4 8
void applyInline(ComputedStyle& s, std::string_view styleAttr) {
    // tokenize by semicolons
    std::size_t start = 0;
    while (start < styleAttr.size()) {
        std::size_t semi = styleAttr.find(';', start);
        std::string_view decl;
        if (semi == std::string_view::npos) {
            decl = styleAttr.substr(start);
            start = styleAttr.size();
        } else {
            decl = styleAttr.substr(start, semi - start);
            start = semi + 1;
        }
        std::size_t colon = decl.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string prop = trim(decl.substr(0, colon));
        std::string val = trim(decl.substr(colon + 1));
        if (prop.empty() || val.empty()) {
            continue;
        }
        // normalize
        if (prop == "width") {
            if (val.ends_with("px")) val.erase(val.size() - 2);
            s.width = parseFloat(val);
        } else if (prop == "height") {
            if (val.ends_with("px")) val.erase(val.size() - 2);
            s.height = parseFloat(val);
        } else if (prop == "margin") {
            // 1 or 4 values space separated
            // simple 1-value
            if (val.find(' ') == std::string::npos) {
                float v = parseFloat(val);
                s.margin = {v, v, v, v};
            } else {
                // naive split
                std::vector<float> parts;
                        std::vector<std::string> toks;
                std::string cur;
                for (char c : val) {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
                    } else { cur.push_back(c); }
                }
                if (!cur.empty()) toks.push_back(cur);
                for (auto& t : toks) {
                    if (t.ends_with("px")) t.erase(t.size() - 2);
                    parts.push_back(parseFloat(t));
                }
                if (parts.size() == 4) {
                    s.margin = {parts[0], parts[1], parts[2], parts[3]};
                } else if (parts.size() == 2) {
                    s.margin = {parts[0], parts[1], parts[0], parts[1]};
                }
            }
        } else if (prop == "padding") {
            if (val.find(' ') == std::string::npos) {
                float v = parseFloat(val);
                std::string v2 = val;
                if (v2.ends_with("px")) v2.erase(v2.size() - 2);
                v = parseFloat(v2);
                s.padding = {v, v, v, v};
            } else {
                std::vector<float> parts;
                std::vector<std::string> toks;
                std::string cur;
                for (char c : val) {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
                    } else { cur.push_back(c); }
                }
                if (!cur.empty()) toks.push_back(cur);
                for (auto& t : toks) {
                    if (t.ends_with("px")) t.erase(t.size() - 2);
                    parts.push_back(parseFloat(t));
                }
                if (parts.size() == 4) {
                    s.padding = {parts[0], parts[1], parts[2], parts[3]};
                } else if (parts.size() == 2) {
                    s.padding = {parts[0], parts[1], parts[0], parts[1]};
                }
            }
        } else if (prop == "background-color" || prop == "background") {
            // hex #rrggbb or #rgb
            if (val.starts_with('#')) {
                std::string hex = val.substr(1);
                auto hex2 = [](char c) -> Uint8 {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                    return 0;
                };
                if (hex.size() == 3) {
                    Uint8 r = (hex2(hex[0]) * 17);
                    Uint8 g = (hex2(hex[1]) * 17);
                    Uint8 b = (hex2(hex[2]) * 17);
                    s.backgroundColor = {r, g, b, 255};
                } else if (hex.size() >= 6) {
                    Uint8 r = (hex2(hex[0]) << 4) | hex2(hex[1]);
                    Uint8 g = (hex2(hex[2]) << 4) | hex2(hex[3]);
                    Uint8 b = (hex2(hex[4]) << 4) | hex2(hex[5]);
                    s.backgroundColor = {r, g, b, 255};
                }
            }
        } else if (prop == "color") {
            if (val.starts_with('#')) {
                std::string hex = val.substr(1);
                auto hex2 = [](char c) -> Uint8 {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                    return 0;
                };
                if (hex.size() == 3) {
                    Uint8 r = (hex2(hex[0]) * 17);
                    Uint8 g = (hex2(hex[1]) * 17);
                    Uint8 b = (hex2(hex[2]) * 17);
                    s.color = {r, g, b, 255};
                } else if (hex.size() >= 6) {
                    Uint8 r = (hex2(hex[0]) << 4) | hex2(hex[1]);
                    Uint8 g = (hex2(hex[2]) << 4) | hex2(hex[3]);
                    Uint8 b = (hex2(hex[4]) << 4) | hex2(hex[5]);
                    s.color = {r, g, b, 255};
                }
            }
        } else if (prop == "font-size") {
            std::string v2 = val;
            if (v2.ends_with("px")) v2.erase(v2.size() - 2);
            s.fontSize = parseFloat(v2);
        } else if (prop == "display") {
            if (val == "none") s.display = ComputedStyle::None;
            else if (val == "inline") s.display = ComputedStyle::Inline;
            else if (val == "inline-block") s.display = ComputedStyle::InlineBlock;
            else if (val == "block") s.display = ComputedStyle::Block;
        }
    }
}

}  // namespace

ComputedStyle StyleResolver::resolve(Element& element) const {
    ComputedStyle s = defaultStyleFor(element.tag());
    if (element.hasAttribute("style")) {
        applyInline(s, element.attribute("style"));
    }
    if (element.hasAttribute("width")) {
        float v = 0.0f;
        std::string raw(element.attribute("width"));
        if (raw.ends_with("px")) raw.erase(raw.size() - 2);
        std::from_chars(raw.data(), raw.data() + raw.size(), v);
        if (v > 0.0f) s.width = v;
    }
    if (element.hasAttribute("height")) {
        float v = 0.0f;
        std::string raw(element.attribute("height"));
        if (raw.ends_with("px")) raw.erase(raw.size() - 2);
        std::from_chars(raw.data(), raw.data() + raw.size(), v);
        if (v > 0.0f) s.height = v;
    }
    return s;
}

}  // namespace sui
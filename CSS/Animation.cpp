#include "CSS/Animation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sui {
namespace {

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

SDL_Color blendColor(SDL_Color from, SDL_Color to, float t) {
    auto channel = [&](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(
            std::lround(std::clamp(lerp(static_cast<float>(a), static_cast<float>(b), t),
                                   0.0f, 255.0f)));
    };
    SDL_Color out;
    out.r = channel(from.r, to.r);
    out.g = channel(from.g, to.g);
    out.b = channel(from.b, to.b);
    out.a = channel(from.a, to.a);
    return out;
}

// True when `token` names an easing exactly (by name or by its function form),
// so the shorthand parsers can tell a timing function apart from a keyword like
// `forwards` or `infinite` that Easing::parse would also accept as linear.
bool isEasingKeyword(std::string_view token) {
    if (token == "linear" || token == "ease" || token == "ease-in" ||
        token == "ease-out" || token == "ease-in-out" || token == "step-start" ||
        token == "step-end") {
        return true;
    }
    if (token.size() > 14 && token.substr(0, 14) == "cubic-bezier(" && token.back() == ')') {
        return true;
    }
    if (token.size() > 6 && token.substr(0, 6) == "steps(" && token.back() == ')') {
        return true;
    }
    return false;
}

// Splits a shorthand comma part into whitespace-separated words.
std::vector<std::string> splitWords(const std::string& part) {
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at < part.size()) {
        const std::size_t start = part.find_first_not_of(" \t", at);
        if (start == std::string::npos) {
            break;
        }
        const std::size_t end = part.find_first_of(" \t", start);
        words.emplace_back(part.substr(start, end == std::string::npos ? std::string::npos
                                                                       : end - start));
        at = end == std::string::npos ? part.size() : end + 1;
    }
    return words;
}

}  // namespace

bool canBlend(const AnimatableValue& from, const AnimatableValue& to) noexcept {
    if (from.kind == AnimatableValue::Kind::None ||
        to.kind == AnimatableValue::Kind::None || from.kind != to.kind) {
        return false;
    }
    if (from.kind == AnimatableValue::Kind::Length) {
        return from.length.unit == to.length.unit && from.length.unit != Length::Auto;
    }
    return true;
}

std::vector<std::string> splitCommaList(std::string_view source) {
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) {
            s.remove_suffix(1);
        }
        return s;
    };
    std::vector<std::string> out;
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '(') {
            ++depth;
        } else if (source[i] == ')') {
            --depth;
        } else if (source[i] == ',' && depth == 0) {
            out.push_back(std::string(trim(source.substr(start, i - start))));
            start = i + 1;
        }
    }
    out.push_back(std::string(trim(source.substr(start))));
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const std::string& s) {
                                 return s.find_first_not_of(" \t\n") == std::string::npos;
                             }),
              out.end());
    return out;
}

bool parseTimeMs(std::string_view token, float& outMs) {
    const std::string text(token);
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str()) {
        return false;
    }
    const std::string_view rest(end);
    if (rest == "s") {
        outMs = value * 1000.0f;
        return true;
    }
    if (rest == "ms") {
        outMs = value;
        return true;
    }
    if (rest.empty() && value == 0.0f) {
        outMs = 0.0f;  // CSS accepts a bare zero
        return true;
    }
    return false;
}

namespace {

// The animatable longhands, in a fixed order so `all` is deterministic. This is
// the property table the resolver reads and writes through animatableValue() and
// blendInto(), and `all`/`border-width`-style tokens expand against it.
const std::vector<std::string>& animatableTable() {
    static const std::vector<std::string> table = {
        "width",               "height",            "min-width",         "min-height",
        "max-width",           "max-height",        "margin-top",        "margin-right",
        "margin-bottom",       "margin-left",       "padding-top",       "padding-right",
        "padding-bottom",      "padding-left",      "border-top-width",  "border-right-width",
        "border-bottom-width", "border-left-width", "left",              "top",
        "right",               "bottom",            "color",             "background-color",
        "border-color",        "border-top-color",  "border-right-color", "border-bottom-color",
        "border-left-color",   "opacity",           "font-size",
    };
    return table;
}

}  // namespace

const std::vector<std::string>& allAnimatableProperties() noexcept {
    return animatableTable();
}

std::vector<std::string> expandTransitionProperty(std::string_view property) {
    if (property == "all") {
        return animatableTable();
    }
    const auto edges = [](const char* a, const char* b, const char* c, const char* d) {
        return std::vector<std::string>{a, b, c, d};
    };
    if (property == "margin") {
        return edges("margin-top", "margin-right", "margin-bottom", "margin-left");
    }
    if (property == "padding") {
        return edges("padding-top", "padding-right", "padding-bottom", "padding-left");
    }
    if (property == "border-width") {
        return edges("border-top-width", "border-right-width", "border-bottom-width",
                     "border-left-width");
    }
    if (property == "border-color") {
        return edges("border-top-color", "border-right-color", "border-bottom-color",
                     "border-left-color");
    }
    return std::vector<std::string>{std::string(property)};
}

AnimatableValue animatableValue(std::string_view property, const ComputedStyle& style) {
    const std::string name(property);
    const auto length = [](Length l) {
        AnimatableValue v;
        v.kind = l.unit == Length::Auto ? AnimatableValue::Kind::None
                                        : AnimatableValue::Kind::Length;
        v.length = l;
        return v;
    };
    const auto color = [](SDL_Color c) {
        AnimatableValue v;
        v.kind = AnimatableValue::Kind::Color;
        v.color = c;
        return v;
    };
    const auto number = [](float n) {
        AnimatableValue v;
        v.kind = AnimatableValue::Kind::Number;
        v.number = n;
        return v;
    };

    if (name == "width") return length(style.width);
    if (name == "height") return length(style.height);
    if (name == "min-width") return length(style.minWidth);
    if (name == "min-height") return length(style.minHeight);
    if (name == "max-width") return length(style.maxWidth);
    if (name == "max-height") return length(style.maxHeight);
    if (name == "margin-top") return length(style.margin.top);
    if (name == "margin-right") return length(style.margin.right);
    if (name == "margin-bottom") return length(style.margin.bottom);
    if (name == "margin-left") return length(style.margin.left);
    if (name == "padding-top") return length(style.padding.top);
    if (name == "padding-right") return length(style.padding.right);
    if (name == "padding-bottom") return length(style.padding.bottom);
    if (name == "padding-left") return length(style.padding.left);
    if (name == "border-top-width") return length(style.borderWidth.top);
    if (name == "border-right-width") return length(style.borderWidth.right);
    if (name == "border-bottom-width") return length(style.borderWidth.bottom);
    if (name == "border-left-width") return length(style.borderWidth.left);
    if (name == "left") return length(style.left);
    if (name == "top") return length(style.top);
    if (name == "right") return length(style.right);
    if (name == "bottom") return length(style.bottom);
    if (name == "color") return color(style.color);
    if (name == "background-color") return color(style.backgroundColor);
    if (name == "border-color") return color(style.borderColor);
    if (name == "border-top-color" || name == "border-right-color" ||
        name == "border-bottom-color" || name == "border-left-color") {
        return color(style.borderColor);
    }
    if (name == "opacity") return number(style.opacity);
    if (name == "font-size") return number(style.fontSize);
    return AnimatableValue{};
}

AnimatableValue blendValues(const AnimatableValue& from, const AnimatableValue& to, float t) {
    if (to.kind == AnimatableValue::Kind::None || !canBlend(from, to)) {
        return to;  // discrete: the caller keeps the target
    }
    if (to.kind == AnimatableValue::Kind::Length) {
        AnimatableValue v = to;
        v.length = Length{to.length.unit, lerp(from.length.value, to.length.value, t)};
        return v;
    }
    if (to.kind == AnimatableValue::Kind::Color) {
        AnimatableValue v = to;
        v.color = blendColor(from.color, to.color, t);
        return v;
    }
    AnimatableValue v = to;
    v.number = lerp(from.number, to.number, t);
    return v;
}

void blendInto(ComputedStyle& style, std::string_view property,
               const AnimatableValue& from, const AnimatableValue& to, float t) {
    const AnimatableValue blended = blendValues(from, to, t);
    if (blended.kind == AnimatableValue::Kind::None) {
        return;
    }
    // Mirror the readers in animatableValue.
    const std::string name(property);
    if (name == "width") { style.width = blended.length; return; }
    if (name == "height") { style.height = blended.length; return; }
    if (name == "min-width") { style.minWidth = blended.length; return; }
    if (name == "min-height") { style.minHeight = blended.length; return; }
    if (name == "max-width") { style.maxWidth = blended.length; return; }
    if (name == "max-height") { style.maxHeight = blended.length; return; }
    if (name == "margin-top") { style.margin.top = blended.length; return; }
    if (name == "margin-right") { style.margin.right = blended.length; return; }
    if (name == "margin-bottom") { style.margin.bottom = blended.length; return; }
    if (name == "margin-left") { style.margin.left = blended.length; return; }
    if (name == "padding-top") { style.padding.top = blended.length; return; }
    if (name == "padding-right") { style.padding.right = blended.length; return; }
    if (name == "padding-bottom") { style.padding.bottom = blended.length; return; }
    if (name == "padding-left") { style.padding.left = blended.length; return; }
    if (name == "border-top-width") { style.borderWidth.top = blended.length; return; }
    if (name == "border-right-width") { style.borderWidth.right = blended.length; return; }
    if (name == "border-bottom-width") { style.borderWidth.bottom = blended.length; return; }
    if (name == "border-left-width") { style.borderWidth.left = blended.length; return; }
    if (name == "left") { style.left = blended.length; return; }
    if (name == "top") { style.top = blended.length; return; }
    if (name == "right") { style.right = blended.length; return; }
    if (name == "bottom") { style.bottom = blended.length; return; }
    if (name == "color") { style.color = blended.color; return; }
    if (name == "background-color") { style.backgroundColor = blended.color; return; }
    if (name == "border-color" || name == "border-top-color" ||
        name == "border-right-color" || name == "border-bottom-color" ||
        name == "border-left-color") {
        style.borderColor = blended.color;
        return;
    }
    if (name == "opacity") { style.opacity = blended.number; return; }
    if (name == "font-size") { style.fontSize = blended.number; return; }
}

namespace {

// One comma part of a `transition` shorthand, e.g. `width 0.3s ease 0.2s`.
// Appends one entry to each of the four parallel lists.
void applyOneTransition(const std::string& part, ComputedStyle& style) {
    std::vector<std::string> properties;
    float duration = 0.0f;
    float delay = 0.0f;
    bool seenDuration = false;
    Easing easing;
    for (const std::string& word : splitWords(part)) {
        float ms = 0.0f;
        if (parseTimeMs(word, ms)) {
            if (seenDuration) {
                delay = ms;
            } else {
                duration = ms;
                seenDuration = true;
            }
            continue;
        }
        if (isEasingKeyword(word)) {
            easing = Easing::parse(word);
            continue;
        }
        if (word == "normal" || word == "allow-discrete" || word == "discrete") {
            continue;  // transition-behavior keywords, carried by the shorthand
        }
        properties.emplace_back(word);
    }
    if (properties.empty()) {
        properties.emplace_back("");  // a timing-only part animates nothing
    }
    style.transitionProperties.push_back(properties.front());
    style.transitionDurationsMs.push_back(duration);
    style.transitionEasings.push_back(easing);
    style.transitionDelaysMs.push_back(delay);
}

// One comma part of an `animation` shorthand, e.g. `spin 2s ease 0.5s infinite
// alternate both`. Times are order-tolerant (duration first, then delay); every
// other token is classified by its keyword.
void applyOneAnimation(const std::string& part, ComputedStyle& style) {
    std::string name;
    float duration = 0.0f;
    float delay = 0.0f;
    bool seenDuration = false;
    float iterations = 1.0f;
    std::string direction = "normal";
    std::string fill = "none";
    bool paused = false;
    Easing easing;
    for (const std::string& word : splitWords(part)) {
        float ms = 0.0f;
        if (parseTimeMs(word, ms)) {
            if (seenDuration) {
                delay = ms;
            } else {
                duration = ms;
                seenDuration = true;
            }
            continue;
        }
        if (isEasingKeyword(word)) {
            easing = Easing::parse(word);
            continue;
        }
        if (word == "infinite") {
            iterations = -1.0f;
            continue;
        }
        if (word == "reverse" || word == "alternate" || word == "alternate-reverse" ||
            word == "normal") {
            direction = word;
            continue;
        }
        if (word == "forwards" || word == "backwards" || word == "both") {
            fill = word;
            continue;
        }
        if (word == "running" || word == "paused") {
            paused = word == "paused";
            continue;
        }
        char* end = nullptr;
        const float number = std::strtof(word.c_str(), &end);
        if (end != word.c_str() && *end == '\0') {
            iterations = number;  // an iteration count like `3` or `2.5`
            continue;
        }
        if (name.empty()) {
            name = word;
        }
    }
    style.animationNames.push_back(name);
    style.animationDurationsMs.push_back(duration);
    style.animationEasings.push_back(easing);
    style.animationDelaysMs.push_back(delay);
    style.animationIterations.push_back(iterations);
    style.animationDirections.push_back(direction);
    style.animationFills.push_back(fill);
    style.animationPaused.push_back(paused);
}

}  // namespace

void applyTransitionShorthand(const std::string& value, ComputedStyle& style) {
    style.transitionProperties.clear();
    style.transitionDurationsMs.clear();
    style.transitionEasings.clear();
    style.transitionDelaysMs.clear();
    if (value == "none") {
        return;
    }
    for (const std::string& part : splitCommaList(value)) {
        applyOneTransition(part, style);
    }
}

void applyAnimationShorthand(const std::string& value, ComputedStyle& style) {
    style.animationNames.clear();
    style.animationDurationsMs.clear();
    style.animationEasings.clear();
    style.animationDelaysMs.clear();
    style.animationIterations.clear();
    style.animationDirections.clear();
    style.animationFills.clear();
    style.animationPaused.clear();
    if (value == "none") {
        return;
    }
    for (const std::string& part : splitCommaList(value)) {
        applyOneAnimation(part, style);
    }
}

namespace {

// Rounds a float for serialisation without trailing ".000000" noise.
std::string formatNumber(float value) {
    char buffer[32];
    if (value == static_cast<float>(static_cast<int>(value)) && std::isfinite(value)) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.4g", value);
    }
    return buffer;
}

std::string joinWords(const std::vector<std::string>& words) {
    std::string out;
    for (const std::string& word : words) {
        if (!out.empty()) {
            out += ", ";
        }
        out += word;
    }
    return out;
}

}  // namespace

std::string serializeTime(float ms) {
    const float whole = std::floor(ms);
    char buffer[32];
    if (whole == ms && std::fmod(ms, 1000.0f) == 0.0f) {
        std::snprintf(buffer, sizeof(buffer), "%.0fs", ms / 1000.0f);
    } else if (whole == ms) {
        std::snprintf(buffer, sizeof(buffer), "%.0fms", ms);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.3fms", ms);
    }
    return buffer;
}

std::string serializeEasing(const Easing& easing) {
    switch (easing.type) {
        case Easing::Type::Linear:
            return "linear";
        case Easing::Type::Steps: {
            const std::string keyword = easing.stepStart ? "jump-start" : "jump-end";
            return "steps(" + std::to_string(easing.steps) + ", " + keyword + ")";
        }
        case Easing::Type::CubicBezier:
            return "cubic-bezier(" + formatNumber(easing.x1) + ", " +
                   formatNumber(easing.y1) + ", " + formatNumber(easing.x2) + ", " +
                   formatNumber(easing.y2) + ")";
    }
    return "linear";
}

std::string serializeTransition(std::string_view property, const ComputedStyle& style) {
    const std::size_t n = style.transitionProperties.size();
    if (property == "transition") {
        std::string out;
        for (std::size_t i = 0; i < n; ++i) {
            if (!out.empty()) {
                out += ", ";
            }
            out += style.transitionProperties[i];
            out += ' ';
            out += serializeTime(listAt(style.transitionDurationsMs, i));
            out += ' ';
            out += serializeEasing(listAt(style.transitionEasings, i));
            out += ' ';
            out += serializeTime(listAt(style.transitionDelaysMs, i));
        }
        return out;
    }
    if (property == "transition-property") {
        return joinWords(style.transitionProperties);
    }
    std::vector<std::string> words;
    for (std::size_t i = 0; i < n; ++i) {
        if (property == "transition-duration") {
            words.push_back(serializeTime(listAt(style.transitionDurationsMs, i)));
        } else if (property == "transition-delay") {
            words.push_back(serializeTime(listAt(style.transitionDelaysMs, i)));
        } else if (property == "transition-timing-function") {
            words.push_back(serializeEasing(listAt(style.transitionEasings, i)));
        }
    }
    return joinWords(words);
}

std::string serializeAnimation(std::string_view property, const ComputedStyle& style) {
    const std::size_t n = style.animationNames.size();
    if (property == "animation") {
        std::string out;
        for (std::size_t i = 0; i < n; ++i) {
            if (!out.empty()) {
                out += ", ";
            }
            out += style.animationNames[i];
            out += ' ';
            out += serializeTime(listAt(style.animationDurationsMs, i));
            out += ' ';
            out += serializeEasing(listAt(style.animationEasings, i));
            out += ' ';
            out += serializeTime(listAt(style.animationDelaysMs, i));
            if (listAt(style.animationIterations, i) < 0.0f) {
                out += " infinite";
            } else if (listAt(style.animationIterations, i) != 1.0f) {
                out += ' ';
                out += formatNumber(listAt(style.animationIterations, i));
            }
            if (listAt(style.animationDirections, i) != "normal") {
                out += ' ';
                out += listAt(style.animationDirections, i);
            }
            if (listAt(style.animationFills, i) != "none") {
                out += ' ';
                out += listAt(style.animationFills, i);
            }
            if (listAt(style.animationPaused, i)) {
                out += " paused";
            }
        }
        return out;
    }
    std::vector<std::string> words;
    for (std::size_t i = 0; i < n; ++i) {
        if (property == "animation-name") {
            words.push_back(style.animationNames[i]);
        } else if (property == "animation-duration") {
            words.push_back(serializeTime(listAt(style.animationDurationsMs, i)));
        } else if (property == "animation-delay") {
            words.push_back(serializeTime(listAt(style.animationDelaysMs, i)));
        } else if (property == "animation-timing-function") {
            words.push_back(serializeEasing(listAt(style.animationEasings, i)));
        } else if (property == "animation-iteration-count") {
            const float count = listAt(style.animationIterations, i);
            words.push_back(count < 0.0f ? "infinite" : formatNumber(count));
        } else if (property == "animation-direction") {
            words.push_back(listAt(style.animationDirections, i));
        } else if (property == "animation-fill-mode") {
            words.push_back(listAt(style.animationFills, i));
        } else if (property == "animation-play-state") {
            words.push_back(listAt(style.animationPaused, i) ? "paused" : "running");
        }
    }
    return joinWords(words);
}

}  // namespace sui
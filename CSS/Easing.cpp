#include "CSS/Easing.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace sui {
namespace {

std::string_view trimValue(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

float parseNumber(std::string_view token) {
    const std::string text(token);
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value)) {
        return 0.0f;
    }
    return value;
}

// X(u) along a cubic bezier P0=(0,0), P1=(x1,y1), P2=(x2,y2), P3=(1,1).
float bezierX(float x1, float x2, float u) {
    const float inv = 1.0f - u;
    return 3.0f * u * inv * inv * x1 + 3.0f * u * u * inv * x2 + u * u * u;
}

float bezierY(float y1, float y2, float u) {
    const float inv = 1.0f - u;
    return 3.0f * u * inv * inv * y1 + 3.0f * u * u * inv * y2 + u * u * u;
}

}  // namespace

Easing Easing::parse(std::string_view value) {
    const std::string_view v = trimValue(value);
    if (v == "linear") {
        return Easing{};
    }
    const auto curve = [](float x1, float y1, float x2, float y2) {
        Easing e;
        e.type = Type::CubicBezier;
        e.x1 = x1;
        e.y1 = y1;
        e.x2 = x2;
        e.y2 = y2;
        return e;
    };
    if (v == "ease") {
        return curve(0.25f, 0.1f, 0.25f, 1.0f);
    }
    if (v == "ease-in") {
        return curve(0.42f, 0.0f, 1.0f, 1.0f);
    }
    if (v == "ease-out") {
        return curve(0.0f, 0.0f, 0.58f, 1.0f);
    }
    if (v == "ease-in-out") {
        return curve(0.42f, 0.0f, 0.58f, 1.0f);
    }
    if (v == "step-start") {
        Easing e;
        e.type = Type::Steps;
        e.steps = 1;
        e.stepStart = true;
        return e;
    }
    if (v == "step-end") {
        Easing e;
        e.type = Type::Steps;
        e.steps = 1;
        e.stepStart = false;
        return e;
    }
    if (v.size() > 13 && v.substr(0, 13) == "cubic-bezier(" && v.back() == ')') {
        const std::string_view inner = v.substr(13, v.size() - 14);
        Easing e;
        e.type = Type::CubicBezier;
        std::size_t at = 0;
        for (int slot = 0; slot < 4; ++slot) {
            const std::size_t comma = inner.find(',', at);
            if (comma == std::string_view::npos && slot < 3) {
                return Easing{};  // fewer than four numbers
            }
            const std::string_view token = trimValue(
                comma == std::string_view::npos ? inner.substr(at) : inner.substr(at, comma - at));
            const float n = parseNumber(token);
            switch (slot) {
                case 0: e.x1 = n; break;
                case 1: e.y1 = n; break;
                case 2: e.x2 = n; break;
                default: e.y2 = n; break;
            }
            at = comma + 1;
        }
        if (e.x1 > 1.0f || e.x2 > 1.0f || e.x1 < 0.0f || e.x2 < 0.0f) {
            return Easing{};  // x must stay in [0,1] or the curve is not a function
        }
        return e;
    }
    if (v.size() > 6 && v.substr(0, 6) == "steps(" && v.back() == ')') {
        const std::string_view inner = v.substr(6, v.size() - 7);
        const std::size_t comma = inner.find(',');
        const std::string_view count = trimValue(
            comma == std::string_view::npos ? inner : inner.substr(0, comma));
        Easing e;
        e.type = Type::Steps;
        e.steps = static_cast<int>(parseNumber(count));
        if (e.steps < 1) {
            e.steps = 1;
        }
        if (comma != std::string_view::npos) {
            const std::string_view keyword = trimValue(inner.substr(comma + 1));
            e.stepStart = keyword == "start" || keyword == "jump-start";
            // anything else (end, jump-end, jump-none, jump-both) keeps the default
        }
        return e;
    }
    return Easing{};
}

float Easing::apply(float t) const noexcept {
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    switch (type) {
        case Type::Linear:
            return t;
        case Type::Steps: {
            const int n = steps >= 1 ? steps : 1;
            if (stepStart) {
                return std::ceil(t * static_cast<float>(n)) / static_cast<float>(n);
            }
            return std::floor(t * static_cast<float>(n)) / static_cast<float>(n);
        }
        case Type::CubicBezier: {
            // Bisect for u with X(u) == t (monotone because x of both controls is
            // clamped to [0,1]), then read off Y(u). Thirty-two halvings give a
            // sub-ppm resolution, which is far below any visual difference.
            float lo = 0.0f;
            float hi = 1.0f;
            for (int i = 0; i < 32; ++i) {
                const float mid = 0.5f * (lo + hi);
                if (bezierX(x1, x2, mid) < t) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            return bezierY(y1, y2, 0.5f * (lo + hi));
        }
    }
    return t;
}

}  // namespace sui
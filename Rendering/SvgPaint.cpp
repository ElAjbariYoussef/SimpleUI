#include "Rendering/SvgPaint.h"

#include "CSS/StyleResolver.h"
#include "SUI/DOM/Document.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace sui {
namespace {

std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// A gradient length: a plain number, or a percentage against `base`. Trailing
// garbage leaves the fallback in place, so a broken offset never shifts a stop.
float gradientNumber(std::string_view text, float base, float fallback) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        return fallback;
    }
    const std::string owned(trimmed);
    char* stop = nullptr;
    const float value = std::strtof(owned.c_str(), &stop);
    if (stop == owned.c_str()) {
        return fallback;
    }
    while (*stop == ' ' || *stop == '\t' || *stop == '\n' || *stop == '\r' ||
           *stop == '\f') {
        ++stop;
    }
    if (*stop == '%') {
        return value * 0.01f * base;
    }
    return *stop == '\0' ? value : fallback;
}

// The `#id` inside a `url(...)` paint value, with the trailing fallback (if
// any) ignored. Empty for anything that is not a URL reference.
std::string_view urlPaintFragment(std::string_view paint) {
    const std::string_view token = trim(paint);
    if (token.size() < 6 || token.substr(0, 4) != "url(") {
        return {};
    }
    const std::size_t close = token.find(')');
    if (close == std::string_view::npos) {
        return {};
    }
    std::string_view args = trim(token.substr(4, close - 4));
    if (args.size() >= 2 && (args.front() == '"' || args.front() == '\'') &&
        args.back() == args.front()) {
        args.remove_prefix(1);
        args.remove_suffix(1);
    }
    return trim(args);
}

float stopOpacity(std::string_view text, float fallback) {
    const float value = gradientNumber(text, 1.0f, fallback);
    return std::clamp(value, 0.0f, 1.0f);
}

}  // namespace

const Element* resolveSvgGradientRef(const Element& shape, std::string_view paint,
                                     bool& radial) {
    Document* document = shape.document();
    if (!document) {
        return nullptr;
    }
    const std::string_view fragment = urlPaintFragment(paint);
    if (fragment.size() < 2 || fragment.front() != '#') {
        return nullptr;
    }
    Element* target = document->getElementById(fragment.substr(1));
    if (!target) {
        return nullptr;
    }
    if (target->tag() == "linearGradient") {
        radial = false;
        return target;
    }
    if (target->tag() == "radialGradient") {
        radial = true;
        return target;
    }
    return nullptr;
}

bool svgPaintFallbackColor(std::string_view paint, Color& out) {
    const std::string_view token = trim(paint);
    if (token.size() < 6 || token.substr(0, 4) != "url(") {
        return false;
    }
    const std::size_t close = token.find(')');
    if (close == std::string_view::npos) {
        return false;
    }
    const std::string_view fallback = trim(token.substr(close + 1));
    if (fallback.empty()) {
        return false;
    }
    SDL_Color parsed{0, 0, 0, 255};
    if (!parseColor(fallback, parsed)) {
        return false;
    }
    out = toColor(parsed);
    return true;
}

bool svgPaintIsUrl(std::string_view paint) {
    const std::string_view token = trim(paint);
    return token.size() >= 5 && token.substr(0, 4) == "url(" &&
           token.find(')') != std::string_view::npos;
}

bool parseSvgGradient(const Element& gradient, bool radial, SvgGradient& out) {
    out = SvgGradient{};
    out.radial = radial;

    const std::string_view units = trim(gradient.attribute("gradientUnits"));
    if (units == "userSpaceOnUse") {
        out.objectBoundingBox = false;
    } else if (!units.empty() && units != "objectBoundingBox") {
        // An invalid units value is ignored and the default stands.
        out.objectBoundingBox = true;
    }

    if (radial) {
        out.cx = gradientNumber(gradient.attribute("cx"), 1.0f, 0.5f);
        out.cy = gradientNumber(gradient.attribute("cy"), 1.0f, 0.5f);
        out.r = std::max(0.0f, gradientNumber(gradient.attribute("r"), 1.0f, 0.5f));
    } else {
        out.x1 = gradientNumber(gradient.attribute("x1"), 1.0f, 0.0f);
        out.y1 = gradientNumber(gradient.attribute("y1"), 1.0f, 0.0f);
        out.x2 = gradientNumber(gradient.attribute("x2"), 1.0f, 1.0f);
        out.y2 = gradientNumber(gradient.attribute("y2"), 1.0f, 0.0f);
    }

    float last = 0.0f;
    for (const auto& child : gradient.children()) {
        if (!child || child->isTextNode() || child->tag() != "stop") {
            continue;
        }
        SvgGradientStop stop;
        stop.offset = std::clamp(gradientNumber(child->attribute("offset"), 1.0f, last),
                                 0.0f, 1.0f);
        // Stops never move backwards; a later smaller offset takes the
        // previous stop's place, as the spec's clamping rule says.
        if (stop.offset < last) {
            stop.offset = last;
        }
        last = stop.offset;
        stop.color = Color{0, 0, 0, 255};
        const std::string_view stopColor = trim(child->attribute("stop-color"));
        if (!stopColor.empty()) {
            SDL_Color parsed{0, 0, 0, 255};
            if (parseColor(stopColor, parsed)) {
                stop.color = toColor(parsed);
            }
        }
        stop.color.a = static_cast<std::uint8_t>(
            std::lround(static_cast<float>(stop.color.a) *
                        stopOpacity(child->attribute("stop-opacity"), 1.0f)));
        out.stops.push_back(stop);
    }
    return !out.stops.empty();
}

Shader svgGradientShader(const SvgGradient& gradient, const SDL_FRect& bbox,
                         const SvgView& view, float alpha) {
    std::vector<GradientStop> stops;
    stops.reserve(gradient.stops.size());
    for (const SvgGradientStop& stop : gradient.stops) {
        stops.push_back(GradientStop{stop.offset, multiplyAlpha(stop.color, alpha)});
    }

    if (gradient.radial) {
        RadialGradient radial;
        if (gradient.objectBoundingBox) {
            radial.cx = bbox.x + gradient.cx * bbox.w;
            radial.cy = bbox.y + gradient.cy * bbox.h;
            radial.radiusX = gradient.r * bbox.w;
            radial.radiusY = gradient.r * bbox.h;
        } else {
            radial.cx = view.x + gradient.cx * view.sx;
            radial.cy = view.y + gradient.cy * view.sy;
            radial.radiusX = gradient.r * view.sx;
            radial.radiusY = gradient.r * view.sy;
        }
        radial.focalX = radial.cx;
        radial.focalY = radial.cy;
        radial.stops = std::move(stops);
        return Shader::radial(radial);
    }

    LinearGradient linear;
    if (gradient.objectBoundingBox) {
        linear.x1 = bbox.x + gradient.x1 * bbox.w;
        linear.y1 = bbox.y + gradient.y1 * bbox.h;
        linear.x2 = bbox.x + gradient.x2 * bbox.w;
        linear.y2 = bbox.y + gradient.y2 * bbox.h;
    } else {
        linear.x1 = view.x + gradient.x1 * view.sx;
        linear.y1 = view.y + gradient.y1 * view.sy;
        linear.x2 = view.x + gradient.x2 * view.sx;
        linear.y2 = view.y + gradient.y2 * view.sy;
    }
    linear.stops = std::move(stops);
    return Shader::linear(linear);
}

}  // namespace sui

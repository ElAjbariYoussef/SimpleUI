#include "Rendering/SvgMarker.h"

#include <cmath>
#include <cstdlib>

namespace sui {

namespace {

// A bare number with an optional `url()` wrapper stripped elsewhere; trailing
// garbage after the number reads as absent, so `10px` never sizes a marker.
bool svgMarkerNumber(std::string_view text, float& out) {
    const std::size_t first = text.find_first_not_of(" \t\n\r\f");
    if (first == std::string_view::npos) {
        return false;
    }
    const std::string number(text.substr(first));
    char* stop = nullptr;
    const float value = std::strtof(number.c_str(), &stop);
    if (stop == number.c_str()) {
        return false;
    }
    const char* end = stop;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' ||
           *end == '\f') {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }
    out = value;
    return true;
}

// The fragment of `url(#id)`, empty for anything else. Quotes inside the
// parentheses are stripped, like the image URL unwrapping.
std::string_view markerUrlFragment(std::string_view attr) {
    const std::size_t first = attr.find_first_not_of(" \t\n\r\f");
    if (first == std::string_view::npos) {
        return {};
    }
    std::string_view token = attr.substr(first);
    if (token.size() < 6 || token.substr(0, 4) != "url(" || token.back() != ')') {
        return {};
    }
    std::string_view args = token.substr(4, token.size() - 5);
    const std::size_t start = args.find_first_not_of(" \t\n\r\f");
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t stop = args.find_last_not_of(" \t\n\r\f");
    args = args.substr(start, stop - start + 1);
    if (args.size() >= 2 && (args.front() == '"' || args.front() == '\'') &&
        args.back() == args.front()) {
        args = args.substr(1, args.size() - 2);
    }
    return args;
}

}  // namespace

SDL_FPoint svgMarkerPlace(const SvgMarkerFrame& frame, float mx, float my) {
    const float dx = frame.scale * mx + frame.offX;
    const float dy = frame.scale * my + frame.offY;
    return SDL_FPoint{frame.vertexX + frame.cosA * dx - frame.sinA * dy,
                      frame.vertexY + frame.sinA * dx + frame.cosA * dy};
}

const Element* resolveMarkerRef(Document& document, std::string_view attr) {
    const std::string_view fragment = markerUrlFragment(attr);
    if (fragment.size() < 2 || fragment.front() != '#') {
        return nullptr;
    }
    Element* target = document.getElementById(fragment.substr(1));
    if (!target || target->tag() != "marker") {
        return nullptr;
    }
    return target;
}

SvgMarkerFrame svgMarkerFrame(const Element& marker, float vertexX, float vertexY,
                               float tangentDeg, float strokeWidth, bool reverse) {
    SvgMarkerFrame frame;
    frame.vertexX = vertexX;
    frame.vertexY = vertexY;
    // Marker units default to the referencing stroke width; userSpaceOnUse
    // measures raw user units instead.
    float unit = strokeWidth > 0.0f ? strokeWidth : 1.0f;
    const std::string_view units = marker.attribute("markerUnits");
    if (!units.empty() && units != "strokeWidth") {
        if (units == "userSpaceOnUse") {
            unit = 1.0f;
        } else {
            return frame;  // garbage units place nothing
        }
    }
    float width = 3.0f;
    float w = 0.0f;
    if (svgMarkerNumber(marker.attribute("markerWidth"), w) && w > 0.0f) {
        width = w;
    }
    float height = 3.0f;
    float h = 0.0f;
    if (svgMarkerNumber(marker.attribute("markerHeight"), h) && h > 0.0f) {
        height = h;
    }
    float refX = 0.0f;
    svgMarkerNumber(marker.attribute("refX"), refX);
    float refY = 0.0f;
    svgMarkerNumber(marker.attribute("refY"), refY);
    // The viewBox meets into the markerWidth/markerHeight viewport; without
    // one, user units map 1:1 through the unit scale.
    float scale = unit;
    float centerX = 0.0f;
    float centerY = 0.0f;
    float box[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (parseSvgViewBox(marker.attribute("viewBox"), box) && box[2] > 0.0f &&
        box[3] > 0.0f) {
        const float meet =
            std::min((width * unit) / box[2], (height * unit) / box[3]);
        scale = meet;
        centerX = (width * unit - box[2] * meet) * 0.5f - box[0] * meet;
        centerY = (height * unit - box[3] * meet) * 0.5f - box[1] * meet;
    }
    frame.scale = scale;
    frame.offX = centerX - scale * refX;
    frame.offY = centerY - scale * refY;
    // `auto` follows the path tangent; a bare number is degrees clockwise in
    // y-down coordinates, matching atan2 below. Anything else holds 0.
    float degrees = 0.0f;
    const std::string_view orient = marker.attribute("orient");
    if (orient == "auto") {
        degrees = tangentDeg;
    } else if (orient == "auto-start-reverse") {
        degrees = tangentDeg + (reverse ? 180.0f : 0.0f);
    } else if (!orient.empty()) {
        float parsed = 0.0f;
        if (svgMarkerNumber(orient, parsed)) {
            degrees = parsed;
        }
    }
    const float radians = degrees * static_cast<float>(M_PI) / 180.0f;
    frame.cosA = std::cos(radians);
    frame.sinA = std::sin(radians);
    return frame;
}

float svgVertexAngle(const std::vector<SDL_FPoint>& points, std::size_t at, bool end,
                     bool closed) {
    if (points.size() < 2 || at >= points.size()) {
        return 0.0f;
    }
    const std::size_t n = points.size();
    const auto seg = [&](std::size_t a, std::size_t b, bool& found, SDL_FPoint& p,
                         SDL_FPoint& q) {
        if (points[a].x != points[b].x || points[a].y != points[b].y) {
            p = points[a];
            q = points[b];
            found = true;
        }
    };
    SDL_FPoint from{0.0f, 0.0f};
    SDL_FPoint to{0.0f, 0.0f};
    bool found = false;
    if (!end) {
        // Outgoing: the first nonzero segment at or after this vertex,
        // wrapping past the seam of a closed loop only.
        for (std::size_t k = 0; k < n && !found; ++k) {
            const std::size_t a = (at + k) % n;
            const std::size_t b = (a + 1) % n;
            if (!closed && b <= a) {
                break;
            }
            seg(a, b, found, from, to);
        }
    } else {
        // Incoming: the last nonzero segment at or before this vertex,
        // wrapping back from the seam of a closed loop only.
        for (std::size_t k = 0; k < n && !found; ++k) {
            const std::size_t b = (at + n - k) % n;
            const std::size_t a = (b + n - 1) % n;
            if (!closed && a >= b) {
                break;
            }
            seg(a, b, found, from, to);
        }
    }
    if (!found) {
        return 0.0f;
    }
    return std::atan2(to.y - from.y, to.x - from.x) * 180.0f /
           static_cast<float>(M_PI);
}

}  // namespace sui

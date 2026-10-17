#include "Rendering/SvgText.h"

#include <cmath>
#include <cstdlib>

namespace sui {

namespace {

// A bare user-unit number with an optional percent against `base`. False when
// nothing parses, so garbage positions read as absent.
bool svgUserNumber(std::string_view text, float base, float& out) {
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
    out = (*end == '%') ? value * base * 0.01f : value;
    return true;
}

// The first number of a comma/space-separated list, which is all chunk
// positioning reads. Multi-value lists assign per glyph, out of scope.
bool svgFirstNumber(std::string_view text, float base, float& out) {
    return svgUserNumber(text, base, out);
}

bool styleTag(std::string_view tag) {
    return tag == "tspan" || tag == "text" || tag == "textPath" || tag == "g" ||
           tag == "svg" || tag == "a";
}

}  // namespace

SvgTextStyle svgTextStyle(const Element& node, float cssFontSize, std::string_view cssFamily,
                          float scaleX) {
    SvgTextStyle style;
    style.pxSize = cssFontSize;
    style.family = std::string(cssFamily);
    bool fillSeen = false;
    bool sizeSeen = false;
    bool familySeen = false;
    for (const Element* at = &node; at; at = at->parent()) {
        const std::string_view tag = at->tag();
        if (!styleTag(tag)) {
            break;
        }
        if (!fillSeen) {
            const std::string_view raw = at->attribute("fill");
            const std::size_t first = raw.find_first_not_of(" \t\n\r\f");
            if (first != std::string_view::npos) {
                const std::size_t last = raw.find_last_not_of(" \t\n\r\f");
                const std::string_view fill = raw.substr(first, last - first + 1);
                if (fill == "none") {
                    style.hasFill = false;
                    fillSeen = true;
                } else {
                    SDL_Color parsed{0, 0, 0, 255};
                    if (parseColor(fill, parsed)) {
                        style.fill = {parsed.r, parsed.g, parsed.b, parsed.a};
                        style.hasFill = true;
                        fillSeen = true;
                    }
                }
            }
        }
        if (!sizeSeen) {
            const std::string_view raw = at->attribute("font-size");
            if (!raw.empty() && raw.find_first_not_of(" \t\n\r\f") !=
                                    std::string_view::npos) {
                style.pxSize = svgTextPxSize(raw, cssFontSize, scaleX);
                sizeSeen = true;
            }
        }
        if (!familySeen) {
            const std::string_view raw = at->attribute("font-family");
            const std::size_t first = raw.find_first_not_of(" \t\n\r\f\"'");
            if (first != std::string_view::npos) {
                const std::size_t last = raw.find_last_not_of(" \t\n\r\f\"'");
                style.family = std::string(raw.substr(first, last - first + 1));
                familySeen = true;
            }
        }
        if (fillSeen && sizeSeen && familySeen) {
            break;
        }
    }
    if (!fillSeen) {
        style.hasFill = true;
        style.fill = {0, 0, 0, 255};
    }
    return style;
}

std::vector<SvgTextChunk> collectSvgTextChunks(const Element& text, float cssFontSize,
                                               std::string_view cssFamily, float scaleX,
                                               float scaleY, float baseW, float baseH) {
    std::vector<SvgTextChunk> chunks;
    // Joint whitespace collapsing: trailing and leading spaces meeting at a
    // chunk boundary become exactly one (or none at the string ends), the way
    // one collapsed string would read. A flush commits a pending space and
    // suppresses the next chunk's leading spaces; the final flush strips.
    bool pendingSpace = false;
    bool started = false;
    bool suppressLeading = false;
    std::string current;
    const Element* currentNode = &text;
    const Element* currentPath = nullptr;
    bool currentPositioned = false;
    float currentX = 0.0f, currentY = 0.0f, currentDx = 0.0f, currentDy = 0.0f;

    const auto flush = [&](bool final) {
        if (pendingSpace) {
            if (!final) {
                // The boundary space survives once; the next chunk's leading
                // spaces then have nothing left to add.
                current += ' ';
                suppressLeading = true;
            }
            pendingSpace = false;
        }
        if (!current.empty()) {
            SvgTextChunk chunk;
            chunk.text = current;
            chunk.styleNode = currentNode;
            chunk.pathNode = currentPath;
            chunk.positioned = currentPositioned;
            chunk.x = currentX;
            chunk.y = currentY;
            chunk.dx = currentDx;
            chunk.dy = currentDy;
            chunk.style = svgTextStyle(*currentNode, cssFontSize, cssFamily, scaleX);
            chunks.push_back(std::move(chunk));
            current.clear();
        }
        currentPositioned = false;
        currentDx = 0.0f;
        currentDy = 0.0f;
    };
    const auto feed = [&](std::string_view piece) {
        for (char c : piece) {
            const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                               c == '\f';
            if (space) {
                if (!suppressLeading) {
                    pendingSpace = started;
                }
                continue;
            }
            if (pendingSpace) {
                current += ' ';
                pendingSpace = false;
            }
            suppressLeading = false;
            current += c;
            started = true;
        }
    };
    // A positioned or styled boundary starts a new chunk. Positioning reads
    // x/y/dx/dy off the boundary node itself.
    const auto boundary = [&](const Element& node) {
        flush(false);
        currentNode = &node;
        float v = 0.0f;
        if (svgFirstNumber(node.attribute("x"), baseW, v)) {
            currentX = v * scaleX;
            currentPositioned = true;
        }
        if (svgFirstNumber(node.attribute("y"), baseH, v)) {
            currentY = v * scaleY;
            currentPositioned = true;
        }
        if (svgFirstNumber(node.attribute("dx"), baseW, v)) {
            currentDx = v * scaleX;
        }
        if (svgFirstNumber(node.attribute("dy"), baseH, v)) {
            currentDy = v * scaleY;
        }
    };
    std::function<void(const Element&)> walk = [&](const Element& el) {
        for (const auto& child : el.children()) {
            if (!child) {
                continue;
            }
            if (child->isTextNode()) {
                feed(child->text());
                continue;
            }
            // A chunk boundary: tspan positions and styles; textPath does too,
            // and everything under it rides the path. Any other element lends
            // its text to the running chunk, which keeps the surrounding style.
            if (child->tag() == "tspan" || child->tag() == "textPath") {
                boundary(*child);
                const Element* savedPath = currentPath;
                if (child->tag() == "textPath") {
                    // Positioning lists belong to straight text: a path and
                    // its startOffset place these chunks instead.
                    currentPositioned = false;
                    currentDx = 0.0f;
                    currentDy = 0.0f;
                    currentPath = child.get();
                }
                walk(*child);
                flush(false);
                currentNode = &el;
                currentPath = savedPath;
            } else {
                walk(*child);
            }
        }
    };
    // The text element's own x/y position the first chunk. The last
    // flush strips, so trailing markup space never paints.
    boundary(text);
    walk(text);
    flush(true);
    return chunks;
}

float svgLoopsLength(const std::vector<std::vector<SDL_FPoint>>& loops) {
    float total = 0.0f;
    for (const auto& loop : loops) {
        for (std::size_t k = 1; k < loop.size(); ++k) {
            const float dx = loop[k].x - loop[k - 1].x;
            const float dy = loop[k].y - loop[k - 1].y;
            total += std::sqrt(dx * dx + dy * dy);
        }
    }
    return total;
}

bool svgPointAtDistance(const std::vector<std::vector<SDL_FPoint>>& loops, float distance,
                        SvgPathPoint& out) {
    if (distance < 0.0f) {
        return false;
    }
    for (const auto& loop : loops) {
        for (std::size_t k = 1; k < loop.size(); ++k) {
            const float dx = loop[k].x - loop[k - 1].x;
            const float dy = loop[k].y - loop[k - 1].y;
            const float span = std::sqrt(dx * dx + dy * dy);
            if (span <= 0.0f) {
                continue;
            }
            if (distance <= span) {
                const float t = distance / span;
                out.x = loop[k - 1].x + dx * t;
                out.y = loop[k - 1].y + dy * t;
                out.angleDeg =
                    std::atan2(dy, dx) * 180.0f / static_cast<float>(M_PI);
                return true;
            }
            distance -= span;
        }
    }
    return false;
}

}  // namespace sui

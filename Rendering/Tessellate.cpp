#include "Rendering/Tessellate.h"

#include <algorithm>
#include <cmath>

namespace sui {

namespace {

float cross(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& c) {
    // The z of (b - a) x (c - b): the sign names the turn at b.
    return (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
}

std::string_view trimSvgToken(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\n\r\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\n\r\f");
    return text.substr(first, last - first + 1);
}

}  // namespace

bool isConvexLoop(const std::vector<SDL_FPoint>& loop) {
    const std::size_t n = loop.size();
    if (n < 3) {
        return false;
    }
    int sign = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const float turn =
            cross(loop[i], loop[(i + 1) % n], loop[(i + 2) % n]);
        if (turn == 0.0f) {
            continue;
        }
        const int here = turn > 0.0f ? 1 : -1;
        if (sign == 0) {
            sign = here;
        } else if (sign != here) {
            return false;
        }
    }
    return true;
}

std::vector<SDL_FPoint> tessellateFill(
    const std::vector<std::vector<SDL_FPoint>>& loops, bool evenOdd) {
    // Band boundaries: every distinct vertex height. Zero-height bands and
    // empty spans contribute nothing below.
    std::vector<float> heights;
    for (const auto& loop : loops) {
        if (loop.size() < 3) {
            continue;
        }
        for (const SDL_FPoint& p : loop) {
            heights.push_back(p.y);
        }
    }
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
    std::vector<SDL_FPoint> triangles;
    for (std::size_t h = 0; h + 1 < heights.size(); ++h) {
        const float yb = heights[h];
        const float yt = heights[h + 1];
        if (!(yt > yb)) {
            continue;
        }
        const float ym = (yb + yt) * 0.5f;
        // Crossings of the midline, with winding for the nonzero rule. An
        // edge counts when the midline passes strictly inside its span, so a
        // vertex shared by two edges counts exactly once.
        std::vector<std::pair<float, int>> cuts;
        for (const auto& loop : loops) {
            const std::size_t n = loop.size();
            if (n < 3) {
                continue;
            }
            for (std::size_t i = 0; i < n; ++i) {
                const SDL_FPoint& a = loop[i];
                const SDL_FPoint& b = loop[(i + 1) % n];
                const float ylo = std::min(a.y, b.y);
                const float yhi = std::max(a.y, b.y);
                if (!(ylo <= ym && ym < yhi)) {
                    continue;
                }
                const float t = (ym - a.y) / (b.y - a.y);
                cuts.emplace_back(a.x + t * (b.x - a.x), b.y > a.y ? 1 : -1);
            }
        }
        std::sort(cuts.begin(), cuts.end(),
                  [](const auto& p, const auto& q) { return p.first < q.first; });
        std::vector<std::pair<float, float>> spans;
        if (evenOdd) {
            for (std::size_t k = 0; k + 1 < cuts.size(); k += 2) {
                spans.push_back({cuts[k].first, cuts[k + 1].first});
            }
        } else {
            int winding = 0;
            float start = 0.0f;
            bool inside = false;
            for (const auto& [x, w] : cuts) {
                if (!inside && winding == 0) {
                    start = x;
                    inside = true;
                }
                winding += w;
                if (inside && winding == 0) {
                    spans.push_back({start, x});
                    inside = false;
                }
            }
        }
        for (const auto& [x0, x1] : spans) {
            if (!(x1 > x0)) {
                continue;
            }
            const SDL_FPoint tl{x0, yb};
            const SDL_FPoint tr{x1, yb};
            const SDL_FPoint bl{x0, yt};
            const SDL_FPoint br{x1, yt};
            triangles.insert(triangles.end(), {tl, bl, tr, tr, bl, br});
        }
    }
    return triangles;
}

bool svgFillEvenOdd(const Element& el) {
    for (const Element* node = &el; node; node = node->parent()) {
        const std::string_view rule = trimSvgToken(node->attribute("fill-rule"));
        if (rule == "evenodd") {
            return true;
        }
        if (rule == "nonzero") {
            return false;
        }
    }
    return false;
}

}  // namespace sui

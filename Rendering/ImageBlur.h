#pragma once

#include <cstdint>

namespace sui {

// Separable two-pass box blur over a tightly packed row-major RGBA image (4
// bytes per pixel). Edges replicate the border pixel, so the blur stays inside
// the given rectangle. Radius is in pixels; 0 or below is a no-op. Modifies
// `rgba` in place.
void blurRGBA(std::uint8_t* rgba, int width, int height, int radius);

}  // namespace sui
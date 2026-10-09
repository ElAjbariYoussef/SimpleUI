#pragma once

#include <cstdint>
#include <string_view>

namespace sui {

// Parses the colour-shifting `filter:` functions -- brightness, contrast,
// saturate, grayscale, sepia, invert and hue-rotate -- into a single 3x4
// affine matrix. The layout is row-major, three rows of [R G B offset]:
//   R' = m[0]·R + m[1]·G + m[2]·B + m[3], and so on. Functions in the chain
// apply left to right (the first reaches the pixel first); alpha is untouched.
// `opacity` and `blur` are handled elsewhere. Returns true when any shift
// function was found, leaving `out` the identity otherwise.
bool filterColorMatrix(std::string_view filter, float out[12]);

// Applies an affine colour matrix to a tightly packed row-major RGBA image,
// rounding each channel and clamping into 0..255. Alpha is unchanged.
void applyColorMatrix(std::uint8_t* rgba, int width, int height, const float m[12]);

}  // namespace sui
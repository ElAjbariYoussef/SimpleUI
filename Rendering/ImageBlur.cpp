#include "Rendering/ImageBlur.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sui {

namespace {

// One horizontal box pass: each output pixel averages the 2r+1 columns centred
// on it, columns outside the row repeating the border pixel. `in` and `out`
// never alias.
void hPass(const std::uint8_t* in, std::uint8_t* out, int width, int height,
           int radius, int channel) {
    const int count = 2 * radius + 1;
    const int last = width - 1;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = in + y * 4 * width;
        std::uint8_t* orow = out + y * 4 * width;
        int sum = 0;
        for (int i = -radius; i <= radius; ++i) {
            sum += row[4 * std::clamp(i, 0, last) + channel];
        }
        for (int x = 0; x < width; ++x) {
            if (x > 0) {
                sum -= row[4 * std::clamp(x - radius - 1, 0, last) + channel];
                sum += row[4 * std::clamp(x + radius, 0, last) + channel];
            }
            orow[4 * x + channel] = static_cast<std::uint8_t>(sum / count);
        }
    }
}

// The vertical twin, striding over whole pixels so the columns line up.
void vPass(const std::uint8_t* in, std::uint8_t* out, int width, int height,
           int radius, int channel) {
    const int count = 2 * radius + 1;
    const int last = height - 1;
    const int pitch = 4 * width;
    for (int x = 0; x < width; ++x) {
        int sum = 0;
        for (int i = -radius; i <= radius; ++i) {
            sum += in[std::clamp(i, 0, last) * pitch + 4 * x + channel];
        }
        for (int y = 0; y < height; ++y) {
            if (y > 0) {
                sum -= in[std::clamp(y - radius - 1, 0, last) * pitch + 4 * x + channel];
                sum += in[std::clamp(y + radius, 0, last) * pitch + 4 * x + channel];
            }
            out[y * pitch + 4 * x + channel] = static_cast<std::uint8_t>(sum / count);
        }
    }
}

}  // namespace

void blurRGBA(std::uint8_t* rgba, int width, int height, int radius) {
    if (radius <= 0 || width <= 0 || height <= 0) {
        return;
    }
    std::vector<std::uint8_t> scratch(4ull * width * height);
    for (int channel = 0; channel < 4; ++channel) {
        hPass(rgba, scratch.data(), width, height, radius, channel);
        vPass(scratch.data(), rgba, width, height, radius, channel);
    }
}

}  // namespace sui
#include "Rendering/ColorMatrix.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace sui {

namespace {

// The rows the spec's colour-shifting matrices interpolate from. `saturate`
// and `grayscale` collapse to their luma row when fully applied; `sepia` to its
// sepia tone. Coefficient values come straight from CSS filter effects.
constexpr float kLumR = 0.2126f;
constexpr float kLumG = 0.7152f;
constexpr float kLumB = 0.0722f;
constexpr float kSatR = 0.213f;
constexpr float kSatG = 0.715f;
constexpr float kSatB = 0.072f;
constexpr float kSep[3][3] = {
    {0.393f, 0.769f, 0.189f},
    {0.349f, 0.686f, 0.168f},
    {0.272f, 0.534f, 0.131f},
};

void makeIdentity(float m[12]) {
    for (int i = 0; i < 12; ++i) {
        m[i] = 0.0f;
    }
    m[0] = m[5] = m[10] = 1.0f;
}

// `outer` carried out after `inner`: the affine product outer . inner. A 3x3
// linear part with a per-row offset, so the offset of the composition picks up
// inner's offset, run through outer's linear rows, plus outer's own.
void compose(const float outer[12], const float inner[12], float out[12]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += outer[r * 4 + k] * inner[k * 4 + c];
            }
            out[r * 4 + c] = sum;
        }
        out[r * 4 + 3] = outer[r * 4] * inner[3] + outer[r * 4 + 1] * inner[7] +
                         outer[r * 4 + 2] * inner[11] + outer[r * 4 + 3];
    }
}

bool parseNumber(std::string_view body, float& out) {
    const std::string owned(body);
    char* stop = nullptr;
    const float value = std::strtof(owned.c_str(), &stop);
    if (stop == owned.c_str()) {
        return false;
    }
    out = value;
    return true;
}

// Puts one shift function's matrix, with `amount` as its argument, into `fn`.
void buildFunction(std::string_view name, float amount, float fn[12]) {
    if (name == "brightness") {
        fn[0] = fn[5] = fn[10] = amount;
    } else if (name == "contrast") {
        const float offset = 128.0f * (1.0f - amount);
        fn[0] = fn[5] = fn[10] = amount;
        fn[3] = fn[7] = fn[11] = offset;
    } else if (name == "saturate") {
        const float n = 1.0f - amount;  // `amount` is the retained saturation.
        fn[0] = kSatR + (1.0f - kSatR) * amount;
        fn[1] = kSatG * n;
        fn[2] = kSatB * n;
        fn[4] = kSatR * n;
        fn[5] = kSatG + (1.0f - kSatG) * amount;
        fn[6] = kSatB * n;
        fn[8] = kSatR * n;
        fn[9] = kSatG * n;
        fn[10] = kSatB + (1.0f - kSatB) * amount;
    } else if (name == "grayscale") {
        // A lerp from the identity toward rows of luma coefficients.
        const float n = 1.0f - amount;  // `amount` is the fraction of luma used.
        fn[0] = kLumR * amount + n;
        fn[1] = kLumG * amount;
        fn[2] = kLumB * amount;
        fn[4] = kLumR * amount;
        fn[5] = kLumG * amount + n;
        fn[6] = kLumB * amount;
        fn[8] = kLumR * amount;
        fn[9] = kLumG * amount;
        fn[10] = kLumB * amount + n;
    } else if (name == "sepia") {
        const float n = 1.0f - amount;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                fn[r * 4 + c] = kSep[r][c] * amount + (r == c ? n : 0.0f);
            }
        }
    } else if (name == "invert") {
        const float scale = 1.0f - 2.0f * amount;
        const float offset = 255.0f * amount;
        fn[0] = fn[5] = fn[10] = scale;
        fn[3] = fn[7] = fn[11] = offset;
    } else if (name == "hue-rotate") {
        constexpr float kPi = 3.14159265f;
        const float rad = amount * kPi / 180.0f;
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        fn[0] = 0.213f + c * 0.787f - s * 0.213f;
        fn[1] = 0.715f - c * 0.715f - s * 0.715f;
        fn[2] = 0.072f - c * 0.072f + s * 0.928f;
        fn[4] = 0.213f - c * 0.213f + s * 0.143f;
        fn[5] = 0.715f + c * 0.285f + s * 0.140f;
        fn[6] = 0.072f - c * 0.072f - s * 0.283f;
        fn[8] = 0.213f - c * 0.213f - s * 0.787f;
        fn[9] = 0.715f - c * 0.715f + s * 0.715f;
        fn[10] = 0.072f + c * 0.928f + s * 0.072f;
    }
}

}  // namespace

bool filterColorMatrix(std::string_view filter, float out[12]) {
    float acc[12];
    makeIdentity(acc);
    bool any = false;
    std::size_t at = 0;
    while (at < filter.size()) {
        const std::size_t open = filter.find('(', at);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t close = filter.find(')', open);
        if (close == std::string_view::npos) {
            break;
        }
        std::size_t nameStart = open;
        while (nameStart > 0 &&
               (std::isalnum(static_cast<unsigned char>(filter[nameStart - 1])) ||
                filter[nameStart - 1] == '-')) {
            --nameStart;
        }
        const std::string_view name = filter.substr(nameStart, open - nameStart);
        const std::string_view body = filter.substr(open + 1, close - open - 1);
        // Only the seven shift functions belong here; `opacity`, `blur` and
        // `drop-shadow` are handled (or ignored) elsewhere, so they must not
        // mark the chain as shifted.
        const bool shiftFunction =
            name == "brightness" || name == "contrast" || name == "saturate" ||
            name == "grayscale" || name == "sepia" || name == "invert" ||
            name == "hue-rotate";
        if (!shiftFunction) {
            at = close + 1;
            continue;
        }
        float amount = 0.0f;
        if (name == "hue-rotate") {
            // No unit suffix; degrees are the only spelling worth accepting.
            parseNumber(body, amount);
        } else if (!parseNumber(body, amount)) {
            at = close + 1;
            continue;
        }
        float fn[12];
        makeIdentity(fn);
        buildFunction(name, amount, fn);
        float next[12];
        compose(fn, acc, next);
        for (int i = 0; i < 12; ++i) {
            acc[i] = next[i];
        }
        any = true;
        at = close + 1;
    }
    for (int i = 0; i < 12; ++i) {
        out[i] = acc[i];
    }
    return any;
}

void applyColorMatrix(std::uint8_t* rgba, int width, int height, const float m[12]) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const int bytes = 4 * width * height;
    for (int i = 0; i < bytes; i += 4) {
        const float r = rgba[i];
        const float g = rgba[i + 1];
        const float b = rgba[i + 2];
        const float nr = std::clamp(m[0] * r + m[1] * g + m[2] * b + m[3], 0.0f, 255.0f);
        const float ng = std::clamp(m[4] * r + m[5] * g + m[6] * b + m[7], 0.0f, 255.0f);
        const float nb = std::clamp(m[8] * r + m[9] * g + m[10] * b + m[11], 0.0f, 255.0f);
        // Round half away from zero, the way the spec's math lands on a byte.
        rgba[i] = static_cast<std::uint8_t>(std::lround(nr));
        rgba[i + 1] = static_cast<std::uint8_t>(std::lround(ng));
        rgba[i + 2] = static_cast<std::uint8_t>(std::lround(nb));
    }
}

}  // namespace sui
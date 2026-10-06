// How an animation or transition maps time progress (0..1) onto value progress.
//
// The named curves fold onto the cubic-bezier() form CSS gives them; the steps
// forms fold onto a Steps entry. Anything unparsable falls back to `linear`:
// CSS would invalidate the whole declaration, but the engine has no "previous
// value" to fall back to, so the nearest honest behaviour is a linear ramp
// (Difference.md §7.3).
#pragma once

#include <cstdint>
#include <string_view>

namespace sui {

struct Easing {
    enum class Type : std::uint8_t { Linear, CubicBezier, Steps };

    Type type = Type::Linear;
    // CubicBezier control points, as authored (P1 and P2; P0=(0,0), P3=(1,1)).
    // CSS constrains the x coordinates to [0,1], which is what keeps the curve
    // a function of time and therefore solvable by bisection.
    float x1 = 0.0f, y1 = 0.0f, x2 = 1.0f, y2 = 1.0f;
    // Steps count and whether the first jump happens at t=0 (jump-start) or
    // t=1/n (jump-end, the default). `step-start`/`step-end` are steps(1, ...).
    int steps = 1;
    bool stepStart = false;

    // Parses one timing-function token: the four names, `cubic-bezier(...)`,
    // `steps(...)` including the `start`/`end`/`jump-start`/`jump-end` keyword.
    // Never fails: unparsable input yields Linear, as documented above.
    static Easing parse(std::string_view value);

    // Maps a raw time progress to a value progress, both in [0,1].
    float apply(float t) const noexcept;
};

}  // namespace sui
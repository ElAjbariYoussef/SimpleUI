// Geometry checks for the renderer that need no device, so the parts a screenshot
// cannot pin down precisely still have a regression test.
#include "Rendering/ImageBlur.h"
#include "Rendering/Renderer.h"
#include "Rendering/Text.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void checkNear(float actual, float expected, float tolerance, const char* what) {
    ++checks;
    if (std::fabs(actual - expected) > tolerance) {
        ++failures;
        std::printf("FAIL %s: got %.3f want %.3f\n", what, actual, expected);
    }
}

// The four corners of a rounded rect sit on the radius circle, and every sampled
// point has to be inside the box.
void testOutlineStaysInBox() {
    const SDL_FRect rect{10.0f, 20.0f, 200.0f, 100.0f};
    const sui::Radii radii = sui::Radii::all(12.0f, 8.0f);
    const auto points = sui::Renderer::roundedOutline(rect, radii);
    check(points.size() > 8, "outline has enough samples");
    for (const SDL_FPoint& p : points) {
        check(p.x >= rect.x - 0.01f && p.x <= rect.x + rect.w + 0.01f,
              "outline x inside the box");
        check(p.y >= rect.y - 0.01f && p.y <= rect.y + rect.h + 0.01f,
              "outline y inside the box");
    }
}

// No rounding means the plain four corners, which is the path the fast fill uses.
void testSquareOutline() {
    const SDL_FRect rect{1.0f, 2.0f, 30.0f, 40.0f};
    const auto points = sui::Renderer::roundedOutline(rect, {});
    check(points.size() == 4, "square outline is four points");
    checkNear(points[0].x, rect.x, 0.001f, "square outline starts at the left edge");
    checkNear(points[0].y, rect.y, 0.001f, "square outline starts at the top edge");
}

// A zero-sized box has no outline to draw.
void testDegenerateBox() {
    check(sui::Renderer::roundedOutline({0.0f, 0.0f, 0.0f, 10.0f}, {}).empty(),
          "zero width has no outline");
    check(sui::Renderer::roundedOutline({0.0f, 0.0f, 10.0f, 0.0f}, {}).empty(),
          "zero height has no outline");
}

// CSS scales over-long radii down rather than letting corners overlap.
void testRadiiClamped() {
    const sui::Radii big = sui::Radii::all(40.0f, 40.0f);
    const sui::Radii fitted = big.clamped(30.0f, 20.0f);
    checkNear(fitted.topLeft.x, 15.0f, 0.001f, "two radii on the top edge fill it exactly");
    checkNear(fitted.topLeft.y, 10.0f, 0.001f, "two radii on the left edge fill it exactly");
    check(fitted.topLeft.x + fitted.topRight.x <= 30.0f + 0.001f, "horizontal radii fit");

    // Radii that already fit are left alone.
    const sui::Radii small = sui::Radii::all(5.0f, 5.0f);
    const sui::Radii untouched = small.clamped(100.0f, 100.0f);
    checkNear(untouched.topLeft.x, 5.0f, 0.001f, "fitting does not shrink small radii");

    // A degenerate box cannot hold any radius.
    check(!big.clamped(0.0f, 10.0f).any(), "zero width drops every radius");
}

// The four radii are independent, so one rounded corner is expressible.
void testAsymmetricRadii() {
    sui::Radii r;
    r.topLeft = {10.0f, 4.0f};
    r.bottomRight = {2.0f, 6.0f};
    check(r.any(), "a single corner counts as rounded");
    check(!r.uniform(), "mixed radii are not uniform");
    check(sui::Radii::all(3.0f, 3.0f).uniform(), "equal radii are uniform");
    check(sui::Radii{}.uniform(), "no radii is trivially uniform");
    const sui::Radii fitted = r.clamped(200.0f, 200.0f);
    checkNear(fitted.topLeft.x, 10.0f, 0.001f, "asymmetric x survives clamping");
    checkNear(fitted.bottomRight.y, 6.0f, 0.001f, "asymmetric y survives clamping");
}

// Group opacity multiplies into the alpha rather than replacing it.
void testOpacity() {
    const sui::Color half = sui::withOpacity(sui::Color{10, 20, 30, 200}, 0.5f);
    checkNear(half.a, 100.0f, 0.001f, "opacity halves the alpha channel");
    checkNear(half.r, 10.0f, 0.001f, "opacity leaves the colour alone");

    const sui::Color clipped = sui::withOpacity(sui::Color{0, 0, 0, 255}, 4.0f);
    checkNear(clipped.a, 255.0f, 0.001f, "opacity is clamped to one");
    checkNear(sui::withOpacity(sui::Color{0, 0, 0, 255}, -1.0f).a, 0.0f, 0.001f,
              "opacity is clamped to zero");
    checkNear(sui::multiplyAlpha(sui::Color{0, 0, 0, 200}, 0.25f).a, 50.0f, 0.001f,
              "nested opacity multiplies");
}

// A gradient needs a mesh only when its colour is not an affine function of position.
void testMeshNeed() {
    sui::LinearGradient one;
    one.stops = {{0.0f, sui::Color{}}, {1.0f, sui::Color{}}};
    sui::Shader twoStop = sui::Shader::linear(one);
    check(!twoStop.needsMesh(), "a two-stop ramp is affine");

    sui::LinearGradient three = one;
    three.stops.push_back({0.5f, sui::Color{}});
    sui::Shader multiStop = sui::Shader::linear(three);
    check(multiStop.needsMesh(), "a ramp with a stop in the middle is not");

    sui::RadialGradient radial;
    radial.radiusX = 50.0f;
    sui::Shader cone = sui::Shader::radial(radial);
    check(cone.needsMesh(), "a radial gradient is never affine");
    check(cone.meshStep() > 0.0f, "a radial gradient has a positive step");
    // A small radius bends faster, so it has to be cut finer.
    radial.radiusX = 10.0f;
    check(sui::Shader::radial(radial).meshStep() < cone.meshStep(),
          "a smaller radius is meshed finer");

    check(!sui::Shader::solid(sui::Color{}).needsMesh(), "a flat fill needs no mesh");
    check(sui::Shader::solid(sui::Color{}).isSolid(), "a flat fill is solid");
}

// A concentric radial gradient is the distance out over the radius; an off-centre
// one is the cone CSS defines, which starts at the focus rather than the centre.
void testRadialPosition() {
    sui::RadialGradient concentric;
    concentric.cx = 100.0f;
    concentric.cy = 100.0f;
    concentric.radiusX = 40.0f;
    concentric.radiusY = 40.0f;
    concentric.focalX = 100.0f;
    concentric.focalY = 100.0f;
    concentric.stops = {{0.0f, sui::Color{255, 255, 255, 255}}, {1.0f, sui::Color{0, 0, 0, 255}}};
    const sui::Shader ring = sui::Shader::radial(concentric);
    checkNear(ring.colorAt(100.0f, 100.0f).r, 255.0f, 2.0f, "the centre is the first stop");
    checkNear(ring.colorAt(140.0f, 100.0f).r, 0.0f, 2.0f, "the ending shape is the last stop");
    const sui::Color half = ring.colorAt(120.0f, 100.0f);
    checkNear(half.r, 128.0f, 3.0f, "half way out is half way along the ramp");

    // Off centre: position 0 must land on the focus, not on the box centre.
    sui::RadialGradient off = concentric;
    off.focalX = 80.0f;
    off.focalY = 90.0f;
    const sui::Shader cone = sui::Shader::radial(off);
    checkNear(cone.colorAt(80.0f, 90.0f).r, 255.0f, 2.0f, "the focus is the first stop");
    checkNear(cone.colorAt(165.0f, 145.0f).r, 0.0f, 2.0f, "the far side is the last stop");
    // The centre of the box sits part way along, not at either end.
    const float atCentre = cone.colorAt(100.0f, 100.0f).r / 255.0f;
    check(atCentre < 0.9f && atCentre > 0.1f, "the box centre is part way along the ramp");
}

// An elliptical radius has to put its arc on the ellipse, not on the circle its x
// radius describes, and the arc has to end where the straight sides begin.
void testEllipticalOutline() {
    const SDL_FRect rect{20.0f, 60.0f, 200.0f, 84.0f};
    const sui::Radii radii = sui::Radii::all(12.0f, 24.0f);
    const auto points = sui::Renderer::sampledOutline(rect, radii);
    constexpr int kPerCorner = 9;
    check(points.size() == kPerCorner * 4, "sampled outline samples every corner");
    const float cx = rect.x + 12.0f;
    const float cy = rect.y + 24.0f;
    for (int i = 0; i < kPerCorner; ++i) {
        const float nx = (points[i].x - cx) / 12.0f;
        const float ny = (points[i].y - cy) / 24.0f;
        checkNear(nx * nx + ny * ny, 1.0f, 0.01f, "the arc lies on the ellipse");
    }
    // The arc runs from the end of one straight side to the start of the next.
    checkNear(points.front().x, rect.x, 0.001f, "the arc starts on the left edge");
    checkNear(points.front().y, rect.y + 24.0f, 0.001f, "the arc starts a full ry down");
    checkNear(points[kPerCorner - 1].x, rect.x + 12.0f, 0.001f, "the arc ends on the top edge");
    checkNear(points[kPerCorner - 1].y, rect.y, 0.001f, "the arc ends on the top edge");
}

// A ring zips an outer outline against an inner one, so the two have to agree on how
// many points that is even where a radius has shrunk away to nothing.
void testSampledOutlineAlwaysSamples() {
    const SDL_FRect rect{0.0f, 0.0f, 100.0f, 60.0f};
    const auto square = sui::Renderer::roundedOutline(rect, {});
    check(square.size() == 4, "a square outline is four points");
    const auto sampled = sui::Renderer::sampledOutline(rect, {});
    check(sampled.size() > 4, "a sampled square outline still samples");
    // The border as wide as the radius collapses the inner radii to nothing, which is
    // exactly the case that used to leave the pair unmatched.
    const auto inner = sui::Renderer::sampledOutline(
        {rect.x + 12.0f, rect.y + 12.0f, rect.w - 24.0f, rect.h - 24.0f}, {});
    check(inner.size() == sampled.size(),
          "the inner outline matches the outer in point count");
}

// The four sides tile the outline: each point of the whole belongs to exactly one side,
// and the only overlap is a shared corner endpoint.
void testSideOutlinesTileTheOutline() {
    const SDL_FRect rect{10.0f, 20.0f, 120.0f, 80.0f};
    const sui::Radii radii = sui::Radii::all(10.0f, 16.0f);
    const auto whole = sui::Renderer::sampledOutline(rect, radii);
    int total = 0;
    for (const sui::Side side :
         {sui::Side::Top, sui::Side::Right, sui::Side::Bottom, sui::Side::Left}) {
        const auto points = sui::Renderer::sideOutline(rect, radii, side);
        check(points.size() >= 2, "each side has points to walk");
        for (const SDL_FPoint& p : points) {
            check(p.x >= rect.x - 0.01f && p.x <= rect.x + rect.w + 0.01f,
                  "side point inside the box in x");
            check(p.y >= rect.y - 0.01f && p.y <= rect.y + rect.h + 0.01f,
                  "side point inside the box in y");
        }
        total += static_cast<int>(points.size());
    }
    check(total == static_cast<int>(whole.size()) + 4,
          "the sides tile the outline with one shared point per corner");
}

// CSS `font-size` names the em square. stb_truetype's ScaleForPixelHeight instead
// maps the font's whole ascent-to-descent extent onto the size, which for DejaVu Sans
// -- 2384 units on a 2048 em -- draws every glyph 16% small. The expected values below
// are the font's own table divided by its em, so they only pass if the scale comes
// from the em.
void testFontSizeIsTheEmSquare() {
    sui::TextFont font;
    const std::string path = std::string(SUI_SOURCE_DIR) + "/assets/fonts/DejaVuSans.ttf";
    if (!font.loadFromFile(path)) {
        check(false, "the bundled font loads");
        return;
    }
    constexpr float kEm = 2048.0f;
    constexpr float kSize = 100.0f;

    const sui::TextFont::VerticalMetrics vm = font.verticalMetrics(kSize);
    checkNear(vm.ascent, 1901.0f / kEm * kSize, 0.05f, "ascent is a fraction of the em");
    checkNear(vm.descent, 483.0f / kEm * kSize, 0.05f, "descent is a fraction of the em");
    checkNear(vm.height, 2384.0f / kEm * kSize, 0.05f,
              "the line box is the font's own height, which is over one em");

    const sui::GlyphMetrics cap = font.metrics('H', kSize);
    checkNear(cap.advance, 1540.0f / kEm * kSize, 0.05f, "an advance is a fraction of the em");
    checkNear(cap.height, 1493.0f / kEm * kSize, 1.0f, "cap height matches the font table");
    checkNear(font.metrics('x', kSize).height, 1120.0f / kEm * kSize, 1.0f,
              "x-height matches the font table");

    // Every metric has to scale with the size, which is only true if the rasteriser
    // scale is derived from the requested em rather than from a fixed pixel height.
    checkNear(font.metrics('H', 50.0f).advance, cap.advance * 0.5f, 0.05f,
              "halving font-size halves the advance");
    checkNear(font.verticalMetrics(200.0f).height, vm.height * 2.0f, 0.1f,
              "doubling font-size doubles the line box");
}

// The CPU box blur `filter: blur()` hands the executor. The kernel averages a
// 2r+1 window with the border pixels replicating, so these values are exact
// hand-checkable fractions of 255 across a 5x5 image with an opaque red block
// in its middle (rows and columns 1..3).
void testBoxBlur() {
    auto sample = [](const std::vector<std::uint8_t>& img, int x, int y) {
        const int w = 5;
        return std::array<std::uint8_t, 4>{img[4 * (y * w + x)], img[4 * (y * w + x) + 1],
                                           img[4 * (y * w + x) + 2], img[4 * (y * w + x) + 3]};
    };
    std::vector<std::uint8_t> img(5 * 5 * 4, 0);
    for (int y = 1; y <= 3; ++y) {
        for (int x = 1; x <= 3; ++x) {
            img[4 * (y * 5 + x)] = 255;
            img[4 * (y * 5 + x) + 3] = 255;
        }
    }
    const std::vector<std::uint8_t> sharp = img;
    sui::blurRGBA(img.data(), 5, 5, 1);
    check(sample(img, 2, 2) == std::array<std::uint8_t, 4>{255, 0, 0, 255},
          "the heart of a uniform block is unchanged");
    check(sample(img, 1, 1) == std::array<std::uint8_t, 4>{113, 0, 0, 113},
          "a block corner averages 4 of 9 red samples");
    check(sample(img, 3, 2) == std::array<std::uint8_t, 4>{170, 0, 0, 170},
          "an edge coats 6 of 9 samples with red");
    check(sample(img, 4, 4) == std::array<std::uint8_t, 4>{28, 0, 0, 28},
          "the far transparent corner keeps most of its emptiness");

    // A uniform image blurs to itself, whatever the radius, and a non-positive
    // radius is a no-op.
    std::vector<std::uint8_t> flat(4 * 7 * 3, 255);
    sui::blurRGBA(flat.data(), 7, 3, 40);
    bool all = true;
    for (const std::uint8_t v : flat) {
        all = all && v == 255;
    }
    check(all, "a flat image is its own blur");
    std::vector<std::uint8_t> untouched = sharp;
    sui::blurRGBA(untouched.data(), 5, 5, 0);
    check(untouched == sharp, "radius zero leaves pixels alone");
}

}  // namespace

int main() {
    testOutlineStaysInBox();
    testEllipticalOutline();
    testSampledOutlineAlwaysSamples();
    testSideOutlinesTileTheOutline();
    testSquareOutline();
    testDegenerateBox();
    testRadiiClamped();
    testAsymmetricRadii();
    testOpacity();
    testMeshNeed();
    testRadialPosition();
    testFontSizeIsTheEmSquare();
    testBoxBlur();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

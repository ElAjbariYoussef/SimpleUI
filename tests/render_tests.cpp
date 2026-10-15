// Geometry checks for the renderer that need no device, so the parts a screenshot
// cannot pin down precisely still have a regression test.
#include "CSS/Style.h"
#include "Core/Application/Application.h"
#include "Core/Application/Background.h"
#include "Rendering/ClipMask.h"
#include "Rendering/ColorMatrix.h"
#include "Rendering/ImageBlur.h"
#include "Rendering/Renderer.h"
#include "Rendering/Text.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
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

// The CPU conic sweep the executor hands a `conic-gradient()` background.
// Twelve o'clock is the sweep start and angles run clockwise; `from` rotates
// the whole dial. Values below are hand-checkable fractions of a
// white-to-black ramp.
void testConicalPosition() {
    sui::ConicalGradient sweep;
    sweep.cx = 100.0f;
    sweep.cy = 100.0f;
    sweep.fromAngle = 0.0f;
    sweep.radius = 40.0f;
    sweep.stops = {{0.0f, sui::Color{255, 255, 255, 255}},
                   {1.0f, sui::Color{0, 0, 0, 255}}};
    const sui::Shader cone = sui::Shader::conical(sweep);
    checkNear(cone.colorAt(100.0f, 60.0f).r, 255.0f, 2.0f, "twelve o'clock starts the sweep");
    checkNear(cone.colorAt(140.0f, 100.0f).r, 191.0f, 3.0f, "three o'clock is a quarter turn");
    checkNear(cone.colorAt(100.0f, 140.0f).r, 128.0f, 3.0f, "six o'clock is half turn");
    checkNear(cone.colorAt(60.0f, 100.0f).r, 64.0f, 3.0f, "nine o'clock is three quarters");

    // `from` rotates the dial: with `from 90deg` twelve o'clock reads 0.75.
    sui::ConicalGradient turned = sweep;
    turned.fromAngle = 90.0f;
    checkNear(sui::Shader::conical(turned).colorAt(100.0f, 60.0f).r, 64.0f, 3.0f,
              "from 90deg moves twelve o'clock three quarters along");

    // Two stops at one offset are a hard edge: just before it reads as the
    // earlier colour, just after as the later one.
    sui::ConicalGradient hard = sweep;
    hard.stops = {{0.0f, sui::Color{0, 0, 0, 255}},
                  {0.25f, sui::Color{255, 0, 0, 255}},
                  {0.25f, sui::Color{0, 0, 255, 255}},
                  {1.0f, sui::Color{255, 255, 255, 255}}};
    const sui::Shader hardCone = sui::Shader::conical(hard);
    const sui::Color before = hardCone.colorAt(139.994f, 99.302f);
    const sui::Color after = hardCone.colorAt(139.994f, 100.698f);
    check(before.r > 200 && before.b < 55, "a double stop is red just before");
    check(after.b > 200 && after.r < 55, "a double stop is blue just after");

    check(sui::Shader::conical(sweep).needsMesh(), "a sweep needs a mesh");
    sui::ConicalGradient wide = sweep;
    wide.radius = 140.0f;
    check(sui::Shader::conical(wide).meshStep() == 10.0f, "the mesh step follows the radius");
}

// A gradient `mask-image` multiplies surface alpha by the painted mask: hard
// edges make every value exact, so a black-to-transparent ramp with doubled
// middle stops reads 255 on one side and 0 on the other, whatever the shape.
void testMaskImage() {
    using Mask = sui::ComputedStyle::MaskImage;
    const auto alphaAt = [](const std::vector<std::uint8_t>& img, int x, int y) {
        return img[4 * (y * 4 + x) + 3];
    };
    const auto opaque = [](float at) {
        return sui::ComputedStyle::GradientStop{
            sui::Length(sui::Length::Percent, at), SDL_Color{0, 0, 0, 255}};
    };
    const auto clear = [](float at) {
        return sui::ComputedStyle::GradientStop{
            sui::Length(sui::Length::Percent, at), SDL_Color{0, 0, 0, 0}};
    };

    // Left-to-right linear fade over a 4px box: the left half stays, the right
    // half goes.
    Mask linear;
    linear.kind = Mask::Kind::LinearGradient;
    linear.linear.angle = 90.0f;
    linear.linear.stops = {opaque(0.0f), opaque(50.0f), clear(50.0f), clear(100.0f)};
    std::vector<std::uint8_t> img(4 * 4 * 4, 0);
    for (int i = 0; i < 16; ++i) {
        img[4 * i + 3] = 255;
    }
    sui::applyMaskImage(img.data(), 4, 4, linear, SDL_FRect{0.0f, 0.0f, 4.0f, 4.0f},
                        16.0f, 16.0f);
    check(alphaAt(img, 0, 0) == 255 && alphaAt(img, 1, 2) == 255,
          "a linear mask keeps before its edge");
    check(alphaAt(img, 2, 0) == 0 && alphaAt(img, 3, 3) == 0,
          "a linear mask drops past its edge");

    // Centred radial fade: the middle stays, the corners go.
    Mask radial;
    radial.kind = Mask::Kind::RadialGradient;
    radial.radial.stops = {opaque(0.0f), opaque(50.0f), clear(50.0f), clear(100.0f)};
    std::vector<std::uint8_t> disc(4 * 4 * 4, 0);
    for (int i = 0; i < 16; ++i) {
        disc[4 * i + 3] = 255;
    }
    sui::applyMaskImage(disc.data(), 4, 4, radial, SDL_FRect{0.0f, 0.0f, 4.0f, 4.0f},
                        16.0f, 16.0f);
    check(alphaAt(disc, 1, 1) == 255, "a radial mask keeps its middle");
    check(alphaAt(disc, 0, 0) == 0, "a radial mask drops its corners");

    // A conic sweep: above the centre stays, below-left goes.
    Mask conic;
    conic.kind = Mask::Kind::ConicalGradient;
    conic.conical.angle = 0.0f;
    conic.conical.stops = {opaque(0.0f), opaque(25.0f), clear(25.0f), clear(100.0f)};
    std::vector<std::uint8_t> sweep(4 * 4 * 4, 0);
    for (int i = 0; i < 16; ++i) {
        sweep[4 * i + 3] = 255;
    }
    sui::applyMaskImage(sweep.data(), 4, 4, conic, SDL_FRect{0.0f, 0.0f, 4.0f, 4.0f},
                        16.0f, 16.0f);
    check(alphaAt(sweep, 2, 0) == 255, "a conic mask keeps before its edge");
    check(alphaAt(sweep, 0, 3) == 0, "a conic mask drops past its edge");

    // Masks that cannot apply leave pixels alone.
    Mask none;
    std::vector<std::uint8_t> flat(16, 200);
    sui::applyMaskImage(flat.data(), 2, 2, none, SDL_FRect{0.0f, 0.0f, 2.0f, 2.0f},
                        16.0f, 16.0f);
    check(flat == std::vector<std::uint8_t>(16, 200), "no mask leaves pixels alone");
    Mask url;
    url.kind = Mask::Kind::Url;
    url.href = "m.png";
    std::vector<std::uint8_t> pic(16, 200);
    sui::applyMaskImage(pic.data(), 2, 2, url, SDL_FRect{0.0f, 0.0f, 2.0f, 2.0f},
                        16.0f, 16.0f);
    check(pic[3] == 0 && pic[7] == 0, "a missing url mask hides");
    Mask empty = linear;
    empty.linear.stops.clear();
    std::vector<std::uint8_t> bare(16, 200);
    sui::applyMaskImage(bare.data(), 2, 2, empty, SDL_FRect{0.0f, 0.0f, 2.0f, 2.0f},
                        16.0f, 16.0f);
    check(bare == std::vector<std::uint8_t>(16, 200), "an empty mask stays quiet");
}

// Url layers sample decoded bytes stretched over the border box, read through
// `mask-mode` and combined through `mask-composite`. One-pixel images make
// every value exact, so these checks pin the math, not the resampler.
void testMaskLayers() {
    using Mask = sui::ComputedStyle::MaskImage;
    using Mode = sui::ComputedStyle::MaskMode;
    using Composite = sui::ComputedStyle::MaskComposite;
    const SDL_FRect dest{0.0f, 0.0f, 2.0f, 2.0f};
    const sui::BackgroundBoxes boxes{dest, dest, dest};
    sui::DecodedImage white{1, 1, {255, 255, 255, 255}};
    sui::DecodedImage black{1, 1, {0, 0, 0, 255}};
    sui::DecodedImage whiteClear{1, 1, {255, 255, 255, 0}};
    const auto fetchFor = [&](const sui::DecodedImage* img) {
        return sui::FetchMaskBytes{
            [img](std::string_view) -> const sui::DecodedImage* { return img; }};
    };
    const auto alphaOf = [](const std::vector<std::uint8_t>& img) { return img[3]; };
    auto urlLayer = [](const char* href) {
        Mask layer;
        layer.kind = Mask::Kind::Url;
        layer.href = href;
        return layer;
    };
    auto opaqueOf = [&](const sui::DecodedImage* img,
                        const std::vector<Mode>& modes = {}) {
        std::vector<std::uint8_t> pixels(16, 0);
        for (int i = 0; i < 4; ++i) {
            pixels[4 * i + 3] = 200;
        }
        sui::applyMaskLayers(pixels.data(), 2, 2, {urlLayer("m.png")}, modes, {}, {}, {},
                             {}, {}, {}, boxes, 16.0f, 16.0f, fetchFor(img));
        return alphaOf(pixels);
    };

    // `match-source` (the default) is luminance for urls: white keeps, black
    // hides, whatever the alpha channel says.
    check(opaqueOf(&white) == 200, "a white url keeps by luminance");
    check(opaqueOf(&black) == 0, "a black url hides by luminance");
    check(opaqueOf(&whiteClear) == 200, "luminance ignores alpha");
    // `alpha` reads the channel instead: the same clear white now hides.
    check(opaqueOf(&whiteClear, {Mode::Alpha}) == 0, "alpha mode reads the channel");
    check(opaqueOf(&white, {Mode::Alpha}) == 200, "an opaque url keeps in alpha");
    check(opaqueOf(&black, {Mode::Luminance}) == 0, "an opaque black hides in luminance");

    // Composites combine two constant layers: white (1) against black (0) and
    // white against white.
    const auto compositeOf = [&](const sui::DecodedImage* first,
                                 const sui::DecodedImage* second, Composite op) {
        std::vector<std::uint8_t> pixels(16, 0);
        for (int i = 0; i < 4; ++i) {
            pixels[4 * i + 3] = 200;
        }
        std::map<std::string, const sui::DecodedImage*> byHref{
            {"a.png", first}, {"b.png", second}};
        sui::FetchMaskBytes fetch{[&](std::string_view href)
                                      -> const sui::DecodedImage* {
            auto found = byHref.find(std::string(href));
            return found == byHref.end() ? nullptr : found->second;
        }};
        sui::applyMaskLayers(pixels.data(), 2, 2, {urlLayer("a.png"), urlLayer("b.png")},
                             {}, {op}, {}, {}, {}, {}, {}, boxes, 16.0f, 16.0f,
                             std::move(fetch));
        return alphaOf(pixels);
    };
    check(compositeOf(&white, &black, Composite::Add) == 200, "add unions");
    check(compositeOf(&white, &black, Composite::Subtract) == 200, "subtract keeps the rest");
    check(compositeOf(&black, &white, Composite::Subtract) == 0,
          "subtract cuts to zero");
    check(compositeOf(&white, &black, Composite::Intersect) == 0, "intersect multiplies");
    check(compositeOf(&white, &white, Composite::Intersect) == 200,
          "intersect keeps twice-kept");
    check(compositeOf(&white, &black, Composite::Exclude) == 200, "exclude xors");
    check(compositeOf(&white, &white, Composite::Exclude) == 0,
          "exclude drops twice-kept");
}

// Each mask layer paints into its own sized/placed destination: `mask-size`
// shrinks it, `mask-position` moves it within the origin box, `mask-repeat`
// tiles it, and `mask-clip` cuts it. Outside the painted area the mask is
// transparent. A 1px white url on a 2px box makes every case exact.
void testMaskGeometry() {
    using Mask = sui::ComputedStyle::MaskImage;
    using Size = sui::ComputedStyle::BackgroundSize;
    using Position = sui::ComputedStyle::BackgroundPosition;
    using Repeat = sui::ComputedStyle::RepeatPair;
    using Box = sui::ComputedStyle::BackgroundBox;
    const SDL_FRect area{0.0f, 0.0f, 2.0f, 2.0f};
    const sui::BackgroundBoxes boxes{area, area, area};
    sui::DecodedImage white{1, 1, {255, 255, 255, 255}};
    sui::FetchMaskBytes fetch{[&](std::string_view)
                                  -> const sui::DecodedImage* { return &white; }};
    auto urlLayer = []() {
        Mask layer;
        layer.kind = Mask::Kind::Url;
        layer.href = "m.png";
        return layer;
    };
    const auto pxSize = [](float w, float h) {
        Size size;
        size.mode = Size::Explicit;
        size.width = sui::Length(w);
        size.height = sui::Length(h);
        return size;
    };
    const auto alphasOf = [&](const std::vector<Size>& sizes,
                              const std::vector<Position>& positions,
                              const std::vector<Repeat>& repeats,
                              const std::vector<Box>& origins,
                              const std::vector<Box>& clips,
                              const sui::BackgroundBoxes& useBoxes) {
        std::vector<std::uint8_t> pixels(16, 0);
        for (int i = 0; i < 4; ++i) {
            pixels[4 * i + 3] = 200;
        }
        sui::applyMaskLayers(pixels.data(), 2, 2, {urlLayer()}, {}, {}, sizes,
                             positions, repeats, origins, clips, useBoxes, 16.0f,
                             16.0f, fetch);
        return std::array<std::uint8_t, 4>{pixels[3], pixels[7], pixels[11],
                                           pixels[15]};
    };
    const Repeat noRepeat{false, false};
    const Repeat repeatX{true, false};
    const Repeat tile{true, true};
    const Position topLeft{Position::Start, Position::Start};
    const Position bottomRight{Position::End, Position::End};

    // A 1px tile on a 2px box covers everything; no-repeat keeps only its cell.
    check(alphasOf({}, {}, {}, {}, {}, boxes) ==
              std::array<std::uint8_t, 4>{200, 200, 200, 200},
          "a default tile covers the box");
    check(alphasOf({pxSize(1.0f, 1.0f)}, {topLeft}, {tile}, {}, {}, boxes) ==
              std::array<std::uint8_t, 4>{200, 200, 200, 200},
          "an explicit tile covers the box");
    check(alphasOf({pxSize(1.0f, 1.0f)}, {topLeft}, {noRepeat}, {}, {}, boxes) ==
              std::array<std::uint8_t, 4>{200, 0, 0, 0},
          "no-repeat keeps only its own cell");
    // Repeat-x tiles the top row and drops the bottom one.
    check(alphasOf({pxSize(1.0f, 1.0f)}, {topLeft}, {repeatX}, {}, {}, boxes) ==
              std::array<std::uint8_t, 4>{200, 200, 0, 0},
          "repeat-x tiles one row");
    // An end position moves the cell to the bottom-right corner.
    check(alphasOf({pxSize(1.0f, 1.0f)}, {bottomRight}, {noRepeat}, {}, {}, boxes) ==
              std::array<std::uint8_t, 4>{0, 0, 0, 200},
          "an end position moves the cell");
    // A 1px clip box keeps only the pixel it covers.
    const sui::BackgroundBoxes clipped{area, area, SDL_FRect{1.0f, 1.0f, 1.0f, 1.0f}};
    check(alphasOf({}, {}, {}, {}, {Box::BackgroundContentBox}, clipped) ==
              std::array<std::uint8_t, 4>{0, 0, 0, 200},
          "the clip box cuts the mask");
    // A padded origin measures the end position from the padding box instead.
    const sui::BackgroundBoxes padded{area, SDL_FRect{0.5f, 0.5f, 1.0f, 1.0f}, area};
    check(alphasOf({pxSize(1.0f, 1.0f)}, {bottomRight}, {noRepeat},
                   {Box::BackgroundPaddingBox}, {}, padded) ==
              std::array<std::uint8_t, 4>{200, 0, 0, 0},
          "the origin box anchors the position");

    // A solid gradient honours the same destination: a 1px-wide slice keeps
    // only its own column.
    Mask solid;
    solid.kind = Mask::Kind::LinearGradient;
    solid.linear.angle = 90.0f;
    solid.linear.stops = {sui::ComputedStyle::GradientStop{
                              sui::Length(sui::Length::Percent, 0.0f),
                              SDL_Color{0, 0, 0, 255}},
                          sui::ComputedStyle::GradientStop{
                              sui::Length(sui::Length::Percent, 100.0f),
                              SDL_Color{0, 0, 0, 255}}};
    std::vector<std::uint8_t> pixels(16, 0);
    for (int i = 0; i < 4; ++i) {
        pixels[4 * i + 3] = 200;
    }
    sui::applyMaskLayers(pixels.data(), 2, 2, {solid}, {}, {}, {pxSize(1.0f, 2.0f)},
                         {topLeft}, {noRepeat}, {}, {}, boxes, 16.0f, 16.0f,
                         sui::FetchMaskBytes{});
    check(std::array<std::uint8_t, 4>{pixels[3], pixels[7], pixels[11], pixels[15]} ==
              std::array<std::uint8_t, 4>{200, 0, 200, 0},
          "a sized gradient keeps only its slice");
}

// A `mask-border` 9-patch slices its source into corners, edges, and a middle:
// corners stretch, edges tile along, and the middle shows only with `fill`.
// Small exact images pin every zone.
void testMaskBorder() {
    using Mask = sui::ComputedStyle::MaskImage;
    using Border = sui::ComputedStyle::MaskBorder;
    using Repeat = Border::Repeat;
    const auto num = [](float v) { return sui::Length(sui::Length::Number, v); };
    const auto px = [](float v) { return sui::Length(v); };
    const auto white = []() { return std::array<std::uint8_t, 4>{255, 255, 255, 255}; };
    const auto black = []() { return std::array<std::uint8_t, 4>{0, 0, 0, 255}; };
    auto imageOf = [](int w, int h,
                      const std::vector<std::array<std::uint8_t, 4>>& cells) {
        sui::DecodedImage img;
        img.width = w;
        img.height = h;
        for (const auto& cell : cells) {
            img.rgba.insert(img.rgba.end(), cell.begin(), cell.end());
        }
        return img;
    };
    const auto alphas = [&](const sui::DecodedImage& img, const Border& border,
                            const sui::BackgroundBoxes& boxes, int w, int h) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(4 * w * h), 0);
        for (int i = 0; i < w * h; ++i) {
            pixels[4 * i + 3] = 200;
        }
        sui::FetchMaskBytes fetch{[&](std::string_view)
                                      -> const sui::DecodedImage* { return &img; }};
        sui::applyMaskBorder(pixels.data(), w, h, border, boxes, 16.0f, 16.0f, fetch);
        std::vector<std::uint8_t> out;
        for (int i = 0; i < w * h; ++i) {
            out.push_back(pixels[4 * i + 3]);
        }
        return out;
    };
    auto urlBorder = []() {
        Border border;
        border.source.kind = Mask::Kind::Url;
        border.source.href = "b.png";
        return border;
    };
    // A 6x6 white frame around a 2x2 black centre: 2px bands keep every sample
    // inside uniform texels, so corners and edges keep exactly and the
    // unfilled middle drops exactly.
    sui::DecodedImage frame6;
    frame6.width = 6;
    frame6.height = 6;
    frame6.rgba.assign(6 * 6 * 4, 255);
    for (int y = 2; y < 4; ++y) {
        for (int x = 2; x < 4; ++x) {
            std::uint8_t* texel = frame6.rgba.data() + 4 * (y * 6 + x);
            texel[0] = texel[1] = texel[2] = 0;
        }
    }
    sui::DecodedImage solidWhite6;
    solidWhite6.width = 6;
    solidWhite6.height = 6;
    solidWhite6.rgba.assign(6 * 6 * 4, 255);
    const SDL_FRect area6{0.0f, 0.0f, 6.0f, 6.0f};
    const sui::BackgroundBoxes boxes6{area6, area6, area6};

    // Corners and edges keep the white frame; the unfilled middle drops.
    Border frame = urlBorder();
    frame.slice.top = frame.slice.right = frame.slice.bottom = frame.slice.left =
        num(2.0f);
    {
        // Only pixels whose bilinear footprint stays inside uniform texels
        // assert exactly: the outer rows and columns plus the black centre.
        // (Band pixels touching the centre blend by design.)
        const std::vector<std::uint8_t> result = alphas(frame6, frame, boxes6, 6, 6);
        bool outer = true;
        for (int x = 0; x < 6; ++x) {
            outer = outer && result[x] == 200 && result[5 * 6 + x] == 200;
        }
        for (int y = 0; y < 6; ++y) {
            outer = outer && result[y * 6] == 200 && result[y * 6 + 5] == 200;
        }
        check(outer, "a sliced frame keeps its outer border");
        check(result[2 * 6 + 2] == 0 && result[2 * 6 + 3] == 0 &&
                  result[3 * 6 + 2] == 0 && result[3 * 6 + 3] == 0,
              "a sliced frame drops its middle");
    }
    // `fill` tiles the middle; without it the middle stays transparent even
    // for an all-white source.
    Border filled = frame;
    filled.slice.fill = true;
    check(alphas(solidWhite6, filled, boxes6, 6, 6) == std::vector<std::uint8_t>(36, 200),
          "fill paints the middle too");
    {
        std::vector<std::uint8_t> expect(36, 200);
        expect[2 * 6 + 2] = expect[2 * 6 + 3] = expect[3 * 6 + 2] = expect[3 * 6 + 3] = 0;
        check(alphas(solidWhite6, frame, boxes6, 6, 6) == expect,
              "an unfilled middle stays transparent");
    }
    // Explicit widths matching the slice paint the same frame.
    Border wide = frame;
    wide.width.top = wide.width.right = wide.width.bottom = wide.width.left = px(2.0f);
    {
        const std::vector<std::uint8_t> result = alphas(frame6, wide, boxes6, 6, 6);
        check(result[0] == 200 && result[2 * 6 + 2] == 0,
              "explicit widths size the bands");
    }

    // An outset grows the painted area: the outer pixel is untouched without
    // it and masked once the outset reaches it. The 2x2 black corner keeps the
    // samples exact.
    sui::DecodedImage darkCorner;
    darkCorner.width = 3;
    darkCorner.height = 3;
    darkCorner.rgba.assign(3 * 3 * 4, 255);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            std::uint8_t* texel = darkCorner.rgba.data() + 4 * (y * 3 + x);
            texel[0] = texel[1] = texel[2] = 0;
        }
    }


    // Edge tiling: a 2px white-black strip over a 4px edge repeats its pair,
    // rounds to whole tiles, and leaves gaps with space.
    const sui::DecodedImage strip =
        imageOf(2, 1, {white(), black()});
    auto edgeBorder = [&](Repeat mode) {
        Border border = urlBorder();
        border.slice.top = num(1.0f);
        border.width.top = px(1.0f);
        border.width.right = border.width.bottom = border.width.left = px(0.0f);
        border.repeatX = mode;
        return border;
    };
    const SDL_FRect strip4{0.0f, 0.0f, 4.0f, 1.0f};
    const sui::BackgroundBoxes stripBoxes{strip4, strip4, strip4};
    check(alphas(strip, edgeBorder(Repeat::Repeat), stripBoxes, 4, 1) ==
              std::vector<std::uint8_t>({150, 50, 150, 50}),
          "repeat clips the last tile");
    const SDL_FRect strip3{0.0f, 0.0f, 3.0f, 1.0f};
    const sui::BackgroundBoxes strip3Boxes{strip3, strip3, strip3};
    check(alphas(strip, edgeBorder(Repeat::Round), strip3Boxes, 3, 1)[1] == 200,
          "round rescales to whole tiles");
    const SDL_FRect strip5{0.0f, 0.0f, 5.0f, 1.0f};
    const sui::BackgroundBoxes strip5Boxes{strip5, strip5, strip5};
    {
        const std::vector<std::uint8_t> result =
            alphas(strip, edgeBorder(Repeat::Space), strip5Boxes, 5, 1);
        check(result[2] == 0, "space leaves gaps between tiles");
        check(result[0] > 0, "space still paints its tiles");
    }

    // Modes read the same texel either way: half-red is dim by luminance and
    // half by alpha.
    sui::DecodedImage halfRed;
    halfRed.width = 1;
    halfRed.height = 1;
    halfRed.rgba = {255, 0, 0, 128};
    Border lum = urlBorder();
    lum.slice.fill = true;
    const SDL_FRect area2{0.0f, 0.0f, 2.0f, 2.0f};
    const sui::BackgroundBoxes boxes2{area2, area2, area2};
    check(alphas(halfRed, lum, boxes2, 2, 2) == std::vector<std::uint8_t>(4, 43),
          "luminance masks by brightness");
    Border alp = lum;
    alp.mode = Border::Mode::Alpha;
    check(alphas(halfRed, alp, boxes2, 2, 2) == std::vector<std::uint8_t>(4, 100),
          "alpha masks by the channel");

    // A solid gradient borders like an image; a missing source hides; none is
    // a no-op.
    Border gradient;
    gradient.source.kind = Mask::Kind::LinearGradient;
    gradient.source.linear.angle = 90.0f;
    gradient.source.linear.stops = {
        sui::ComputedStyle::GradientStop{sui::Length(sui::Length::Percent, 0.0f),
                                         SDL_Color{255, 255, 255, 255}},
        sui::ComputedStyle::GradientStop{sui::Length(sui::Length::Percent, 100.0f),
                                         SDL_Color{255, 255, 255, 255}}};
    gradient.slice.top = gradient.slice.right = gradient.slice.bottom =
        gradient.slice.left = num(1.0f);
    gradient.slice.fill = true;
    gradient.width.top = gradient.width.right = gradient.width.bottom =
        gradient.width.left = px(1.0f);
    check(alphas(solidWhite6, gradient, boxes6, 6, 6) ==
              std::vector<std::uint8_t>(36, 200),
          "a solid gradient border keeps");
    Border missing = urlBorder();
    missing.slice.fill = true;
    {
        sui::DecodedImage nothing;
        std::vector<std::uint8_t> pixels(16, 0);
        for (int i = 0; i < 4; ++i) {
            pixels[4 * i + 3] = 200;
        }
        sui::FetchMaskBytes noFetch{
            [&](std::string_view) -> const sui::DecodedImage* { return nullptr; }};
        sui::applyMaskBorder(pixels.data(), 2, 2, missing, boxes2, 16.0f, 16.0f,
                             noFetch);
        check(pixels[3] == 0 && pixels[15] == 0, "a missing border source hides");
    }
    {
        Border none;
        std::vector<std::uint8_t> pixels(16, 200);
        sui::applyMaskBorder(pixels.data(), 2, 2, none, boxes2, 16.0f, 16.0f,
                             sui::FetchMaskBytes{});
        check(pixels == std::vector<std::uint8_t>(16, 200), "no border leaves pixels");
    }
}

// A `border-image` tiles its 9-patch into blits: corners stretch once, edges
// tile along, gaps stay empty, and the middle needs `fill`. The builder is
// pure geometry, so every count and rect asserts exactly.
void testBorderImageTiles() {
    using Image = sui::ComputedStyle::BorderImage;
    using Repeat = Image::Repeat;
    const auto num = [](float v) { return sui::Length(sui::Length::Number, v); };
    const auto px = [](float v) { return sui::Length(v); };
    auto urlImage = []() {
        Image image;
        image.source.kind = sui::ComputedStyle::BackgroundImage::Url;
        image.source.href = "f.png";
        return image;
    };
    const auto boxes = [](float w, float h) {
        const SDL_FRect area{0.0f, 0.0f, w, h};
        return sui::BackgroundBoxes{area, area, area};
    };
    const auto tilesOf = [&](Image image, float srcW, float srcH, float w, float h) {
        return sui::borderImageTiles(image, srcW, srcH, boxes(w, h), 16.0f, 16.0f);
    };

    // A 6x6 source sliced 2 with auto widths on a 6x6 box: four 2x2 corners,
    // four 2x2 edges, no middle without fill.
    Image frame = urlImage();
    frame.slice.top = frame.slice.right = frame.slice.bottom = frame.slice.left =
        num(2.0f);
    {
        const std::vector<sui::BorderTile> tiles = tilesOf(frame, 6.0f, 6.0f, 6.0f, 6.0f);
        check(tiles.size() == 8, "corners and edges tile once each");
        check(tiles[0].src.w == 2.0f && tiles[0].dst.w == 2.0f,
              "a corner stretches its slice");
    }
    Image filled = frame;
    filled.slice.fill = true;
    {
        const std::vector<sui::BorderTile> tiles = tilesOf(filled, 6.0f, 6.0f, 6.0f, 6.0f);
        check(tiles.size() == 9, "fill adds the middle tile");
        check(tiles.back().dst.x == 2.0f && tiles.back().dst.y == 2.0f,
              "the middle tile sits centred");
    }
    // An outset grows the painted area past the border box.
    Image grown = frame;
    grown.outset.top = grown.outset.right = grown.outset.bottom = grown.outset.left =
        px(1.0f);
    check(tilesOf(grown, 6.0f, 6.0f, 6.0f, 6.0f)[0].dst.x == -1.0f,
          "an outset reaches past the box");

    // A 2px strip tiled over an 8px edge repeats four pairs: slice the sides
    // away so only the top edge band paints.
    auto stripImage = [&](Repeat mode) {
        Image image = urlImage();
        image.slice.top = num(1.0f);
        image.slice.left = num(1.0f);
        image.slice.right = num(1.0f);
        image.width.top = px(1.0f);
        image.width.right = image.width.bottom = image.width.left = px(0.0f);
        image.repeatX = mode;
        return image;
    };
    {
        const std::vector<sui::BorderTile> tiles =
            tilesOf(stripImage(Repeat::Repeat), 4.0f, 2.0f, 8.0f, 1.0f);
        // Slice 1 of a 4x2 source: corners empty (zero bands), one edge band
        // with a 2px middle; tile pairs at 0, 2, 4, 6.
        check(tiles.size() == 4, "repeat clips the last tile");
        check(tiles[0].dst.x == 0.0f && tiles[0].src.x == 1.0f,
              "tiles start at the edge region");
        check(tiles[3].dst.x == 6.0f, "the last tile starts on time");
    }
    // Round rescales to whole tiles: four 1.75px tiles over 7px.
    {
        const std::vector<sui::BorderTile> tiles =
            tilesOf(stripImage(Repeat::Round), 4.0f, 2.0f, 7.0f, 1.0f);
        check(tiles.size() == 4, "round keeps whole tiles");
        check(tiles.back().dst.w == 1.75f, "round rescales the tile width");
    }
    // Space leaves even gaps: two tiles at 0.25 and 2.75 over 5px.
    {
        const std::vector<sui::BorderTile> tiles =
            tilesOf(stripImage(Repeat::Space), 4.0f, 2.0f, 5.0f, 1.0f);
        check(tiles.size() == 2, "space skips its gaps");
        check(tiles[0].dst.x == 0.25f && tiles[1].dst.x == 2.75f,
              "space centres whole tiles with gaps");
    }
    // Degenerate inputs tile nothing.
    {
        Image none;
        check(tilesOf(none, 4.0f, 4.0f, 6.0f, 6.0f).empty(), "no source tiles nothing");
        check(tilesOf(frame, 0.0f, 4.0f, 6.0f, 6.0f).empty(),
              "no source size tiles nothing");
    }
}

// A list item's marker slot: a loaded image takes its own size, vertically
// centred and a gap left of the box; anything else leaves the glyph, which the
// caller measures and places with the same gap.
void testListMarkerSlot() {
    const SDL_FRect box{100.0f, 50.0f, 200.0f, 40.0f};
    sui::ListMarkerSlot image = sui::listMarkerSlot(box, 16.0f, 16.0f);
    check(image.image && image.rect.w == 16.0f && image.rect.h == 16.0f,
          "a loaded image takes its own size");
    check(image.rect.x == 76.0f, "the image keeps the gap left of the box");
    check(image.rect.y == 62.0f, "the image centres in the item");
    // A wide image still centres on its own height.
    sui::ListMarkerSlot wide = sui::listMarkerSlot(box, 12.0f, 8.0f);
    check(wide.rect.y == 66.0f, "a short image centres too");
    // Nothing loaded (or nothing set) falls back to the glyph.
    sui::ListMarkerSlot glyph = sui::listMarkerSlot(box, 0.0f, 0.0f);
    check(!glyph.image && glyph.rect.w == 0.0f, "a missing image falls back");
    check(glyph.rect.x == 92.0f && glyph.rect.y == 50.0f, "the glyph starts at the gap");
}

// An SVG `<image>` meet-fits its raster inside the box: uniform scale
// centred, or a straight stretch for `none`. Degenerate inputs fit nothing.
void testSvgImageFit() {
    const SDL_FRect meet = sui::svgImageFit(100.0f, 50.0f, 200.0f, 200.0f, false);
    check(meet.w == 200.0f && meet.h == 100.0f, "meet scales by the binding axis");
    check(meet.x == 0.0f && meet.y == 50.0f, "meet centres the spare axis");
    const SDL_FRect tall = sui::svgImageFit(50.0f, 100.0f, 200.0f, 200.0f, false);
    check(tall.w == 100.0f && tall.h == 200.0f && tall.x == 50.0f && tall.y == 0.0f,
          "a tall raster letterboxes the other way");
    const SDL_FRect stretch = sui::svgImageFit(100.0f, 50.0f, 200.0f, 200.0f, true);
    check(stretch.w == 200.0f && stretch.h == 200.0f && stretch.x == 0.0f &&
              stretch.y == 0.0f,
          "none stretches to the box");
    const SDL_FRect exact = sui::svgImageFit(200.0f, 200.0f, 200.0f, 200.0f, false);
    check(exact.x == 0.0f && exact.y == 0.0f && exact.w == 200.0f,
          "an exact fit sits at the origin");
    const SDL_FRect empty = sui::svgImageFit(0.0f, 50.0f, 200.0f, 200.0f, false);
    check(empty.w == 0.0f && empty.h == 0.0f, "an empty raster fits nothing");
    const SDL_FRect nobox = sui::svgImageFit(100.0f, 50.0f, 0.0f, 200.0f, false);
    check(nobox.w == 0.0f, "an empty box fits nothing");
}

// A path `d` flattens to subpaths: absolute and relative commands, the
// shorthand curves, arcs, implicit repeats, and the close.
void testSvgPath() {
    std::vector<sui::SvgSubpath> subs;
    // Each case starts from nothing: the parser appends to what it is given.
    const auto parse = [&](const char* d) {
        subs.clear();
        return sui::parseSvgPath(d, subs);
    };
    // A square, closed.
    check(parse("M0 0 L10 0 L10 10 L0 10 Z") && subs.size() == 1 &&
              subs[0].closed && subs[0].points.size() == 4,
          "a square flattens to one closed subpath");
    check(subs[0].points[1].x == 10.0f && subs[0].points[2].y == 10.0f,
          "the square's points land where the commands said");
    // Horizontal and vertical shorthands.
    check(parse("M0 0 H5 V5") && subs.size() == 1 &&
              subs[0].points.size() == 3 && !subs[0].closed,
          "h and v add one point each");
    check(subs[0].points[2].x == 5.0f && subs[0].points[2].y == 5.0f, "h and v land right");
    // Relative commands measure from the current point, including the first.
    check(parse("m10 10 l5 0 l0 5") && subs[0].points[1].x == 15.0f &&
              subs[0].points[2].y == 15.0f,
          "relative moves and lines step from the last point");
    // A repeated coordinate repeats the previous command; a second moveto pair
    // is an implicit lineto.
    check(parse("M0 0 5 5 10 10") && subs.size() == 1 &&
              subs[0].points.size() == 3,
          "bare numbers repeat the last command");
    check(parse("M0 0 5 5") && subs[0].points.size() == 2 &&
              subs[0].points[1].x == 5.0f && subs[0].points[1].y == 5.0f,
          "a repeated moveto pair draws a line");
    // Two moves make two subpaths.
    check(parse("M0 0 L1 1 M5 5 L6 6") && subs.size() == 2,
          "a second moveto opens a subpath");
    // A cubic flattens into several points between its ends.
    check(parse("M0 0 C0 10 10 10 10 0") && subs.size() == 1 &&
              subs[0].points.size() > 3 && subs[0].points.back().x == 10.0f,
          "a cubic bulges between its endpoints");
    bool bulge = false;
    for (const SDL_FPoint& p : subs[0].points) {
        bulge = bulge || p.y > 6.0f;
    }
    check(bulge, "the cubic's middle rises above both endpoints");
    // Flattening follows the control polygon: a long curve gets many samples, a
    // short one just enough to round.
    parse("M0 0 C0 10 10 10 10 0");
    check(subs[0].points.size() >= 6, "a long curve flattens finely");
    parse("M0 0 C0 1 1 1 1 0");
    check(subs[0].points.size() >= 5, "a short curve still rounds");
    // The shorthand reflects the previous control point when there was one, and
    // treats the current point as the control when there was not. A reflected
    // second curve bulges as deep as the first; an unreflected one barely.
    check(parse("M0 0 C0 10 10 10 10 0 S20 -10 20 0"), "s follows a cubic");
    float reflected = 0.0f;
    for (const SDL_FPoint& p : subs[0].points) {
        reflected = std::min(reflected, p.y);
    }
    check(parse("M0 0 S10 10 20 0"), "s without a cubic still curves");
    float plain = 0.0f;
    for (const SDL_FPoint& p : subs[0].points) {
        plain = std::min(plain, p.y);
    }
    check(reflected < -6.0f && plain > reflected / 2.0f,
          "s mirrors the control point only after a curve");
    // Quadratics and their shorthand.
    check(parse("M0 0 Q5 10 10 0") && subs[0].points.back().x == 10.0f,
          "a quadratic ends at its point");
    check(parse("M0 0 Q5 10 10 0 T20 0"), "t follows a quadratic");
    float quadReflected = 0.0f;
    for (const SDL_FPoint& p : subs[0].points) {
        quadReflected = std::min(quadReflected, p.y);
    }
    check(parse("M0 0 T20 0"), "t without a quadratic still curves");
    float quadPlain = 0.0f;
    for (const SDL_FPoint& p : subs[0].points) {
        quadPlain = std::min(quadPlain, p.y);
    }
    check(quadReflected < -2.0f && quadPlain > quadReflected / 2.0f,
          "t mirrors the control point only after a curve");
    // Arcs: a half circle from (0,0) to (20,0) with radius 10 bows upward or
    // downward depending on the sweep flag.
    check(parse("M0 0 A10 10 0 0 1 20 0") && subs.size() == 1 &&
              subs[0].points.size() >= 8,
          "an arc flattens into several points");
    float topDown = 0.0f;
    for (const SDL_FPoint& p : subs[0].points) {
        topDown = std::min(topDown, p.y);
    }
    float topUp = 0.0f;
    check(parse("M0 0 A10 10 0 0 0 20 0"), "the other sweep parses");
    for (const SDL_FPoint& p : subs[0].points) {
        topUp = std::max(topUp, p.y);
    }
    check(topDown < 0.0f && topUp > 0.0f, "the sweep flag picks the arc's side");
    // The large-arc flag takes the long way round: with radii far longer than
    // the chord, the arc wraps most of the way instead of clipping it.
    parse("M0 0 A30 30 0 0 1 20 0");
    float shallow = 0.0f;
    for (const SDL_FPoint& p : subs[0].points) {
        shallow = std::min(shallow, p.y);
    }
    const std::size_t few = subs[0].points.size();
    parse("M0 0 A30 30 0 1 1 20 0");
    float deep = 0.0f;
    for (const SDL_FPoint& p : subs[0].points) {
        deep = std::min(deep, p.y);
    }
    check(deep < shallow * 4.0f && subs[0].points.size() > few * 2,
          "the large-arc flag takes the long way");
    // Numbers may run together: "10-20" is two numbers.
    check(parse("M10-20") && subs[0].points[0].x == 10.0f &&
              subs[0].points[0].y == -20.0f,
          "numbers run together without separators");
    // A zero radius degenerates to a straight line, as the spec says.
    check(parse("M0 0 A0 0 0 0 1 20 0") && subs[0].points.size() == 2 &&
              subs[0].points[1].x == 20.0f,
          "a zero-radius arc is a line");
    // Radii too small for the chord grow until they span it: five is too small
    // for a twenty-wide chord, so the arc comes out as the ten it needed.
    check(parse("M0 0 A5 5 0 0 1 20 0"), "an arc too small for its chord parses");
    float grew = 0.0f;
    for (const SDL_FPoint& p : subs[0].points) {
        grew = std::min(grew, p.y);
    }
    check(grew <= -9.0f, "too-small radii grow to span the chord");
    // Nothing, and rubbish, both refuse.
    check(!parse(""), "an empty path parses to nothing");
    check(!parse("banana"), "an unknown command parses to nothing");
    // A path cut short keeps what it already described.
    check(parse("M0 0 L10 10 L") && subs.size() == 1 &&
              subs[0].points.size() == 2,
          "a truncated path keeps its points");
    // A path that breaks the rules still opens where it says: a leading lineto
    // becomes a one-point subpath, which draws nothing.
    check(parse("L10 10") && subs.size() == 1 &&
              subs[0].points.size() == 1 && subs[0].points[0].x == 10.0f,
          "a leading lineto opens a bare subpath");
}

// SVG viewports map user units onto the box: uniform meet scaling centred,
// or independent axes for `none`. viewBox text parses strictly.
void testSvgViewport() {
    float box[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    check(sui::parseSvgViewBox("0 0 100 50", box) && box[2] == 100.0f && box[3] == 50.0f,
          "a space viewBox parses");
    check(sui::parseSvgViewBox("10,20,30,40", box) && box[0] == 10.0f && box[1] == 20.0f,
          "commas read as spaces");
    check(!sui::parseSvgViewBox("0 0 100", box), "three numbers fail");
    check(!sui::parseSvgViewBox("0 0 0 50", box), "an empty extent fails");
    check(!sui::parseSvgViewBox("0 0 100 banana", box), "garbage fails");
    // A 200x100 box on a 100x50 view: meet scales 2x about the centre.
    sui::SvgView meet = sui::svgViewport(200.0f, 100.0f, 0.0f, 0.0f, 100.0f, 50.0f, false);
    check(meet.sx == 2.0f && meet.sy == 2.0f && meet.x == 0.0f && meet.y == 0.0f,
          "meet scales uniformly");
    // A wide box letterboxes: 400x100 on 100x50 scales 2x with 100px sides.
    sui::SvgView wide = sui::svgViewport(400.0f, 100.0f, 0.0f, 0.0f, 100.0f, 50.0f, false);
    check(wide.sx == 2.0f && wide.x == 100.0f && wide.y == 0.0f,
          "meet centres the spare axis");
    // `none` stretches each axis alone.
    sui::SvgView stretch =
        sui::svgViewport(400.0f, 100.0f, 0.0f, 0.0f, 100.0f, 50.0f, true);
    check(stretch.sx == 4.0f && stretch.sy == 2.0f, "none scales axes apart");
    // A min offset shifts through the scale.
    sui::SvgView shifted =
        sui::svgViewport(200.0f, 100.0f, 10.0f, 5.0f, 100.0f, 50.0f, false);
    check(shifted.x == -20.0f && shifted.y == -10.0f, "min offsets shift scaled");
}

// An SVG `<text>` run sizes from its attribute in user units and lands through
// the viewport: `text-anchor` shifts it and y names the baseline.
void testSvgText() {
    // A 10-unit font-size in a 2x view renders twenty pixels; without one the
    // CSS size passes through untouched.
    check(sui::svgTextPxSize("10", 16.0f, 2.0f) == 20.0f, "an attribute scales");
    check(sui::svgTextPxSize("", 16.0f, 2.0f) == 16.0f, "a missing size keeps CSS");
    check(sui::svgTextPxSize("banana", 16.0f, 2.0f) == 16.0f, "garbage keeps CSS");
    check(sui::svgTextPxSize("0", 16.0f, 2.0f) == 16.0f, "a zero size keeps CSS");
    check(sui::svgTextPxSize("-5", 16.0f, 2.0f) == 16.0f, "a negative size keeps CSS");
    check(sui::svgTextPxSize(" 12 ", 16.0f, 2.0f) == 24.0f, "stray spaces parse");

    // Origin math on a 2x view at an offset: start keeps x, middle and end
    // pull left by half and all of the run, and y lifts by the ascent.
    const sui::SvgView view = sui::svgViewport(200.0f, 100.0f, 0.0f, 0.0f, 100.0f,
                                               50.0f, false);
    const SDL_FPoint start = sui::svgTextOrigin(view, 10.0f, 20.0f, "start", 30.0f, 8.0f);
    check(start.x == 20.0f && start.y == 32.0f, "a start run keeps its x");
    const SDL_FPoint middle =
        sui::svgTextOrigin(view, 10.0f, 20.0f, "middle", 30.0f, 8.0f);
    check(middle.x == 5.0f && middle.y == 32.0f, "middle centres the run");
    const SDL_FPoint end = sui::svgTextOrigin(view, 10.0f, 20.0f, "end", 30.0f, 8.0f);
    check(end.x == -10.0f, "end right-aligns the run");
    const SDL_FPoint plain = sui::svgTextOrigin(view, 10.0f, 20.0f, "", 30.0f, 8.0f);
    check(plain.x == 20.0f, "no anchor reads as start");
    // A percentage font-size scales the CSS size, not user units.
    check(sui::svgTextPxSize("50%", 16.0f, 2.0f) == 8.0f, "a percentage scales CSS");
}

// Lengths resolve percentages against the viewBox: x-like against its width,
// y-like against its height, radii against the diagonal. A nested viewport
// places and sizes in the parent's user units, then maps its own box inside.
void testSvgViewBoxUnits() {
    check(sui::svgLengthBase("x", 100.0f, 50.0f) == 100.0f, "x rides the width");
    check(sui::svgLengthBase("width", 100.0f, 50.0f) == 100.0f, "width rides the width");
    check(sui::svgLengthBase("cx", 100.0f, 50.0f) == 100.0f, "cx rides the width");
    check(sui::svgLengthBase("y", 100.0f, 50.0f) == 50.0f, "y rides the height");
    check(sui::svgLengthBase("height", 100.0f, 50.0f) == 50.0f, "height rides the height");
    check(sui::svgLengthBase("cy", 100.0f, 50.0f) == 50.0f, "cy rides the height");
    const float diag = sui::svgLengthBase("r", 100.0f, 50.0f);
    check(diag > 79.0f && diag < 79.1f, "a radius rides the diagonal");
    check(sui::svgLengthBase("stroke-width", 100.0f, 50.0f) == diag,
          "a stroke rides the diagonal too");
    check(sui::svgLengthBase("banana", 100.0f, 50.0f) == 0.0f, "unknown resolves zero");

    // A nested svg without a viewBox is 1:1 at its parent-unit offset.
    const sui::SvgView parent = sui::svgViewport(200.0f, 100.0f, 0.0f, 0.0f, 100.0f,
                                                 50.0f, false);
    const sui::SvgView plain =
        sui::svgSubViewport(parent, 5.0f, 5.0f, 10.0f, 10.0f, false, false, 0.0f, 0.0f,
                            0.0f, 0.0f);
    check(plain.sx == 1.0f && plain.x == 10.0f && plain.y == 10.0f,
          "a boxless nest offsets 1:1");
    check(plain.vbw == 10.0f && plain.vbh == 10.0f, "a nest carries its own extents");
    // With a viewBox the nest meets inside its parent-unit size.
    const sui::SvgView boxed =
        sui::svgSubViewport(parent, 5.0f, 5.0f, 10.0f, 10.0f, false, true, 0.0f, 0.0f,
                            10.0f, 10.0f);
    check(boxed.sx == 2.0f && boxed.sy == 2.0f, "a boxed nest meets uniformly");
    check(boxed.x == 10.0f && boxed.y == 10.0f, "a fitting nest starts at its offset");
    check(boxed.vbw == 10.0f && boxed.vbh == 10.0f, "a boxed nest keeps its box");
    // A nest inside an offset parent (a viewBox with a min) inherits the shift.
    const sui::SvgView shiftedParent =
        sui::svgViewport(200.0f, 100.0f, 10.0f, 5.0f, 100.0f, 50.0f, false);
    const sui::SvgView shifted =
        sui::svgSubViewport(shiftedParent, 5.0f, 5.0f, 10.0f, 10.0f, false, false, 0.0f,
                            0.0f, 0.0f, 0.0f);
    check(shifted.x == -10.0f && shifted.y == 0.0f, "a nest follows its parent's shift");
}

// HarfBuzz shaping: ligatures fuse codepoints into single glyphs, marks ride
// with zero advance, and clusters map glyphs back to source bytes. ASCII
// stays on the fast path; anything past it shapes. Needs a real font, so the
// suite skips quietly when DejaVu is absent.
void testShaping() {
    check(!sui::TextFont::needsShaping("hello"), "ascii stays on the fast path");
    check(sui::TextFont::needsShaping("fi\xCC\x81"), "a mark needs shaping");
    sui::TextFont font;
    if (!font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")) {
        std::printf("(skipping shaping: no DejaVu Sans)\n");
        return;
    }
    const std::vector<sui::ShapedGlyph> fi = font.shapeText("fi", 16.0f);
    check(fi.size() == 1, "fi fuses into one ligature");
    if (!fi.empty()) {
        check(fi[0].cluster == 0, "a ligature clusters at its first byte");
        check(fi[0].advance > 0.0f, "a ligature advances");
    }
    // Lam plus alef fuses the same way (U+0644 U+0627).
    const std::vector<sui::ShapedGlyph> lamAlef =
        font.shapeText("\xD9\x84\xD8\xA7", 16.0f);
    check(lamAlef.size() == 1, "lam-alef fuses into one glyph");
    // A base plus a combining mark that never precomposes stays two glyphs,
    // the mark spaceless (U+0332 combining low line).
    const std::vector<sui::ShapedGlyph> marked = font.shapeText("e\xCC\xB2", 16.0f);
    check(marked.size() == 2, "a mark does not fuse with its base");
    if (marked.size() == 2) {
        check(marked[1].advance == 0.0f, "a mark takes no advance");
        check(marked[1].cluster == 0, "a mark clusters with its base");
    }
    // Measurement matches the shaped advances, and counts rendered glyphs.
    float shaped = 0.0f;
    for (const auto& glyph : lamAlef) {
        shaped += glyph.advance;
    }
    check(font.measureText("\xD9\x84\xD8\xA7", 16.0f) == shaped,
          "measurement matches the shaped advances");
    // Kerning pairs kern by default and stand apart with `none`.
    const float kerned = font.measureText("AV", 16.0f, true);
    const float unkearned = font.measureText("AV", 16.0f, false);
    check(kerned < unkearned, "a kern pair tucks with kerning on");
    check(font.measureText("AV", 16.0f) == kerned, "kerning defaults on");
    // `font-stretch` scales the font's horizontal axis, so advances widen and
    // narrow with the glyphs rather than just spreading them.
    const float wide = font.measureText("AV", 16.0f, true, 1.5f);
    const float narrow = font.measureText("AV", 16.0f, true, 0.5f);
    check(wide > kerned, "an expanded run measures wider");
    check(narrow < kerned, "a condensed run measures narrower");
    check(wide / kerned > 1.4f && wide / kerned < 1.6f, "expansion scales by the factor");
    check(narrow / kerned > 0.4f && narrow / kerned < 0.6f, "condensation scales by the factor");
    check(font.measureText("AV", 16.0f, true, 1.0f) == kerned, "a normal stretch is identity");
    // Small caps substitute uppercase and shrink to four fifths: already-upper
    // text measures identical, lowered text measures 0.8x its capitals.
    const float caps = font.measureText("ABC", 16.0f, false);
    const float small = font.measureText("abc", 16.0f, false, 1.0f, true);
    check(font.measureText("ABC", 16.0f, false, 1.0f, true) == caps,
          "capitals ignore small caps");
    check(std::fabs(small / caps - 0.8f) < 0.05f, "lowercase shrinks to its capitals");
    const auto smallGlyphs = font.shapeText("abc", 16.0f, false, 1.0f, true);
    const auto capGlyphs = font.shapeText("ABC", 16.0f, false);
    check(smallGlyphs.size() == capGlyphs.size(), "substitution keeps glyph count");
    bool sameIds = !smallGlyphs.empty();
    bool allSmall = !smallGlyphs.empty();
    for (std::size_t i = 0; i < smallGlyphs.size() && i < capGlyphs.size(); ++i) {
        sameIds = sameIds && smallGlyphs[i].glyph == capGlyphs[i].glyph;
        allSmall = allSmall && smallGlyphs[i].small;
    }
    check(sameIds, "lowercase shapes to capital glyph ids");
    check(allSmall, "substituted glyphs carry the small flag");
    check(!font.shapeText("ABC", 16.0f).front().small, "capitals carry no flag");
    // Latin-1 lowers too; anything unmapped measures untouched.
    const float grave = font.measureText("\xC3\x80", 16.0f, false);
    check(std::fabs(font.measureText("\xC3\xA0", 16.0f, false, 1.0f, true) / grave - 0.8f) <
              0.05f,
          "latin-1 lowercase shrinks as well");
    check(font.measureText("\xCE\xB1", 16.0f, false, 1.0f, true) ==
              font.measureText("\xCE\xB1", 16.0f, false),
          "greek renders unchanged");
    // The raster follows the advances: the glyphs themselves widen and narrow.
    const unsigned int lamGlyph = font.shapeText("\xD9\x84", 16.0f).front().glyph;
    int lx = 0, ly = 0, lw = 0, lh = 0;
    int wx = 0, wy = 0, ww = 0, wh = 0;
    int nx = 0, ny = 0, nw = 0, nh = 0;
    check(font.glyphBitmapBox(lamGlyph, 16.0f, 1.0f, lx, ly, lw, lh) && lw > 0 && lh > 0,
          "a shaped glyph reports its box");
    check(font.glyphBitmapBox(lamGlyph, 16.0f, 2.0f, wx, wy, ww, wh) && ww > lw * 2 - 2,
          "a doubled stretch doubles the glyph width");
    check(wh == lh, "stretching leaves the height alone");
    check(font.glyphBitmapBox(lamGlyph, 16.0f, 0.5f, nx, ny, nw, nh) && nw <= lw,
          "a condensed glyph is no wider");
    check(font.glyphCount("fi") == 1, "ascii ligatures count fused");
    check(font.glyphCount("\xD9\x84\xD8\xA7") == lamAlef.size(),
          "shaped text counts glyphs");
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

// The CPU affine colour matrix the `filter:` shift chain hands the executor.
// `filterColorMatrix` composes the chain into one matrix; `applyColorMatrix`
// maps each pixel through it, rounding to the nearest byte and clipping.
// `object-position` plants the fitted content in the leftover space:
// percentages ride the leftover, lengths offset as-is.
void testObjectFitDst() {
    using Fit = sui::ComputedStyle::ObjectFit;
    const sui::Length c50{sui::Length::Percent, 50.0f};
    const sui::Length c0{sui::Length::Percent, 0.0f};
    const sui::Length c100{sui::Length::Percent, 100.0f};
    // Fill ignores everything and takes the box.
    const SDL_FRect fill = sui::objectFitDst(Fit::ObjFill, c0, c0, 200.0f, 100.0f, 10.0f, 10.0f);
    check(fill.x == 0.0f && fill.y == 0.0f && fill.w == 200.0f && fill.h == 100.0f,
          "fill takes the content box");
    // A square image in a wide box contains at 100x100, centred by default.
    const SDL_FRect contain =
        sui::objectFitDst(Fit::ObjContain, c50, c50, 200.0f, 100.0f, 100.0f, 100.0f);
    check(contain.x == 50.0f && contain.y == 0.0f && contain.w == 100.0f &&
              contain.h == 100.0f,
          "contain centres by default");
    const SDL_FRect leftTop =
        sui::objectFitDst(Fit::ObjContain, c0, c0, 200.0f, 100.0f, 100.0f, 100.0f);
    check(leftTop.x == 0.0f && leftTop.y == 0.0f, "left top pins the corner");
    const SDL_FRect rightBottom =
        sui::objectFitDst(Fit::ObjContain, c100, c100, 200.0f, 100.0f, 100.0f, 100.0f);
    check(rightBottom.x == 100.0f && rightBottom.y == 0.0f, "right bottom pins out");
    // Cover overflows: the 200x200 scaled square centres its crop.
    const SDL_FRect cover =
        sui::objectFitDst(Fit::ObjCover, c50, c50, 200.0f, 100.0f, 100.0f, 100.0f);
    check(cover.x == 0.0f && cover.y == -50.0f && cover.w == 200.0f && cover.h == 200.0f,
          "cover centres the overflow");
    const SDL_FRect coverTop =
        sui::objectFitDst(Fit::ObjCover, c50, c0, 200.0f, 100.0f, 100.0f, 100.0f);
    check(coverTop.y == 0.0f, "cover top pins the crop");
    // None keeps intrinsic size; scale-down only ever shrinks.
    const SDL_FRect none =
        sui::objectFitDst(Fit::ObjNone, c50, c50, 200.0f, 100.0f, 100.0f, 100.0f);
    check(none.x == 50.0f && none.w == 100.0f, "none centres the intrinsic size");
    const SDL_FRect big =
        sui::objectFitDst(Fit::ObjScaleDown, c50, c50, 200.0f, 100.0f, 400.0f, 400.0f);
    check(big.w == 100.0f && big.h == 100.0f && big.x == 50.0f,
          "scale-down shrinks a big image");
    const SDL_FRect small =
        sui::objectFitDst(Fit::ObjScaleDown, c50, c50, 200.0f, 100.0f, 50.0f, 50.0f);
    check(small.w == 50.0f && small.x == 75.0f, "scale-down never grows");
    // Pixel offsets land as-is.
    const SDL_FRect px = sui::objectFitDst(Fit::ObjContain, sui::Length{10.0f},
                                           sui::Length{20.0f}, 200.0f, 100.0f, 100.0f,
                                           100.0f);
    check(px.x == 10.0f && px.y == 20.0f, "lengths offset directly");
}

// A blended surface over its backdrop, per channel with source-over alpha.
// One opaque pixel over one opaque pixel tells each mode apart.
void testBlendImage() {
    auto blend = [](std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, std::uint8_t sa,
                    std::uint8_t br, std::uint8_t bg, std::uint8_t bb, std::uint8_t ba,
                    sui::ComputedStyle::MixBlendMode mode) {
        std::vector<std::uint8_t> src{sr, sg, sb, sa};
        const std::vector<std::uint8_t> back{br, bg, bb, ba};
        sui::blendImage(src.data(), 1, 1, back.data(), mode);
        return src;
    };
    using Blend = sui::ComputedStyle::MixBlendMode;
    // Mid-grey over mid-grey: multiply darkens, screen lightens.
    check(blend(128, 128, 128, 255, 128, 128, 128, 255, Blend::Multiply)[0] == 64,
          "multiply squares the channel");
    check(blend(128, 128, 128, 255, 128, 128, 128, 255, Blend::Screen)[0] == 192,
          "screen lifts the channel");
    // Order-sensitive extremes.
    check(blend(200, 0, 0, 255, 100, 0, 0, 255, Blend::Darken)[0] == 100,
          "darken keeps the darker");
    check(blend(200, 0, 0, 255, 100, 0, 0, 255, Blend::Lighten)[0] == 200,
          "lighten keeps the lighter");
    check(blend(200, 0, 0, 255, 100, 0, 0, 255, Blend::Difference)[0] == 100,
          "difference takes the distance");
    check(blend(255, 0, 0, 255, 255, 0, 0, 255, Blend::Difference)[0] == 0,
          "identical differences cancel");
    check(blend(128, 0, 0, 255, 128, 0, 0, 255, Blend::Exclusion)[0] == 127,
          "exclusion halves identical mid-greys");
    // Alpha: a clear source leaves the backdrop alone, and a clear backdrop
    // leaves the source alone.
    const std::vector<std::uint8_t> keep =
        blend(0, 0, 0, 0, 10, 20, 30, 40, Blend::Multiply);
    check(keep[0] == 0 && keep[1] == 0 && keep[2] == 0 && keep[3] == 0,
          "a clear source paints nothing");
    const std::vector<std::uint8_t> alone =
        blend(10, 20, 30, 40, 0, 0, 0, 0, Blend::Screen);
    check(alone[0] == 10 && alone[1] == 20 && alone[2] == 30 && alone[3] == 40,
          "a clear backdrop keeps the source");
    // A half source over an opaque backdrop mixes the formula halfway.
    const std::vector<std::uint8_t> half =
        blend(255, 255, 255, 128, 0, 0, 0, 255, Blend::Screen);
    check(half[0] == 128 && half[3] == 255, "screened white halves over black");
    const std::vector<std::uint8_t> grey =
        blend(0, 0, 0, 128, 255, 255, 255, 255, Blend::Multiply);
    check(grey[0] > 120 && grey[0] < 136 && grey[3] == 255,
          "a half black multiply greys the backdrop");
}

// Backdrop opacity fades every alpha and leaves colours alone; drop shadows
// composite a shifted, tinted silhouette underneath the content.
void testBackdropFilters() {
    // Halving: colours intact, alphas halved with rounding.
    std::vector<std::uint8_t> img{255, 0, 0, 255, 0, 255, 0, 128};
    sui::applyBackdropOpacity(img.data(), 2, 1, 0.5f);
    check(img[0] == 255 && img[1] == 0 && img[2] == 0 && img[3] == 128,
          "opacity halves an opaque alpha, colours intact");
    check(img[4] == 0 && img[5] == 255 && img[6] == 0 && img[7] == 64,
          "opacity halves a partial alpha");
    // Clamping both ways.
    std::vector<std::uint8_t> full{10, 20, 30, 200};
    sui::applyBackdropOpacity(full.data(), 1, 1, 2.0f);
    check(full[3] == 200, "an over-one amount clamps to identity");
    sui::applyBackdropOpacity(full.data(), 1, 1, -1.0f);
    check(full[3] == 0, "a negative amount clears the alpha");

    // One opaque white pixel in a 5x1 row, shadowed one right in black.
    std::vector<std::uint8_t> row(5 * 4, 0);
    row[2 * 4] = 255;
    row[2 * 4 + 1] = 255;
    row[2 * 4 + 2] = 255;
    row[2 * 4 + 3] = 255;
    sui::applyBackdropDropShadow(row.data(), 5, 1, 1.0f, 0.0f, 0, 0, 0, 0, 255);
    check(row[2 * 4] == 255 && row[2 * 4 + 3] == 255, "content survives its shadow");
    check(row[3 * 4] == 0 && row[3 * 4 + 3] == 255, "the shadow lands shifted");
    check(row[4 * 4 + 3] == 0, "nothing spills past the shift");
    check(row[1 * 4 + 3] == 0, "nothing paints upwind of the content");

    // The shadow color (with its own alpha) is what paints, not the content.
    std::vector<std::uint8_t> tint(5 * 4, 0);
    tint[4] = 255;
    tint[5] = 255;
    tint[6] = 255;
    tint[7] = 255;
    sui::applyBackdropDropShadow(tint.data(), 5, 1, 1.0f, 0.0f, 0, 255, 0, 0, 128);
    check(tint[8] == 255 && tint[9] == 0 && tint[10] == 0 && tint[11] == 128,
          "a translucent red shadow lands shifted");
    check(tint[4] == 255 && tint[7] == 255, "content keeps its color and alpha");

    // Blur spreads the silhouette: a neighbour the sharp shadow never reaches
    // gains partial coverage.
    std::vector<std::uint8_t> soft(7 * 4, 0);
    soft[3 * 4] = 255;
    soft[3 * 4 + 1] = 255;
    soft[3 * 4 + 2] = 255;
    soft[3 * 4 + 3] = 255;
    sui::applyBackdropDropShadow(soft.data(), 7, 1, 2.0f, 0.0f, 2, 0, 0, 0, 255);
    const std::uint8_t near = soft[4 * 4 + 3];
    check(near > 0 && near < 255, "blur softens the shadow's edge");
    check(soft[0] == 0 && soft[1 * 4 + 3] == 0, "far pixels stay clear");
}

void testColorMatrix() {
    auto apply = [](const float m[12], std::uint8_t r, std::uint8_t g, std::uint8_t b,
                    std::uint8_t a) {
        std::vector<std::uint8_t> img{r, g, b, a};
        sui::applyColorMatrix(img.data(), 1, 1, m);
        return std::array<std::uint8_t, 4>{img[0], img[1], img[2], img[3]};
    };
    auto expect = [](std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        return std::array<std::uint8_t, 4>{r, g, b, a};
    };
    float m[12];

    sui::filterColorMatrix("brightness(0.5)", m);
    check(apply(m, 255, 0, 0, 255) == expect(128, 0, 0, 255),
          "brightness(0.5) halves red and rounds half up");
    check(apply(m, 10, 20, 30, 200) == expect(5, 10, 15, 200),
          "brightness scales only the colour channels");

    sui::filterColorMatrix("grayscale(1)", m);
    check(apply(m, 255, 0, 0, 255) == expect(54, 54, 54, 255),
          "grayscale(1) collapses red to its luma (0.2126)");

    sui::filterColorMatrix("sepia(1)", m);
    check(apply(m, 255, 0, 0, 255) == expect(100, 89, 69, 255),
          "sepia(1) tones pure red through the spec's rows");

    sui::filterColorMatrix("invert(1)", m);
    check(apply(m, 255, 0, 0, 255) == expect(0, 255, 255, 255),
          "invert(1) flips every channel");

    sui::filterColorMatrix("contrast(2)", m);
    check(apply(m, 100, 100, 100, 255) == expect(72, 72, 72, 255),
          "contrast(2) pulls below the 128 pivot toward black");

    sui::filterColorMatrix("saturate(0)", m);
    check(apply(m, 255, 0, 0, 255) == expect(54, 54, 54, 255),
          "saturate(0) desaturates red to the spec's luma row");

    sui::filterColorMatrix("hue-rotate(0)", m);
    check(apply(m, 123, 45, 67, 255) == expect(123, 45, 67, 255),
          "hue-rotate(0) is the identity");

    // A chain composes left to right: brightness first, then contrast. The
    // combined map is diagonal 2 with a -384 offset, so 64 lands at 2·64-384.
    sui::filterColorMatrix("brightness(0.5) contrast(4)", m);
    checkNear(m[0], 2.0f, 0.001f, "brightness-then-contrast multiplies the scales");
    checkNear(m[3], -384.0f, 0.001f, "the farther function keeps its offset");
    check(apply(m, 64, 64, 64, 255) == expect(0, 0, 0, 255),
          "the composed map clips 2·64-384 to black");
    sui::filterColorMatrix("contrast(4) brightness(0.5)", m);
    checkNear(m[3], -192.0f, 0.001f, "reversing the chain reorders the offsets");

    // A filter with no shift function, or with one carrying a unit suffix the
    // parser may see, must still read as identity with no shift flagged.
    auto isIdentity = [](const float n[12]) {
        if (n[0] != 1.0f || n[5] != 1.0f || n[10] != 1.0f) {
            return false;
        }
        for (int i = 0; i < 12; ++i) {
            if (i != 0 && i != 5 && i != 10 && n[i] != 0.0f) {
                return false;
            }
        }
        return true;
    };
    check(!sui::filterColorMatrix("opacity(0.5)", m) && isIdentity(m),
          "opacity() is not a shift and stays identity");
    check(!sui::filterColorMatrix("blur(6px)", m) && isIdentity(m),
          "blur() is not a shift and stays identity");
    check(!sui::filterColorMatrix("", m) && isIdentity(m),
          "an empty filter chain stays identity");
}

// The CPU clip-path mask the executor hands a composited group. Coverage is
// exact at 0, 0.5 and 1 by construction (signed distance through a half-pixel
// ramp), so every value below is hand-checkable: rect edges read 0.5 on the
// boundary, 1 inside, 0 outside; circles and ellipses behave the same around
// their own metrics; polygons wind correctly.
void testClipCoverage() {
    using Kind = sui::ResolvedClip::Kind;
    sui::ResolvedClip rect;
    rect.kind = Kind::Rect;
    rect.x = 0.0f;
    rect.y = 0.0f;
    rect.w = 10.0f;
    rect.h = 10.0f;
    check(sui::clipCoverage(rect, 5.0f, 5.0f) == 1.0f, "a rect keeps its middle");
    check(sui::clipCoverage(rect, 10.0f, 5.0f) == 0.5f, "a rect edge covers half");
    check(sui::clipCoverage(rect, 11.0f, 5.0f) == 0.0f, "a rect drops the outside");
    check(sui::clipCoverage(rect, -0.5f, 5.0f) == 0.0f, "half a pixel out is empty");

    sui::ResolvedClip rounded = rect;
    rounded.radius = 2.0f;
    check(sui::clipCoverage(rounded, 5.0f, 5.0f) == 1.0f, "a round rect keeps its middle");
    check(sui::clipCoverage(rounded, 0.0f, 0.0f) == 0.0f,
          "a round rect cuts its sharp corner");
    check(sui::clipCoverage(rounded, 0.0f, 2.0f) == 0.5f,
          "a round rect edge covers half where the arc meets the side");
    check(sui::clipCoverage(rounded, 2.0f, 0.0f) == 0.5f,
          "a round rect edge covers half where the side meets the arc");

    sui::ResolvedClip circle;
    circle.kind = Kind::Circle;
    circle.cx = 5.0f;
    circle.cy = 5.0f;
    circle.rx = 4.0f;
    circle.ry = 4.0f;
    check(sui::clipCoverage(circle, 5.0f, 5.0f) == 1.0f, "a circle keeps its centre");
    check(sui::clipCoverage(circle, 9.0f, 5.0f) == 0.5f, "a circle edge covers half");
    check(sui::clipCoverage(circle, 9.5f, 5.0f) == 0.0f, "a circle drops the outside");

    sui::ResolvedClip ellipse;
    ellipse.kind = Kind::Ellipse;
    ellipse.cx = 5.0f;
    ellipse.cy = 5.0f;
    ellipse.rx = 4.0f;
    ellipse.ry = 2.0f;
    check(sui::clipCoverage(ellipse, 5.0f, 5.0f) == 1.0f, "an ellipse keeps its centre");
    check(sui::clipCoverage(ellipse, 9.0f, 5.0f) == 0.5f,
          "an ellipse edge covers half on x");
    check(sui::clipCoverage(ellipse, 5.0f, 7.0f) == 0.5f,
          "an ellipse edge covers half on y");
    check(sui::clipCoverage(ellipse, 5.0f, 7.5f) == 0.0f, "an ellipse drops the outside");

    sui::ResolvedClip triangle;
    triangle.kind = Kind::Polygon;
    triangle.points = {SDL_FPoint{0.0f, 0.0f}, SDL_FPoint{10.0f, 0.0f},
                       SDL_FPoint{5.0f, 10.0f}};
    check(sui::clipCoverage(triangle, 5.0f, 3.0f) == 1.0f, "a polygon keeps its middle");
    check(sui::clipCoverage(triangle, 5.0f, 0.0f) == 0.5f, "a polygon edge covers half");
    check(sui::clipCoverage(triangle, 5.0f, 11.0f) == 0.0f, "a polygon drops the outside");
    check(sui::clipContains(triangle, 5.0f, 3.0f), "contains reads inside as inside");
    check(!sui::clipContains(triangle, 5.0f, 11.0f), "contains reads outside as outside");

    sui::ResolvedClip empty;
    empty.kind = Kind::Polygon;
    empty.points = {SDL_FPoint{0.0f, 0.0f}, SDL_FPoint{1.0f, 1.0f}};
    check(sui::clipCoverage(empty, 0.5f, 0.5f) == 0.0f, "a digon keeps nothing");

    sui::ResolvedClip none;
    check(sui::clipCoverage(none, -100.0f, 999.0f) == 1.0f, "no clip keeps everything");
}

// `applyClipMask` scales alpha by coverage and leaves colour alone, over a 4x4
// image with a 2x2 rect kept at (1,1): the kept pixels stay opaque, the rest
// go transparent, and the red channel is untouched throughout.
void testClipMask() {
    sui::ResolvedClip rect;
    rect.kind = sui::ResolvedClip::Kind::Rect;
    rect.x = 1.0f;
    rect.y = 1.0f;
    rect.w = 2.0f;
    rect.h = 2.0f;
    std::vector<std::uint8_t> img(4 * 4 * 4, 0);
    for (int i = 0; i < 16; ++i) {
        img[4 * i] = 200;
        img[4 * i + 3] = 255;
    }
    sui::applyClipMask(img.data(), 4, 4, rect);
    const auto alpha = [&](int x, int y) { return img[4 * (y * 4 + x) + 3]; };
    check(alpha(1, 1) == 255 && alpha(2, 2) == 255, "kept pixels stay opaque");
    check(alpha(0, 0) == 0 && alpha(3, 3) == 0 && alpha(1, 0) == 0,
          "clipped pixels go transparent");
    bool red = true;
    for (int i = 0; i < 16; ++i) {
        red = red && img[4 * i] == 200;
    }
    check(red, "masking leaves colour alone");

    sui::ResolvedClip none;
    std::vector<std::uint8_t> flat(16, 77);
    sui::applyClipMask(flat.data(), 2, 2, none);
    check(flat == std::vector<std::uint8_t>(16, 77), "no clip leaves pixels alone");

    sui::ResolvedClip offset = rect;
    sui::offsetResolvedClip(offset, -1.0f, -1.0f);
    check(offset.x == 0.0f && offset.y == 0.0f, "offsetting rebases a rect");
    sui::ResolvedClip poly;
    poly.kind = sui::ResolvedClip::Kind::Polygon;
    poly.points = {SDL_FPoint{10.0f, 20.0f}};
    sui::offsetResolvedClip(poly, 5.0f, -5.0f);
    check(poly.points[0].x == 15.0f && poly.points[0].y == 15.0f,
          "offsetting rebases polygon points");

    // `applyClipToSurface` is the executor's step: rebase a window-space shape
    // into the surface, then mask. A rect at (1,1,2,2) over a surface starting
    // at (1,1) keeps exactly its top-left 2x2.
    std::vector<std::uint8_t> framed(4 * 4 * 4, 0);
    for (int i = 0; i < 16; ++i) {
        framed[4 * i + 3] = 255;
    }
    sui::applyClipToSurface(framed.data(), 4, 4, rect, 1.0f, 1.0f);
    const auto kept = [&](int x, int y) { return framed[4 * (y * 4 + x) + 3]; };
    check(kept(0, 0) == 255 && kept(1, 1) == 255, "a rebased mask keeps the shape");
    check(kept(3, 3) == 0 && kept(2, 0) == 0, "a rebased mask drops the rest");
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
    testBackdropFilters();
    testBlendImage();
    testObjectFitDst();
    testColorMatrix();
    testClipCoverage();
    testClipMask();
    testConicalPosition();
    testMaskImage();
    testMaskLayers();
    testMaskGeometry();
    testMaskBorder();
    testBorderImageTiles();
    testShaping();
    testSvgPath();
    testSvgViewport();
    testSvgText();
    testSvgViewBoxUnits();
    testSvgImageFit();
    testListMarkerSlot();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

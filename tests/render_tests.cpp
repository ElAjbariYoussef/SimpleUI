// Geometry checks for the renderer that need no device, so the parts a screenshot
// cannot pin down precisely still have a regression test.
#include "CSS/Style.h"
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
        sui::applyMaskLayers(pixels.data(), 2, 2, {urlLayer("m.png")}, modes, {},
                             dest, 16.0f, 16.0f, fetchFor(img));
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
                             {}, {op}, dest, 16.0f, 16.0f, std::move(fetch));
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
    testColorMatrix();
    testClipCoverage();
    testClipMask();
    testConicalPosition();
    testMaskImage();
    testMaskLayers();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

#include "Core/Application/Application.h"

#include "Core/Application/Background.h"
#include "Core/Application/PaintOrder.h"

#include "CSS/Parser.h"
#include "CSS/PseudoElements.h"
#include "CSS/StyleResolver.h"
#include "Layout/BlockLayout.h"
#include "Rendering/ClipMask.h"
#include "Rendering/ColorMatrix.h"
#include "Rendering/ImageBlur.h"
#include "Rendering/ImageCache.h"
#include "Rendering/Renderer.h"
#include "Rendering/Text.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"
#include "SUI/Script/Script.h"
#include "SUI/Script/Timers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <filesystem>

#include <SDL3/SDL.h>

#include <charconv>
#include <cstdio>
#include <limits>
#include <utility>

namespace sui {
namespace {

// One open group on the paint stack. A composited group renders into `texture`;
// `previous` is the target that was active before it and gets it back when the
// group ends. When no surface could be made (zero-size bounds, or the renderer
// refused the texture), `texture` stays null and the group's members paint
// straight into whatever target is current instead, with `alpha` folded into
// each box -- the flat approximation, only as a fallback.
struct GroupFrame {
    SDL_Texture* texture = nullptr;
    SDL_Texture* previous = nullptr;
    SDL_FRect bounds{};
    float alpha = 1.0f;
    float originX = 0.0f;
    float originY = 0.0f;
    bool transformed = false;
    float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    SDL_FPoint origin{};
    // A `filter: blur()` radius to diffuse the finished surface with before the
    // blit; 0 leaves it sharp.
    float blur = 0.0f;
    // A `filter:` colour-shift chain: when `shift` is set, the affine
    // `filter[12]` is applied to the finished surface's readback before the
    // blit (blur and shift share that readback, as the two commute).
    bool shift = false;
    float filter[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    // A `filter: drop-shadow()`: a recoloured, blurred silhouette of the
    // readback, blitted offset under the content.
    DropShadowFilter dropShadow{};
    // A `clip-path` shape in window coordinates, rebased into the surface
    // before masking. The mask runs after blur (filters first, then the clip)
    // so the shadow pass traces the clipped silhouette.
    bool clip = false;
    ResolvedClip clipShape;
    // Mask layers: the specs and geometry, the three boxes rebased into the
    // surface, and the font size their lengths resolve against. Runs after the
    // clip mask, before the shadow, for the same filter-then-mask ordering.
    bool mask = false;
    std::vector<ComputedStyle::MaskImage> maskLayers;
    std::vector<ComputedStyle::MaskMode> maskModes;
    std::vector<ComputedStyle::MaskComposite> maskComposites;
    std::vector<ComputedStyle::BackgroundSize> maskSizes;
    std::vector<ComputedStyle::BackgroundPosition> maskPositions;
    std::vector<ComputedStyle::RepeatPair> maskRepeats;
    std::vector<ComputedStyle::BackgroundBox> maskOrigins;
    std::vector<ComputedStyle::BackgroundBox> maskClips;
    ComputedStyle::MaskBorder maskBorder;
    BackgroundBoxes maskBoxes{};
    float maskFontSize = 16.0f;
};

// window width/height/state arrive as markup attributes, so parse them leniently.
int attributeInt(Element& element, std::string_view key, int fallback) {
    const std::string_view raw = element.attribute(key);
    if (raw.empty()) {
        return fallback;
    }
    int value = 0;
    const auto* first = raw.data();
    const auto* last = raw.data() + raw.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{}) {
        return fallback;
    }
    return value;
}

bool attributeFlag(Element& element, std::string_view key, bool fallback) {
    const std::string_view raw = element.attribute(key);
    if (raw.empty()) {
        return fallback;
    }
    return raw != "false" && raw != "0";
}

void dumpLayout(Element& el, int depth) {
    std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    std::string text(el.text());
    if (text.size() > 24) {
        text.resize(24);
    }
    SDL_Log("%s<%.*s> x=%.1f y=%.1f w=%.1f h=%.1f runs=%zu \"%s\"",
            indent.c_str(), static_cast<int>(el.tag().size()), el.tag().data(),
            el.layoutX(), el.layoutY(), el.layoutWidth(), el.layoutHeight(),
            el.textRuns().size(), text.c_str());
    // The padding and content boxes decide what `overflow` crops to and what a
    // background layer is measured against, and neither is visible in the rects above.
    if (el.layoutWidth() > 0.0f && el.layoutHeight() > 0.0f) {
        SDL_Log("%s  padding %.1f,%.1f %.1fx%.1f  content %.1f,%.1f %.1fx%.1f",
                indent.c_str(), el.paddingX(), el.paddingY(), el.paddingWidth(),
                el.paddingHeight(), el.contentX(), el.contentY(), el.contentWidth(),
                el.contentHeight());
    }
    for (const auto& child : el.children()) {
        if (child) {
            dumpLayout(*child, depth + 1);
        }
    }
}

// Hit-testing walks the tree in paint order: the deepest drawn element whose
// border box contains the point wins, which is the element the user is actually
// pointing at. `offX`/`offY` accumulate the scroll
// offsets of clipping ancestors, so a scrolled box's children are hit where they
// are drawn rather than where layout put them.
Element* hitTest(Element& el, float x, float y, const StyleResolver& resolver, float offX,
                 float offY) {
    Element* found = nullptr;
    float childOffX = offX;
    float childOffY = offY;
    if (el.isVisual()) {
        const ComputedStyle s = resolver.resolve(el);
        const bool drawn = s.display != ComputedStyle::None && s.visible;
        if (drawn && !s.pointerEventsNone && el.layoutWidth() > 0.0f && el.layoutHeight() > 0.0f &&
            x + offX >= el.layoutX() && x + offX < el.layoutX() + el.layoutWidth() &&
            y + offY >= el.layoutY() && y + offY < el.layoutY() + el.layoutHeight()) {
            found = &el;
        }
        // A clipping box translates what is inside it, so the children are hit
        // against positions shifted by its scroll offsets.
        if (clipsAnything(s)) {
            childOffX += s.overflowX != ComputedStyle::Visible ? el.scrollLeft() : 0.0f;
            childOffY += s.overflowY != ComputedStyle::Visible ? el.scrollTop() : 0.0f;
        }
    }
    for (const auto& child : el.children()) {
        if (child && !child->isTextNode()) {
            if (Element* deeper = hitTest(*child, x, y, resolver, childOffX, childOffY)) {
                found = deeper;
            }
        }
    }
    return found;
}

// Inline attribute holding the handler for a DOM event.
const char* attributeForEvent(InputKind kind) {
    switch (kind) {
        case InputKind::PointerDown:
            return "onclick";
        case InputKind::PointerUp:
            return "onmouseup";
        case InputKind::PointerMove:
            return "onmousemove";
        case InputKind::Wheel:
            break;
        case InputKind::KeyDown:
            return "onkeydown";
        case InputKind::KeyUp:
            return "onkeyup";
        case InputKind::TextInput:
            return "oninput";
        case InputKind::TextEdit:
            return nullptr;
    }
    return nullptr;
}

// Fonts bundled with the engine take priority; the system list is only a
// development fallback so a checkout without assets still renders text.
constexpr std::array<std::string_view, 4> kBundledFonts = {
    "/assets/fonts/DejaVuSans.ttf", "/assets/fonts/Roboto-Regular.ttf",
    "/assets/fonts/Go-Regular.ttf", "/assets/fonts/FreeSans.ttf"};

constexpr std::array<std::string_view, 4> kSystemFonts = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/fonts-go/Go-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf"};

bool loadFirstAvailable(TextFont& font, const Resources* resources) {
    if (resources) {
        for (const auto& href : kBundledFonts) {
            if (!resources->exists(href)) {
                continue;
            }
            if (auto data = resources->readText(href)) {
                if (font.loadFromMemory(reinterpret_cast<const unsigned char*>(data->data()),
                                        static_cast<int>(data->size()))) {
                    return true;
                }
            }
        }
    }
    for (const auto& path : kSystemFonts) {
        if (std::filesystem::exists(path) && font.loadFromFile(std::string(path))) {
            return true;
        }
    }
    return false;
}

// A percentage radius resolves against the box's own width and height, and an `em`
// or `rem` against the font sizes, so this cannot be settled until the box is known.
sui::Radii radiiOf(const ComputedStyle& s, const SDL_FRect& box, float rootFontSize) {
    const ComputedStyle::Radii& authored = s.borderRadii;
    const auto px = [&](const Length& l, float base) {
        return l.resolve(base, s.fontSize, rootFontSize);
    };
    return sui::Radii{
        {px(authored.topLeft.x, box.w), px(authored.topLeft.y, box.h)},
        {px(authored.topRight.x, box.w), px(authored.topRight.y, box.h)},
        {px(authored.bottomRight.x, box.w), px(authored.bottomRight.y, box.h)},
        {px(authored.bottomLeft.x, box.w), px(authored.bottomLeft.y, box.h)}};
}

sui::SideDashes dashesOf(const ComputedStyle& s) {
    const auto dash = [](ComputedStyle::BorderStyle style) {
        switch (style) {
            case ComputedStyle::BorderDashed:
                return sui::Dash::Dashed;
            case ComputedStyle::BorderDotted:
                return sui::Dash::Dotted;
            default:
                return sui::Dash::Solid;
        }
    };
    return sui::SideDashes{dash(s.borderStyles.top), dash(s.borderStyles.right),
                           dash(s.borderStyles.bottom), dash(s.borderStyles.left)};
}

bool borderEdgeVisible(ComputedStyle::BorderStyle style) {
    return style != ComputedStyle::BorderStyleNone;
}

// A blurred shadow is drawn as concentric rings at falling alpha. Each ring is a real
// annulus, drawn as a stroke on its own path, which is what lets the alpha fall off
// in either direction: nested *filled* shapes can only ever accumulate coverage, so
// an inward falloff would be impossible with them.
//
// The blur radius sets how far the rings reach, and the weights are a linear ramp
// rather than a Gaussian. Nothing in CSS asks for the exact curve, and a staircase
// of a few bands is what the eye reads as a soft edge anyway.
constexpr int kShadowSteps = 6;

void paintShadowRings(const ComputedStyle::BoxShadow& shadow, const SDL_FRect& shape,
                      const sui::Radii& radii, const ComputedStyle& s, Renderer& renderer,
                      float rootFontSize, float opacity, bool inset) {
    const float blur =
        std::max(0.0f, shadow.blurRadius.resolve(0.0f, s.fontSize, rootFontSize));
    const Color base = multiplyAlpha(toColor(shadow.color), opacity);
    if (base.a == 0) {
        return;
    }
    if (blur <= 0.0f) {
        // An unblurred shadow is a single hard-edged copy of the silhouette.
        if (inset) {
            renderer.fillRect(shape, Shader::solid(base), radii);
        } else {
            renderer.strokeRect(shape, base, 1.0f, radii);
        }
        return;
    }
    const float step = blur / kShadowSteps;
    // Weights fall off linearly outward and sum to one, so the ring at the
    // silhouette carries the whole authored alpha and the far tail carries almost
    // none of it.
    const float weightSum = static_cast<float>(kShadowSteps * (kShadowSteps + 1)) * 0.5f;
    for (int i = 1; i <= kShadowSteps; ++i) {
        // A ring is filled inward from its own path, so this places band `i` over
        // exactly the slice from (i-1) steps out to i, and the six of them tile the
        // blur radius with no gap at the silhouette and no overlap anywhere.
        const float reach = static_cast<float>(i - 1) * step;
        // A ring fills inward from its own path. An outset band therefore has to be
        // the path *outside* its band, while an inset one is the path *inside* its own.
        const float outside = inset ? reach : reach + step;
        const SDL_FRect path{inset ? shape.x + reach : shape.x - outside,
                             inset ? shape.y + reach : shape.y - outside,
                             inset ? shape.w - reach * 2.0f : shape.w + outside * 2.0f,
                             inset ? shape.h - reach * 2.0f : shape.h + outside * 2.0f};
        if (path.w <= 0.0f || path.h <= 0.0f) {
            continue;
        }
        const sui::Radii ringRadii =
            radii.any() ? sui::Radii::all(inset ? std::max(0.0f, radii.topLeft.x - reach)
                                                : radii.topLeft.x + reach,
                                          inset ? std::max(0.0f, radii.topLeft.y - reach)
                                                : radii.topLeft.y + reach)
                        : radii;
        const float weight = static_cast<float>(kShadowSteps + 1 - i) * 2.0f / weightSum;
        renderer.strokeRect(path, multiplyAlpha(base, weight), step, ringRadii);
    }
}

// An outset shadow paints behind the box. A list of them all draws, with the first
// entry on top, so the list is walked backwards.
void paintBoxShadow(const ComputedStyle& s, const SDL_FRect& box, const sui::Radii& radii,
                    Renderer& renderer, float rootFontSize, float opacity) {
    for (auto it = s.boxShadows.rbegin(); it != s.boxShadows.rend(); ++it) {
        const ComputedStyle::BoxShadow& shadow = *it;
        if (shadow.inset) {
            continue;
        }
        const float dx = shadow.offsetX.resolve(0.0f, s.fontSize, rootFontSize);
        const float dy = shadow.offsetY.resolve(0.0f, s.fontSize, rootFontSize);
        const float spread = shadow.spread.resolve(0.0f, s.fontSize, rootFontSize);
        const SDL_FRect shape{box.x + dx - spread, box.y + dy - spread,
                              box.w + spread * 2.0f, box.h + spread * 2.0f};
        if (shape.w <= 0.0f || shape.h <= 0.0f) {
            continue;
        }
        // A positive spread pushes the silhouette out and the radius with it, so the
        // shadow keeps the corner's proportions instead of sharpening as it grows.
        const sui::Radii grown =
            radii.any() ? sui::Radii::all(std::max(0.0f, radii.topLeft.x + spread),
                                          std::max(0.0f, radii.topLeft.y + spread))
                        : radii;
        paintShadowRings(shadow, shape, grown, s, renderer, rootFontSize, opacity, false);
    }
}

void paintInsetShadow(const ComputedStyle& s, const SDL_FRect& box, const sui::Radii& radii,
                      Renderer& renderer, float rootFontSize, float opacity) {
    if (s.boxShadows.empty() || box.w <= 0.0f || box.h <= 0.0f) {
        return;
    }
    renderer.pushClip(box);
    for (const ComputedStyle::BoxShadow& shadow : s.boxShadows) {
        if (!shadow.inset) {
            continue;
        }
        const float spread = shadow.spread.resolve(0.0f, s.fontSize, rootFontSize);
        // A negative spread pulls the shadow's edge in from the box's own, so it only
        // darkens what is left of the band between the two.
        const SDL_FRect shape{box.x - spread, box.y - spread, box.w + spread * 2.0f,
                              box.h + spread * 2.0f};
        paintShadowRings(shadow, shape, radii, s, renderer, rootFontSize, opacity, true);
    }
    renderer.popClip();
}

// One text shadow: the run drawn again, offset, then blurred by stamping the glyphs
// out along the blur radius at falling alpha. A stencil per band would need a surface
// the offscreen path does not have yet, so the stamps stand in for it.
void paintTextShadow(const ComputedStyle::TextShadow& shadow, const Element::TextRun& run,
                     const TextFont& font, SDL_Renderer* sdlRenderer, Renderer& renderer,
                     float fontSize, float rootFontSize, float opacity) {
    const float dx = shadow.offsetX.resolve(0.0f, fontSize, rootFontSize);
    const float dy = shadow.offsetY.resolve(0.0f, fontSize, rootFontSize);
    const float blur = std::max(0.0f, shadow.blurRadius.resolve(0.0f, fontSize, rootFontSize));
    const Color base = multiplyAlpha(toColor(shadow.color), opacity);
    if (base.a == 0) {
        return;
    }
    const SDL_Color tint{base.r, base.g, base.b, base.a};
    const int bands = blur > 0.0f ? kShadowSteps : 1;
    for (int band = 0; band < bands; ++band) {
        // The stamp nearest the run carries the most of the alpha, and the ones further
        // out fill in the blur. Linear, like the box shadow's ramp.
        const float reach = bands > 1 ? static_cast<float>(band) * blur / (bands - 1) : 0.0f;
        const float weight =
            bands > 1 ? 1.0f - 0.6f * static_cast<float>(band) / (bands - 1) : 1.0f;
        // Offsetting along the shadow's own direction reads as blur; spreading it evenly
        // reads as a drop shadow, which is what an unblurred one is anyway.
        const float len = std::hypot(dx, dy);
        const float ux = len > 0.0f ? dx / len : 0.707f;
        const float uy = len > 0.0f ? dy / len : 0.707f;
        SDL_Texture* texture = font.renderText(sdlRenderer, run.text, run.fontSize, tint,
                                               run.bold, run.italic);
        if (!texture) {
            continue;
        }
        float texW = 0.0f, texH = 0.0f;
        SDL_GetTextureSize(texture, &texW, &texH);
        renderer.drawTexture(
            texture, SDL_FRect{std::floor(run.x + dx + ux * reach),
                               std::floor(run.y + dy + uy * reach), texW, texH},
            nullptr, Color{255, 255, 255, static_cast<Uint8>(weight * 255.0f)});


        SDL_DestroyTexture(texture);
    }
}

// A focused text field (an <input> or <textarea>) edits its value. Textareas
// are always textual; an input is textual unless its type names a control such
// as checkbox, radio, button, range, or file. Date/time pickers and color
// swatches edit as text (the value reads back verbatim, like type=number).
bool isEditableField(const Element* el) {
    if (!el) {
        return false;
    }
    if (el->tag() == "textarea") {
        return true;
    }
    if (el->tag() != "input") {
        return false;
    }
    const std::string_view type = el->attribute("type");
    return type.empty() || type == "text" || type == "password" ||
           type == "search" || type == "email" || type == "url" ||
           type == "tel" || type == "number" || type == "date" ||
           type == "time" || type == "datetime-local" || type == "month" ||
           type == "week" || type == "color";
}

// A checkbox/radio keeps its state as a boolean `checked` attribute.
void setChecked(Element& el, bool checked) {
    if (checked) {
        el.setAttribute("checked", "");
    } else {
        el.removeAttribute("checked");
    }
}

// Clears `checked` from every radio under `root` that shares `name` with
// `except`. The engine has no form scoping, so the group is document-wide.
void clearRadioGroup(Element& root, const Element& except, std::string_view name) {
    for (const auto& child : root.children()) {
        if (!child || child->isTextNode()) {
            continue;
        }
        if (child.get() != &except && child->tag() == "input" &&
            child->attribute("type") == "radio" && child->attribute("name") == name) {
            setChecked(*child, false);
        }
        clearRadioGroup(*child, except, name);
    }
}

// Applies the click a control owns: a checkbox flips, a radio takes the state
// and drops it from the rest of its group. Other elements do nothing here.
void activateControl(Element& el, Element& root) {
    if (el.hasAttribute("disabled") || el.tag() != "input") {
        return;
    }
    const std::string_view type = el.attribute("type");
    if (type == "checkbox") {
        setChecked(el, !el.hasAttribute("checked"));
    } else if (type == "radio") {
        clearRadioGroup(root, el, el.attribute("name"));
        setChecked(el, true);
    }
}

// A decimal HTML attribute (a range's `min`/`max`/`step`/`value`): surrounding
// whitespace is ignored and anything unparsable falls back, so markup can
// never smuggle a NaN into the slider.
float floatAttribute(const Element& el, const char* name, float fallback) {
    const std::string_view raw = el.attribute(name);
    std::size_t first = raw.find_first_not_of(" \t\n\r\f");
    if (first == std::string_view::npos) {
        return fallback;
    }
    const std::size_t last = raw.find_last_not_of(" \t\n\r\f");
    const char* begin = raw.data() + first;
    const char* end = raw.data() + last + 1;
    float value = 0.0f;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return fallback;
    }
    return value;
}

// A range's bounds and step, sanitized: a max below min clamps to min, and a
// missing or non-positive step means the default single step (`step="any"`
// also lands here: every value is reachable by pointer, keys move by one).
struct RangeLimits {
    float min = 0.0f;
    float max = 100.0f;
    float step = 1.0f;
};

RangeLimits rangeLimits(const Element& el) {
    RangeLimits lim;
    lim.min = floatAttribute(el, "min", 0.0f);
    lim.max = floatAttribute(el, "max", 100.0f);
    if (lim.max < lim.min) {
        lim.max = lim.min;
    }
    lim.step = floatAttribute(el, "step", 1.0f);
    if (!(lim.step > 0.0f)) {
        lim.step = 1.0f;
    }
    return lim;
}

// A range's current value: its live `value` clamped to [min, max] and snapped
// to the step grid from min. An absent value starts halfway, like a browser.
float rangeValue(const Element& el) {
    const RangeLimits lim = rangeLimits(el);
    const float fallback = lim.min + (lim.max - lim.min) * 0.5f;
    float v = floatAttribute(el, "value", fallback);
    v = std::min(std::max(v, lim.min), lim.max);
    if (lim.step > 0.0f && lim.max > lim.min) {
        v = lim.min + std::round((v - lim.min) / lim.step) * lim.step;
        v = std::min(std::max(v, lim.min), lim.max);
    }
    return v;
}

void setRangeValue(Element& el, float v) {
    const RangeLimits lim = rangeLimits(el);
    float c = std::min(std::max(v, lim.min), lim.max);
    if (lim.step > 0.0f && lim.max > lim.min) {
        c = lim.min + std::round((c - lim.min) / lim.step) * lim.step;
        c = std::min(std::max(c, lim.min), lim.max);
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4g", static_cast<double>(c));
    // A write that changes nothing still mutates the document, so skip it:
    // arrows held against a bound and drags inside one step must stay quiet.
    if (el.value() != buffer) {
        el.setValue(buffer);
    }
}

// Whether `el` is a range the pointer or keys may move. `readonly` does not
// apply to a slider; only `disabled` blocks it.
bool isRangeControl(const Element* el) {
    return el && el->tag() == "input" && el->attribute("type") == "range" &&
           !el->hasAttribute("disabled");
}

// Moves a range to the value under the pointer: the fraction of the content
// box left of `x` maps onto [min, max]. Anything else is left alone.
void dragRange(Element& el, float x) {
    if (!isRangeControl(&el)) {
        return;
    }
    const RangeLimits lim = rangeLimits(el);
    const float w = el.contentWidth();
    float f = w > 0.0f ? (x - el.contentX()) / w : 0.0f;
    f = std::min(std::max(f, 0.0f), 1.0f);
    setRangeValue(el, lim.min + f * (lim.max - lim.min));
}

// Index of the selected option: the marked one, else the first, which reads
// as selected. Callers check for an empty select first.
std::size_t selectSelectedIndex(const Element& select) {
    const std::vector<Element*> options = select.selectOptions();
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i]->hasAttribute("selected")) {
            return i;
        }
    }
    return 0;
}

// Marks option `index` selected, clearing the rest. A disabled option (or an
// out-of-range index) picks nothing and reports false, leaving an open popup
// open.
bool selectOption(Element& select, std::size_t index) {
    const std::vector<Element*> options = select.selectOptions();
    if (index >= options.size() || options[index]->hasAttribute("disabled")) {
        return false;
    }
    for (Element* option : options) {
        if (option == options[index]) {
            option->setAttribute("selected", "");
        } else {
            option->removeAttribute("selected");
        }
    }
    return true;
}

// Flips one option's mark, leaving the rest alone. Disabled options and
// out-of-range indexes change nothing.
void toggleOptionSelect(Element& select, std::size_t index) {
    const std::vector<Element*> options = select.selectOptions();
    if (index >= options.size() || options[index]->hasAttribute("disabled")) {
        return;
    }
    if (options[index]->hasAttribute("selected")) {
        options[index]->removeAttribute("selected");
    } else {
        options[index]->setAttribute("selected", "");
    }
}

// Marks exactly the options from `a` to `b`, skipping disabled ones. Writes
// land only where the mark actually changes, so re-clicking a range stays
// quiet.
void rangeOptionSelect(Element& select, std::size_t a, std::size_t b) {
    const std::vector<Element*> options = select.selectOptions();
    if (options.empty()) {
        return;
    }
    const std::size_t lo = std::min(a, b);
    const std::size_t hi = std::min(std::max(a, b), options.size() - 1);
    for (std::size_t i = 0; i < options.size(); ++i) {
        const bool want =
            i >= lo && i <= hi && !options[i]->hasAttribute("disabled");
        if (want && !options[i]->hasAttribute("selected")) {
            options[i]->setAttribute("selected", "");
        } else if (!want && options[i]->hasAttribute("selected")) {
            options[i]->removeAttribute("selected");
        }
    }
}

// Scrolls an open list so the option is visible, clamped by the select's own
// scroll range. Both boxes are layout-static, so ancestor scrolls cancel out.
void revealSelectOption(Element& select, const Element& option) {
    const float top = option.layoutY();
    const float bottom = top + option.layoutHeight();
    const float viewTop = select.contentY() + select.scrollTop();
    const float viewBottom = viewTop + select.clientHeight();
    if (top < viewTop) {
        select.setScrollTop(select.scrollTop() - (viewTop - top));
    } else if (bottom > viewBottom) {
        select.setScrollTop(select.scrollTop() + (bottom - viewBottom));
    }
}

// Moves an option index by `delta`, skipping disabled options and clamping at
// the ends. Closed-select arrows move the selection with this; open-popup
// arrows move the highlight.
std::size_t stepOptionIndex(const std::vector<Element*>& options, std::size_t from,
                            int delta) {
    if (options.empty()) {
        return 0;
    }
    std::size_t i = std::min(from, options.size() - 1);
    if (delta > 0) {
        for (int n = 0; n < delta; ++n) {
            std::size_t next = i + 1;
            while (next < options.size() && options[next]->hasAttribute("disabled")) {
                ++next;
            }
            if (next >= options.size()) {
                break;
            }
            i = next;
        }
    } else {
        for (int n = 0; n < -delta; ++n) {
            if (i == 0) {
                break;
            }
            std::size_t prev = i - 1;
            while (prev > 0 && options[prev]->hasAttribute("disabled")) {
                --prev;
            }
            if (options[prev]->hasAttribute("disabled")) {
                break;
            }
            i = prev;
        }
    }
    return i;
}

// The open popup's list rect in window coordinates: one row per option, hung
// below the select's border box and exactly its width. The row height is the
// select's own line, so paint and hit-test agree with or without a font.
struct SelectPopupGeom {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float rowH = 0.0f;
    std::size_t rows = 0;
};

SelectPopupGeom selectPopupGeom(const Element& select, float offX, float offY,
                                const ComputedStyle& style, std::size_t count) {
    return {select.layoutX() - offX, select.layoutY() - offY + select.layoutHeight(),
            select.layoutWidth(), style.fontSize * style.lineHeight, count};
}

// The option row under (x, y), or -1 outside the list.
int selectPopupRowAt(const SelectPopupGeom& geom, float x, float y) {
    if (geom.rows == 0 || geom.rowH <= 0.0f || x < geom.x || x >= geom.x + geom.w ||
        y < geom.y) {
        return -1;
    }
    const std::size_t row = static_cast<std::size_t>((y - geom.y) / geom.rowH);
    return row < geom.rows ? static_cast<int>(row) : -1;
}

// The select's paint offsets (ancestor scrolls) from a paint-op sequence, for
// positioning its popup the way hit-testing positions the select itself.
// False when the select paints no box (author `display:none`, say).
bool selectBoxOffset(const std::vector<PaintOp>& ops, const Element* el, float& dx,
                     float& dy) {
    for (const auto& op : ops) {
        if (op.kind == PaintOp::Kind::Box && op.item.el == el) {
            dx = op.item.dx;
            dy = op.item.dy;
            return true;
        }
    }
    return false;
}

// Whether `el` is still reachable from the document root. The open select is
// a raw pointer into the tree, so a mutation that drops it must close the
// popup rather than paint or hit-test a freed element.
bool isReachable(Element& root, const Element* el) {
    if (&root == el) {
        return true;
    }
    for (const auto& child : root.children()) {
        if (child && !child->isTextNode() && isReachable(*child, el)) {
            return true;
        }
    }
    return false;
}

// Paints an open select's dropdown: a bordered list below the select's border
// box, one row per option, the highlighted row accented and the selected row
// shaded. Runs after the whole frame with no ancestor clip, the way a browser
// lifts a popup above the page. Row text needs a font; the list chrome paints
// without one.
void paintSelectPopup(Element& select, Renderer& renderer, SDL_Renderer* sdlRenderer,
                      const ComputedStyle& style, TextFont& font, float offX, float offY,
                      std::size_t highlight, float opacity) {
    const std::vector<Element*> options = select.selectOptions();
    if (options.empty()) {
        return;
    }
    const SelectPopupGeom geom = selectPopupGeom(select, offX, offY, style, options.size());
    const std::size_t selected = selectSelectedIndex(select);
    renderer.fillRect(SDL_FRect{geom.x, geom.y, geom.w, geom.rowH * static_cast<float>(geom.rows)},
                      multiplyAlpha(toColor(style.backgroundColor), opacity));
    for (std::size_t i = 0; i < options.size(); ++i) {
        const SDL_FRect row{geom.x, geom.y + geom.rowH * static_cast<float>(i), geom.w,
                            geom.rowH};
        const bool hot = i == highlight;
        if (hot) {
            renderer.fillRect(row, multiplyAlpha(sui::Color{59, 130, 246, 255}, opacity));
        } else if (i == selected) {
            renderer.fillRect(row, multiplyAlpha(sui::Color{63, 63, 70, 255}, opacity));
        }
        if (!font.hasFont()) {
            continue;
        }
        const std::string label = options[i]->optionLabel();
        if (label.empty()) {
            continue;
        }
        const SDL_Color base =
            hot ? SDL_Color{255, 255, 255, 255}
                : SDL_Color{style.color.r, style.color.g, style.color.b, style.color.a};
        SDL_Texture* texture = font.renderText(
            sdlRenderer, label, style.fontSize,
            SDL_Color{base.r, base.g, base.b,
                      static_cast<Uint8>(static_cast<float>(base.a) * opacity)});
        if (!texture) {
            continue;
        }
        float texW = 0.0f, texH = 0.0f;
        SDL_GetTextureSize(texture, &texW, &texH);
        const SDL_FRect dst{std::floor(geom.x + 8.0f),
                            std::floor(geom.y + geom.rowH * static_cast<float>(i)), texW,
                            texH};
        SDL_RenderTexture(sdlRenderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }
    renderer.strokeRect(
        SDL_FRect{geom.x, geom.y, geom.w, geom.rowH * static_cast<float>(geom.rows)},
        multiplyAlpha(toColor(style.borderColor), opacity), 1.0f);
}

// SDL modifier keycodes (LCTRL..RGUI and MODE). A modifier never moves the
// caret or changes a field's value, so it must not restart the caret blink.
// The OS repeats a held modifier (Shift in particular); restarting the blink on
// every repeat would leave the caret solid instead of blinking.
bool isModifierKey(int key) {
    return (key >= 0x400000e0 && key <= 0x400000e7) || key == 0x40000101;
}

// Whether a caret with the given blink start is in the visible half of its
// 500 ms cycle. `caretBlinkStart_` can be newer than the frame's `ticks`
// snapshot because input is dispatched after that snapshot is taken; that reset
// still counts as visible rather than underflowing the unsigned subtraction.
bool caretVisible(std::uint64_t ticks, std::uint64_t blinkStart) {
    return ticks < blinkStart || ((ticks - blinkStart) / 500) % 2 == 0;
}

// Width of `text` at `fontSize`: whatever the loaded font measures, or the
// layout fallback of half an em per glyph when no font is available.
float fieldTextWidth(const TextFont* font, std::string_view text, float fontSize) {
    if (font && font->hasFont() && fontSize > 0.0f) {
        return font->measureText(text, fontSize);
    }
    std::size_t cps = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++cps;
        }
    }
    return static_cast<float>(cps) * fontSize * 0.5f;
}

std::size_t glyphCount(std::string_view text) {
    std::size_t cps = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++cps;
        }
    }
    return cps;
}

float resolvedLetterSpacing(const ComputedStyle& s) {
    return s.letterSpacing.unit != sui::Length::Auto
               ? s.letterSpacing.resolve(s.fontSize, s.fontSize, 16.0f)
               : 0.0f;
}

// How many bytes of a word's text run one source character draws. A masked
// password run renders bullets (three bytes per character), a plain run renders
// the text itself, so this is 3 or 1.
std::size_t runMaskFactor(std::string_view runText, std::size_t wordGlyphs) {
    return wordGlyphs > 0 && runText.size() >= wordGlyphs ? runText.size() / wordGlyphs : 1;
}

// Width of the first `bytes` bytes of a word's text run, with the field's
// letter-spacing added per glyph. The run holds the glyphs exactly as the line
// grid drew them, so measuring it matches the rendered caret column.
float wordPrefixWidth(const TextFont* font, std::string_view runText, std::size_t bytes,
                      float fontSize, float letterSpacing) {
    if (bytes > runText.size()) {
        bytes = runText.size();
    }
    const std::string_view prefix = runText.substr(0, bytes);
    float w = fieldTextWidth(font, prefix, fontSize);
    const std::size_t cps = glyphCount(prefix);
    if (letterSpacing != 0.0f && cps > 0) {
        w += letterSpacing * static_cast<float>(cps);
    }
    return w;
}

// A field's only laid-out content is its own value, and the line grid draws one
// run per whitespace-separated word of it (collapsing the gaps exactly like the
// renderer's fallback). This returns, for every run, the byte range of the
// value it drew, so a byte offset in the value maps back to a run and a column.
std::vector<std::pair<std::size_t, std::size_t>> wordSpansFor(const std::string& value,
                                                              std::size_t runCount) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    spans.reserve(runCount);
    std::size_t i = 0;
    while (spans.size() < runCount && i < value.size()) {
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        if (i == start) {
            break;
        }
        spans.push_back({start, i});
    }
    return spans;
}

// Byte offset of the start of the logical line holding byte `pos`.
std::size_t lineStart(const std::string& value, std::size_t pos) {
    const std::size_t nl = value.rfind('\n', pos == 0 ? std::string::npos : pos - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

// Byte offset of the newline that ends the logical line holding byte `pos`.
std::size_t lineEnd(const std::string& value, std::size_t pos) {
    const std::size_t nl = value.find('\n', pos);
    return nl == std::string::npos ? value.size() : nl;
}

// On-screen position of the caret at byte `pos` of a field's value. The run
// whose word contains `pos` fixes the line and the measured prefix of that word
// fixes the column. A caret in the whitespace on one line sits at the end of the
// earlier word; a caret right after a newline sits at the start of the following
// line, the way browsers place it.
void caretGeometry(const Element& el, const std::string& value, std::size_t pos,
                   const ComputedStyle& s, const TextFont* font, float offX, float offY,
                   float& x, float& y, float& h) {
    const auto& runs = el.textRuns();
    const float tx = offX + (s.overflowX != ComputedStyle::Visible ? el.scrollLeft() : 0.0f);
    const float ty = offY + (s.overflowY != ComputedStyle::Visible ? el.scrollTop() : 0.0f);
    if (runs.empty()) {
        x = std::floor(el.contentX() - offX);
        y = std::floor(el.contentY() - offY);
        h = s.fontSize > 0.0f ? s.fontSize * 1.2f : 16.0f;
        if (el.contentHeight() > h) {
            y += std::floor((el.contentHeight() - h) * 0.5f);
        }
        return;
    }
    const std::vector<std::pair<std::size_t, std::size_t>> spans = wordSpansFor(value, runs.size());
    const bool password = el.attribute("type") == "password";
    const float ls = resolvedLetterSpacing(s);

    // The run to anchor on and how many of its word's glyphs precede the caret.
    std::size_t run = runs.size() - 1;
    std::size_t runBytes = 0;
    if (!spans.empty()) {
        for (std::size_t k = 0; k < spans.size(); ++k) {
            if (pos <= spans[k].first) {
                // At or before this word. A caret in the gap between two words of
                // one line belongs to the earlier word's end; a gap that runs over
                // a newline starts the following line at this word's origin.
                if (k > 0 && pos > spans[k - 1].second) {
                    bool newlineGap = false;
                    for (std::size_t g = spans[k - 1].second; g < spans[k].first; ++g) {
                        if (value[g] == '\n') {
                            newlineGap = true;
                            break;
                        }
                    }
                    if (!newlineGap) {
                        run = k - 1;
                        runBytes = runs[run].text.size();
                        break;
                    }
                }
                run = k;
                runBytes = 0;
                break;
            }
            if (pos <= spans[k].second) {
                run = k;
                const std::size_t valueBytes = pos - spans[k].first;
                const std::size_t factor =
                    runMaskFactor(runs[run].text, glyphCount(std::string_view(value).substr(
                                                      spans[k].first, spans[k].second - spans[k].first)));
                runBytes = valueBytes * factor;
                if (password) {
                    runBytes = std::min(runBytes, runs[run].text.size());
                }
                break;
            }
        }
        if (pos > spans.back().second) {
            run = spans.size() - 1;
            runBytes = runs[run].text.size();  // trailing space or end of value
        }
    }
    const Element::TextRun& r = runs[run];
    const float prefix = wordPrefixWidth(font, r.text, runBytes, r.fontSize, ls);
    x = std::floor(r.x - tx + prefix);
    y = std::floor(r.y - ty);
    h = r.height > 0.0f ? r.height : (r.fontSize * 1.2f);
}

// Maps a screen position inside a field to a byte offset of its value. The
// vertical coordinate picks the line whose run band is nearest, then the
// horizontal coordinate picks the byte inside the closest word of that line.
std::size_t byteAt(const Element& el, const std::string& value, float screenX, float screenY,
                   const ComputedStyle& s, const TextFont* font) {
    const auto& runs = el.textRuns();
    if (runs.empty()) {
        return value.size();
    }
    const std::vector<std::pair<std::size_t, std::size_t>> spans = wordSpansFor(value, runs.size());
    if (spans.empty()) {
        return value.size();
    }
    const float tx = (s.overflowX != ComputedStyle::Visible ? el.scrollLeft() : 0.0f);
    const float ls = resolvedLetterSpacing(s);

    // The nearest run band fixes the line.
    std::size_t line = 0;
    float bestBand = std::numeric_limits<float>::max();
    for (std::size_t k = 0; k < runs.size(); ++k) {
        const float d = std::fabs((runs[k].y + runs[k].height * 0.5f) - screenY);
        if (d < bestBand) {
            bestBand = d;
            line = k;
        }
    }
    // The nearest word of that line fixes the word.
    std::size_t word = line;
    float bestX = std::numeric_limits<float>::max();
    for (std::size_t k = 0; k < runs.size(); ++k) {
        if (std::fabs(runs[k].y - runs[line].y) > 0.5f) {
            continue;
        }
        const float d = std::fabs((runs[k].x + runs[k].width * 0.5f) - screenX);
        if (d < bestX) {
            bestX = d;
            word = k;
        }
    }
    if (word >= spans.size()) {
        return value.size();
    }

    const Element::TextRun& r = runs[word];
    const float localX = screenX - (r.x - tx);
    if (localX <= 0.0f) {
        return spans[word].first;
    }
    if (localX >= r.width) {
        return spans[word].second;
    }
    // The closest glyph boundary inside the word.
    std::size_t bestByte = 0;
    float bestDist = std::fabs(localX);
    for (std::size_t b = 0; b <= r.text.size();) {
        const float w = wordPrefixWidth(font, r.text, b, r.fontSize, ls);
        const float d = std::fabs(localX - w);
        if (d < bestDist) {
            bestDist = d;
            bestByte = b;
        }
        if (b == r.text.size()) {
            break;
        }
        ++b;
        while (b < r.text.size() && (static_cast<unsigned char>(r.text[b]) & 0xC0) == 0x80) {
            ++b;
        }
    }
    const std::size_t valueBytes =
        bestByte /
        runMaskFactor(r.text, glyphCount(std::string_view(value).substr(
                                    spans[word].first, spans[word].second - spans[word].first)));
    return spans[word].first + valueBytes;
}

// The byte offset the caret moves to when it travels one logical line up
// (dir < 0) or down (dir > 0), keeping its column: the width of the current
// line's prefix before the caret is rematched against the target line.
std::size_t moveCaretLine(const TextFont* font, const std::string& value, std::size_t pos,
                          int dir, const ComputedStyle& s) {
    if (value.empty()) {
        return 0;
    }
    const std::size_t start = lineStart(value, pos);
    const std::size_t end = lineEnd(value, pos);
    const std::size_t columnBytes = pos - start;
    const std::string_view columnText = std::string_view(value).substr(start, columnBytes);
    std::size_t targetStart = 0;
    if (dir < 0) {
        if (start == 0) {
            return pos;  // already on the first line
        }
        targetStart = lineStart(value, start - 1);
    } else {
        if (end >= value.size()) {
            return pos;  // already on the last line
        }
        targetStart = end + 1;
    }
    const std::size_t targetEnd = lineEnd(value, targetStart);
    const std::string_view target =
        std::string_view(value).substr(targetStart, targetEnd - targetStart);
    if (target.empty()) {
        return targetStart;
    }
    const float ls = resolvedLetterSpacing(s);
    const float column = fieldTextWidth(font, columnText, s.fontSize) +
                         (ls != 0.0f ? ls * static_cast<float>(glyphCount(columnText)) : 0.0f);
    std::size_t best = 0;
    float bestDist = std::fabs(column);
    for (std::size_t b = 0; b <= target.size();) {
        const std::string_view prefix = target.substr(0, b);
        const float w = fieldTextWidth(font, prefix, s.fontSize) +
                        (ls != 0.0f ? ls * static_cast<float>(glyphCount(prefix)) : 0.0f);
        const float d = std::fabs(column - w);
        if (d < bestDist) {
            bestDist = d;
            best = b;
        }
        if (b == target.size()) {
            break;
        }
        ++b;
        while (b < target.size() && (static_cast<unsigned char>(target[b]) & 0xC0) == 0x80) {
            ++b;
        }
    }
    return targetStart + best;
}

void paintBox(Element& el, Renderer& renderer, SDL_Renderer* sdlRenderer,
              const StyleResolver& resolver, const TextFont& font, float opacity,
              ImageCache& images, float offX, float offY, int caretBytePos = -1,
              bool showCaret = false, int selStart = -1, int selEnd = -1,
              const char* compositionText = nullptr, int compCursor = 0) {
    const ComputedStyle s = resolver.resolve(el);
    const SDL_FRect box{el.layoutX() - offX, el.layoutY() - offY, el.layoutWidth(),
                        el.layoutHeight()};
    const float rootFontSize = resolver.rootFontSize();
    // Border widths take no percentages or font-relative units, so they need no
    // containing block to resolve.
    const sui::Radii radii = radiiOf(s, box, rootFontSize);
    const sui::Color border = multiplyAlpha(toColor(s.borderColor), opacity);

    if (box.w > 0.0f && box.h > 0.0f) {
        // A `backdrop-filter` processes what is already on the target behind
        // this box, before the box paints anything of its own over it. Only
        // blur() and colour operations apply; `opacity()` and `drop-shadow()`
        // in the chain stay unapplied.
        if (!s.backdropFilter.empty() && s.backdropFilter != "none") {
            SDL_Rect clipRect;
            const bool clipped = SDL_GetRenderClipRect(sdlRenderer, &clipRect);
            int rx = static_cast<int>(std::floor(box.x));
            int ry = static_cast<int>(std::floor(box.y));
            int rw = static_cast<int>(std::ceil(box.w));
            int rh = static_cast<int>(std::ceil(box.h));
            if (clipped) {
                const int right = std::min(rx + rw, clipRect.x + clipRect.w);
                const int bottom = std::min(ry + rh, clipRect.y + clipRect.h);
                rx = std::max(rx, clipRect.x);
                ry = std::max(ry, clipRect.y);
                rw = right - rx;
                rh = bottom - ry;
            }
            if (rw > 0 && rh > 0) {
                const SDL_Rect region{rx, ry, rw, rh};
                SDL_Surface* read = SDL_RenderReadPixels(sdlRenderer, &region);
                if (read) {
                    SDL_Surface* conv =
                        SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
                    SDL_DestroySurface(read);
                    if (conv) {
                        std::uint8_t* pixels = static_cast<std::uint8_t*>(conv->pixels);
                        float matrix[12];
                        if (sui::filterColorMatrix(s.backdropFilter, matrix)) {
                            sui::applyColorMatrix(pixels, conv->w, conv->h, matrix);
                        }
                        const int blurRadius =
                            std::lround(filterBlur(s.backdropFilter));
                        if (blurRadius > 0) {
                            sui::blurRGBA(pixels, conv->w, conv->h, blurRadius);
                        }
                        SDL_Texture* backdrop =
                            SDL_CreateTextureFromSurface(sdlRenderer, conv);
                        SDL_DestroySurface(conv);
                        if (backdrop) {
                            SDL_SetTextureBlendMode(backdrop, SDL_BLENDMODE_BLEND);
                            const SDL_FRect dst{static_cast<float>(rx), static_cast<float>(ry),
                                                static_cast<float>(rw), static_cast<float>(rh)};
                            SDL_RenderTexture(sdlRenderer, backdrop, nullptr, &dst);
                            SDL_DestroyTexture(backdrop);
                        }
                    }
                }
            }
        }

        // An outset shadow paints behind the box, an inset one over the background
        // and below the content, which is the order CSS uses.
        paintBoxShadow(s, box, radii, renderer, rootFontSize, opacity);

        // The three boxes a background layer can be relative to, all in window
        // coordinates. Layout already recorded the inner two, so the percentages in
        // the padding are not resolved a second time and have to be re-derived here.
        const BackgroundBoxes boxes{
            box,
            {el.paddingX() - offX, el.paddingY() - offY, el.paddingWidth(),
             el.paddingHeight()},
            {el.contentX() - offX, el.contentY() - offY, el.contentWidth(),
             el.contentHeight()}};
        paintBackground(s, boxes, s.fontSize, rootFontSize, opacity, renderer, images, radii);

        // An <img> or <video> paints its decoded source into the content box, honoring
        // the `object-fit` property (contain / cover / fill / none / scale-down).
        if (el.tag() == "img" || el.tag() == "video") {
            std::string_view src = el.attribute("src");
            if (el.tag() == "video") {
                const std::string_view poster = el.attribute("poster");
                if (!poster.empty()) {
                    src = poster;
                }
            }
            if (!src.empty()) {
                if (SDL_Texture* tex = images.texture(sdlRenderer, src)) {
                    float iw = 0.0f, ih = 0.0f;
                    SDL_GetTextureSize(tex, &iw, &ih);
                    const SDL_FRect content{el.contentX() - offX, el.contentY() - offY,
                                            el.contentWidth(), el.contentHeight()};
                    if (iw > 0.0f && ih > 0.0f && content.w > 0.0f && content.h > 0.0f) {
                        SDL_FRect dst{};
                        SDL_FRect* src_clip = nullptr;
                        SDL_FRect clip_rect{};
                        const ComputedStyle::ObjectFit fit = s.objectFit;
                        if (fit == ComputedStyle::ObjFill) {
                            // Stretch to fill; ignore aspect ratio.
                            dst = content;
                        } else if (fit == ComputedStyle::ObjCover) {
                            // Scale up/down to cover the whole box, then crop.
                            const float scale = std::max(content.w / iw, content.h / ih);
                            const float dw = iw * scale;
                            const float dh = ih * scale;
                            dst = {content.x + (content.w - dw) * 0.5f,
                                   content.y + (content.h - dh) * 0.5f, dw, dh};
                        } else if (fit == ComputedStyle::ObjNone) {
                            // Intrinsic size, centred; may overflow or be smaller than box.
                            dst = {content.x + (content.w - iw) * 0.5f,
                                   content.y + (content.h - ih) * 0.5f, iw, ih};
                        } else {
                            // ObjContain or ObjScaleDown: aspect-fit (scale down only for
                            // scale-down).
                            float scale = std::min(content.w / iw, content.h / ih);
                            if (fit == ComputedStyle::ObjScaleDown) {
                                scale = std::min(scale, 1.0f);
                            }
                            const float dw = iw * scale;
                            const float dh = ih * scale;
                            dst = {content.x + (content.w - dw) * 0.5f,
                                   content.y + (content.h - dh) * 0.5f, dw, dh};
                        }
                        // For cover and none the destination may overflow; clip to content box.
                        if (fit == ComputedStyle::ObjCover || fit == ComputedStyle::ObjNone) {
                            // Compute what portion of the *texture* maps to the clipped rect.
                            const float tx0 = (content.x - dst.x) / dst.w * iw;
                            const float ty0 = (content.y - dst.y) / dst.h * ih;
                            const float tw  = content.w / dst.w * iw;
                            const float th  = content.h / dst.h * ih;
                            clip_rect = {std::max(0.0f, tx0), std::max(0.0f, ty0),
                                         std::min(iw - std::max(0.0f, tx0), tw),
                                         std::min(ih - std::max(0.0f, ty0), th)};
                            src_clip = &clip_rect;
                            dst = content;
                        }
                        renderer.drawTexture(tex, dst, src_clip,
                                             Color{255, 255, 255,
                                                   static_cast<Uint8>(opacity * 255.0f)});
                    }
                }
            }
        }

        // Render audio or video controls bar
        if (el.tag() == "audio" || (el.tag() == "video" && el.hasAttribute("controls"))) {
            const SDL_FRect content{el.contentX() - offX, el.contentY() - offY,
                                    el.contentWidth(), el.contentHeight()};
            if (content.w > 0.0f && content.h > 0.0f) {
                float barH = el.tag() == "audio" ? content.h : std::min(32.0f, content.h * 0.25f);
                float barY = el.tag() == "audio" ? content.y : (content.y + content.h - barH);
                const SDL_FRect barRect{content.x, barY, content.w, barH};
                const sui::Color barBg = multiplyAlpha(sui::Color{39, 39, 42, 220}, opacity);
                SDL_SetRenderDrawColor(sdlRenderer, barBg.r, barBg.g, barBg.b, barBg.a);
                SDL_RenderFillRect(sdlRenderer, &barRect);

                float iconSize = std::min(12.0f, barH * 0.5f);
                float iconX = content.x + 8.0f;
                float iconY = barY + (barH - iconSize) * 0.5f;
                const sui::Color iconCol = multiplyAlpha(sui::Color{244, 244, 245, 255}, opacity);
                const SDL_FPoint tri[4] = {
                    {iconX, iconY},
                    {iconX + iconSize, iconY + iconSize * 0.5f},
                    {iconX, iconY + iconSize},
                    {iconX, iconY}
                };
                SDL_SetRenderDrawColor(sdlRenderer, iconCol.r, iconCol.g, iconCol.b, iconCol.a);
                SDL_RenderLines(sdlRenderer, tri, 4);

                float trackX = iconX + iconSize + 8.0f;
                float trackW = std::max(0.0f, content.x + content.w - 12.0f - trackX);
                if (trackW > 0.0f) {
                    const SDL_FRect track{trackX, barY + (barH - 4.0f) * 0.5f, trackW, 4.0f};
                    const sui::Color trackCol = multiplyAlpha(sui::Color{82, 82, 91, 200}, opacity);
                    SDL_SetRenderDrawColor(sdlRenderer, trackCol.r, trackCol.g, trackCol.b, trackCol.a);
                    SDL_RenderFillRect(sdlRenderer, &track);
                }
            }
        }

        paintInsetShadow(s, box, radii, renderer, rootFontSize, opacity);

        if (border.a > 0) {
            // A side CSS leaves invisible is dropped outright rather than drawn with a
            // zero width, and four sides that disagree about width or pattern are drawn
            // one at a time so they are not forced into a single ring.
            const float widths[4] = {
                borderEdgeVisible(s.borderStyles.top) ? s.borderWidth.top.resolve() : 0.0f,
                borderEdgeVisible(s.borderStyles.right) ? s.borderWidth.right.resolve() : 0.0f,
                borderEdgeVisible(s.borderStyles.bottom) ? s.borderWidth.bottom.resolve() : 0.0f,
                borderEdgeVisible(s.borderStyles.left) ? s.borderWidth.left.resolve() : 0.0f};
            const sui::SideDashes dashes = dashesOf(s);
            const bool uniform = widths[0] > 0.0f && widths[0] == widths[1] &&
                                 widths[0] == widths[2] && widths[0] == widths[3];
            if (uniform) {
                renderer.strokeRect(box, border, widths[0], radii, dashes);
            } else {
                const Side sides[4] = {Side::Top, Side::Right, Side::Bottom, Side::Left};
                for (int i = 0; i < 4; ++i) {
                    if (widths[i] > 0.0f) {
                        renderer.strokeSide(box, sides[i], border, widths[i], radii,
                                            dashes.of(sides[i]));
                    }
                }
            }
        }

        // The outline sits outside the border box and takes no space, so it is drawn
        // without touching layout at all.
        const float outlineWidth = s.outline.width.resolve(0.0f, s.fontSize, rootFontSize);
        if (borderEdgeVisible(s.outline.style) && outlineWidth > 0.0f) {
            const float offset = s.outline.offset.resolve(0.0f, s.fontSize, rootFontSize);
            const SDL_FRect shape{box.x - offset - outlineWidth, box.y - offset - outlineWidth,
                                  box.w + (offset + outlineWidth) * 2.0f,
                                  box.h + (offset + outlineWidth) * 2.0f};
            const sui::Radii outlineRadii =
                radii.any() ? sui::Radii::all(radii.topLeft.x + offset + outlineWidth,
                                              radii.topLeft.y + offset + outlineWidth)
                            : radii;
            const auto dash = [](ComputedStyle::BorderStyle style) {
                switch (style) {
                    case ComputedStyle::BorderDashed:
                        return sui::Dash::Dashed;
                    case ComputedStyle::BorderDotted:
                        return sui::Dash::Dotted;
                    default:
                        return sui::Dash::Solid;
                }
            };
            renderer.strokeRect(shape, multiplyAlpha(toColor(s.outline.color), opacity),
                               outlineWidth, outlineRadii,
                               sui::SideDashes::all(dash(s.outline.style)));
        }
    }

    // A checked checkbox/radio draws its own control: a tick for the box, a dot
    // for the circle. The background and border were painted above, and neither
    // mark needs a font.
    if (el.tag() == "input" && el.hasAttribute("checked")) {
        const std::string_view type = el.attribute("type");
        if (type == "checkbox" || type == "radio") {
            const SDL_FRect content{el.contentX() - offX, el.contentY() - offY,
                                    el.contentWidth(), el.contentHeight()};
            if (content.w > 0.0f && content.h > 0.0f) {
                const sui::Color mark = multiplyAlpha(toColor(s.color), opacity);
                if (type == "radio") {
                    renderer.fillCircle(content.x + content.w * 0.5f,
                                        content.y + content.h * 0.5f,
                                        std::min(content.w, content.h) * 0.28f, mark);
                } else {
                    const SDL_FPoint a{content.x + content.w * 0.20f,
                                       content.y + content.h * 0.52f};
                    const SDL_FPoint b{content.x + content.w * 0.42f,
                                       content.y + content.h * 0.76f};
                    const SDL_FPoint c{content.x + content.w * 0.82f,
                                       content.y + content.h * 0.24f};
                    renderer.line(a.x, a.y, b.x, b.y, mark, 1.5f);
                    renderer.line(b.x, b.y, c.x, c.y, mark, 1.5f);
                }
            }
        }
    }

    // A range draws its own slider: a groove across the content box and a thumb
    // at the value's fraction. No font needed, and a disabled range still paints.
    if (el.tag() == "input" && el.attribute("type") == "range") {
        const SDL_FRect content{el.contentX() - offX, el.contentY() - offY,
                                el.contentWidth(), el.contentHeight()};
        if (content.w > 0.0f && content.h > 0.0f) {
            const RangeLimits lim = rangeLimits(el);
            const float span = lim.max - lim.min;
            const float f = span > 0.0f ? (rangeValue(el) - lim.min) / span : 0.0f;
            const float trackH = std::min(4.0f, content.h);
            const SDL_FRect track{content.x, content.y + (content.h - trackH) * 0.5f,
                                  content.w, trackH};
            renderer.fillRect(track, multiplyAlpha(toColor(s.borderColor), opacity));
            const float r = std::min(content.h * 0.5f - 1.0f, 7.0f);
            if (r > 0.0f) {
                renderer.fillCircle(content.x + f * content.w,
                                    content.y + content.h * 0.5f, r,
                                    multiplyAlpha(toColor(s.color), opacity));
            }
        }
    }

    // Date/time/color/file inputs paint their own indicator: a calendar or
    // clock glyph, a swatch of the value, or a Browse button. The value (or
    // file name) flows as normal text runs, and the UA padding keeps it clear
    // of the indicator. All shapes paint without a font; only the button label
    // needs one.
    if (el.tag() == "input") {
        const std::string_view type = el.attribute("type");
        const bool calendar = type == "date" || type == "month" || type == "week" ||
                              type == "datetime-local";
        if (calendar || type == "time" || type == "color" || type == "file") {
            const SDL_FRect content{el.contentX() - offX, el.contentY() - offY,
                                    el.contentWidth(), el.contentHeight()};
            if (content.w > 0.0f && content.h > 0.0f) {
                const sui::Color ink = multiplyAlpha(toColor(s.color), opacity);
                if (calendar) {
                    const float gx = content.x + content.w - 15.0f;
                    const float gy = content.y + (content.h - 14.0f) * 0.5f;
                    renderer.strokeRect(SDL_FRect{gx, gy, 14.0f, 14.0f}, ink, 1.0f);
                    renderer.line(gx, gy + 4.0f, gx + 14.0f, gy + 4.0f, ink, 1.0f);
                    renderer.line(gx + 3.5f, gy - 2.5f, gx + 3.5f, gy + 2.5f, ink, 1.5f);
                    renderer.line(gx + 10.5f, gy - 2.5f, gx + 10.5f, gy + 2.5f, ink, 1.5f);
                } else if (type == "time") {
                    const float cx = content.x + content.w - 8.0f;
                    const float cy = content.y + content.h * 0.5f;
                    std::vector<SDL_FPoint> ring;
                    ring.reserve(16);
                    for (int k = 0; k < 16; ++k) {
                        const float a = 2.0f * 3.14159265f * static_cast<float>(k) / 16.0f;
                        ring.push_back(
                            SDL_FPoint{cx + 7.0f * std::cos(a), cy + 7.0f * std::sin(a)});
                    }
                    renderer.strokePath(ring, ink, 1.0f, true);
                    renderer.line(cx, cy, cx, cy - 4.5f, ink, 1.5f);
                    renderer.line(cx, cy, cx + 3.5f, cy, ink, 1.5f);
                } else if (type == "color") {
                    const float sw = 22.0f;
                    const float sh = std::min(14.0f, content.h);
                    const SDL_FRect swatch{content.x + content.w - sw - 2.0f,
                                           content.y + (content.h - sh) * 0.5f, sw, sh};
                    SDL_Color parsed{0, 0, 0, 255};
                    if (!parseColor(el.value(), parsed)) {
                        parsed = SDL_Color{0, 0, 0, 255};
                    }
                    renderer.fillRect(swatch, multiplyAlpha(toColor(parsed), opacity));
                    renderer.strokeRect(swatch, multiplyAlpha(toColor(s.borderColor), opacity),
                                        1.0f);
                } else {
                    // A Browse button hangs in the left padding room.
                    const SDL_FRect button{content.x - 78.0f, content.y - 4.0f, 72.0f,
                                           content.h + 8.0f};
                    renderer.fillRect(button,
                                      multiplyAlpha(sui::Color{39, 39, 42, 255}, opacity));
                    renderer.strokeRect(button,
                                        multiplyAlpha(sui::Color{63, 63, 70, 255}, opacity),
                                        1.0f);
                    if (font.hasFont()) {
                        SDL_Texture* texture = font.renderText(
                            sdlRenderer, "Browse...", s.fontSize,
                            SDL_Color{s.color.r, s.color.g, s.color.b,
                                      static_cast<Uint8>(static_cast<float>(s.color.a) *
                                                         opacity)});
                        if (texture) {
                            float texW = 0.0f, texH = 0.0f;
                            SDL_GetTextureSize(texture, &texW, &texH);
                            const SDL_FRect dst{button.x + (button.w - texW) * 0.5f,
                                                button.y + (button.h - texH) * 0.5f, texW,
                                                texH};
                            SDL_RenderTexture(sdlRenderer, texture, nullptr, &dst);
                            SDL_DestroyTexture(texture);
                        }
                    }
                }
            }
        }
    }

    // A listbox option shows its selection: dropdown options never paint a box,
    // so any option box belongs to an open list.
    if (el.tag() == "option" && el.hasAttribute("selected")) {
        const SDL_FRect box{el.layoutX() - offX, el.layoutY() - offY, el.layoutWidth(),
                            el.layoutHeight()};
        if (box.w > 0.0f && box.h > 0.0f) {
            renderer.fillRect(box, multiplyAlpha(sui::Color{59, 130, 246, 255}, opacity));
        }
    }

    if (!font.hasFont()) {
        return;
    }

    // List marker: draw bullet or ordinal to the left of <li> box
    if (el.tag() == "li" && s.listStyleType != ComputedStyle::LSTNone) {
        int ordinal = 1;
        if (s.listStyleType == ComputedStyle::LSTDecimal) {
            if (const Element* par = el.parent()) {
                for (const auto& sib : par->children()) {
                    if (!sib || sib.get() == &el) break;
                    if (!sib->isTextNode() && sib->tag() == "li") ++ordinal;
                }
            }
        }
        std::string marker;
        if (s.listStyleType == ComputedStyle::LSTDecimal) {
            marker = std::to_string(ordinal) + ".";
        } else if (s.listStyleType == ComputedStyle::LSTCircle) {
            marker = "\xE2\x97\x8B";  // U+25CB ○
        } else if (s.listStyleType == ComputedStyle::LSTSquare) {
            marker = "\xE2\x96\xAA";  // U+25AA ▪
        } else {
            marker = "\xE2\x80\xA2";  // U+2022 •
        }

        const SDL_Color markerColor{
            s.color.r, s.color.g, s.color.b,
            static_cast<Uint8>(static_cast<float>(s.color.a) * opacity)};
        SDL_Texture* markerTex = font.renderText(sdlRenderer, marker, s.fontSize,
                                                 markerColor, s.fontBold,
                                                 s.fontStyle != ComputedStyle::FontNormal);
        if (markerTex) {
            float mTexW = 0.0f, mTexH = 0.0f;
            SDL_GetTextureSize(markerTex, &mTexW, &mTexH);
            const float markerX = box.x - mTexW - 8.0f;
            const float markerY = box.y;
            const SDL_FRect dst{std::floor(markerX), std::floor(markerY), mTexW, mTexH};
            SDL_RenderTexture(sdlRenderer, markerTex, nullptr, &dst);
            SDL_DestroyTexture(markerTex);
        }
    }

    // Draw the selection highlight for a focused field before drawing glyphs.
    // Runs are grouped by line so every chosen line paints one bar, covering the
    // whitespace gaps between words the way a browser's selection does.
    if (selStart >= 0 && selEnd > selStart && isEditableField(&el) &&
        !el.textRuns().empty()) {
        const std::string value = el.value();
        const std::vector<std::pair<std::size_t, std::size_t>> spans =
            wordSpansFor(value, el.textRuns().size());
        const float tx = offX + (s.overflowX != ComputedStyle::Visible ? el.scrollLeft() : 0.0f);
        const float ty = offY + (s.overflowY != ComputedStyle::Visible ? el.scrollTop() : 0.0f);
        const float ls = resolvedLetterSpacing(s);
        std::size_t lineStartIdx = 0;
        while (lineStartIdx < el.textRuns().size()) {
            std::size_t lineEndIdx = lineStartIdx + 1;
            while (lineEndIdx < el.textRuns().size() &&
                   std::fabs(el.textRuns()[lineEndIdx].y - el.textRuns()[lineStartIdx].y) <= 0.5f) {
                ++lineEndIdx;
            }
            // Byte range the whole line covers, and how much of it is selected.
            const std::size_t first = spans.empty() ? 0 : spans[lineStartIdx].first;
            const std::size_t last = spans.empty()
                                         ? 0
                                         : spans[lineEndIdx - 1].second;
            const std::size_t coverStart = std::max(static_cast<std::size_t>(selStart), first);
            const std::size_t coverEnd = std::min(static_cast<std::size_t>(selEnd), last);
            if (coverEnd > coverStart) {
                // Left edge: the start of the line, or the prefix of the word
                // holding the selection's start.
                float startX = 0.0f;
                float endX = 0.0f;
                for (std::size_t k = lineStartIdx; k < lineEndIdx; ++k) {
                    const Element::TextRun& r = el.textRuns()[k];
                    if (k == lineStartIdx && coverStart <= spans[k].first) {
                        startX = r.x - tx;
                    } else if (coverStart > spans[k].first && coverStart <= spans[k].second) {
                        startX = r.x - tx + wordPrefixWidth(&font, r.text, coverStart - spans[k].first,
                                                            r.fontSize, ls);
                    }
                    if (k == lineEndIdx - 1 && coverEnd >= spans[k].second) {
                        endX = r.x - tx + r.width;
                    } else if (coverEnd >= spans[k].first && coverEnd < spans[k].second) {
                        endX = r.x - tx + wordPrefixWidth(&font, r.text, coverEnd - spans[k].first,
                                                          r.fontSize, ls);
                    }
                }
                const Element::TextRun& firstRun = el.textRuns()[lineStartIdx];
                const float selH = firstRun.height > 0.0f ? firstRun.height : (firstRun.fontSize * 1.2f);
                const SDL_FRect selRect{std::floor(startX), std::floor(firstRun.y - ty),
                                        std::max(1.0f, endX - startX), selH};
                SDL_SetRenderDrawColor(sdlRenderer, 51, 144, 255,
                                       static_cast<Uint8>(128.0f * opacity));
                SDL_RenderFillRect(sdlRenderer, &selRect);
            }
            lineStartIdx = lineEndIdx;
        }
    }

    // A text shadow is the run drawn again, offset and blurred, before the run
    // itself. Both carry the group's opacity.
    for (const auto& run : el.textRuns()) {
        if (run.text.empty() || run.fontSize <= 0.0f) {
            continue;
        }
        // The list draws back to front so that the first entry lands on top, the same
        // order CSS uses. Each shadow re-renders the run, so a blur costs a render per
        // band rather than a filter on an offscreen surface.
        for (auto shadow = s.textShadows.rbegin(); shadow != s.textShadows.rend(); ++shadow) {
            paintTextShadow(*shadow, run, font, sdlRenderer, renderer, s.fontSize,
                            rootFontSize, opacity);
        }
        SDL_Texture* texture = font.renderText(
            sdlRenderer, run.text, run.fontSize,
            SDL_Color{run.r, run.g, run.b,
                      static_cast<Uint8>(static_cast<float>(run.a) * opacity)},
            run.bold, run.italic);
        if (!texture) {
            continue;
        }
        // The texture is drawn 1:1 on the pixel grid. Scaling it to the line box
        // would resample the bitmap and soften every stem, and the glyph box
        // already starts at the top of the line box, since the run's baseline is
        // the font's own ascent.
        float texW = 0.0f, texH = 0.0f;
        SDL_GetTextureSize(texture, &texW, &texH);
        // A box that clips scrolls its own text along with its descendants: the
        // run moves by this box's scroll on top of every ancestor's.
        const float tx = offX + (s.overflowX != ComputedStyle::Visible ? el.scrollLeft() : 0.0f);
        const float ty = offY + (s.overflowY != ComputedStyle::Visible ? el.scrollTop() : 0.0f);
        const SDL_FRect dst{std::floor(run.x - tx), std::floor(run.y - ty), texW, texH};
        SDL_RenderTexture(sdlRenderer, texture, nullptr, &dst);

        // Text decoration
        if (s.textDecorationLine != ComputedStyle::DecNone) {
            const float txd = offX + (s.overflowX != ComputedStyle::Visible ? el.scrollLeft() : 0.0f);
            const float tyd = offY + (s.overflowY != ComputedStyle::Visible ? el.scrollTop() : 0.0f);
            const float x0 = std::floor(run.x - txd);
            const float y0 = std::floor(run.y - tyd);
            const float w = texW;
            const float lineThick = std::max(1.0f, run.fontSize * 0.06f);
            const SDL_Color decCol = s.hasTextDecorationColor
                                         ? s.textDecorationColor
                                         : SDL_Color{run.r, run.g, run.b, run.a};
            const SDL_Color c = {
                decCol.r, decCol.g, decCol.b,
                static_cast<Uint8>(static_cast<float>(decCol.a) * opacity)};
            SDL_SetRenderDrawColor(sdlRenderer, c.r, c.g, c.b, c.a);
            // Each decoration is one rule at its own offset from the baseline:
            // under it, over the ascender, or through roughly the middle.
            float lineY = y0 + run.baseline;
            if (s.textDecorationLine == ComputedStyle::DecOverline) {
                lineY = y0 + run.baseline - run.fontSize;
            } else if (s.textDecorationLine == ComputedStyle::DecLineThrough) {
                lineY = y0 + run.baseline - run.fontSize * 0.3f;
            }
            const SDL_FRect rule{x0, lineY, w, lineThick};
            SDL_RenderFillRect(sdlRenderer, &rule);
        }
        SDL_DestroyTexture(texture);
    }

    // A closed select shows the selected option's text and a drop arrow. Its
    // options never lay out, so there are no text runs to draw; like an input
    // the text may spill when an author under-sizes the box.
    if (el.tag() == "select") {
        const SDL_FRect content{el.contentX() - offX, el.contentY() - offY,
                                el.contentWidth(), el.contentHeight()};
        const std::string label = el.value();
        if (!label.empty()) {
            SDL_Texture* texture = font.renderText(
                sdlRenderer, label, s.fontSize,
                SDL_Color{s.color.r, s.color.g, s.color.b,
                          static_cast<Uint8>(static_cast<float>(s.color.a) * opacity)});
            if (texture) {
                float texW = 0.0f, texH = 0.0f;
                SDL_GetTextureSize(texture, &texW, &texH);
                const SDL_FRect dst{std::floor(content.x), std::floor(content.y), texW,
                                    texH};
                SDL_RenderTexture(sdlRenderer, texture, nullptr, &dst);
                SDL_DestroyTexture(texture);
            }
        }
        const float ax = content.x + content.w - 7.0f;
        const float ay = content.y + content.h * 0.5f;
        renderer.fillPolygon({{ax - 5.0f, ay - 2.5f}, {ax + 5.0f, ay - 2.5f}, {ax, ay + 3.5f}},
                             multiplyAlpha(toColor(s.color), opacity));
    }

    if (caretBytePos >= 0 && isEditableField(&el) && !el.hasAttribute("disabled")) {
        float caretX = 0.0f;
        float caretY = 0.0f;
        float caretH = s.fontSize > 0.0f ? s.fontSize * 1.2f : 16.0f;
        caretGeometry(el, el.value(), static_cast<std::size_t>(caretBytePos), s, &font,
                      offX, offY, caretX, caretY, caretH);
        if (compositionText && *compositionText && font.hasFont()) {
            SDL_Texture* compTex = font.renderText(
                sdlRenderer, compositionText, s.fontSize,
                SDL_Color{s.color.r, s.color.g, s.color.b,
                          static_cast<Uint8>(static_cast<float>(s.color.a) * opacity)});
            if (compTex) {
                float cw = 0.0f, ch = 0.0f;
                SDL_GetTextureSize(compTex, &cw, &ch);
                const SDL_FRect compDst{caretX, caretY, cw, ch};
                SDL_RenderTexture(sdlRenderer, compTex, nullptr, &compDst);
                SDL_DestroyTexture(compTex);

                // Underline composition text
                const SDL_FRect compLine{caretX, caretY + caretH - 1.5f, cw, 1.5f};
                const sui::Color caretCol = multiplyAlpha(toColor(s.color), opacity);
                SDL_SetRenderDrawColor(sdlRenderer, caretCol.r, caretCol.g, caretCol.b, caretCol.a);
                SDL_RenderFillRect(sdlRenderer, &compLine);

                // Advance caret within composition
                if (compCursor >= 0) {
                    std::string_view compView(compositionText);
                    std::size_t cLen = std::min(static_cast<std::size_t>(compCursor), compView.size());
                    caretX += font.measureText(compView.substr(0, cLen), s.fontSize);
                }
            }
        }
        if (showCaret) {
            const SDL_FRect caretRect{caretX, caretY, 1.5f, caretH};
            const sui::Color caretCol = multiplyAlpha(toColor(s.color), opacity);
            SDL_SetRenderDrawColor(sdlRenderer, caretCol.r, caretCol.g, caretCol.b, caretCol.a);
            SDL_RenderFillRect(sdlRenderer, &caretRect);
        }
    }
}

std::vector<std::string> splitOnSemicolons(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t stop = text.find(';', start);
        const std::string_view piece = text.substr(
            start, stop == std::string_view::npos ? std::string_view::npos : stop - start);
        if (!piece.empty()) {
            parts.emplace_back(piece);
        }
        if (stop == std::string_view::npos) {
            break;
        }
        start = stop + 1;
    }
    return parts;
}

}  // namespace

Application::Application() = default;

Application::~Application() {
    shutdown();
}

void Application::selectAll() noexcept {
    if (document_ && document_->focused() && isEditableField(document_->focused())) {
        selectionAnchor_ = 0;
        caretBytePos_ = document_->focused()->value().size();
    }
}

void Application::resetCaretBlink() {
    caretBlinkStart_ = SDL_GetTicks();
    ++caretBlinkResets_;
}

Document& Application::document() const {
    return *document_;
}

void Application::setDocument(std::unique_ptr<Document> document) {
    document_ = std::move(document);
    scriptContext_.document = document_.get();
    scriptContext_.styles = &resolver_;
    script::setContext(&scriptContext_);
    // The open popup points into the old tree.
    openSelect_ = nullptr;
    selectHighlight_ = 0;
    selectAnchor_ = 0;
    selectEdge_ = 0;
}

WindowConfig Application::windowConfigFrom(Document& document) {
    WindowConfig config;

    Element* window = document.windowElement();
    if (!window) {
        return config;
    }

    if (const std::string_view title = window->attribute("title"); !title.empty()) {
        config.title = std::string(title);
    }
    config.width = attributeInt(*window, "width", config.width);
    config.height = attributeInt(*window, "height", config.height);

    const std::string_view state = window->attribute("state");
    config.maximized = state == "maximized" || state == "fullscreen";
    config.resizable = attributeFlag(*window, "resizable", !config.maximized);
    config.borderless = state == "borderless";
    config.hidden = attributeFlag(*window, "hidden", false);

    return config;
}

bool Application::start(std::string_view entrySuiPath, std::string& error) {
    resources_ =
        std::make_unique<FileResources>(projectRootFor(std::string(entrySuiPath)));
    images_ = std::make_unique<ImageCache>(*resources_);

    const std::string absolute = resources_->resolve(entrySuiPath);
    auto source = resources_->readText(entrySuiPath);
    if (!source) {
        error = "cannot read " + absolute;
        return false;
    }

    // SUI_SCREENSHOT renders one frame to a BMP and exits, so layout can be
    // verified without a visible window.
    if (const char* shot = SDL_getenv("SUI_SCREENSHOT")) {
        screenshotPath_ = shot;
    }

    ParseError parseError;
    document_ = MarkupParser::parse(*source, parseError);
    if (!document_) {
        error = std::string(entrySuiPath) + ":" + std::to_string(parseError.line) +
                ": " + parseError.message;
        return false;
    }

    SDL_SetAppMetadata("SimpleUI", "0.1.0", "org.simpleui.app");

    platform_ = makePlatform();
    if (!platform_->initialize(windowConfigFrom(*document_), error)) {
        platform_.reset();
        document_.reset();
        return false;
    }

    SDL_Log("loaded %s (%zu stylesheets, %zu scripts)", std::string(entrySuiPath).c_str(),
            document_->stylesheets().size(), document_->scripts().size());

    for (const auto& link : document_->stylesheets()) {
        auto css = resources_->readText(link.href);
        if (!css) {
            script::warn("cannot read stylesheet " + link.href);
            continue;
        }
        std::vector<Rule> rules;
        std::vector<KeyframeBlock> keyframes;
        // Imported sheets inline where their statements stand, fetched through
        // the same resources; relative hrefs resolve against the importing
        // sheet.
        CSSParser::parseWithKeyframes(
            *css, rules, keyframes,
            [this](std::string_view href) -> std::optional<std::string> {
                return resources_->readText(href);
            },
            link.href);
        for (auto& rule : rules) {
            resolver_.addRule(std::move(rule));
        }
        resolver_.addKeyframes(std::move(keyframes));
    }

    scriptContext_.document = document_.get();
    scriptContext_.styles = &resolver_;
    script::setContext(&scriptContext_);

    return true;
}

void Application::runScripts(bool afterBody) {
    if (!document_) {
        return;
    }
    for (const auto& reference : document_->scripts()) {
        if (reference.afterBody != afterBody) {
            continue;
        }
        const script::EntryPoint entry = script::findModule(reference.href);
        if (!entry) {
            // A .sui can name a script the build never compiled. Say so once,
            // naming the href, rather than failing silently at click time.
            script::error("no compiled script for href " + reference.href +
                          " (declare it with SUI_SCRIPT in a translation unit the "
                          "build links into this executable)");
            continue;
        }
        entry();
    }
}

void Application::updateCursor(Element* target) {
    ComputedStyle::Cursor cursor = ComputedStyle::CursorAuto;
    if (target) {
        cursor = resolver_.resolve(*target).cursor;
    }
    if (cursor == activeCursor_) {
        return;
    }
    activeCursor_ = cursor;
    // No window yet (headless, or a unit test that only set a document): the
    // decision is still observable through activeCursor(), which is the point.
    if (platform_) {
        platform_->setCursor(cursorName(cursor));
    }
}

void Application::dispatch(const InputEvent& event) {
    if (!document_ || !document_->root()) {
        return;
    }

    const bool keyboard = event.kind == InputKind::KeyDown ||
                          event.kind == InputKind::KeyUp || event.kind == InputKind::TextInput ||
                          event.kind == InputKind::TextEdit;
    // A key event has no coordinates and belongs to whatever holds focus; a pointer
    // event belongs to whatever is under it in paint order (topmost).
    Element* target = nullptr;
    // A pick from an open select popup retargets onto the select itself, and a
    // click on its own open box only closes it, so the toggle below must not
    // reopen it on the same press.
    bool suppressSelectToggle = false;
    if (keyboard) {
        target = document_->focused();
    } else {
        const auto ops = computePaintOps(*document_->root(), resolver_);
        target = hitTestPaintOrder(ops, event.x, event.y);
        if (openSelect_ && !isReachable(*document_->root(), openSelect_)) {
            openSelect_ = nullptr;
        }
        if (openSelect_ && event.kind == InputKind::PointerDown) {
            // The popup is an overlay, not an element, so it never wins the hit
            // test above; check its rows first. The select's own scroll offsets
            // come from its paint op, the way hit-testing positions it.
            float offX = 0.0f, offY = 0.0f;
            if (!selectBoxOffset(ops, openSelect_, offX, offY)) {
                // Hidden while open (author `display:none`, say): drop the popup.
                openSelect_ = nullptr;
            } else {
                const ComputedStyle style = resolver_.resolve(*openSelect_);
                const SelectPopupGeom geom = selectPopupGeom(
                    *openSelect_, offX, offY, style, openSelect_->selectOptions().size());
                const int row = selectPopupRowAt(geom, event.x, event.y);
                if (row >= 0) {
                    // A pick lands before any `onclick` handler so the handler
                    // observes the control's new value.
                    Element* picked = openSelect_;
                    if (selectOption(*picked, static_cast<std::size_t>(row))) {
                        openSelect_ = nullptr;
                    }
                    target = picked;
                    suppressSelectToggle = true;
                } else if (target == openSelect_) {
                    // Toggle off: the press closes, and must not reopen below.
                    openSelect_ = nullptr;
                    suppressSelectToggle = true;
                } else {
                    // Anywhere else closes; normal handling continues.
                    openSelect_ = nullptr;
                }
            }
        }
    }

    // Interactive state is set first and unconditionally. A move that changes what
    // is under the pointer has to clear `:hover` even when it lands on nothing, and
    // a press that misses the document has to clear `:focus`.
    // Every setter bumps the resolver generation, which is what makes the frame loop
    // treat the frame as stale.
    switch (event.kind) {
        case InputKind::PointerMove:
            resolver_.setHovered(target);
            // The pointer shape follows whatever is under it. Resolved here, after
            // the hover state is set, so a `:hover` rule that changes `cursor` is
            // honoured on the same move that entered it.
            updateCursor(target);
            // A drag keeps the active element under the cursor, so `:active` follows
            // the pointer instead of sticking to wherever the press landed. The button
            // field is zero for a plain hover, which is what separates the two.
            if (event.button != 0) {
                resolver_.setActive(target);
                // Mouse drag updates selection on a focused text field
                Element* focused = document_ ? document_->focused() : nullptr;
                if (focused && isEditableField(focused) && !focused->hasAttribute("disabled") &&
                    !focused->textRuns().empty()) {
                    const ComputedStyle cs = resolver_.resolve(*focused);
                    caretBytePos_ = static_cast<int>(byteAt(
                        *focused, focused->value(), event.x, event.y, cs, font_.get()));
                    resetCaretBlink();
                }
                // ... or moves the focused range's thumb.
                if (isRangeControl(focused)) {
                    dragRange(*focused, event.x);
                }
            }
            // Hovering an open select popup moves its highlight. The row geometry
            // comes from the select's paint offsets, the way hit-testing
            // positions it; the highlight needs no document mutation to repaint
            // because the move itself marks the frame stale.
            if (openSelect_ && document_->root() && isReachable(*document_->root(), openSelect_)) {
                const auto hoverOps = computePaintOps(*document_->root(), resolver_);
                float offX = 0.0f, offY = 0.0f;
                if (selectBoxOffset(hoverOps, openSelect_, offX, offY)) {
                    const ComputedStyle style = resolver_.resolve(*openSelect_);
                    const SelectPopupGeom geom = selectPopupGeom(
                        *openSelect_, offX, offY, style, openSelect_->selectOptions().size());
                    const int row = selectPopupRowAt(geom, event.x, event.y);
                    if (row >= 0) {
                        selectHighlight_ = static_cast<std::size_t>(row);
                    }
                }
            }
            return;
        case InputKind::PointerDown:
            resolver_.setActive(target);
            compositionText_.clear();
            compositionCursor_ = 0;
            // Clicking anywhere, including on nothing, moves focus. Disabled elements
            // cannot take focus.
            if (target && !target->hasAttribute("disabled")) {
                document_->setFocused(target);
                resolver_.setFocused(target);
                lastFocused_ = target;
                if (isEditableField(target)) {
                    resetCaretBlink();
                    caretBytePos_ = target->value().size();
                    if (!target->textRuns().empty()) {
                        const ComputedStyle cs = resolver_.resolve(*target);
                        caretBytePos_ = static_cast<int>(byteAt(
                            *target, target->value(), event.x, event.y, cs, font_.get()));
                    }
                    selectionAnchor_ = caretBytePos_;
                } else {
                    caretBytePos_ = 0;
                    selectionAnchor_ = 0;
                }
            } else {
                document_->setFocused(nullptr);
                resolver_.setFocused(nullptr);
                lastFocused_ = nullptr;
                caretBytePos_ = 0;
                selectionAnchor_ = 0;
                compositionText_.clear();
                compositionCursor_ = 0;
            }
            // A checkbox/radio toggles on click, before any `onclick` handler
            // so the handler observes the control's new state.
            if (target && document_->root()) {
                activateControl(*target, *document_->root());
            }
            // A range jumps to the value under the pointer; the drag path in
            // PointerMove takes it from there.
            if (target) {
                dragRange(*target, event.x);
            }
            // A select opens its popup on click, or closes an open one, before
            // any `onclick` handler so the handler observes the new state. A
            // pick or a toggle-off on the same press suppresses reopening. An
            // open list never pops up: its rows handle the click below.
            if (target && target->tag() == "select" && target->listboxRows() == 0 &&
                !target->hasAttribute("disabled") && !suppressSelectToggle) {
                if (openSelect_ == target) {
                    openSelect_ = nullptr;
                } else {
                    openSelect_ = target;
                    selectHighlight_ = selectSelectedIndex(*target);
                }
            }
            // An open list picks its clicked row: a plain click selects,
            // Ctrl/Cmd toggles, Shift extends from the anchor (all only when
            // `multiple`; otherwise every click selects). Focus moves to the
            // select itself so arrows keep working.
            if (target && target->tag() == "option") {
                Element* owner = target->owningSelect();
                if (owner && owner->listboxRows() > 0 &&
                    !owner->hasAttribute("disabled") &&
                    !target->hasAttribute("disabled")) {
                    document_->setFocused(owner);
                    resolver_.setFocused(owner);
                    lastFocused_ = owner;
                    const std::vector<Element*> options = owner->selectOptions();
                    std::size_t index = options.size();
                    for (std::size_t i = 0; i < options.size(); ++i) {
                        if (options[i] == target) {
                            index = i;
                            break;
                        }
                    }
                    const bool multi = owner->hasAttribute("multiple");
                    if (index < options.size()) {
                        if (multi && shiftDown_) {
                            selectEdge_ = index;
                            rangeOptionSelect(*owner, selectAnchor_, selectEdge_);
                        } else if (multi && ctrlDown_) {
                            toggleOptionSelect(*owner, index);
                            selectAnchor_ = index;
                            selectEdge_ = index;
                        } else {
                            if (selectOption(*owner, index)) {
                                selectAnchor_ = index;
                                selectEdge_ = index;
                            }
                        }
                    }
                }
            }
            break;
        case InputKind::PointerUp:
            resolver_.setActive(nullptr);
            // Releasing over an open popup's row picks it, so press-drag-release
            // works alongside click-click. Anything else leaves the popup open.
            if (openSelect_ && document_->root() &&
                isReachable(*document_->root(), openSelect_)) {
                const auto upOps = computePaintOps(*document_->root(), resolver_);
                float offX = 0.0f, offY = 0.0f;
                if (selectBoxOffset(upOps, openSelect_, offX, offY)) {
                    const ComputedStyle style = resolver_.resolve(*openSelect_);
                    const SelectPopupGeom geom = selectPopupGeom(
                        *openSelect_, offX, offY, style, openSelect_->selectOptions().size());
                    const int row = selectPopupRowAt(geom, event.x, event.y);
                    if (row >= 0 &&
                        selectOption(*openSelect_, static_cast<std::size_t>(row))) {
                        openSelect_ = nullptr;
                    }
                }
            }
            break;
        case InputKind::Wheel: {
            if (!target) {
                return;
            }
            // The wheel scrolls the nearest clipping ancestor of whatever is under
            // the pointer, per notch at a fixed pixel pitch. The setter clamps to
            // the box's range and marks the page stale.
            const float pitch = 40.0f;
            for (Element* current = target; current; current = current->parent()) {
                const ComputedStyle scrolled = resolver_.resolve(*current);
                if (clipsAnything(scrolled)) {
                    if (scrolled.overflowY != ComputedStyle::Visible) {
                        current->setScrollTop(current->scrollTop() - event.dy * pitch);
                    }
                    if (scrolled.overflowX != ComputedStyle::Visible) {
                        current->setScrollLeft(current->scrollLeft() + event.dx * pitch);
                    }
                    break;
                }
            }
            return;
        }
        case InputKind::KeyDown:
            if (event.key == 0x400000e1 || event.key == 0x400000e5) {
                shiftDown_ = true;
            } else if (event.key == 0x400000e0 || event.key == 0x400000e4 ||
                       event.key == 0x400000e3 || event.key == 0x400000e7) {
                ctrlDown_ = true;
            }
            break;
        case InputKind::KeyUp:
            if (event.key == 0x400000e1 || event.key == 0x400000e5) {
                shiftDown_ = false;
            } else if (event.key == 0x400000e0 || event.key == 0x400000e4 ||
                       event.key == 0x400000e3 || event.key == 0x400000e7) {
                ctrlDown_ = false;
            }
            break;
        case InputKind::TextInput:
        case InputKind::TextEdit:
            break;
    }

    // A focused text field edits its `value` before any handler sees the event, so
    // an `oninput` handler reads the text the field now holds.
    // Readonly and disabled fields reject edits. Non-textual types (e.g. button, submit)
    // do not edit their value on text input.
    if (target && isEditableField(target)) {
        constexpr int kBackspace = 0x08;
        constexpr int kDelete = 0x7F;
        constexpr int kHome = 0x4000004A;
        constexpr int kEnd = 0x4000004D;
        constexpr int kRight = 0x4000004F;
        constexpr int kLeft = 0x40000050;
        constexpr int kUp = 0x40000052;
        constexpr int kDown = 0x40000051;
        constexpr int kReturn = 0x0D;
        constexpr int kKPEnter = 0x40000058;
        constexpr int kA = 0x61;
        const bool readOnly = target->hasAttribute("readonly") || target->hasAttribute("disabled");
        const std::string_view type = target->attribute("type");
        // A textarea is always a multiline text editor; an input is textual
        // unless its type is a control such as button or submit. This mirrors
        // isEditableField above: both lists name the same textual types.
        const bool isTextual = target->tag() == "textarea" ||
                               (type.empty() || type == "text" || type == "password" ||
                                type == "search" || type == "email" || type == "url" ||
                                type == "tel" || type == "number" || type == "date" ||
                                type == "time" || type == "datetime-local" || type == "month" ||
                                type == "week" || type == "color");
        if (isTextual) {
            std::string value = target->value();
            if (target != lastFocused_) {
                lastFocused_ = target;
                caretBytePos_ = value.size();
                selectionAnchor_ = caretBytePos_;
                compositionText_.clear();
                compositionCursor_ = 0;
                resetCaretBlink();
            }
            if (caretBytePos_ > value.size()) {
                caretBytePos_ = value.size();
            }
            if (selectionAnchor_ > value.size()) {
                selectionAnchor_ = value.size();
            }
            while (caretBytePos_ > 0 && caretBytePos_ < value.size() &&
                   (static_cast<unsigned char>(value[caretBytePos_]) & 0xC0) == 0x80) {
                --caretBytePos_;
            }
            while (selectionAnchor_ > 0 && selectionAnchor_ < value.size() &&
                   (static_cast<unsigned char>(value[selectionAnchor_]) & 0xC0) == 0x80) {
                --selectionAnchor_;
            }

            const bool isShift = shiftDown_ || ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
            const bool isCtrl = ctrlDown_ || ((SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0);

            if (event.kind == InputKind::TextEdit) {
                if (!readOnly) {
                    compositionText_ = event.text;
                    compositionCursor_ = event.editStart;
                    resetCaretBlink();
                }
            } else if (event.kind == InputKind::KeyDown) {
                if (!isModifierKey(event.key)) {
                    resetCaretBlink();
                }
                if (isCtrl && (event.key == kA || event.key == 'A')) {
                    selectionAnchor_ = 0;
                    caretBytePos_ = value.size();
                } else if (event.key == kLeft) {
                    if (isShift) {
                        if (caretBytePos_ > 0) {
                            --caretBytePos_;
                            while (caretBytePos_ > 0 &&
                                   (static_cast<unsigned char>(value[caretBytePos_]) & 0xC0) == 0x80) {
                                --caretBytePos_;
                            }
                        }
                    } else {
                        if (selectionAnchor_ != caretBytePos_) {
                            caretBytePos_ = std::min(selectionAnchor_, caretBytePos_);
                            selectionAnchor_ = caretBytePos_;
                        } else {
                            if (caretBytePos_ > 0) {
                                --caretBytePos_;
                                while (caretBytePos_ > 0 &&
                                       (static_cast<unsigned char>(value[caretBytePos_]) & 0xC0) == 0x80) {
                                    --caretBytePos_;
                                }
                            }
                            selectionAnchor_ = caretBytePos_;
                        }
                    }
                } else if (event.key == kRight) {
                    if (isShift) {
                        if (caretBytePos_ < value.size()) {
                            ++caretBytePos_;
                            while (caretBytePos_ < value.size() &&
                                   (static_cast<unsigned char>(value[caretBytePos_]) & 0xC0) == 0x80) {
                                ++caretBytePos_;
                            }
                        }
                    } else {
                        if (selectionAnchor_ != caretBytePos_) {
                            caretBytePos_ = std::max(selectionAnchor_, caretBytePos_);
                            selectionAnchor_ = caretBytePos_;
                        } else {
                            if (caretBytePos_ < value.size()) {
                                ++caretBytePos_;
                                while (caretBytePos_ < value.size() &&
                                       (static_cast<unsigned char>(value[caretBytePos_]) & 0xC0) == 0x80) {
                                    ++caretBytePos_;
                                }
                            }
                            selectionAnchor_ = caretBytePos_;
                        }
                    }
                } else if (event.key == kHome) {
                    // Home goes to the line start in a textarea, the value start
                    // in a single-line input.
                    caretBytePos_ = target->tag() == "textarea"
                                        ? static_cast<int>(lineStart(value, static_cast<std::size_t>(caretBytePos_)))
                                        : 0;
                    if (!isShift) {
                        selectionAnchor_ = caretBytePos_;
                    }
                } else if (event.key == kEnd) {
                    caretBytePos_ = target->tag() == "textarea"
                                        ? static_cast<int>(lineEnd(value, static_cast<std::size_t>(caretBytePos_)))
                                        : static_cast<int>(value.size());
                    if (!isShift) {
                        selectionAnchor_ = caretBytePos_;
                    }
                } else if (target->tag() == "textarea" && (event.key == kUp || event.key == kDown)) {
                    const ComputedStyle cs = resolver_.resolve(*target);
                    caretBytePos_ = static_cast<int>(moveCaretLine(
                        font_.get(), value, static_cast<std::size_t>(caretBytePos_),
                        event.key == kUp ? -1 : 1, cs));
                    if (!isShift) {
                        selectionAnchor_ = caretBytePos_;
                    }
                } else if (target->tag() == "textarea" && !readOnly &&
                           (event.key == kReturn || event.key == kKPEnter)) {
                    // Return starts a new line at the caret.
                    if (selectionAnchor_ != caretBytePos_) {
                        const std::size_t sStart = std::min(selectionAnchor_, caretBytePos_);
                        const std::size_t sEnd = std::max(selectionAnchor_, caretBytePos_);
                        value.erase(sStart, sEnd - sStart);
                        caretBytePos_ = sStart;
                        selectionAnchor_ = sStart;
                    }
                    value.insert(caretBytePos_, "\n");
                    caretBytePos_ += 1;
                    selectionAnchor_ = caretBytePos_;
                    target->setValue(std::move(value));
                } else if (!readOnly && (event.key == kBackspace || event.key == kDelete)) {
                    if (selectionAnchor_ != caretBytePos_) {
                        const std::size_t sStart = std::min(selectionAnchor_, caretBytePos_);
                        const std::size_t sEnd = std::max(selectionAnchor_, caretBytePos_);
                        value.erase(sStart, sEnd - sStart);
                        caretBytePos_ = sStart;
                        selectionAnchor_ = sStart;
                        target->setValue(std::move(value));
                    } else if (event.key == kBackspace) {
                        if (caretBytePos_ > 0) {
                            std::size_t prev = caretBytePos_ - 1;
                            while (prev > 0 &&
                                   (static_cast<unsigned char>(value[prev]) & 0xC0) == 0x80) {
                                --prev;
                            }
                            value.erase(prev, caretBytePos_ - prev);
                            caretBytePos_ = prev;
                            selectionAnchor_ = prev;
                            target->setValue(std::move(value));
                        }
                    } else if (event.key == kDelete) {
                        if (caretBytePos_ < value.size()) {
                            std::size_t next = caretBytePos_ + 1;
                            while (next < value.size() &&
                                   (static_cast<unsigned char>(value[next]) & 0xC0) == 0x80) {
                                ++next;
                            }
                            value.erase(caretBytePos_, next - caretBytePos_);
                            selectionAnchor_ = caretBytePos_;
                            target->setValue(std::move(value));
                        }
                    }
                }
            } else if (!readOnly && event.kind == InputKind::TextInput) {
                compositionText_.clear();
                compositionCursor_ = 0;
                if (!event.text.empty()) {
                    resetCaretBlink();
                    if (selectionAnchor_ != caretBytePos_) {
                        const std::size_t sStart = std::min(selectionAnchor_, caretBytePos_);
                        const std::size_t sEnd = std::max(selectionAnchor_, caretBytePos_);
                        value.erase(sStart, sEnd - sStart);
                        caretBytePos_ = sStart;
                    }
                    value.insert(caretBytePos_, event.text);
                    caretBytePos_ += event.text.size();
                    selectionAnchor_ = caretBytePos_;
                    target->setValue(std::move(value));
                }
            }
        }
    }

    // A focused range steps its value on arrows/Home/End before any handler, so
    // an `onkeydown` handler reads the value the control now holds. Anything
    // that leaves the snapped value unchanged writes nothing.
    if (target && event.kind == InputKind::KeyDown && isRangeControl(target)) {
        constexpr int kHome = 0x4000004A;
        constexpr int kEnd = 0x4000004D;
        constexpr int kRight = 0x4000004F;
        constexpr int kLeft = 0x40000050;
        constexpr int kUp = 0x40000052;
        constexpr int kDown = 0x40000051;
        const RangeLimits lim = rangeLimits(*target);
        const float before = rangeValue(*target);
        float stepped = before;
        bool handled = true;
        if (event.key == kLeft || event.key == kDown) {
            stepped = before - lim.step;
        } else if (event.key == kRight || event.key == kUp) {
            stepped = before + lim.step;
        } else if (event.key == kHome) {
            stepped = lim.min;
        } else if (event.key == kEnd) {
            stepped = lim.max;
        } else {
            handled = false;
        }
        if (handled && stepped != before) {
            setRangeValue(*target, stepped);
        }
    }

    // A focused select picks on arrows/Enter/Escape before any handler, so an
    // `onkeydown` handler reads the value the control now holds. Arrows move
    // the selection when closed and the highlight when open; Enter opens, then
    // picks the highlight; Escape closes. There is no type-ahead.
    if (target && event.kind == InputKind::KeyDown && target->tag() == "select" &&
        !target->hasAttribute("disabled")) {
        constexpr int kReturn = 0x0D;
        constexpr int kEscape = 0x1B;
        constexpr int kSpace = 0x20;
        constexpr int kRight = 0x4000004F;
        constexpr int kLeft = 0x40000050;
        constexpr int kUp = 0x40000052;
        constexpr int kDown = 0x40000051;
        const std::vector<Element*> options = target->selectOptions();
        const bool up = event.key == kUp || event.key == kLeft;
        const bool down = event.key == kDown || event.key == kRight;
        const bool open = openSelect_ == target;
        const bool listbox = target->listboxRows() > 0;
        const bool multi = target->hasAttribute("multiple");
        if (event.key == kEscape) {
            // Closing works with no options at all.
            if (open) {
                openSelect_ = nullptr;
            }
        } else if (!options.empty()) {
            if (open) {
                if (up || down) {
                    selectHighlight_ = stepOptionIndex(options, selectHighlight_, up ? -1 : 1);
                } else if (event.key == kReturn || event.key == kSpace) {
                    if (selectOption(*target, selectHighlight_)) {
                        openSelect_ = nullptr;
                    }
                }
            } else if (up || down) {
                if (listbox && multi && shiftDown_) {
                    // Extend the range from the anchor through the moved edge.
                    selectEdge_ = stepOptionIndex(options, selectEdge_, up ? -1 : 1);
                    rangeOptionSelect(*target, selectAnchor_, selectEdge_);
                    revealSelectOption(*target, *options[selectEdge_]);
                } else {
                    const std::size_t to =
                        stepOptionIndex(options, selectSelectedIndex(*target), up ? -1 : 1);
                    if (to != selectSelectedIndex(*target)) {
                        selectOption(*target, to);
                        selectAnchor_ = to;
                        selectEdge_ = to;
                    }
                    if (listbox) {
                        revealSelectOption(*target, *options[to]);
                    }
                }
            } else if ((event.key == kReturn || event.key == kSpace) && !listbox) {
                openSelect_ = target;
                selectHighlight_ = selectSelectedIndex(*target);
            }
        }
    }

    if (!target) {
        return;
    }

    const char* attribute = attributeForEvent(event.kind);
    if (!attribute) {
        return;
    }

    // Fire on the target, then bubble to its ancestors, stopping at the first
    // handler that explicitly stops propagation.
    for (Element* current = target; current; current = current->parent()) {
        const std::string_view handler = current->attribute(attribute);
        if (handler.empty()) {
            continue;
        }
        script::invokeInlineHandler(handler, current);
        if (current->attribute("data-stoppropagation") == "true") {
            break;
        }
    }
}

void Application::run() {
    Renderer renderer(platform_->renderer());

    font_ = std::make_unique<TextFont>();
    if (!loadFirstAvailable(*font_, resources_.get())) {
        SDL_Log("no font available; text will not render");
    }

    // Scripts placed before <body> run before anything is drawn.
    runScripts(false);

    // `@media` is evaluated against the viewport and the resolver starts with
    // none, so an unreported size would read as 0x0 and let every `max-width`
    // rule through. Report it before the first resolve, not after it.
    {
        int width = 0;
        int height = 0;
        platform_->windowSize(width, height);
        resolver_.setViewportSize(static_cast<float>(width), static_cast<float>(height));
    }

    // `rem` and `font-size: <percentage>` on the root resolve against the
    // document's own font size, so it has to be known before the first layout.
    if (Element* body = document_->querySelector("body")) {
        resolver_.setRootFontSize(resolver_.resolve(*body).fontSize);
    }

    // SUI_CLICK=x,y[;x,y...] dispatches one synthetic press per frame, so click
    // handling can be exercised without a visible window. Allowing several is
    // what makes it possible to click an already-idle page, which is where a
    // frame-skipping bug would swallow the event.
    std::deque<InputEvent> scriptedClicks;
    if (const char* clicks = SDL_getenv("SUI_CLICK")) {
        for (const std::string& pair : splitOnSemicolons(clicks)) {
            float x = 0.0f;
            float y = 0.0f;
            if (std::sscanf(pair.c_str(), "%f,%f", &x, &y) == 2) {
                scriptedClicks.push_back(InputEvent{InputKind::PointerDown, x, y, 1});
            } else {
                SDL_Log("SUI_CLICK expects x,y; got '%s'", pair.c_str());
            }
        }
    }

    // SUI_MOVE=x,y[;x,y...] does the same for pointer motion, which is how the
    // `:hover` restyle path is exercised without a visible cursor. A third field names
    // the button held during the move, which is what makes it a drag, so `:active`
    // following the pointer can be reached the same way.
    std::deque<InputEvent> scriptedMoves;
    if (const char* moves = SDL_getenv("SUI_MOVE")) {
        for (const std::string& pair : splitOnSemicolons(moves)) {
            float x = 0.0f;
            float y = 0.0f;
            int button = 0;
            const int fields = std::sscanf(pair.c_str(), "%f,%f,%d", &x, &y, &button);
            if (fields >= 2) {
                scriptedMoves.push_back(InputEvent{InputKind::PointerMove, x, y, button});
            } else {
                SDL_Log("SUI_MOVE expects x,y[,button]; got '%s'", pair.c_str());
            }
        }
    }

    // SUI_WHEEL=x,y,dx,dy[;...] dispatches one synthetic wheel per frame, so the
    // scroll path works headlessly. A pair with no delta is a zero scroll, which
    // is useless, so both are required.
    std::deque<InputEvent> scriptedWheels;
    if (const char* wheels = SDL_getenv("SUI_WHEEL")) {
        for (const std::string& pair : splitOnSemicolons(wheels)) {
            float x = 0.0f;
            float y = 0.0f;
            float dx = 0.0f;
            float dy = 0.0f;
            if (std::sscanf(pair.c_str(), "%f,%f,%f,%f", &x, &y, &dx, &dy) == 4) {
                scriptedWheels.push_back(InputEvent{InputKind::Wheel, x, y, 0, dx, dy});
            } else {
                SDL_Log("SUI_WHEEL expects x,y,dx,dy; got '%s'", pair.c_str());
            }
        }
    }

    // SUI_FRAMES=n keeps the loop alive for n pumped frames instead of stopping
    // at the first clean one, so the skip path can be measured rather than
    // assumed: compare frames pumped against frames painted.
    unsigned minFrames = 0;
    if (const char* frames = SDL_getenv("SUI_FRAMES")) {
        if (std::sscanf(frames, "%u", &minFrames) != 1) {
            SDL_Log("SUI_FRAMES expects a count; got '%s'", frames);
            minFrames = 0;
        }
    }

    // SUI_RESIZE=w,h resizes the window once the first frame is on screen, so the
    // relayout path can be exercised without a visible window. It is the only way to
    // reach the code that runs when the drawable size no longer matches the frame.
    int resizeWidth = 0;
    int resizeHeight = 0;
    bool resizeRequested = false;
    if (const char* resize = SDL_getenv("SUI_RESIZE")) {
        if (std::sscanf(resize, "%dx%d", &resizeWidth, &resizeHeight) != 2 || resizeWidth <= 0 ||
            resizeHeight <= 0) {
            SDL_Log("SUI_RESIZE expects WxH; got '%s'", resize);
        } else {
            resizeRequested = true;
        }
    }

    BlockLayout layout;
    while (platform_->pumpEvents()) {
        ++framesPumped_;
        int width = 0;
        int height = 0;
        platform_->windowSize(width, height);

        // The renderer's logical canvas has to follow the drawable, or a resize would
        // only letterbox the frame that is already on screen. The frame loop below
        // already treats a size change as stale, so layout re-runs against the new
        // viewport in the same pass that repaints.
        if (width != viewportWidth_ || height != viewportHeight_) {
            viewportWidth_ = width;
            viewportHeight_ = height;
            platform_->setViewportSize(width, height);
            // A resize re-decides which `@media` blocks apply, so the resolver has
            // to hear about it in the same pass that re-lays-out. The generation
            // bump is also what makes the frame loop below treat the page stale.
            resolver_.setViewportSize(static_cast<float>(width), static_cast<float>(height));
        }

        // Timers and animation frames are the loop's own events: a fired
        // callback can change the page, so it counts as stale input the same way
        // a pointer event does. An animation loop that re-arms rAF every frame
        // keeps the page painting every frame, which is the contract. A running
        // CSS transition or keyframe animation keeps the frame stale on its own,
        // so an animated page repaints without any script.
        const bool timerFired = sui::script::pumpTimers();
        const bool animating = resolver_.hasRunningAnimations();
        const bool inputFocused = document_ && isEditableField(document_->focused());
        const Uint64 ticks = SDL_GetTicks();
        const bool caretBlinkPhase = inputFocused && caretVisible(ticks, caretBlinkStart_);
        static bool lastCaretBlinkPhase = false;
        const bool caretPhaseChanged = inputFocused && (caretBlinkPhase != lastCaretBlinkPhase);
        if (caretPhaseChanged) {
            lastCaretBlinkPhase = caretBlinkPhase;
        }

        // An idle page repaints nothing. DOM mutations, stylesheet changes,
        // explicit script invalidation, resizes, and unhandled input are the
        // only reasons the picture can differ from the last frame; when none of
        // them moved, the previous frame is still correct and can stay on screen
        // untouched. Input has to be listed because pumping the queue discards
        // it, so a skipped frame would swallow the click it carried.
        const bool stale =
            !frameDrawn_ || width != stamp_.width || height != stamp_.height ||
            timerFired || animating || caretPhaseChanged || !platform_->inputEvents().empty() || !scriptedClicks.empty() ||
            !scriptedMoves.empty() || !scriptedWheels.empty() ||
            (document_ && document_->mutationCount() != stamp_.mutations) ||
            resolver_.generation() != stamp_.rules || scriptContext_.generation != stamp_.script;
        if (stale) {
            platform_->clear();

            if (document_ && document_->root()) {
                LayoutInput input;
                input.resolver = &resolver_;
                input.font = font_.get();
                input.viewportWidth = static_cast<float>(width);
                input.viewportHeight = static_cast<float>(height);

                // Generated content becomes real boxes, so it has to exist before
                // layout measures anything. Rebuilding it notes no mutation, which
                // is what stops the frame loop from treating its own output as a
                // reason to draw another frame.
                generatePseudoElements(*document_, resolver_);

                layout.layout(document_->root(), input);

                // Events are routed after layout so hit tests use this frame's
                // geometry rather than the previous frame's.
                bool mutated = false;
                for (const auto& event : platform_->inputEvents()) {
                    dispatch(event);
                    mutated = true;
                }
                if (!scriptedClicks.empty()) {
                    dispatch(scriptedClicks.front());
                    scriptedClicks.pop_front();
                    mutated = true;
                }
                if (!scriptedMoves.empty()) {
                    dispatch(scriptedMoves.front());
                    scriptedMoves.pop_front();
                    mutated = true;
                }
                if (!scriptedWheels.empty()) {
                    dispatch(scriptedWheels.front());
                    scriptedWheels.pop_front();
                    mutated = true;
                }
                if (mutated) {
                    // A handler may have rewritten the tree or the stylesheet; lay
                    // out again so the frame about to be painted reflects the
                    // change.
                    generatePseudoElements(*document_, resolver_);
                    layout.layout(document_->root(), input);
                }

                if (SDL_getenv("SUI_DUMP_LAYOUT")) {
                    dumpLayout(*document_->root(), 0);
                }

                // Paint in CSS order: back to front, each box clipped to whatever
                // its ancestors' `overflow` allows. A stacking context that carries
                // an alpha under 1 is rendered into an offscreen surface and blended
                // back as one unit, so its overlapping members mix first.
                const std::vector<PaintOp> paintOps =
                    computePaintOps(*document_->root(), resolver_);
                SDL_Renderer* sdlRenderer = platform_->renderer();
                std::vector<GroupFrame> groups;
                // Where the open select painted this frame, if it did: an open
                // popup overlays the whole frame, so its list is positioned from
                // the select's own offsets rather than re-derived.
                float popupOffX = 0.0f, popupOffY = 0.0f, popupOpacity = 1.0f;
                bool popupFound = false;
                for (const PaintOp& op : paintOps) {
                    if (op.kind == PaintOp::Kind::BeginGroup) {
                        const int width = static_cast<int>(std::ceil(op.bounds.w));
                        const int height = static_cast<int>(std::ceil(op.bounds.h));
                        GroupFrame frame;
                        frame.bounds = op.bounds;
                        frame.alpha = op.alpha;
                        frame.transformed = op.transformed;
                        frame.origin = op.origin;
                        frame.blur = op.blur;
                        frame.shift = op.shift;
                        frame.dropShadow = op.dropShadow;
                        frame.clip = op.item.clipShape.kind != ResolvedClip::Kind::None;
                        frame.clipShape = op.item.clipShape;
                        frame.mask = hasMaskSurface(op.item.maskImages) ||
                                     hasMaskBorderSurface(op.item.maskBorder);
                        if (frame.mask && op.item.el) {
                            frame.maskLayers = op.item.maskImages;
                            frame.maskModes = op.item.maskModes;
                            frame.maskComposites = op.item.maskComposites;
                            frame.maskSizes = op.item.maskSizes;
                            frame.maskPositions = op.item.maskPositions;
                            frame.maskRepeats = op.item.maskRepeats;
                            frame.maskOrigins = op.item.maskOrigins;
                            frame.maskClips = op.item.maskClips;
                            frame.maskBorder = op.item.maskBorder;
                            frame.maskFontSize = resolver_.resolve(*op.item.el).fontSize;
                            // Boxes in surface coords: layout minus the
                            // offsets paint uses, rebased to the surface origin.
                            float ox = 0.0f, oy = 0.0f;
                            for (auto it = groups.rbegin(); it != groups.rend(); ++it) {
                                if (it->texture) {
                                    ox = it->originX;
                                    oy = it->originY;
                                    break;
                                }
                            }
                            const float ex = op.item.dx + ox + op.bounds.x;
                            const float ey = op.item.dy + oy + op.bounds.y;
                            frame.maskBoxes = BackgroundBoxes{
                                SDL_FRect{op.item.el->layoutX() - ex,
                                          op.item.el->layoutY() - ey,
                                          op.item.el->layoutWidth(),
                                          op.item.el->layoutHeight()},
                                SDL_FRect{op.item.el->paddingX() - ex,
                                          op.item.el->paddingY() - ey,
                                          op.item.el->paddingWidth(),
                                          op.item.el->paddingHeight()},
                                SDL_FRect{op.item.el->contentX() - ex,
                                          op.item.el->contentY() - ey,
                                          op.item.el->contentWidth(),
                                          op.item.el->contentHeight()}};
                        }
                        for (int i = 0; i < 12; ++i) {
                            frame.filter[i] = op.filter[i];
                        }
                        frame.m[0] = op.m[0];
                        frame.m[1] = op.m[1];
                        frame.m[2] = op.m[2];
                        frame.m[3] = op.m[3];
                        frame.m[4] = op.m[4];
                        frame.m[5] = op.m[5];
                        if (width > 0 && height > 0) {
                            SDL_PropertiesID props = SDL_CreateProperties();
                            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                                  SDL_TEXTUREACCESS_TARGET);
                            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                                                  width);
                            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                                                  height);
                            frame.texture = SDL_CreateTextureWithProperties(sdlRenderer, props);
                            SDL_DestroyProperties(props);
                        }
                        if (frame.texture) {
                            frame.previous = SDL_GetRenderTarget(sdlRenderer);
                            frame.originX = op.bounds.x;
                            frame.originY = op.bounds.y;
                            SDL_SetTextureBlendMode(frame.texture, SDL_BLENDMODE_BLEND);
                            SDL_SetRenderTarget(sdlRenderer, frame.texture);
                            SDL_SetRenderClipRect(sdlRenderer, nullptr);
                            SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 0);
                            SDL_RenderClear(sdlRenderer);
                        }
                        groups.push_back(frame);
                        continue;
                    }
                    if (op.kind == PaintOp::Kind::EndGroup) {
                        const GroupFrame frame = groups.back();
                        groups.pop_back();
                        if (frame.texture) {
                            // A `filter: blur()`, a colour-shift chain, or a
                            // drop-shadow reworks the finished surface: read it
                            // back while it is still the target, shift and blur
                            // it, and for a drop-shadow build a recoloured,
                            // blurred silhouette to blit under it. A failed
                            // readback keeps the sharp surface rather than
                            // dropping the group.
                            SDL_Texture* texture = frame.texture;
                            SDL_Texture* shadow = nullptr;
                            const int blurRadius =
                                frame.blur > 0.0f ? std::lround(frame.blur) : 0;
                            if (frame.shift || blurRadius > 0 || frame.dropShadow.present ||
                                frame.clip) {
                                SDL_Surface* read = SDL_RenderReadPixels(sdlRenderer, nullptr);
                                if (read) {
                                    SDL_Surface* conv = SDL_ConvertSurface(
                                        read, SDL_PIXELFORMAT_ABGR8888);
                                    SDL_DestroySurface(read);
                                    if (conv) {
                                        std::uint8_t* pixels =
                                            static_cast<std::uint8_t*>(conv->pixels);
                                        if (frame.shift) {
                                            sui::applyColorMatrix(pixels, conv->w, conv->h,
                                                                  frame.filter);
                                        }
                                        if (blurRadius > 0) {
                                            sui::blurRGBA(pixels, conv->w, conv->h, blurRadius);
                                        }
                                        if (frame.clip) {
                                            // The mask runs after the filters
                                            // (filters first, then the clip),
                                            // so the shadow pass traces the
                                            // clipped silhouette.
                                            sui::applyClipToSurface(
                                                pixels, conv->w, conv->h, frame.clipShape,
                                                frame.bounds.x, frame.bounds.y);
                                        }
                                        if (frame.mask) {
                                            // Mask layers multiply what the clip
                                            // left, in the same surface space;
                                            // url() layers sample decoded bytes
                                            // through the image cache. The
                                            // border mask multiplies after
                                            // them, from the same boxes.
                                            sui::applyMaskLayers(
                                                pixels, conv->w, conv->h, frame.maskLayers,
                                                frame.maskModes, frame.maskComposites,
                                                frame.maskSizes, frame.maskPositions,
                                                frame.maskRepeats, frame.maskOrigins,
                                                frame.maskClips, frame.maskBoxes,
                                                frame.maskFontSize,
                                                resolver_.rootFontSize(),
                                                [&](std::string_view href)
                                                    -> const DecodedImage* {
                                                    return images_ ? images_->decoded(href)
                                                                   : nullptr;
                                                });
                                            if (hasMaskBorderSurface(frame.maskBorder)) {
                                                sui::applyMaskBorder(
                                                    pixels, conv->w, conv->h,
                                                    frame.maskBorder, frame.maskBoxes,
                                                    frame.maskFontSize,
                                                    resolver_.rootFontSize(),
                                                    [&](std::string_view href)
                                                        -> const DecodedImage* {
                                                        return images_ ? images_->decoded(href)
                                                                       : nullptr;
                                                    });
                                            }
                                        }
                                        if (frame.dropShadow.present) {
                                            // The silhouette: the content's own
                                            // shape recoloured with the shadow
                                            // colour -- alpha scaled by the
                                            // shadow's own alpha -- then softened
                                            // by its blur radius.
                                            SDL_Surface* silhouette =
                                                SDL_DuplicateSurface(conv);
                                            if (silhouette) {
                                                std::uint8_t* silPixels =
                                                    static_cast<std::uint8_t*>(silhouette->pixels);
                                                const int bytes = 4 * silhouette->w * silhouette->h;
                                                const DropShadowFilter& ds = frame.dropShadow;
                                                for (int i = 0; i < bytes; i += 4) {
                                                    silPixels[i] = ds.color.r;
                                                    silPixels[i + 1] = ds.color.g;
                                                    silPixels[i + 2] = ds.color.b;
                                                    silPixels[i + 3] = static_cast<Uint8>(
                                                        (silPixels[i + 3] * ds.color.a) / 255);
                                                }
                                                const int shadowBlur = ds.blur > 0.0f
                                                    ? std::lround(ds.blur)
                                                    : 0;
                                                if (shadowBlur > 0) {
                                                    sui::blurRGBA(silPixels, silhouette->w,
                                                                  silhouette->h, shadowBlur);
                                                }
                                                shadow = SDL_CreateTextureFromSurface(
                                                    sdlRenderer, silhouette);
                                                SDL_DestroySurface(silhouette);
                                                if (shadow) {
                                                    SDL_SetTextureBlendMode(shadow,
                                                                            SDL_BLENDMODE_BLEND);
                                                }
                                            }
                                        }
                                        SDL_Texture* blurred =
                                            SDL_CreateTextureFromSurface(sdlRenderer, conv);
                                        SDL_DestroySurface(conv);
                                        if (blurred) {
                                            SDL_SetTextureBlendMode(blurred, SDL_BLENDMODE_BLEND);
                                            SDL_DestroyTexture(texture);
                                            texture = blurred;
                                        }
                                    }
                                }
                            }
                            float ox = 0.0f, oy = 0.0f;
                            for (auto it = groups.rbegin(); it != groups.rend(); ++it) {
                                if (it->texture) {
                                    ox = it->originX;
                                    oy = it->originY;
                                    break;
                                }
                            }
                            SDL_SetRenderTarget(sdlRenderer, frame.previous);
                            SDL_SetRenderClipRect(sdlRenderer, nullptr);
                            SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(
                                std::lround(frame.alpha * 255.0f)));
                            if (shadow) {
                                SDL_SetTextureAlphaMod(shadow, static_cast<Uint8>(
                                    std::lround(frame.alpha * 255.0f)));
                            }
                            if (frame.transformed) {
                                // The surface covers the group's pre-transform
                                // position; the affine maps it to the screen about
                                // the resolved transform-origin point, all rebased
                                // into the current target. `right` and `down` map
                                // the srcrect's far corners.
                                const float bx = frame.bounds.x - ox;
                                const float by = frame.bounds.y - oy;
                                const float ox2 = frame.origin.x - ox;
                                const float oy2 = frame.origin.y - oy;
                                const float a = frame.m[0], b = frame.m[1];
                                const float c = frame.m[2], d = frame.m[3];
                                const float e = frame.m[4], f = frame.m[5];
                                const SDL_FPoint q0{
                                    ox2 + a * (bx - ox2) + c * (by - oy2) + e,
                                    oy2 + b * (bx - ox2) + d * (by - oy2) + f};
                                const SDL_FPoint right{q0.x + a * frame.bounds.w,
                                                       q0.y + b * frame.bounds.w};
                                const SDL_FPoint down{q0.x + c * frame.bounds.h,
                                                      q0.y + d * frame.bounds.h};
                                if (shadow) {
                                    // The shadow rides the same affine, reached
                                    // by shifting the pre-transform top-left by
                                    // the drop-shadow's own offset first.
                                    const float sx = a * frame.dropShadow.dx +
                                                     c * frame.dropShadow.dy;
                                    const float sy = b * frame.dropShadow.dx +
                                                     d * frame.dropShadow.dy;
                                    const SDL_FPoint sq0{q0.x + sx, q0.y + sy};
                                    const SDL_FPoint sright{right.x + sx, right.y + sy};
                                    const SDL_FPoint sdown{down.x + sx, down.y + sy};
                                    SDL_RenderTextureAffine(sdlRenderer, shadow, nullptr, &sq0,
                                                            &sright, &sdown);
                                }
                                SDL_RenderTextureAffine(sdlRenderer, texture, nullptr, &q0,
                                                        &right, &down);
                            } else {
                                if (shadow) {
                                    const SDL_FRect shadowDst{
                                        frame.bounds.x - ox + frame.dropShadow.dx,
                                        frame.bounds.y - oy + frame.dropShadow.dy,
                                        frame.bounds.w, frame.bounds.h};
                                    SDL_RenderTexture(sdlRenderer, shadow, nullptr, &shadowDst);
                                }
                                const SDL_FRect dst{frame.bounds.x - ox, frame.bounds.y - oy,
                                                    frame.bounds.w, frame.bounds.h};
                                SDL_RenderTexture(sdlRenderer, texture, nullptr, &dst);
                            }
                            if (shadow) {
                                SDL_DestroyTexture(shadow);
                            }
                            SDL_DestroyTexture(texture);
                        }
                        continue;
                    }

                    const PaintItem& item = op.item;
                    float ox = 0.0f, oy = 0.0f;
                    for (auto it = groups.rbegin(); it != groups.rend(); ++it) {
                        if (it->texture) {
                            ox = it->originX;
                            oy = it->originY;
                            break;
                        }
                    }
                    float groupOpacity = item.opacity;
                    for (const GroupFrame& group : groups) {
                        if (!group.texture) {
                            groupOpacity *= group.alpha;
                        }
                    }
                    if (openSelect_ && item.el == openSelect_) {
                        popupOffX = item.dx + ox;
                        popupOffY = item.dy + oy;
                        popupOpacity = groupOpacity;
                        popupFound = true;
                    }
                    if (item.clip.w > 0.0f && item.clip.h > 0.0f) {
                        const SDL_Rect rect{static_cast<int>(std::floor(item.clip.x - ox)),
                                            static_cast<int>(std::floor(item.clip.y - oy)),
                                            static_cast<int>(std::ceil(item.clip.w)),
                                            static_cast<int>(std::ceil(item.clip.h))};
                        SDL_SetRenderClipRect(sdlRenderer, &rect);
                    } else {
                        SDL_SetRenderClipRect(sdlRenderer, nullptr);
                    }
                    int caretPos = -1;
                    bool showCaret = false;
                    int selStart = -1, selEnd = -1;
                    const char* compText = nullptr;
                    int compCursor = 0;
                    if (document_ && document_->focused() == item.el) {
                        caretPos = static_cast<int>(caretBytePos_);
                        showCaret = caretVisible(ticks, caretBlinkStart_);
                        if (selectionAnchor_ != caretBytePos_) {
                            selStart = static_cast<int>(std::min(selectionAnchor_, caretBytePos_));
                            selEnd = static_cast<int>(std::max(selectionAnchor_, caretBytePos_));
                        }
                        if (!compositionText_.empty()) {
                            compText = compositionText_.c_str();
                            compCursor = compositionCursor_;
                        }
                    }
                    paintBox(*item.el, renderer, sdlRenderer, resolver_, *font_, groupOpacity,
                             *images_, item.dx + ox, item.dy + oy, caretPos, showCaret, selStart, selEnd,
                             compText, compCursor);
                }
                // An open select popup floats above the whole frame with no ancestor
                // clip, the way a browser lifts a dropdown. A select that painted
                // no box was hidden while open, so the popup closes with it. (An
                // ancestor transform is not followed: the list hangs below the
                // select's untransformed position.)
                if (openSelect_ && document_->root() &&
                    !isReachable(*document_->root(), openSelect_)) {
                    openSelect_ = nullptr;
                }
                if (openSelect_ && popupFound) {
                    SDL_SetRenderClipRect(sdlRenderer, nullptr);
                    paintSelectPopup(*openSelect_, renderer, sdlRenderer,
                                     resolver_.resolve(*openSelect_), *font_, popupOffX,
                                     popupOffY, selectHighlight_, popupOpacity);
                } else if (openSelect_) {
                    openSelect_ = nullptr;
                }
            } else {
                SDL_FRect panel{40.0f, 40.0f, static_cast<float>(width) - 80.0f,
                                static_cast<float>(height) - 80.0f};
                renderer.fillRect(panel, Color{39, 39, 42, 255});
                renderer.strokeRect(panel, Color{63, 63, 70, 255}, 1.0f);
            }

            platform_->present();

            stamp_.mutations = document_ ? document_->mutationCount() : 0;
            stamp_.rules = resolver_.generation();
            stamp_.script = scriptContext_.generation;
            stamp_.width = width;
            stamp_.height = height;
            frameDrawn_ = true;
            ++framesDrawn_;

            // Scripts placed after </body> run once the frame is on screen.
            if (!scriptsAfterBodyRun_) {
                scriptsAfterBodyRun_ = true;
                runScripts(true);
            }

            if (resizeRequested) {
                resizeRequested = false;
                SDL_SetWindowSize(platform_->window(), resizeWidth, resizeHeight);
            }
        }

        // Termination is checked every iteration, skipped frames included: an
        // idle page skips almost all of them, so putting this inside the branch
        // that does the drawing would leave it waiting for work that never comes.
        if (!screenshotPath_.empty() && scriptedClicks.empty() && framesPumped_ >= minFrames) {
            if (platform_->saveScreenshot(screenshotPath_)) {
                SDL_Log("wrote %s", screenshotPath_.c_str());
            } else {
                SDL_Log("screenshot failed: %s", SDL_GetError());
            }
            break;
        }
        SDL_Delay(16);
    }

    if (SDL_getenv("SUI_LOG_FRAMES")) {
        SDL_Log("%u frame(s) pumped, %u painted", framesPumped_, framesDrawn_);
    }
}

void Application::shutdown() {
    script::setContext(nullptr);
    if (platform_) {
        platform_->shutdown();
        platform_.reset();
    }
    // Before the resources, since the cache borrows them.
    lastFocused_ = nullptr;
    openSelect_ = nullptr;
    selectHighlight_ = 0;
    selectAnchor_ = 0;
    selectEdge_ = 0;
    caretBytePos_ = 0;
    selectionAnchor_ = 0;
    compositionText_.clear();
    compositionCursor_ = 0;
    shiftDown_ = false;
    ctrlDown_ = false;
    images_.reset();
    font_.reset();
    document_.reset();
    resources_.reset();
}

}  // namespace sui

#include "Core/Application/Application.h"

#include "Core/Application/Background.h"

#include "CSS/Parser.h"
#include "CSS/PseudoElements.h"
#include "CSS/StyleResolver.h"
#include "Layout/BlockLayout.h"
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

namespace sui {
namespace {

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

// Deepest drawn element whose border box contains the point. Walking front to
// back over the paint order means the last match wins, which is the element the
// user is actually pointing at.
bool clipsAnything(const ComputedStyle& s);

// Hit-testing walks the tree in paint order. `offX`/`offY` accumulate the scroll
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
        if (drawn && el.layoutWidth() > 0.0f && el.layoutHeight() > 0.0f &&
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

// CSS paint order, as far as a flat list can express it. In CSS the root stacking
// context paints in this order: its own background, then negative `z-index`,
// then every in-flow box, then positioned boxes with `z-index: auto` or `0`, then
// positive `z-index`. Document order breaks ties inside a layer.
//
// The engine has one stacking context per document. That is exact for everything
// except nested contexts, which CSS also creates from `transform`, `opacity` under
// 1, `filter`, `will-change`, and a few others -- none of which the engine
// supports, so no nesting can arise.
struct PaintItem {
    Element* el;
    int layer = 0;
    int zIndex = 0;
    std::size_t order = 0;
    // The tightest rect every ancestor's `overflow` imposes, in window
    // coordinates. A width or height of zero or less means no clip at all.
    SDL_FRect clip{0.0f, 0.0f, 0.0f, 0.0f};
    // Scroll offsets accumulated from clipping ancestors. Content moves up and
    // left by them, so this element's box is drawn at layout minus the offset;
    // its own scroll only shifts what is inside it.
    float dx = 0.0f;
    float dy = 0.0f;
    // Every ancestor's `opacity` multiplied together. Carried per item rather than
    // composited through an offscreen buffer, which differs only where two elements
    // inside the same group overlap.
    float opacity = 1.0f;
};

SDL_FRect intersectClip(const SDL_FRect& a, const SDL_FRect& b) {
    if (a.w <= 0.0f || a.h <= 0.0f) {
        return b;
    }
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return a;
    }
    const float x = std::max(a.x, b.x);
    const float y = std::max(a.y, b.y);
    return {x, y, std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - x),
            std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - y)};
}

bool clipsAnything(const ComputedStyle& s) {
    return s.overflowX != ComputedStyle::Visible || s.overflowY != ComputedStyle::Visible;
}

// Depth-first collection, so `order` is document order. The inherited clip is
// threaded down: an element's own box is clipped by its ancestors, and it in turn
// clips its descendants. `offX`/`offY` are the scroll offsets of the clipping
// ancestors, which move this element's content up and left on screen.
void collectPaint(Element& el, const StyleResolver& resolver, const SDL_FRect& inherited,
                  float opacity, std::vector<PaintItem>& out, float offX, float offY) {
    SDL_FRect clip = inherited;
    bool drawable = false;
    float childOffX = offX;
    float childOffY = offY;
    ComputedStyle s;
    if (el.isVisual()) {
        s = resolver.resolve(el);
        drawable = s.display != ComputedStyle::None && s.visible;
        opacity *= s.opacity;
        if (drawable && clipsAnything(s)) {
            // `overflow` crops to the padding box, which in CSS includes the padding.
            clip = intersectClip(clip,
                                 SDL_FRect{el.paddingX(), el.paddingY(), el.paddingWidth(),
                                           el.paddingHeight()});
            // What the clip keeps is shifted by this box's own scroll offsets.
            childOffX += s.overflowX != ComputedStyle::Visible ? el.scrollLeft() : 0.0f;
            childOffY += s.overflowY != ComputedStyle::Visible ? el.scrollTop() : 0.0f;
        }
    }

    if (drawable) {
        PaintItem item;
        item.el = &el;
        item.zIndex = s.positioned ? s.zIndex : 0;
        item.layer = !s.positioned ? 0 : (s.zIndex < 0 ? -1 : (s.zIndex > 0 ? 2 : 1));
        item.order = out.size();
        // `overflow` crops the element's own content as well as its descendants,
        // so the clip has already included this element by the time it is stored.
        item.clip = clip;
        item.dx = offX;
        item.dy = offY;
        item.opacity = opacity;
        out.push_back(item);
    }

    // Non-visual wrappers (<root>, <head>) still contain drawable descendants.
    for (const auto& child : el.children()) {
        if (child && !child->isTextNode()) {
            collectPaint(*child, resolver, clip, opacity, out, childOffX, childOffY);
        }
    }
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
        SDL_Texture* texture = font.renderText(sdlRenderer, run.text, run.fontSize, tint);
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

void paintBox(Element& el, Renderer& renderer, SDL_Renderer* sdlRenderer,
              const StyleResolver& resolver, const TextFont& font, float opacity,
              ImageCache& images, float offX, float offY) {
    const ComputedStyle s = resolver.resolve(el);
    const SDL_FRect box{el.layoutX() - offX, el.layoutY() - offY, el.layoutWidth(),
                        el.layoutHeight()};
    const float rootFontSize = resolver.rootFontSize();
    // Border widths take no percentages or font-relative units, so they need no
    // containing block to resolve.
    const sui::Radii radii = radiiOf(s, box, rootFontSize);
    const sui::Color border = multiplyAlpha(toColor(s.borderColor), opacity);

    if (box.w > 0.0f && box.h > 0.0f) {
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

        // An <img> paints its decoded source into the content box. The source keeps
        // its aspect ratio and is scaled down to fit, which is the common case for a
        // photo dropped into a fixed box.
        if (el.tag() == "img") {
            const std::string_view src = el.attribute("src");
            if (!src.empty()) {
                if (SDL_Texture* tex = images.texture(sdlRenderer, src)) {
                    float iw = 0.0f, ih = 0.0f;
                    SDL_GetTextureSize(tex, &iw, &ih);
                    const SDL_FRect content{el.contentX() - offX, el.contentY() - offY,
                                            el.contentWidth(), el.contentHeight()};
                    if (iw > 0.0f && ih > 0.0f && content.w > 0.0f && content.h > 0.0f) {
                        const float scale = std::min(content.w / iw, content.h / ih);
                        const float dw = iw * scale;
                        const float dh = ih * scale;
                        const SDL_FRect dst{content.x + (content.w - dw) * 0.5f,
                                            content.y + (content.h - dh) * 0.5f, dw, dh};
                        renderer.drawTexture(tex, dst, nullptr,
                                             Color{255, 255, 255,
                                                   static_cast<Uint8>(opacity * 255.0f)});
                    }
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

    if (!font.hasFont()) {
        return;
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
                      static_cast<Uint8>(static_cast<float>(run.a) * opacity)});
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
            const SDL_Color c = {run.r, run.g, run.b, static_cast<Uint8>(static_cast<float>(run.a) * opacity)};
            SDL_SetRenderDrawColor(sdlRenderer, c.r, c.g, c.b, c.a);
            if (s.textDecorationLine == ComputedStyle::DecUnderline) {
                SDL_FRect r = {x0, y0 + run.baseline, w, lineThick};
                SDL_RenderFillRect(sdlRenderer, &r);
            } else if (s.textDecorationLine == ComputedStyle::DecOverline) {
                SDL_FRect r = {x0, y0 + (run.y - (run.y - (run.baseline - run.fontSize))) ? y0 + run.baseline - run.fontSize : y0, w, lineThick};
            }
            if (s.textDecorationLine == ComputedStyle::DecOverline) {
                SDL_FRect r = {x0, y0 + run.baseline - run.fontSize, w, lineThick};
                SDL_RenderFillRect(sdlRenderer, &r);
            } else if (s.textDecorationLine == ComputedStyle::DecLineThrough) {
                SDL_FRect r = {x0, y0 + run.baseline - run.fontSize * 0.3f, w, lineThick};
                SDL_RenderFillRect(sdlRenderer, &r);
            }
        }
        SDL_DestroyTexture(texture);
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

Document& Application::document() const {
    return *document_;
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
        CSSParser::parseWithKeyframes(*css, rules, keyframes);
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

void Application::dispatch(const InputEvent& event) {
    if (!document_ || !document_->root()) {
        return;
    }

    Element* target = hitTest(*document_->root(), event.x, event.y, resolver_, 0.0f, 0.0f);

    // Interactive state is set first and unconditionally. A move that changes what
    // is under the pointer has to clear `:hover` even when it lands on nothing, and
    // a press that misses the document has to clear `:focus`.
    // Every setter bumps the resolver generation, which is what makes the frame loop
    // treat the frame as stale.
    switch (event.kind) {
        case InputKind::PointerMove:
            resolver_.setHovered(target);
            // A drag keeps the active element under the cursor, so `:active` follows
            // the pointer instead of sticking to wherever the press landed. The button
            // field is zero for a plain hover, which is what separates the two.
            if (event.button != 0) {
                resolver_.setActive(target);
            }
            return;
        case InputKind::PointerDown:
            resolver_.setActive(target);
            // Clicking anywhere, including on nothing, moves focus.
            document_->setFocused(target);
            resolver_.setFocused(target);
            break;
        case InputKind::PointerUp:
            resolver_.setActive(nullptr);
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

        // An idle page repaints nothing. DOM mutations, stylesheet changes,
        // explicit script invalidation, resizes, and unhandled input are the
        // only reasons the picture can differ from the last frame; when none of
        // them moved, the previous frame is still correct and can stay on screen
        // untouched. Input has to be listed because pumping the queue discards
        // it, so a skipped frame would swallow the click it carried.
        const bool stale =
            !frameDrawn_ || width != stamp_.width || height != stamp_.height ||
            timerFired || animating || !platform_->inputEvents().empty() || !scriptedClicks.empty() ||
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

                // Paint in CSS order: a flat list sorted by stacking layer and
                // `z-index`, each box clipped to whatever its ancestors' `overflow`
                // allows.
                std::vector<PaintItem> paintList;
                collectPaint(*document_->root(), resolver_, SDL_FRect{}, 1.0f, paintList,
                             0.0f, 0.0f);
                std::stable_sort(paintList.begin(), paintList.end(),
                                 [](const PaintItem& a, const PaintItem& b) {
                                     if (a.layer != b.layer) {
                                         return a.layer < b.layer;
                                     }
                                     if (a.zIndex != b.zIndex) {
                                         return a.zIndex < b.zIndex;
                                     }
                                     return a.order < b.order;
                                 });
                SDL_Renderer* sdlRenderer = platform_->renderer();
                for (const PaintItem& item : paintList) {
                    SDL_Rect previous;
                    SDL_GetRenderClipRect(sdlRenderer, &previous);
                    if (item.clip.w > 0.0f && item.clip.h > 0.0f) {
                        const SDL_Rect rect{static_cast<int>(std::floor(item.clip.x)),
                                            static_cast<int>(std::floor(item.clip.y)),
                                            static_cast<int>(std::ceil(item.clip.w)),
                                            static_cast<int>(std::ceil(item.clip.h))};
                        SDL_SetRenderClipRect(sdlRenderer, &rect);
                    } else {
                        SDL_SetRenderClipRect(sdlRenderer, nullptr);
                    }
                    paintBox(*item.el, renderer, sdlRenderer, resolver_, *font_, item.opacity,
                             *images_, item.dx, item.dy);
                    SDL_SetRenderClipRect(sdlRenderer, &previous);
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
    images_.reset();
    font_.reset();
    document_.reset();
    resources_.reset();
}

}  // namespace sui

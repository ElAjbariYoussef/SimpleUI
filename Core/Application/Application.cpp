#include "Core/Application/Application.h"

#include "CSS/Parser.h"
#include "CSS/PseudoElements.h"
#include "CSS/StyleResolver.h"
#include "Layout/BlockLayout.h"
#include "Rendering/Renderer.h"
#include "Rendering/Text.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"
#include "SUI/Script/Script.h"

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

Color toColor(SDL_Color c) {
    return Color{static_cast<std::uint8_t>(c.r), static_cast<std::uint8_t>(c.g),
                 static_cast<std::uint8_t>(c.b), static_cast<std::uint8_t>(c.a)};
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
    for (const auto& child : el.children()) {
        if (child) {
            dumpLayout(*child, depth + 1);
        }
    }
}

// Deepest drawn element whose border box contains the point. Walking front to
// back over the paint order means the last match wins, which is the element the
// user is actually pointing at.
Element* hitTest(Element& el, float x, float y, const StyleResolver& resolver) {
    Element* found = nullptr;
    if (el.isVisual()) {
        const ComputedStyle s = resolver.resolve(el);
        const bool drawn = s.display != ComputedStyle::None && s.visible;
        if (drawn && el.layoutWidth() > 0.0f && el.layoutHeight() > 0.0f && x >= el.layoutX() &&
            x < el.layoutX() + el.layoutWidth() && y >= el.layoutY() &&
            y < el.layoutY() + el.layoutHeight()) {
            found = &el;
        }
    }
    for (const auto& child : el.children()) {
        if (child && !child->isTextNode()) {
            if (Element* deeper = hitTest(*child, x, y, resolver)) {
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
// clips its descendants.
void collectPaint(Element& el, const StyleResolver& resolver, const SDL_FRect& inherited,
                  std::vector<PaintItem>& out) {
    SDL_FRect clip = inherited;
    bool drawable = false;
    ComputedStyle s;
    if (el.isVisual()) {
        s = resolver.resolve(el);
        drawable = s.display != ComputedStyle::None && s.visible;
        if (drawable && clipsAnything(s)) {
            clip = intersectClip(clip, SDL_FRect{el.clipX(), el.clipY(), el.clipWidth(),
                                                 el.clipHeight()});
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
        out.push_back(item);
    }

    // Non-visual wrappers (<root>, <head>) still contain drawable descendants.
    for (const auto& child : el.children()) {
        if (child && !child->isTextNode()) {
            collectPaint(*child, resolver, clip, out);
        }
    }
}

void paintBox(Element& el, Renderer& renderer, SDL_Renderer* sdlRenderer,
              const StyleResolver& resolver, const TextFont& font) {
    const ComputedStyle s = resolver.resolve(el);
    const SDL_FRect box{el.layoutX(), el.layoutY(), el.layoutWidth(), el.layoutHeight()};
    // Border widths take no percentages or font-relative units, so they need no
    // containing block to resolve.
    const float borderTop = s.borderWidth.top.resolve();
    if (box.w > 0.0f && box.h > 0.0f) {
        if (s.backgroundColor.a > 0) {
            renderer.fillRect(box, toColor(s.backgroundColor));
        }
        if (borderTop > 0.0f || s.borderWidth.left.resolve() > 0.0f) {
            renderer.strokeRect(box, toColor(s.borderColor), borderTop);
        }
    }

    if (!font.hasFont()) {
        return;
    }
    for (const auto& run : el.textRuns()) {
        if (run.text.empty() || run.fontSize <= 0.0f) {
            continue;
        }
        SDL_Texture* texture = font.renderText(
            sdlRenderer, run.text, run.fontSize, SDL_Color{run.r, run.g, run.b, run.a});
        if (!texture) {
            continue;
        }
        // The texture is drawn 1:1 on the pixel grid. Scaling it to the line box
        // would resample the bitmap and soften every stem, and the glyph box
        // already starts at the top of the line box, since the run's baseline is
        // the font's own ascent.
        float texW = 0.0f, texH = 0.0f;
        SDL_GetTextureSize(texture, &texW, &texH);
        const SDL_FRect dst{std::floor(run.x), std::floor(run.y), texW, texH};
        SDL_RenderTexture(sdlRenderer, texture, nullptr, &dst);
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
        for (const auto& rule : CSSParser::parse(*css)) {
            resolver_.addRule(rule);
        }
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

    Element* target = hitTest(*document_->root(), event.x, event.y, resolver_);

    // Interactive state is set first and unconditionally. A move that changes what
    // is under the pointer has to clear `:hover` even when it lands on nothing, and
    // a press that misses the document has to clear `:focus`.
    // Every setter bumps the resolver generation, which is what makes the frame loop
    // treat the frame as stale.
    switch (event.kind) {
        case InputKind::PointerMove:
            resolver_.setHovered(target);
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
    // `:hover` restyle path is exercised without a visible cursor.
    std::deque<InputEvent> scriptedMoves;
    if (const char* moves = SDL_getenv("SUI_MOVE")) {
        for (const std::string& pair : splitOnSemicolons(moves)) {
            float x = 0.0f;
            float y = 0.0f;
            if (std::sscanf(pair.c_str(), "%f,%f", &x, &y) == 2) {
                scriptedMoves.push_back(InputEvent{InputKind::PointerMove, x, y, 0});
            } else {
                SDL_Log("SUI_MOVE expects x,y; got '%s'", pair.c_str());
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

    BlockLayout layout;
    while (platform_->pumpEvents()) {
        ++framesPumped_;
        int width = 0;
        int height = 0;
        platform_->windowSize(width, height);

        // An idle page repaints nothing. DOM mutations, stylesheet changes,
        // explicit script invalidation, resizes, and unhandled input are the
        // only reasons the picture can differ from the last frame; when none of
        // them moved, the previous frame is still correct and can stay on screen
        // untouched. Input has to be listed because pumping the queue discards
        // it, so a skipped frame would swallow the click it carried.
        const bool stale =
            !frameDrawn_ || width != stamp_.width || height != stamp_.height ||
            !platform_->inputEvents().empty() || !scriptedClicks.empty() ||
            !scriptedMoves.empty() ||
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
                collectPaint(*document_->root(), resolver_, SDL_FRect{}, paintList);
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
                    paintBox(*item.el, renderer, sdlRenderer, resolver_, *font_);
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
    font_.reset();
    document_.reset();
    resources_.reset();
}

}  // namespace sui

#include "Core/Application/Application.h"

#include "CSS/Parser.h"
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

void paintElement(Element& el, Renderer& renderer, SDL_Renderer* sdlRenderer,
                  const StyleResolver& resolver, const TextFont& font) {
    const bool drawable = el.isVisual();
    if (drawable) {
        const ComputedStyle s = resolver.resolve(el);
        if (s.display != ComputedStyle::None && s.visible) {
            const SDL_FRect box{el.layoutX(), el.layoutY(), el.layoutWidth(),
                                el.layoutHeight()};
            if (box.w > 0.0f && box.h > 0.0f) {
                if (s.backgroundColor.a > 0) {
                    renderer.fillRect(box, toColor(s.backgroundColor));
                }
                if (s.borderWidth.top > 0.0f || s.borderWidth.left > 0.0f) {
                    renderer.strokeRect(box, toColor(s.borderColor), s.borderWidth.top);
                }
            }

            if (font.hasFont()) {
                for (const auto& run : el.textRuns()) {
                    if (run.text.empty() || run.fontSize <= 0.0f) {
                        continue;
                    }
                    SDL_Texture* texture = font.renderText(
                        sdlRenderer, run.text, run.fontSize,
                        SDL_Color{run.r, run.g, run.b, run.a});
                    if (!texture) {
                        continue;
                    }
                    SDL_FRect dst{run.x, run.y, run.width, run.height};
                    SDL_RenderTexture(sdlRenderer, texture, nullptr, &dst);
                    SDL_DestroyTexture(texture);
                }
            }
        }
    }

    // Non-visual wrappers (<root>, <head>) still contain drawable descendants.
    for (const auto& child : el.children()) {
        if (child && !child->isTextNode()) {
            paintElement(*child, renderer, sdlRenderer, resolver, font);
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
    if (event.kind == InputKind::PointerMove) {
        return;
    }

    Element* target = hitTest(*document_->root(), event.x, event.y, resolver_);
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
                if (mutated) {
                    // A handler may have rewritten the tree; lay out again so
                    // the frame about to be painted reflects the change.
                    layout.layout(document_->root(), input);
                }

                if (SDL_getenv("SUI_DUMP_LAYOUT")) {
                    dumpLayout(*document_->root(), 0);
                }
                paintElement(*document_->root(), renderer, platform_->renderer(), resolver_,
                             *font_);
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

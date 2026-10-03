#include "Core/Application/Application.h"

#include "CSS/Parser.h"
#include "CSS/StyleResolver.h"
#include "Layout/BlockLayout.h"
#include "Rendering/Renderer.h"
#include "Rendering/Text.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"

#include <algorithm>
#include <array>
#include <filesystem>

#include <SDL3/SDL.h>

#include <charconv>

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
            document_->stylesheets().size(), document_->scriptHrefs().size());

    return true;
}

void Application::run() {
    Renderer renderer(platform_->renderer());

    TextFont font;
    if (!loadFirstAvailable(font, resources_.get())) {
        SDL_Log("no font available; text will not render");
    }

    StyleResolver resolver;
    if (document_) {
        for (const auto& link : document_->stylesheets()) {
            if (auto css = resources_->readText(link.href)) {
                for (const auto& rule : CSSParser::parse(*css)) {
                    resolver.addRule(rule);
                }
            }
        }
    }

    BlockLayout layout;
    while (platform_->pumpEvents()) {
        platform_->clear();

        int width = 0;
        int height = 0;
        platform_->windowSize(width, height);

        if (document_ && document_->root()) {
            LayoutInput input;
            input.resolver = &resolver;
            input.font = &font;
            input.viewportWidth = static_cast<float>(width);
            input.viewportHeight = static_cast<float>(height);

            layout.layout(document_->root(), input);
            if (SDL_getenv("SUI_DUMP_LAYOUT")) {
                dumpLayout(*document_->root(), 0);
            }
            paintElement(*document_->root(), renderer, platform_->renderer(), resolver, font);
        } else {
            SDL_FRect panel{40.0f, 40.0f, static_cast<float>(width) - 80.0f,
                            static_cast<float>(height) - 80.0f};
            renderer.fillRect(panel, Color{39, 39, 42, 255});
            renderer.strokeRect(panel, Color{63, 63, 70, 255}, 1.0f);
        }

        platform_->present();
        if (!screenshotPath_.empty()) {
            if (platform_->saveScreenshot(screenshotPath_)) {
                SDL_Log("wrote %s", screenshotPath_.c_str());
            } else {
                SDL_Log("screenshot failed: %s", SDL_GetError());
            }
            break;
        }
        SDL_Delay(16);
    }
}

void Application::shutdown() {
    if (platform_) {
        platform_->shutdown();
        platform_.reset();
    }
    document_.reset();
    resources_.reset();
}

}  // namespace sui

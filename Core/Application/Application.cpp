#include "Core/Application/Application.h"

#include "CSS/StyleResolver.h"
#include "Layout/BlockLayout.h"
#include "Layout/InlineFormatter.h"
#include "Layout/Layout.h"
#include "Rendering/Renderer.h"
#include "Rendering/Text.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"

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

    while (platform_->pumpEvents()) {
        platform_->clear();

        int width = 0;
        int height = 0;
        platform_->windowSize(width, height);

        // Layout + paint
        if (document_) {
            sui::LayoutContext ctx;
            ctx.containerWidth = static_cast<float>(width);
            ctx.containerHeight = static_cast<float>(height);
            sui::StyleResolver resolver;
            sui::BlockLayout layout;
            if (document_->root()) {
                layout.layout(document_->root(), ctx, resolver);
            }
            // Paint visual elements
            static TextFont font;
            static bool fontLoaded = false;
            if (!fontLoaded) {
                // Try to load a system font if present
                for (const auto& p : {"/usr/share/fonts/fonts-go/Go-Regular.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                      "/usr/share/fonts/truetype/freefont/FreeSans.ttf"}) {
                    if (std::filesystem::exists(p)) {
                        font.loadFromFile(p);
                        fontLoaded = true;
                        break;
                    }
                }
                if (!fontLoaded) {
                    fontLoaded = true;  // try without font
                }
            }

            if (document_->root()) {
                InlineFormatter formatter;
                document_->root()->visit([&](Element& el) {
                    if (!el.isVisual()) {
                        return;
                    }
                    const float x = el.layoutX();
                    const float y = el.layoutY();
                    const float w = el.layoutWidth();
                    const float h = el.layoutHeight();
                    const auto s = resolver.resolve(el);
                    if (w > 0.0f && h > 0.0f) {
                        SDL_FRect r{x, y, w, h};
                        renderer.fillRect(r, Color{s.backgroundColor.r, s.backgroundColor.g,
                                                   s.backgroundColor.b, s.backgroundColor.a});
                        if (s.borderWidth.left > 0.0f || s.borderWidth.top > 0.0f ||
                            s.borderWidth.right > 0.0f || s.borderWidth.bottom > 0.0f) {
                            renderer.strokeRect(r,
                                               Color{s.borderColor.r, s.borderColor.g,
                                                     s.borderColor.b, s.borderColor.a},
                                               1.0f);
                        }
                    }
                    // Text: only if element has text and no visual children that draw text? simple: draw if textContent non-empty
                    if (!el.textContent().empty() && font.hasFont() && s.fontSize > 0.0f) {
                        auto lines = formatter.format(el, s, w > 0.0f ? w : 800.0f);
                        float ly = y + s.padding.top;
                        for (const auto& line : lines) {
                            for (const auto& run : line.runs) {
                                if (!run.text.empty()) {
                                    SDL_Texture* tex = font.renderText(platform_->renderer(),
                                                                      run.text,
                                                                      s.fontSize,
                                                                      SDL_Color{s.color.r, s.color.g, s.color.b, s.color.a});
                                    if (tex) {
                                        SDL_FRect dst{x + s.padding.left, ly, run.width, run.height};
                                        SDL_RenderTexture(platform_->renderer(), tex, nullptr, &dst);
                                        SDL_DestroyTexture(tex);
                                    }
                                    ly += 0;  // runs on same line
                                }
                            }
                            ly += line.height;
                        }
                    }
                });
            }
        } else {
            SDL_FRect panel{40.0f, 40.0f, static_cast<float>(width) - 80.0f,
                            static_cast<float>(height) - 80.0f};
            renderer.fillRect(panel, Color{39, 39, 42, 255});
            renderer.strokeRect(panel, Color{63, 63, 70, 255}, 1.0f);
        }

        platform_->present();
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
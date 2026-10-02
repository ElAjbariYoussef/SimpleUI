#include "Core/Application/Application.h"

#include "Rendering/Renderer.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"

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

        // Placeholder fill so the first window is visibly alive; real painting
        // arrives with goal 2 (box model) and goal 4 (CSS).
        SDL_FRect panel{40.0f, 40.0f,
                        static_cast<float>(width) - 80.0f,
                        static_cast<float>(height) - 80.0f};
        renderer.fillRect(panel, Color{39, 39, 42, 255});
        renderer.strokeRect(panel, Color{63, 63, 70, 255}, 1.0f);

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
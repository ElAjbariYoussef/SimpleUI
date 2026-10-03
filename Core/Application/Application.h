// Application lifecycle: load the entry .sui, build the window described by its
// <window> element, run the linked scripts in the order the markup implies, then
// run the event loop.
#pragma once

#include "CSS/StyleResolver.h"
#include "Core/Resources/Resources.h"
#include "Platform/Platform.h"
#include "SUI/Script/Script.h"

#include <memory>
#include <string>
#include <string_view>

namespace sui {

class Document;
class TextFont;

class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Reads and parses the entry file, then opens the window it declares.
    bool start(std::string_view entrySuiPath, std::string& error);
    void run();
    void shutdown();

    Document& document() const;

private:
    static WindowConfig windowConfigFrom(Document& document);

    // Runs every <script> in `phase`, reporting hrefs that were never compiled in.
    void runScripts(bool afterBody);

    // Routes one pointer event through the DOM, firing handlers on the target
    // and then on each ancestor.
    void dispatch(const InputEvent& event);

    std::unique_ptr<Resources> resources_;
    std::unique_ptr<Platform> platform_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<TextFont> font_;
    StyleResolver resolver_;
    script::Context scriptContext_;
    std::string screenshotPath_;
    bool scriptsAfterBodyRun_ = false;
};

}  // namespace sui
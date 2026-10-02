// Application lifecycle: load the entry .sui, build the window described by its
// <window> element, then run the event loop.
#pragma once

#include "Core/Resources/Resources.h"
#include "Platform/Platform.h"

#include <memory>
#include <string>
#include <string_view>

namespace sui {

class Document;

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

    std::unique_ptr<Resources> resources_;
    std::unique_ptr<Platform> platform_;
    std::unique_ptr<Document> document_;
};

}  // namespace sui
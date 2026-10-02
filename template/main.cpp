#include "Core/Application/Application.h"

#include <SDL3/SDL.h>

#include <iostream>
#include <string_view>

int main() {
    // The app entry points at a .sui file; the engine handles everything else.
    constexpr std::string_view entry = "index.sui";

    sui::Application app;

    std::string error;
    if (!app.start(entry, error)) {
        std::cerr << "SimpleUI: " << error << '\n';
        return 1;
    }

    app.run();
    return 0;
}
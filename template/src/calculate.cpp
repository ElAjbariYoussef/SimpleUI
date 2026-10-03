// Runs after the first paint, because the .sui places this <script> below
// </body>. It owns the click handler and also shows the other half of the script
// surface: reading and writing CSS from script.

#include "counter.h"
#include "SUI/Script/Script.h"

#include <string>

void addone() {
    setCounter(counter() + 1);
    console.log("addone() -> " + std::to_string(counter()));
}

void toggleHighlight() {
    Element* sidebar = document.querySelector(".sidebar");
    if (!sidebar) {
        console.warn("no .sidebar element to highlight");
        return;
    }

    // classList is how a web developer toggles state; the cascade does the rest.
    sidebar->toggleClass("highlight", !sidebar->hasClass("highlight"));
    document.invalidate();
}

void calculate_onLoad() {
    console.log("calculate.cpp ran after the first paint");
}

SUI_SCRIPT("/src/calculate.cpp", calculate_onLoad)

// Both functions are reachable from inline attributes such as
// `onclick="addone();"` by their bare names.
SUI_HANDLER(addone)
SUI_HANDLER(toggleHighlight)
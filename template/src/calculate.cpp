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

// The same effect without touching the class list: write the inline declaration
// directly. element.style is the live view of the style attribute, so the change
// round-trips through the normal cascade and stays visible in the markup.
void recolorSidebar() {
    Element* sidebar = document.querySelector(".sidebar");
    if (!sidebar) {
        console.warn("no .sidebar element to recolor");
        return;
    }

    sidebar->style().setProperty("background-color",
                                 sidebar->style().getPropertyValue("background-color") ==
                                         "#7f1d1d"
                                     ? "#27272a"
                                     : "#7f1d1d");
    console.log("sidebar background is now " +
                document.getComputedStyle(sidebar).getPropertyValue("background-color"));
}

void calculate_onLoad() {
    console.log("calculate.cpp ran after the first paint");

    // A stylesheet built at runtime joins the author origin after the linked
    // sheets, so it behaves like a <style> block appended last.
    document.styleSheets().insertRule(
        ".sidebar .badge { color: #facc15 }");
    Element* counter = document.getElementById("counter");
    if (counter) {
        console.log("counter font-size resolves to " +
                    document.getComputedStyle(counter).getPropertyValue("font-size"));
    }
}

SUI_SCRIPT("/src/calculate.cpp", calculate_onLoad)

// All three functions are reachable from inline attributes such as
// `onclick="addone();"` by their bare names.
SUI_HANDLER(addone)
SUI_HANDLER(toggleHighlight)
SUI_HANDLER(recolorSidebar)
// Runs before the first paint, because the .sui places this <script> above
// <body>. This is where a page would put its "loading" work: anything that must
// be true before the first frame is drawn.

#include "counter.h"
#include "SUI/Script/Script.h"

#include <string>

namespace {
int counterValue = 0;
}  // namespace

int counter() {
    return counterValue;
}

void setCounter(int value) {
    counterValue = value;

    // The web-style surface: look the element up by id, then write its text.
    if (Element* element = document.getElementById("counter")) {
        element->setInnerText(std::to_string(counterValue));
    }
    document.invalidate();
}

void loading_onLoad() {
    console.log("loading.cpp ran before the first paint");
    setCounter(0);
}

SUI_SCRIPT("/src/loading.cpp", loading_onLoad)
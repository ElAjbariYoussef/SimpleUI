// Extremely simple block/inline layout for Goal 2. This is intentionally
// minimal (no flex/grid yet) to get the sample rendering quickly.
#pragma once

#include "CSS/Style.h"

#include <SDL3/SDL_rect.h>

#include <vector>

namespace sui {

class Element;

struct LayoutContext {
    float containerWidth = 0.0f;
    float containerHeight = 0.0f;
};

class LayoutEngine {
public:
    // Computes BoxModel.bounds for each element in the subtree rooted at root.
    // Coordinates are relative to (0,0) in the current container.
    void layout(Element* root, const LayoutContext& context);
};

}  // namespace sui
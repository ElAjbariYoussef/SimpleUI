#pragma once

#include "Layout/Layout.h"
#include "Layout/LayoutNode.h"

#include <memory>
#include <vector>

namespace sui {

class Element;
class StyleResolver;

class BlockLayout {
public:
    // Lay out a subtree starting at root in block flow, filling container width.
    void layout(Element* root, const LayoutContext& ctx,
                const StyleResolver& resolver);
};

}  // namespace sui
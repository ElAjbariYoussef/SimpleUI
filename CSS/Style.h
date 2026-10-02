#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

struct BoxEdge {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
};

struct BoxModel {
    BoxEdge margin;
    BoxEdge border;
    BoxEdge padding;

    // Content box size (without margin/border/padding)
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;

    // Computed outer bounds relative to parent content box
    SDL_FRect bounds = {0.0f, 0.0f, 0.0f, 0.0f};

    float outerWidth() const noexcept {
        return margin.left + border.left + padding.left + contentWidth +
               padding.right + border.right + margin.right;
    }

    float outerHeight() const noexcept {
        return margin.top + border.top + padding.top + contentHeight +
               padding.bottom + border.bottom + margin.bottom;
    }

    void setContentSize(float w, float h) noexcept {
        contentWidth = std::max(0.0f, w);
        contentHeight = std::max(0.0f, h);
    }
};

struct ComputedStyle {
    enum Display : std::uint8_t { Block, Inline, InlineBlock, None };

    Display display = Block;

    // Dimensions
    float width = 0.0f;   // 0 = auto
    float height = 0.0f;  // 0 = auto
    float minWidth = 0.0f;
    float minHeight = 0.0f;
    float maxWidth = 0.0f;   // 0 = none
    float maxHeight = 0.0f;  // 0 = none

    BoxEdge margin;
    BoxEdge padding;
    BoxEdge borderWidth;

    // Colors
    SDL_Color backgroundColor = {0, 0, 0, 0};  // transparent
    SDL_Color color = {255, 255, 255, 255};    // default text
    SDL_Color borderColor = {63, 63, 70, 255};

    // Typography
    float fontSize = 14.0f;  // px
    std::string fontFamily;   // empty = default
    bool fontBold = false;
    float lineHeight = 1.2f;  // multiplier

    // Text alignment
    enum TextAlign : std::uint8_t { Left, Center, Right, Justify };
    TextAlign textAlign = Left;

    // Positioning (minimal for now)
    enum Position : std::uint8_t { Static, Relative, Absolute, Fixed };
    Position position = Static;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    // Visibility
    bool visible = true;
};

}  // namespace sui
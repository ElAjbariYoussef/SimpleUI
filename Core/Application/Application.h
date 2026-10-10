// Application lifecycle: load the entry .sui, build the window described by its
// <window> element, run the linked scripts in the order the markup implies, then
// run the event loop.
#pragma once

#include "CSS/StyleResolver.h"
#include "Core/Resources/Resources.h"
#include "Rendering/ImageCache.h"
#include "Platform/Platform.h"
#include "SUI/Script/Script.h"

#include <memory>
#include <string>
#include <string_view>
#include <cstdint>

namespace sui {

class Document;
class Element;
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
    void setDocument(std::unique_ptr<Document> document);

    // Routes one input event through the DOM, updating input fields or firing handlers.
    void dispatch(const InputEvent& event);

    std::size_t caretPosition() const noexcept { return caretBytePos_; }
    void setCaretPosition(std::size_t pos) noexcept {
        caretBytePos_ = pos;
        selectionAnchor_ = pos;
    }

    std::size_t selectionStart() const noexcept { return std::min(selectionAnchor_, caretBytePos_); }
    std::size_t selectionEnd() const noexcept { return std::max(selectionAnchor_, caretBytePos_); }
    bool hasSelection() const noexcept { return selectionAnchor_ != caretBytePos_; }
    void setSelection(std::size_t start, std::size_t end) noexcept {
        selectionAnchor_ = start;
        caretBytePos_ = end;
    }
    void selectAll() noexcept;
    void clearSelection() noexcept { selectionAnchor_ = caretBytePos_; }

    const std::string& compositionText() const noexcept { return compositionText_; }
    int compositionCursor() const noexcept { return compositionCursor_; }
    bool hasComposition() const noexcept { return !compositionText_.empty(); }
    void clearComposition() noexcept { compositionText_.clear(); }

    // The pointer shape the hovered element last asked for, after inheritance.
    // Exposed so the platform-independent decision can be checked without a window.
    ComputedStyle::Cursor activeCursor() const noexcept { return activeCursor_; }

private:
    static WindowConfig windowConfigFrom(Document& document);

    // Resolves `target`'s `cursor` and, when it changed, hands the keyword to the
    // platform. Called on every pointer move, so an unchanged hover is a no-op.
    void updateCursor(Element* target);

    // Runs every <script> in `phase`, reporting hrefs that were never compiled in.
    void runScripts(bool afterBody);

    std::unique_ptr<Resources> resources_;
    std::unique_ptr<Platform> platform_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<TextFont> font_;
    // Decoded background and content images, shared across every element and every
    // frame. Constructed once the resources exist, so it borrows rather than owns them.
    std::unique_ptr<ImageCache> images_;
    StyleResolver resolver_;
    script::Context scriptContext_;
    std::string screenshotPath_;

    // The inputs that decide whether a frame can be skipped. Anything that can
    // change what the page looks like has to move one of these counters,
    // otherwise an idle window would keep repainting a stale tree.
    struct FrameStamp {
        std::uint64_t mutations = 0;
        std::uint64_t rules = 0;
        std::uint64_t script = 0;
        int width = -1;
        int height = -1;
    };
    FrameStamp stamp_;
    // The drawable size the renderer's logical canvas is currently set to, which is
    // also what made the frame loop mark a size change as stale.
    int viewportWidth_ = -1;
    int viewportHeight_ = -1;
    bool frameDrawn_ = false;
    bool scriptsAfterBodyRun_ = false;

    // Only observable through SUI_LOG_FRAMES, which exists so the skip path can
    // be checked rather than assumed.
    std::uint32_t framesPumped_ = 0;
    std::uint32_t framesDrawn_ = 0;

    // Caret state for focused text inputs. `caretBytePos_` is a byte offset
    // into the focused field's `value`; it is always kept on a UTF-8 codepoint
    // boundary. `caretBlinkStart_` is the SDL tick (ms) at which the blink
    // cycle was last reset; the caret is visible while
    // (SDL_GetTicks() - caretBlinkStart_) / 500 is even.
    Element* lastFocused_ = nullptr;
    std::size_t caretBytePos_ = 0;
    std::size_t selectionAnchor_ = 0;
    std::uint64_t caretBlinkStart_ = 0;
    bool shiftDown_ = false;
    bool ctrlDown_ = false;
    std::string compositionText_;
    int compositionCursor_ = 0;

    // The cursor keyword currently in effect, re-derived from the hovered element
    // whenever the pointer moves.
    ComputedStyle::Cursor activeCursor_ = ComputedStyle::CursorAuto;
};

}  // namespace sui
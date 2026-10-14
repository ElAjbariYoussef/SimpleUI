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
#include <mutex>
#include <string>
#include <string_view>
#include <cstdint>
#include <vector>

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

    // How many times the caret blink cycle has been restarted. The blink itself
    // is time-based, so this counts the resets a test can pin down: keys and
    // edits that touch the field reset it, modifier keys must not.
    std::uint64_t caretBlinkResets() const noexcept { return caretBlinkResets_; }

    // Whether the focused element wants visible focus UI, for `:focus-visible`.
    // Exposed so the modality plumbing can be checked without a window.
    bool focusVisible() const noexcept { return resolver_.focusVisible(); }

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

    // The select whose dropdown popup is open, if any. Exposed so tests can
    // assert open/close/pick without a window; the popup itself only paints.
    Element* openSelect() const noexcept { return openSelect_; }
    // The highlighted option row while a popup is open. Hover and arrows move
    // it without touching the tree; picking writes `selected`.
    std::size_t selectHighlight() const noexcept { return selectHighlight_; }
    // The input whose datalist popup is open, if any, with its highlighted
    // suggestion row. Picking writes the input's `value`.
    Element* openDatalist() const noexcept { return openDatalist_; }
    std::size_t datalistHighlight() const noexcept { return datalistHighlight_; }

    // Writes a picked file path into a file input, dropping it when the
    // element is gone, replaced, or no file control. The dialog callback runs
    // off-thread, so it only queues; this applies on the main thread and is
    // what tests drive directly.
    void applyPickedFile(Element* el, const char* path);

private:
    static WindowConfig windowConfigFrom(Document& document);

    // Resolves `target`'s `cursor` and, when it changed, hands the keyword to the
    // platform. Called on every pointer move, so an unchanged hover is a no-op.
    void updateCursor(Element* target);

    // Runs every <script> in `phase`, reporting hrefs that were never compiled in.
    void runScripts(bool afterBody);

    // Restarts the caret blink cycle and notes it, so a test can tell a key that
    // moved the caret apart from a modifier key that only repeated.
    void resetCaretBlink();

    // Opens the OS file picker for a file input, and applies queued picks on
    // the main thread. See the public applyPickedFile for the testable half.
    void openFileDialog(Element& input);
    void applyPendingFiles();
    // Settles a scroller on its snap points after a gesture moved it.
    void snapScroll(Element& scroller, const ComputedStyle& scrolled, bool movedX,
                    bool movedY);
    // The SDL dialog callback: queues the first pick, never touches the tree.
    static void fileDialogCallback(void* userdata, const char* const* filelist,
                                   int filter);

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
    // Whether the last interaction was keyboard-driven. Starts true (the page
    // loads keyboard-ready); any pointer press clears it, any key sets it.
    // Focus taken while set, or on a text-entry control, wants visible UI.
    bool keyboardModality_ = true;
    std::size_t caretBytePos_ = 0;
    std::size_t selectionAnchor_ = 0;
    std::uint64_t caretBlinkStart_ = 0;
    std::uint64_t caretBlinkResets_ = 0;
    bool shiftDown_ = false;
    bool ctrlDown_ = false;
    std::string compositionText_;
    int compositionCursor_ = 0;

    // The cursor keyword currently in effect, re-derived from the hovered element
    // whenever the pointer moves.
    ComputedStyle::Cursor activeCursor_ = ComputedStyle::CursorAuto;

    // The select whose dropdown popup is open, and the highlighted option row.
    // The popup is an overlay: it paints after the frame and never lays out,
    // so the highlight needs no document mutation to show (pointer and key
    // events already mark the frame stale).
    Element* openSelect_ = nullptr;
    std::size_t selectHighlight_ = 0;
    // The input whose datalist popup is open, and the highlighted suggestion
    // row. Like the select popup it paints after the frame and never lays
    // out; picking writes the input's `value`.
    Element* openDatalist_ = nullptr;
    std::size_t datalistHighlight_ = 0;
    // A scrollbar thumb drag: the scroller, the axis, the grab offset into
    // the thumb, and the track span the thumb travels. Moves retarget the
    // scroll offset; release drops the drag.
    Element* scrollDrag_ = nullptr;
    bool scrollDragX_ = false;
    float scrollGrab_ = 0.0f;
    float scrollTrack_ = 0.0f;
    float scrollTravel_ = 0.0f;
    float scrollMax_ = 0.0f;
    // A file dialog resolves off-thread, so its callback only queues picks;
    // the run loop applies them where the element still lives. Guarded because
    // the callback may run on another thread.
    std::mutex fileMutex_;
    Element* pendingFileInput_ = nullptr;
    std::vector<std::pair<Element*, std::string>> pendingFiles_;
    // Where a Shift+click or Shift+arrow range starts in an open list.
    std::size_t selectAnchor_ = 0;
    // Where the range currently ends: clicks and arrows move it, Shift+click
    // jumps it, and the marks always span anchor..edge.
    std::size_t selectEdge_ = 0;
};

}  // namespace sui
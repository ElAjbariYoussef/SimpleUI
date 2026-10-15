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
#include <deque>
#include <map>
#include <vector>

namespace sui {

class Document;
class Element;
class TextFont;
class Renderer;

// One page: a document plus everything that belongs to showing it. A page that
// is bound to a window paints there; a page with no window is headless (a test
// driving `dispatch` directly, or a page between windows). The application
// keeps one working copy of this state as its own members -- the fields below
// mirror them -- and `bindPage` / `flushBound` move the working copy in and out
// of the page the current event or frame belongs to, so the fifty methods that
// read the working copy never learn pages exist. `boundPage_` says which page
// the working copy currently holds (-1: none, freshly flushed).
struct Page {
    std::unique_ptr<Document> document;
    StyleResolver resolver;
    // The href the page was loaded from, named by `currentPage()`.
    std::string href;
    // The script-invalidations this page caused. Swapped with the working
    // context's generation on bind/flush, so one page's invalidation never
    // repaints another window.
    std::uint64_t scriptGeneration = 0;
    // Before-body scripts run once per page, the first time it frames -- a page
    // opened later must run its own, not inherit the entry page having run.
    bool scriptsBeforeRun = false;
    bool scriptsAfterBodyRun = false;
    Element* lastFocused_ = nullptr;
    std::size_t caretBytePos_ = 0;
    std::size_t selectionAnchor_ = 0;
    std::uint64_t caretBlinkStart_ = 0;
    std::uint64_t caretBlinkResets_ = 0;
    std::string compositionText_;
    int compositionCursor_ = 0;
    Element* openSelect_ = nullptr;
    std::size_t selectHighlight_ = 0;
    Element* openDatalist_ = nullptr;
    std::size_t datalistHighlight_ = 0;
    Element* scrollDrag_ = nullptr;
    bool scrollDragX_ = false;
    float scrollGrab_ = 0.0f;
    float scrollTrack_ = 0.0f;
    float scrollTravel_ = 0.0f;
    float scrollMax_ = 0.0f;
    std::size_t selectAnchor_ = 0;
    std::size_t selectEdge_ = 0;
    struct SmoothScroll {
        Element* box = nullptr;
        float left = 0.0f;
        float top = 0.0f;
    };
    std::vector<SmoothScroll> smoothScrolls_;
};

// Where a list item's outside marker lands. An image that loaded takes its own
// size, vertically centred in the item box and a gap left of it; a zero size
// (no image, or one that failed to load) falls back to the glyph, whose size
// the caller measures and which the same gap places.
struct ListMarkerSlot {
    bool image = false;
    SDL_FRect rect{};
};
ListMarkerSlot listMarkerSlot(const SDL_FRect& box, float imageWidth, float imageHeight);

// The element a `<use>` clones: the same-document `#id` its href names. Null
// for anything else -- a missing id, an external URL, no document, or a text
// node -- so callers paint nothing instead of guessing.
const Element* resolveSvgUse(const Element& svg, std::string_view href);

// Whether expanding `target` would re-enter a use-expansion already on the
// chain, which is how a self-referencing use is refused instead of looping.
bool svgUseEntersCycle(const std::vector<const Element*>& chain, const Element* target);

// One frame of a smooth scroll: the offset closes a fixed share of the gap and
// lands exactly on the target once the gap is under a pixel, so the animation
// ends where the gesture asked rather than near it.
float smoothScrollStep(float current, float target);

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

    // --- Pages ---
    //
    // A project is any number of .sui files under one directory: the entry
    // file the app started from is just the first page. `loadPage` reads one by
    // href -- resolved against the project root, exactly like every other href
    // in a page -- parses it, loads the stylesheets and scripts it names, and
    // makes it the live document. The window the host created is left alone;
    // only the title bar follows the page.
    //
    // A page that cannot be read or parsed is refused and leaves whatever was
    // live still live, so a broken link cannot blank the app.
    bool loadPage(std::string_view href, std::string& error);

    // Reads and installs the first page of a project rooted at that file's
    // directory, without opening a window. `start` is this plus the window; an
    // embedder that owns its own window, and a test that only needs pages, use
    // this alone.
    bool openProject(std::string_view entrySuiPath, std::string& error);

    // `loadPage` without a caller for the error: reports it and answers whether
    // the page loaded. This is what navigation goes through.
    bool navigate(std::string_view href);
    // `navigate` in one window: `blank` opens a new window (`target="_blank"`)
    // instead of replacing the window's page. This is what a link press goes
    // through; scripts use `navigate` / `openPage` directly.
    bool navigateInWindow(WindowId window, std::string_view href, bool blank);

    // The href of the primary page on screen, empty before the first load.
    const std::string& currentPage() const;

    // The document a window shows, or null when it shows none (or there is no
    // such window). Null is also what a window shows right after its page was
    // dropped, before anything binds another.
    Document* pageDocument(WindowId id) const;

    // Opens a page in a new window: loads it like `loadPage`, shows it in a
    // window built from its own `<window>` element, and binds the two. Returns
    // the window, or `kInvalidWindow` (with an error) when the page will not
    // load or no window opens. This is the call behind `<a target="_blank">`;
    // `navigate` stays the same-window one.
    WindowId openPage(std::string_view href, std::string& error);

    // Closes the calling page's window: the bound page's window while handling
    // one of its events or frames, else the primary window's. The window closes
    // at once; its page (and the page's timers) drops on the next pump. False
    // when there is no window to close.
    bool closePage();

    // Advances every `scroll-behavior: smooth` scroll one frame and reports
    // whether one is still moving. The frame loop calls it before deciding the
    // page is stale, so an animating scroller keeps repainting.
    bool tickScrolling();

    // Renders one frame of the live page into one window and presents it.
    // True when anything painted; false when the frame was correctly skipped
    // (nothing changed) or there was nothing to paint into (no platform, no
    // such window, no document). The `run` loop calls this once per open
    // window per pump; a test calls it directly to drive a headless window.
    // Each window keeps its own skip state, so an idle window stays quiet
    // while another window paints.
    bool renderFrame(WindowId id);

    // The platform behind the windows, or null before `start`. Exposed so a
    // test driving `renderFrame` headlessly can open the windows it renders
    // into.
    Platform* platform() const noexcept { return platform_.get(); }

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
    // A page's title bar text, empty when it declares none.
    static std::string windowTitleFrom(Document& document);
    // Makes an already-parsed document the live page at `index`, loading
    // everything the page brings with it. The entry file and every navigation
    // take the same path; the working copy is left holding the new page.
    void installPageAt(std::size_t index, std::unique_ptr<Document> document,
                       std::string_view href);
    bool loadPageAt(std::size_t index, std::string_view href, std::string& error);
    // The page `navigate` replaces: the primary window's, else the primary.
    std::size_t navigateIndex() const;

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
    // The offsets a snap container settles on for a scroll heading to
    // (atX, atY). Either negative means "where it already is", which is how a
    // smooth scroll snaps its target rather than the offset it is easing from.
    std::pair<float, float> snapOffsets(Element& scroller, const ComputedStyle& scrolled,
                                        bool movedX, bool movedY, float atX, float atY);
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
    // One window's skip state. The stamp is per window so an idle window stays
    // quiet while another paints; the renderer is per window because it caches
    // clip state and mesh scratch that belong to one SDL renderer.
    struct WindowFrame {
        std::unique_ptr<Renderer> renderer;
        int viewportWidth = -1;
        int viewportHeight = -1;
        FrameStamp stamp;
        bool drawn = false;
    };
    std::map<WindowId, WindowFrame> frames_;
    // The frame for a window, creating it (with its own renderer) the first
    // time the window paints. Null when there is no platform or no such window.
    WindowFrame* frameFor(WindowId id);
    // Drops the skip state of windows the platform no longer has, so a closed
    // window's renderer and stamp die with it.
    void dropClosedFrames();
    // The one-time frame setup `run` used to do inline: the font, the
    // before-body scripts, the colour preference, and the scripted test hooks.
    // Idempotent, so `renderFrame` can call it directly and `run` stays a loop.
    void ensureFrameStartup();
    bool frameStartupDone_ = false;
    // Scripted test hooks (SUI_CLICK / SUI_MOVE / SUI_WHEEL / SUI_FRAMES /
    // SUI_RESIZE), parsed once at startup. They address the primary window,
    // which is what `kInvalidWindow` on a synthetic event already means.
    std::deque<InputEvent> scriptedClicks_;
    std::deque<InputEvent> scriptedMoves_;
    std::deque<InputEvent> scriptedWheels_;
    unsigned minFrames_ = 0;
    int resizeWidth_ = 0;
    int resizeHeight_ = 0;
    bool resizeRequested_ = false;
    // The drawable size the renderer's logical canvas is currently set to, which is
    // also what made the frame loop mark a size change as stale.
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
    // One box easing toward a scroll target, for `scroll-behavior: smooth`. A
    // second notch on the same box retargets it rather than queueing. The
    // working copy of the bound page's scrolls; see `bindPage`.
    std::vector<Page::SmoothScroll> smoothScrolls_;
    void queueSmoothScroll(Element& box, float left, float top);
    // Every live page, headless or window-bound. `pages_[0]` is the primary
    // page: the one `setDocument` fills, `document()` returns, and `navigate`
    // replaces when no window names another.
    std::vector<std::unique_ptr<Page>> pages_;
    // Which page each window shows. A window with no entry shows a panel.
    std::map<WindowId, std::size_t> bindings_;
    // Which page the working copy currently holds, or -1 for none.
    int boundPage_ = -1;
    // The primary page, creating it when nothing loaded yet.
    Page& ensurePrimary();
    // The page an event belongs to: its window's page, or the primary page for
    // a synthetic event with no window. -1 when the window shows no page.
    int pageForEvent(const InputEvent& event) const;
    // The page a window shows, or -1 for a panel.
    int pageForWindow(WindowId id) const;
    // Moves the working copy into the named page (`bindPage`) or back out of
    // whichever page holds it (`flushBound`). The script context follows the
    // working copy so handlers always see the page they belong to.
    void bindPage(std::size_t index);
    void flushBound();
    // Drops the pages whose windows the platform just closed. The working copy
    // is flushed first, so a page being painted is never erased from under it.
    void dropClosedPages();
};

}  // namespace sui
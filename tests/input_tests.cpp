// What the platform turns raw SDL mouse events into. The scripted SUI_MOVE hook
// feeds the engine a finished event, so it cannot catch a mapping mistake here --
// and a motion that is dropped outright leaves `:hover` silently dead, with no other
// test failing. Events are pushed onto SDL's own queue so the real pump runs.
#include "Platform/Platform.h"
#include "Core/Application/Application.h"
#include "SUI/Script/Timers.h"
#include "Layout/BlockLayout.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void pushMotion(int x, int y, Uint32 buttons, SDL_WindowID sdlId = 0) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = sdlId;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.state = buttons;
    SDL_PushEvent(&event);
}

void pushLeave(SDL_WindowID sdlId = 0) {
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
    event.window.windowID = sdlId;
    SDL_PushEvent(&event);
}

void drain() {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
    }
}

sui::Application* g_navApp = nullptr;
std::string g_navSeen;

// A timer probe: fires into whichever element it was armed with.
sui::Element* g_timerTarget = nullptr;
bool g_timerFired = false;

// Modal dialog event flags.
bool g_modalCancel = false;
bool g_modalShut = false;

void flagModalCancel() { g_modalCancel = true; }
void flagModalShut() { g_modalShut = true; }

void fireTimerProbe() {
    g_timerFired = true;
    if (g_timerTarget) {
        g_timerTarget->setValue("timer");
    }
}

void recordNavPage() {
    if (g_navApp) {
        g_navSeen = g_navApp->currentPage();
    }
}

// Pumps once and asserts the queue held exactly the one event the test pushed.
void expectOne(sui::Platform& platform, sui::InputKind kind, float x, float y, int button,
               const char* what) {
    const auto& events = platform.inputEvents();
    ++checks;
    if (events.size() != 1) {
        ++failures;
        std::printf("FAIL %s: got %zu events\n", what, events.size());
        return;
    }
    const sui::InputEvent& event = events.front();
    const bool ok = event.kind == kind && event.x == x && event.y == y && event.button == button;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s: got kind %d at (%.1f, %.1f) button %d\n", what, static_cast<int>(event.kind),
                    event.x, event.y, event.button);
    }
}

}  // namespace

int main() {
    sui::WindowConfig config;
    config.width = 400;
    config.height = 300;
    std::string error;
    std::unique_ptr<sui::Platform> platform = sui::makePlatform();
    if (!platform->initialize(config, error)) {
        std::printf("platform initialize failed: %s\n", error.c_str());
        return 1;
    }

    // The dummy driver queues motion of its own while the window is created, so every
    // check below starts from an empty queue and accounts for exactly the one event it
    // pushed.
    drain();
    const SDL_WindowID primarySdl = SDL_GetWindowID(platform->window());

    // A plain hover, with nothing held: this is the case that used to be dropped, and
    // dropping it is what made `:hover` never fire.
    pushMotion(120, 80, 0, primarySdl);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerMove, 120.0f, 80.0f, 0,
              "a motion with no button held reports a move");

    // The same motion mid-drag. `button` is what separates the two, because `:active`
    // follows the cursor during a drag but must not stick without a press.
    pushMotion(121, 81, SDL_BUTTON_LMASK, primarySdl);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerMove, 121.0f, 81.0f, SDL_BUTTON_LEFT,
              "a left drag reports a move carrying the held button");

    pushMotion(122, 82, SDL_BUTTON_RMASK, primarySdl);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerMove, 122.0f, 82.0f, SDL_BUTTON_RIGHT,
              "a right drag reports the right button");

    pushMotion(123, 83, SDL_BUTTON_MMASK, primarySdl);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerMove, 123.0f, 83.0f, SDL_BUTTON_MIDDLE,
              "a middle drag reports the middle button");

    // Leaving the window must clear the hover rather than freeze it on whatever was
    // under the cursor. Off-screen is how the engine is told nothing is hovered.
    pushLeave(primarySdl);
    platform->pumpEvents();
    const auto& left = platform->inputEvents();
    ++checks;
    const bool leftOk = left.size() == 1 && left.front().kind == sui::InputKind::PointerMove &&
                        left.front().x < 0.0f && left.front().y < 0.0f;
    if (!leftOk) {
        ++failures;
        std::printf("FAIL leaving the window reports a move that can hit nothing\n");
    }

    // A press is still a press and a release still a release.
    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.windowID = primarySdl;
    down.button.x = 50;
    down.button.y = 60;
    down.button.button = SDL_BUTTON_LEFT;
    SDL_PushEvent(&down);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerDown, 50.0f, 60.0f, SDL_BUTTON_LEFT,
              "a press is still a press");

    SDL_Event up{};
    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    up.button.windowID = primarySdl;
    up.button.x = 50;
    up.button.y = 60;
    up.button.button = SDL_BUTTON_LEFT;
    SDL_PushEvent(&up);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerUp, 50.0f, 60.0f, SDL_BUTTON_LEFT,
              "a release is still a release");

    // A wheel notch reaches the engine as a Wheel event carrying the delta it was
    // aimed at and the cursor position under which it turned.
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.windowID = primarySdl;
    wheel.wheel.x = 1.5f;
    wheel.wheel.y = -2.0f;
    wheel.wheel.mouse_x = 100.0f;
    wheel.wheel.mouse_y = 110.0f;
    SDL_PushEvent(&wheel);
    platform->pumpEvents();
    {
        const auto& events = platform->inputEvents();
        ++checks;
        const bool wheelOk = events.size() == 1 && events.front().kind == sui::InputKind::Wheel &&
                             events.front().x == 100.0f && events.front().y == 110.0f &&
                             events.front().dx == 1.5f && events.front().dy == -2.0f;
        if (!wheelOk) {
            ++failures;
            std::printf("FAIL a wheel notch reports the delta and cursor position\n");
        }
    }

    // A key press reaches the engine as its platform key code, which is what a
    // focused field inspects for Backspace and the like.
    SDL_Event keyDown{};
    keyDown.type = SDL_EVENT_KEY_DOWN;
    keyDown.key.windowID = primarySdl;
    keyDown.key.key = SDLK_A;
    SDL_PushEvent(&keyDown);
    platform->pumpEvents();
    {
        const auto& events = platform->inputEvents();
        ++checks;
        const bool ok = events.size() == 1 && events.front().kind == sui::InputKind::KeyDown &&
                        events.front().key == SDLK_A;
        if (!ok) {
            ++failures;
            std::printf("FAIL a key press reports its key code\n");
        }
    }

    SDL_Event keyUp{};
    keyUp.type = SDL_EVENT_KEY_UP;
    keyUp.key.windowID = primarySdl;
    keyUp.key.key = SDLK_A;
    SDL_PushEvent(&keyUp);
    platform->pumpEvents();
    {
        const auto& events = platform->inputEvents();
        ++checks;
        const bool keyUpOk = events.size() == 1 && events.front().kind == sui::InputKind::KeyUp &&
                             events.front().key == SDLK_A;
        if (!keyUpOk) {
            ++failures;
            std::printf("FAIL a key release reports the same key code\n");
        }
    }

    // Typed characters arrive separately from the raw key, which is what lets a
    // text field insert what the layout actually produced.
    SDL_Event input{};
    input.type = SDL_EVENT_TEXT_INPUT;
    input.text.windowID = primarySdl;
    input.text.text = "hi";
    SDL_PushEvent(&input);
    platform->pumpEvents();
    {
        const auto& events = platform->inputEvents();
        ++checks;
        const bool textOk = events.size() == 1 &&
                            events.front().kind == sui::InputKind::TextInput &&
                            events.front().text == "hi";
        if (!textOk) {
            ++failures;
            std::printf("FAIL typed text reports the committed characters\n");
        }
    }

    // IME composition events report text being edited, start, and length.
    SDL_Event editEv{};
    editEv.type = SDL_EVENT_TEXT_EDITING;
    editEv.edit.windowID = primarySdl;
    editEv.edit.text = const_cast<char*>("nihon");
    editEv.edit.start = 2;
    editEv.edit.length = 5;
    SDL_PushEvent(&editEv);
    platform->pumpEvents();
    {
        const auto& events = platform->inputEvents();
        ++checks;
        const bool editOk = events.size() == 1 &&
                            events.front().kind == sui::InputKind::TextEdit &&
                            events.front().text == "nihon" &&
                            events.front().editStart == 2 &&
                            events.front().editLength == 5;
        if (!editOk) {
            ++failures;
            std::printf("FAIL text editing event reports composition text and ranges\n");
        }
    }

    // Windows carry their own identity through the event queue. A second window
    // opens beside the first, events name the window they came from, and closing
    // one window closes one window.
    {
        check(sui::InputEvent{}.window == sui::kInvalidWindow,
              "an event with no window names none");
        const sui::WindowId primary = platform->primaryWindow();
        check(primary != sui::kInvalidWindow && platform->hasWindow(primary),
              "the entry window is the primary window");
        sui::WindowConfig second;
        second.width = 400;
        second.height = 300;
        std::string windowError;
        const sui::WindowId other = platform->openWindow(second, windowError);
        check(other != sui::kInvalidWindow && other != primary,
              "a second window opens with its own id");
        check(platform->hasWindow(other) && platform->primaryWindow() == primary,
              "opening a window changes nothing about the primary");
        check(platform->window(other) != nullptr && platform->renderer(other) != nullptr,
              "a second window owns its SDL pair");
        check(platform->window(sui::kInvalidWindow) == nullptr,
              "no window means no SDL pair");

        // An event on the second window arrives tagged with the second window.
        drain();
        const SDL_WindowID otherSdl = SDL_GetWindowID(platform->window(other));
        pushMotion(10, 20, 0, otherSdl);
        platform->pumpEvents();
        {
            const auto& events = platform->inputEvents();
            ++checks;
            const bool routed = events.size() == 1 && events.front().window == other;
            if (!routed) {
                ++failures;
                std::printf("FAIL an event on the second window names the second window\n");
            }
        }
        // An event for a window the engine does not own is nobody's business.
        drain();
        pushMotion(10, 20, 0, static_cast<SDL_WindowID>(987654321));
        platform->pumpEvents();
        check(platform->inputEvents().empty(), "an event for an unknown window is dropped");

        // A close request closes that window and no other, and the application
        // learns about it from `closedWindows`.
        drain();
        SDL_Event close{};
        close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        close.window.windowID = otherSdl;
        SDL_PushEvent(&close);
        platform->pumpEvents();
        check(platform->closedWindows().size() == 1 &&
                  platform->closedWindows().front() == other,
              "a close request reports the window it closed");
        check(!platform->hasWindow(other) && platform->hasWindow(primary),
              "closing one window closes one window");
        check(platform->primaryWindow() == primary, "the primary survives the close");
        platform->closeWindow(static_cast<sui::WindowId>(424242));
        check(platform->hasWindow(primary), "closing an unknown window changes nothing");
    }

    // Test Application dispatch for input fields (editing, readonly, disabled, password, button, backspace).
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<input id=\"txt\" value=\"abc\"/>"
            "<input id=\"ro\" value=\"abc\" readonly/>"
            "<input id=\"dis\" value=\"abc\" disabled/>"
            "<input id=\"btn\" type=\"button\" value=\"click\"/>"
            "</body></sui>", err);
        check(doc != nullptr, "test document parsed");
        if (doc) {
            sui::Element* txt = doc->getElementById("txt");
            sui::Element* ro = doc->getElementById("ro");
            sui::Element* dis = doc->getElementById("dis");
            sui::Element* btn = doc->getElementById("btn");

            app.setDocument(std::move(doc));
            sui::Document& appDoc = app.document();

            // 1. Text input on normal text input:
            appDoc.setFocused(txt);
            app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "d"));
            check(txt && txt->value() == "abcd", "text input appends text to editable field");

            // Backspace deletes last character:
            sui::InputEvent backspaceEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x08);
            app.dispatch(backspaceEv);
            check(txt && txt->value() == "abc", "backspace deletes character from editable field");

            // 2. Readonly input rejects text input and backspace:
            appDoc.setFocused(ro);
            app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "x"));
            check(ro && ro->value() == "abc", "readonly input ignores text input");
            app.dispatch(backspaceEv);
            check(ro && ro->value() == "abc", "readonly input ignores backspace");

            // 3. Disabled input rejects text input and backspace:
            appDoc.setFocused(dis);
            app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "x"));
            check(dis && dis->value() == "abc", "disabled input ignores text input");
            app.dispatch(backspaceEv);
            check(dis && dis->value() == "abc", "disabled input ignores backspace");

            // 4. Non-textual input (button) rejects text input:
            appDoc.setFocused(btn);
            app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "x"));
            check(btn && btn->value() == "click", "button input ignores text input");

            // 5. UTF-8 multi-byte backspace:
            appDoc.setFocused(txt);
            app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "\xE2\x80\xA2"));
            check(txt && txt->value() == "abc\xE2\x80\xA2", "multibyte utf8 appended to input");
            app.dispatch(backspaceEv);
            check(txt && txt->value() == "abc", "backspace deletes full multibyte utf8 codepoint");

            // 6. Caret navigation and positioned insertion:
            // Value is "abc", length is 3, caret is at 3.
            check(app.caretPosition() == 3, "caret starts at end of text field");
            sui::InputEvent leftEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000050);
            sui::InputEvent rightEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x4000004F);
            sui::InputEvent homeEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x4000004A);
            sui::InputEvent endEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x4000004D);
            sui::InputEvent deleteEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x7F);

            // Move left to position 2 (between 'b' and 'c')
            app.dispatch(leftEv);
            check(app.caretPosition() == 2, "left arrow moves caret left by 1");
            app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "X"));
            check(txt && txt->value() == "abXc", "text input inserts at caret position");
            check(app.caretPosition() == 3, "caret advances by inserted length");

            // Move left back to position 2 (before 'X') and press Delete
            app.dispatch(leftEv);
            check(app.caretPosition() == 2, "left arrow moves back to position 2");
            app.dispatch(deleteEv);
            check(txt && txt->value() == "abc", "delete key removes character after caret");
            check(app.caretPosition() == 2, "delete key preserves caret position");

            // Home key jumps to 0
            app.dispatch(homeEv);
            check(app.caretPosition() == 0, "home key moves caret to position 0");
            app.dispatch(backspaceEv);
            check(txt && txt->value() == "abc", "backspace at start does nothing");
            check(app.caretPosition() == 0, "caret remains at 0 after no-op backspace");

            // Insert at beginning
            app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "Z"));
            check(txt && txt->value() == "Zabc", "insert at position 0 prepends text");
            check(app.caretPosition() == 1, "caret is at 1 after prepending");

            // End key jumps to end
            app.dispatch(endEv);
            check(app.caretPosition() == 4, "end key moves caret to end of text");
            app.dispatch(deleteEv);
            check(txt && txt->value() == "Zabc", "delete key at end does nothing");
            check(app.caretPosition() == 4, "caret remains at end after no-op delete");

            // Right arrow at end stays at end
            app.dispatch(rightEv);
            check(app.caretPosition() == 4, "right arrow at end clamps to end");

            // 7. Multi-byte UTF-8 caret navigation:
            // Reset value to "a•b" ("a\xE2\x80\xA2" "b" -> 1 byte + 3 bytes + 1 byte = 5 bytes)
            txt->setValue("a\xE2\x80\xA2" "b");
            app.setCaretPosition(5); // at end
            app.dispatch(leftEv); // skips 'b' (1 byte) -> at 4
            check(app.caretPosition() == 4, "left arrow skips single byte to byte 4");
            app.dispatch(leftEv); // skips '•' (3 bytes) -> at 1
            check(app.caretPosition() == 1, "left arrow skips entire 3-byte utf8 codepoint to byte 1");
            app.dispatch(rightEv); // skips '•' forward -> at 4
            check(app.caretPosition() == 4, "right arrow skips entire 3-byte utf8 codepoint to byte 4");
            app.dispatch(leftEv); // back to 1
            app.dispatch(deleteEv); // deletes '•'
            check(txt && txt->value() == "ab", "delete key removes entire 3-byte utf8 codepoint");
            check(app.caretPosition() == 1, "caret remains at byte 1 after delete");

            // 8. Text selection range, shortcuts, collapse, replacement, and deletion:
            txt->setValue("abcdef");
            app.setCaretPosition(6);
            check(!app.hasSelection(), "no initial selection");

            // Ctrl+A selects all
            sui::InputEvent ctrlDown(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x400000e0);
            sui::InputEvent ctrlUp(sui::InputKind::KeyUp, 0, 0, 0, 0, 0, 0x400000e0);
            sui::InputEvent aKey(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x61);
            app.dispatch(ctrlDown);
            app.dispatch(aKey);
            app.dispatch(ctrlUp);
            check(app.hasSelection(), "Ctrl+A creates selection");
            check(app.selectionStart() == 0, "Ctrl+A selection starts at 0");
            check(app.selectionEnd() == 6, "Ctrl+A selection ends at value length");

            // Left arrow without shift collapses selection to start (0)
            app.dispatch(leftEv);
            check(!app.hasSelection(), "left arrow collapses selection");
            check(app.caretPosition() == 0, "left arrow collapsed caret to start (0)");

            // Right arrow without shift on selection collapses to end
            app.selectAll();
            check(app.hasSelection(), "selectAll sets selection");
            app.dispatch(rightEv);
            check(!app.hasSelection(), "right arrow collapses selection");
            check(app.caretPosition() == 6, "right arrow collapsed caret to end (6)");

            // Shift+Left extends selection backwards from end (6 to 4)
            sui::InputEvent shiftDown(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x400000e1);
            sui::InputEvent shiftUp(sui::InputKind::KeyUp, 0, 0, 0, 0, 0, 0x400000e1);
            app.dispatch(shiftDown);
            app.dispatch(leftEv);
            app.dispatch(leftEv);
            app.dispatch(shiftUp);
            check(app.hasSelection(), "Shift+Left creates selection");
            check(app.selectionStart() == 4, "selection start is at 4");
            check(app.selectionEnd() == 6, "selection end is at 6");
            check(app.caretPosition() == 4, "caret is at 4");

            // Backspace deletes selected text [4, 6] ("ef") -> "abcd"
            app.dispatch(backspaceEv);
            check(txt && txt->value() == "abcd", "backspace deletes selected text");
            check(!app.hasSelection(), "backspace clears selection");
            check(app.caretPosition() == 4, "caret remains at selection start");

            // Set selection [1, 3] ("bc") and replace with "XYZ"
            app.setSelection(1, 3);
            check(app.hasSelection(), "setSelection activates selection");
            app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "XYZ"));
            check(txt && txt->value() == "aXYZd", "typing replaces selected text");
            check(!app.hasSelection(), "typing clears selection");
            check(app.caretPosition() == 4, "caret advances to end of inserted text");

            // Delete key on selection [1, 4] ("XYZ") -> "ad"
            app.setSelection(1, 4);
            check(app.hasSelection(), "setSelection activates selection [1, 4]");
            app.dispatch(deleteEv);
            check(txt && txt->value() == "ad", "delete key deletes selected text");
            check(!app.hasSelection(), "delete key clears selection");
            check(app.caretPosition() == 1, "caret is at 1 after delete");

            // IME composition on focused input
            sui::InputEvent imeEdit(sui::InputKind::TextEdit, 0, 0, 0, 0, 0, 0, "nihon", 3, 5);
            app.dispatch(imeEdit);
            check(app.hasComposition(), "app has composition after TextEdit");
            check(app.compositionText() == "nihon", "composition text matches");
            check(app.compositionCursor() == 3, "composition cursor matches");

            // Committing IME via TextInput
            app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "\xE6\x97\xA5\xE6\x9C\xAC"));
            check(!app.hasComposition(), "TextInput clears composition");
            check(app.compositionText().empty(), "composition text empty after commit");
            check(txt && txt->value() == "a\xE6\x97\xA5\xE6\x9C\xAC""d", "committed text inserted into input");

            // TextEdit on readonly input does not set composition
            appDoc.setFocused(ro);
            app.dispatch(sui::InputEvent(sui::InputKind::TextEdit, 0, 0, 0, 0, 0, 0, "test", 0, 4));
            check(!app.hasComposition(), "TextEdit on readonly input does not set composition");

            // PointerDown clears composition
            appDoc.setFocused(txt);
            app.dispatch(imeEdit);
            check(app.hasComposition(), "composition restored on txt");
            app.dispatch(sui::InputEvent(sui::InputKind::PointerDown, 0.0f, 0.0f, 1));
            check(!app.hasComposition(), "PointerDown clears composition");

            // A modifier key (Shift in particular) repeats while held, and each
            // repeat must not restart the caret blink or the caret looks stuck
            // solid. Keys that touch the field still restart it. The field is
            // primed first because the key that re-establishes focus after a
            // click legitimately restarts the blink.
            appDoc.setFocused(txt);
            app.dispatch(leftEv);
            const std::uint64_t blinkBefore = app.caretBlinkResets();
            app.dispatch(shiftDown);
            check(app.caretBlinkResets() == blinkBefore,
                  "a modifier key press does not restart the caret blink");
            app.dispatch(shiftUp);
            check(app.caretBlinkResets() == blinkBefore,
                  "a modifier key release does not restart the caret blink");
            app.dispatch(leftEv);
            check(app.caretBlinkResets() == blinkBefore + 1,
                  "an arrow key press restarts the caret blink");
        }
    }

    // A checkbox flips on click, and a radio takes the click for its group,
    // clearing `checked` from the siblings that share its `name`. A disabled
    // control ignores the click.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<input id=\"cb\" type=\"checkbox\"/>"
            "<input id=\"r1\" type=\"radio\" name=\"g\"/>"
            "<input id=\"r2\" type=\"radio\" name=\"g\"/>"
            "<input id=\"dcb\" type=\"checkbox\" disabled/>"
            "</body></sui>", err);
        check(doc != nullptr, "control test document parsed");
        if (doc) {
            sui::Element* cb = doc->getElementById("cb");
            sui::Element* r1 = doc->getElementById("r1");
            sui::Element* r2 = doc->getElementById("r2");
            sui::Element* dcb = doc->getElementById("dcb");
            check(cb && r1 && r2 && dcb, "control elements found");
            if (cb && r1 && r2 && dcb) {
                app.setDocument(std::move(doc));

                // A pointer press lands by geometry, so the document needs a
                // layout pass before the click can find a control.
                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                const auto click = [&](sui::Element* el) {
                    app.dispatch(sui::InputEvent(sui::InputKind::PointerDown,
                                                 el->layoutX() + el->layoutWidth() * 0.5f,
                                                 el->layoutY() + el->layoutHeight() * 0.5f, 1));
                };

                check(!cb->hasAttribute("checked"), "a checkbox starts unchecked");
                click(cb);
                check(cb->hasAttribute("checked"), "clicking a checkbox checks it");
                click(cb);
                check(!cb->hasAttribute("checked"), "clicking a checkbox again unchecks it");

                click(r1);
                check(r1->hasAttribute("checked"), "clicking a radio checks it");
                click(r2);
                check(r2->hasAttribute("checked"), "clicking the other radio checks it");
                check(!r1->hasAttribute("checked"),
                      "a radio click clears the rest of its group");

                click(dcb);
                check(!dcb->hasAttribute("checked"),
                      "clicking a disabled checkbox does nothing");
            }
        }
    }

    // `:focus-visible` follows modality: a clicked text field shows its ring,
    // a clicked button does not, and focus leaving clears the flag.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<input id=\"t\" type=\"text\"/>"
            "<button id=\"b\">go</button>"
            "</body></sui>", err);
        check(doc != nullptr, "focus test document parsed");
        if (doc) {
            sui::Element* t = doc->getElementById("t");
            sui::Element* b = doc->getElementById("b");
            check(t && b, "focus elements found");
            if (t && b) {
                app.setDocument(std::move(doc));

                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                const auto click = [&](sui::Element* el) {
                    app.dispatch(sui::InputEvent(sui::InputKind::PointerDown,
                                                 el->layoutX() + el->layoutWidth() * 0.5f,
                                                 el->layoutY() + el->layoutHeight() * 0.5f, 1));
                };

                check(!app.focusVisible(), "nothing focused shows nothing");
                click(t);
                check(app.focusVisible(), "a clicked text field shows its ring");
                click(b);
                check(!app.focusVisible(), "a clicked button hides its ring");
            }
        }
    }

    // Tab cycles focus through the controls in document order, wrapping at
    // either end; Shift+Tab walks back. Disabled controls are skipped, and
    // keyboard-driven focus always shows its ring.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<input id=\"a\" type=\"text\"/>"
            "<input id=\"skip\" type=\"text\" disabled/>"
            "<button id=\"b\">go</button>"
            "</body></sui>", err);
        check(doc != nullptr, "tab test document parsed");
        if (doc) {
            sui::Element* a = doc->getElementById("a");
            sui::Element* skip = doc->getElementById("skip");
            sui::Element* b = doc->getElementById("b");
            check(a && skip && b, "tab elements found");
            if (a && skip && b) {
                app.setDocument(std::move(doc));

                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                const auto tab = [&]() {
                    app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0,
                                                 0, 0x09));
                };
                check(app.document().focused() == nullptr, "nothing focused at first");
                tab();
                check(app.document().focused() == a, "tab first stops on the field");
                check(app.focusVisible(), "keyboard focus shows its ring");
                tab();
                check(app.document().focused() == b, "tab skips disabled controls");
                tab();
                check(app.document().focused() == a, "tab wraps past the end");
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0, 0,
                                             0x400000e1));
                tab();
                check(app.document().focused() == b, "shift+tab walks back");
                app.dispatch(sui::InputEvent(sui::InputKind::KeyUp, 0, 0, 0, 0, 0,
                                             0x400000e1));
            }
        }
    }

    // A datalist pops suggestions for a listed input: focusing opens, typing
    // filters, rows and arrows/Enter pick, Escape and outside clicks close.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<input id=\"t\" type=\"text\" list=\"fruits\"/>"
            "<datalist id=\"fruits\">"
            "<option value=\"apple\"></option>"
            "<option value=\"apricot\"></option>"
            "<option value=\"banana\" disabled=\"\"></option>"
            "<option value=\"cherry\"></option>"
            "</datalist>"
            "<input id=\"plain\" type=\"text\"/>"
            "<input id=\"missing\" type=\"text\" list=\"nope\"/>"
            "</body></sui>", err);
        check(doc != nullptr, "datalist document parsed");
        if (doc) {
            sui::Element* t = doc->getElementById("t");
            sui::Element* plain = doc->getElementById("plain");
            sui::Element* missing = doc->getElementById("missing");
            check(t && plain && missing, "datalist elements found");
            if (t && plain && missing) {
                app.setDocument(std::move(doc));

                sui::StyleResolver layoutResolver;
                layoutResolver.setRules(sui::CSSParser::parse("input{display:block}"));
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                constexpr float kRowH = 16.0f;
                const auto box = [&](sui::Element* el) {
                    return sui::InputEvent(sui::InputKind::PointerDown,
                                           el->layoutX() + el->layoutWidth() * 0.5f,
                                           el->layoutY() + el->layoutHeight() * 0.5f, 1);
                };
                const auto row = [&](sui::Element* el, int index) {
                    return sui::InputEvent(sui::InputKind::PointerDown,
                                           el->layoutX() + 5.0f,
                                           el->layoutY() + el->layoutHeight() +
                                               (static_cast<float>(index) + 0.5f) * kRowH,
                                           1);
                };
                const auto type = [&](const char* text) {
                    app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0,
                                                 0, 0, text));
                };
                const auto key = [&](int code) {
                    app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0,
                                                 0, code));
                };

                check(app.openDatalist() == nullptr, "no suggestions start open");
                app.dispatch(box(t));
                check(app.openDatalist() == t, "focusing a listed input opens");
                // Clicking the fourth row picks cherry over the disabled banana.
                app.dispatch(row(t, 3));
                check(t->value() == "cherry", "clicking a row picks that suggestion");
                check(app.openDatalist() == nullptr, "a pick closes the popup");

                // Typing filters to matches; Enter picks the first.
                t->setValue("");
                app.dispatch(box(t));
                type("ap");
                check(app.openDatalist() == t, "typing keeps matches open");
                key(0x0D);
                check(t->value() == "apple", "enter picks the first match");
                check(app.openDatalist() == nullptr, "picking closes the popup");

                // Arrows skip the disabled banana for cherry.
                t->setValue("");
                app.dispatch(box(t));
                key(0x40000051);
                key(0x40000051);
                check(app.datalistHighlight() == 3, "arrows skip disabled options");
                key(0x0D);
                check(t->value() == "cherry", "enter picks the highlight");

                // Escape closes; typing past every match hides the popup.
                app.dispatch(box(t));
                key(0x1B);
                check(app.openDatalist() == nullptr, "escape closes the popup");
                t->setValue("");
                app.dispatch(box(t));
                type("zzz");
                app.dispatch(box(plain));
                check(app.openDatalist() == nullptr, "leaving closes the popup");
                check(t->value() == "zzz", "unmatched typing stays put");

                // Plain and dangling inputs never pop up.
                app.dispatch(box(plain));
                check(app.openDatalist() == nullptr, "an unlisted input stays shut");
                app.dispatch(box(missing));
                check(app.openDatalist() == nullptr, "a dangling list stays shut");
            }
        }
    }

    // Scroll snapping settles a wheel notch on child snap points: mandatory
    // always takes the nearest, proximity only within half a viewport.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<div id=\"m\" style=\"height:100px;overflow-y:auto;scroll-snap-type:y "
            "mandatory\">"
            "<div style=\"height:60px;scroll-snap-align:start\"></div>"
            "<div style=\"height:60px;scroll-snap-align:start\"></div>"
            "<div style=\"height:60px;scroll-snap-align:start\"></div>"
            "</div>"
            "<div id=\"p\" style=\"height:100px;overflow-y:auto;scroll-snap-type:y "
            "proximity\">"
            "<div style=\"height:200px;scroll-snap-align:start\"></div>"
            "<div style=\"height:200px;scroll-snap-align:start\"></div>"
            "<div style=\"height:200px;scroll-snap-align:start\"></div>"
            "</div>"
            "<div id=\"n\" style=\"height:100px;overflow-y:auto\">"
            "<div style=\"height:60px\"></div>"
            "<div style=\"height:60px\"></div>"
            "<div style=\"height:60px\"></div>"
            "</div>"
            "<div id=\"d\" style=\"height:100px;overflow-y:auto;scroll-snap-type:y "
            "mandatory;scroll-padding-top:20px\">"
            "<div style=\"height:60px;scroll-snap-align:start\"></div>"
            "<div style=\"height:60px;scroll-snap-align:start\"></div>"
            "<div style=\"height:60px;scroll-snap-align:start\"></div>"
            "</div>"
            "<div id=\"g\" style=\"height:100px;overflow-y:auto;scroll-snap-type:y "
            "mandatory\">"
            "<div style=\"height:60px;scroll-snap-align:start\"></div>"
            "<div style=\"height:60px;scroll-snap-align:start;scroll-margin-top:10px\">"
            "</div>"
            "<div style=\"height:60px;scroll-snap-align:start\"></div>"
            "</div>"
            "</body></sui>", err);
        check(doc != nullptr, "snap document parsed");
        if (doc) {
            sui::Element* m = doc->getElementById("m");
            sui::Element* p = doc->getElementById("p");
            sui::Element* n = doc->getElementById("n");
            sui::Element* d = doc->getElementById("d");
            sui::Element* g = doc->getElementById("g");
            check(m && p && n && d && g, "snap elements found");
            if (m && p && n && d && g) {
                app.setDocument(std::move(doc));

                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 600.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                const auto wheel = [&](sui::Element* el, float dx, float dy) {
                    app.dispatch(sui::InputEvent(sui::InputKind::Wheel,
                                                 el->layoutX() + 10.0f,
                                                 el->layoutY() + 10.0f, 0, dx, dy));
                };
                // One notch down lands at 40, then snaps to the 60px start.
                wheel(m, 0.0f, -1.0f);
                check(m->scrollTop() == 60.0f, "mandatory snap takes the nearest start");
                // Without a type the notch rests where it lands.
                wheel(n, 0.0f, -1.0f);
                check(n->scrollTop() == 40.0f, "no snap type means no snap");
                // Proximity far from every point rests too: 100 + 40 lands at
                // 140, sixty from the 200px start.
                p->setScrollTop(100.0f);
                wheel(p, 0.0f, -1.0f);
                check(p->scrollTop() == 140.0f, "proximity skips distant points");
                // Near one it settles: 165 + 40 lands five from 200.
                p->setScrollTop(165.0f);
                wheel(p, 0.0f, -1.0f);
                check(p->scrollTop() == 200.0f, "proximity takes near points");
                // Padding insets the snapport: the 60px starts snap at 40.
                wheel(d, 0.0f, -1.0f);
                check(d->scrollTop() == 40.0f, "padding shifts snap points");
                // A margin offsets its own area: the second 60px start snaps at 50.
                wheel(g, 0.0f, -1.0f);
                check(g->scrollTop() == 50.0f, "margins shift snap points");
            }
        }
    }

    // `scroll-behavior: smooth` eases a notch toward its target over several
    // frames instead of jumping, and lands exactly where a jump would.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<div id=\"s\" style=\"height:100px;overflow-y:auto;"
            "scroll-behavior:smooth\">"
            "<div style=\"height:300px\"></div></div>"
            "</body></sui>",
            err);
        check(doc != nullptr, "smooth document parsed");
        if (doc) {
            sui::Element* s = doc->getElementById("s");
            check(s != nullptr, "smooth scroller found");
            if (s) {
                app.setDocument(std::move(doc));

                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 600.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                app.dispatch(sui::InputEvent(sui::InputKind::Wheel, s->layoutX() + 10.0f,
                                            s->layoutY() + 10.0f, 0, 0.0f, -1.0f));
                check(s->scrollTop() == 0.0f, "a smooth notch does not jump");
                check(app.tickScrolling(), "the scroll reports itself still moving");
                const float first = s->scrollTop();
                check(first > 0.0f && first < 40.0f, "the first frame closes part of the gap");
                app.tickScrolling();
                check(s->scrollTop() > first, "the next frame moves further");
                int frames = 0;
                while (app.tickScrolling() && frames < 200) {
                    ++frames;
                }
                check(frames > 1 && frames < 60, "the scroll takes several frames");
                check(s->scrollTop() == 40.0f, "the scroll lands exactly on the notch");
                check(!app.tickScrolling(), "a finished scroll reports itself idle");
            }
        }
    }

    // Navigating between .sui pages. A project is a directory of pages with one
    // entry file; every href resolves against that directory, so a link to
    // `pages/settings.sui` is the same page from anywhere in the project.
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "sui-nav-check";
        fs::remove_all(root);
        fs::create_directories(root / "pages");
        const auto write = [](const fs::path& at, const char* text) {
            std::ofstream out(at, std::ios::binary);
            out << text;
        };
        write(root / "index.sui",
              "<sui><head><link rel=\"stylesheet\" href=\"home.css\"/></head>"
              "<body><a id=\"to-settings\" href=\"pages/settings.sui\">Settings</a>"
              "<p id=\"where\">home</p></body></sui>");
        write(root / "home.css", "#where { color: #ff0000 }");
        write(root / "pages" / "settings.sui",
              "<sui><head><link rel=\"stylesheet\" href=\"theme.css\"/></head>"
              "<body><p id=\"where\">settings</p></body></sui>");
        write(root / "theme.css", "#where { font-size: 30px }");
        write(root / "broken.sui", "<sui><body><p>never closed</body></sui>");

        // The entry is named relative to the project root, so the process has
        // to be standing in it -- which is how a packaged app launches.
        const fs::path previous = fs::current_path();
        fs::current_path(root);
        struct Restore {
            fs::path previous;
            ~Restore() {
                std::error_code ec;
                fs::current_path(previous, ec);
            }
        } restore{previous};

        sui::Application app;
        std::string error;
        check(app.openProject("index.sui", error), "a project opens");
        check(app.currentPage() == "index.sui", "the entry is the live page");
        sui::Element* where = app.document().getElementById("where");
        check(where && app.document().getElementById("to-settings"),
              "the entry page is on screen");

        // A page's own stylesheets load with it, and its identifiers replace the
        // previous page's.
        check(app.navigate("pages/settings.sui"), "a linked page loads");
        check(app.currentPage() == "pages/settings.sui", "the navigated page is live");
        where = app.document().getElementById("where");
        check(where && !app.document().getElementById("to-settings"),
              "the new page replaced the old tree");

        // The page brought its own stylesheet and the previous page's is gone:
        // the entry styled `where` red and the new page does not, so anything
        // but the user-agent colour means the old rules outlived their page.
        sui::StyleResolver* live = sui::script::context()->styles;
        const sui::ComputedStyle styled =
            live && where ? live->resolve(*where) : sui::ComputedStyle{};
        check(styled.color.r != 0xff, "the previous page's rules did not survive");
        check(styled.fontSize == 30.0f, "the new page's stylesheet loaded");

        // A page that will not parse is refused, and the live one survives.
        check(!app.navigate("broken.sui"), "a broken page is refused");
        check(app.currentPage() == "pages/settings.sui",
              "the live page survives a broken link");
        check(app.document().getElementById("where") != nullptr,
              "the old tree is still there");
        check(!app.navigate("pages/missing.sui"), "a missing page is refused");
        check(!app.navigate("https://example.com/") &&
                  app.currentPage() == "pages/settings.sui",
              "an external url is refused and changes nothing");

        fs::remove_all(root);
    }

    // Overlay scrollbars drag by the thumb and page on track clicks. The
    // thumb travels its track proportionally to the scroll range.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<div id=\"s\" style=\"height:100px;overflow-y:auto\">"
            "<div style=\"height:60px\"></div>"
            "<div style=\"height:60px\"></div>"
            "<div style=\"height:60px\"></div>"
            "<div style=\"height:60px\"></div>"
            "<div style=\"height:60px\"></div>"
            "</div>"
            "<div id=\"f\" style=\"height:100px;overflow-y:auto\">"
            "<div style=\"height:20px\"></div>"
            "</div>"
            "</body></sui>", err);
        check(doc != nullptr, "scrollbar document parsed");
        if (doc) {
            sui::Element* s = doc->getElementById("s");
            sui::Element* f = doc->getElementById("f");
            check(s && f, "scrollbar elements found");
            if (s && f) {
                app.setDocument(std::move(doc));

                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 600.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                // Content 300 over a 100 viewport: a 33px thumb at the top.
                // Drag math divides unevenly, so the landing asserts near.
                const float edge = s->layoutX() + s->layoutWidth() - 5.0f;
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown, edge,
                                             s->layoutY() + 10.0f, 1));
                app.dispatch(sui::InputEvent(sui::InputKind::PointerMove, edge,
                                             s->layoutY() + 60.0f, 1));
                check(std::fabs(s->scrollTop() - 150.0f) < 0.5f,
                      "dragging the thumb scrolls along");
                app.dispatch(sui::InputEvent(sui::InputKind::PointerUp, edge,
                                             s->layoutY() + 60.0f, 1));
                app.dispatch(sui::InputEvent(sui::InputKind::PointerMove, edge,
                                             s->layoutY() + 90.0f, 1));
                check(std::fabs(s->scrollTop() - 150.0f) < 0.5f,
                      "release drops the drag");

                // A track click below the thumb pages down by a viewport.
                s->setScrollTop(0.0f);
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown, edge,
                                             s->layoutY() + 80.0f, 1));
                check(s->scrollTop() == 100.0f, "a track click pages toward it");

                // Nothing scrollable, nothing to grab.
                app.dispatch(sui::InputEvent(
                    sui::InputKind::PointerDown, f->layoutX() + f->layoutWidth() - 5.0f,
                    f->layoutY() + 10.0f, 1));
                app.dispatch(sui::InputEvent(
                    sui::InputKind::PointerMove, f->layoutX() + f->layoutWidth() - 5.0f,
                    f->layoutY() + 60.0f, 1));
                check(f->scrollTop() == 0.0f, "a flat box drags nowhere");
            }
        }
    }

    // A file input's Browse button asks the OS picker; the pick lands through
    // applyPickedFile, which drops paths for dropped trees and non-controls.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<input id=\"f\" type=\"file\"/>"
            "<div id=\"d\">x</div>"
            "</body></sui>", err);
        check(doc != nullptr, "file test document parsed");
        if (doc) {
            sui::Element* f = doc->getElementById("f");
            sui::Element* d = doc->getElementById("d");
            check(f && d, "file elements found");
            if (f && d) {
                app.setDocument(std::move(doc));

                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                // The Browse button hangs in the left padding room, where the
                // box paints it.
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown,
                                             f->contentX() - 42.0f,
                                             f->contentY() + f->contentHeight() * 0.5f,
                                             1));
                check(f->value().empty(), "a headless picker picks nothing");

                app.applyPickedFile(f, "/tmp/photo.png");
                check(f->value() == "/tmp/photo.png", "a picked path fills the value");
                check(f->fileName() == "photo.png", "the control shows the file name");

                app.applyPickedFile(d, "/tmp/other.png");
                check(d->value().empty(), "a pick for a div lands nowhere");
                app.applyPickedFile(nullptr, "/tmp/null.png");

                // A pick for an element of a replaced document is dropped.
                sui::Element* stale = f;
                auto doc2 = sui::MarkupParser::parse("<sui><body></body></sui>", err);
                check(doc2 != nullptr, "second document parsed");
                if (doc2) {
                    app.setDocument(std::move(doc2));
                    app.applyPickedFile(stale, "/tmp/stale.png");
                    check(stale->value() == "/tmp/photo.png",
                          "a pick for a dropped tree lands nowhere");
                }
            }
        }
    }

    // A tilted 3D box is hit through its projected quad: its centre catches,
    // but a point inside the flat box yet outside the warp misses.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<div id=\"tilt\" style=\"width:100px;height:100px;transform:rotateY(60deg)\"></div>"
            "</body></sui>", err);
        check(doc != nullptr, "hit test document parsed");
        if (doc) {
            sui::Element* tilt = doc->getElementById("tilt");
            check(tilt, "hit element found");
            if (tilt) {
                app.setDocument(std::move(doc));

                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                const float cx = tilt->layoutX() + tilt->layoutWidth() * 0.5f;
                const float cy = tilt->layoutY() + tilt->layoutHeight() * 0.5f;
                // cos60 halves the box about its centre: the flat left edge
                // falls outside the quad.
                const float outside = tilt->layoutX() + tilt->layoutWidth() * 0.1f;
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown, cx, cy, 1));
                check(app.document().focused() == tilt, "a tilted box catches its centre");
                app.dispatch(
                    sui::InputEvent(sui::InputKind::PointerDown, outside, cy, 1));
                check(app.document().focused() != tilt,
                      "a point outside the warped quad misses");
            }
        }
    }

    // A range jumps to the value under the pointer, drags with the pointer
    // held, and steps on arrows/Home/End. A disabled range ignores everything.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<input id=\"r\" type=\"range\"/>"
            "<input id=\"s\" type=\"range\" min=\"0\" max=\"10\" step=\"2\" value=\"4\"/>"
            "<input id=\"d\" type=\"range\" disabled/>"
            "</body></sui>", err);
        check(doc != nullptr, "range test document parsed");
        if (doc) {
            sui::Element* r = doc->getElementById("r");
            sui::Element* s = doc->getElementById("s");
            sui::Element* d = doc->getElementById("d");
            check(r && s && d, "range elements found");
            if (r && s && d) {
                app.setDocument(std::move(doc));

                // One range per line so no neighbour overlaps a click target.
                sui::StyleResolver layoutResolver;
                layoutResolver.setRules(sui::CSSParser::parse("input{display:block}"));
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                const auto at = [&](sui::Element* el, float fraction) {
                    return sui::InputEvent(sui::InputKind::PointerDown,
                                           el->layoutX() + el->layoutWidth() * fraction,
                                           el->layoutY() + el->layoutHeight() * 0.5f, 1);
                };

                // An absent value starts halfway.
                check(r->value().empty(), "a range starts with no live value");
                app.dispatch(at(r, 0.25f));
                check(r->value() == "25", "clicking a quarter along sets 25");
                app.dispatch(at(r, 0.0f));
                check(r->value() == "0", "clicking the start sets the min");
                app.dispatch(at(r, 0.99f));
                check(r->value() == "99", "clicking near the end sets 99");

                // Dragging with the button held moves the thumb without a press,
                // and clamps past the ends (the drag follows focus, not the hit).
                app.dispatch(at(r, 0.5f));
                check(r->value() == "50", "clicking the middle sets 50");
                app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                             r->layoutX() + r->layoutWidth() * 1.5f,
                                             r->layoutY() + r->layoutHeight() * 0.5f, 1));
                check(r->value() == "100", "dragging past the end clamps to the max");
                app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                             r->layoutX() - 20.0f,
                                             r->layoutY() + r->layoutHeight() * 0.5f, 1));
                check(r->value() == "0", "dragging past the start clamps to the min");
                // A hover (no button) is not a drag.
                app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                             r->layoutX() + r->layoutWidth() * 0.75f,
                                             r->layoutY() + r->layoutHeight() * 0.5f, 0));
                check(r->value() == "0", "a buttonless move leaves the value alone");

                // Keys step a focused range; Home/End jump to the bounds.
                sui::Document& appDoc = app.document();
                appDoc.setFocused(r);
                sui::InputEvent leftEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000050);
                sui::InputEvent rightEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x4000004F);
                sui::InputEvent homeEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x4000004A);
                sui::InputEvent endEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x4000004D);
                app.dispatch(endEv);
                check(r->value() == "100", "End moves a range to its max");
                app.dispatch(leftEv);
                check(r->value() == "99", "Left steps a range down by one");
                app.dispatch(rightEv);
                app.dispatch(rightEv);
                check(r->value() == "100", "Right steps a range up, clamped at the max");
                app.dispatch(homeEv);
                check(r->value() == "0", "Home moves a range to its min");
                app.dispatch(leftEv);
                check(r->value() == "0", "stepping past the min stays at the min");

                // min/max/step shape the value: 4 snaps to the grid, Right adds 2.
                check(s->value() == "4", "a stepped range keeps its markup value");
                appDoc.setFocused(s);
                app.dispatch(rightEv);
                check(s->value() == "6", "Right steps by the step attribute");
                app.dispatch(at(s, 0.35f));
                check(s->value() == "4", "a click snaps to the step grid");

                // A disabled range ignores the pointer and the keys.
                app.dispatch(at(d, 0.75f));
                check(d->value().empty(), "clicking a disabled range does nothing");
                appDoc.setFocused(d);
                app.dispatch(rightEv);
                check(d->value().empty(), "keys on a disabled range do nothing");
            }
        }
    }

    // A select opens its popup on click and picks the clicked row; arrows move
    // the selection when closed and the highlight when open; Enter opens, then
    // picks the highlight; Escape closes; release over a row picks it. A
    // disabled select and an empty one stay inert.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<select id=\"s\"><option>apple</option><option>banana</option>"
            "<option>cherry</option></select>"
            "<select id=\"m\"><option>aye</option>"
            "<option value=\"B\" selected>bee</option></select>"
            "<select id=\"d\" disabled><option>one</option></select>"
            "<select id=\"e\"></select>"
            "</body></sui>", err);
        check(doc != nullptr, "select test document parsed");
        if (doc) {
            sui::Element* s = doc->getElementById("s");
            sui::Element* m = doc->getElementById("m");
            sui::Element* d = doc->getElementById("d");
            sui::Element* e = doc->getElementById("e");
            check(s && m && d && e, "select elements found");
            if (s && m && d && e) {
                app.setDocument(std::move(doc));

                // One select per line so rows never overlap a neighbour. Rows
                // are the select's own line (16px at the default size).
                sui::StyleResolver layoutResolver;
                layoutResolver.setRules(sui::CSSParser::parse("select{display:block}"));
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                constexpr float kRowH = 16.0f;
                const auto box = [&](sui::Element* el) {
                    return sui::InputEvent(sui::InputKind::PointerDown,
                                           el->layoutX() + el->layoutWidth() * 0.5f,
                                           el->layoutY() + el->layoutHeight() * 0.5f, 1);
                };
                const auto row = [&](sui::Element* el, int index, float dy = 0.5f) {
                    return sui::InputEvent(sui::InputKind::PointerDown,
                                           el->layoutX() + 5.0f,
                                           el->layoutY() + el->layoutHeight() +
                                               (static_cast<float>(index) + dy) * kRowH,
                                           1);
                };

                check(s->value() == "apple", "a select value starts at its first option");
                check(app.openSelect() == nullptr, "no popup starts open");
                check(m->value() == "B", "an option value attribute beats its text");
                check(e->value().empty(), "an empty select has no value");

                // Click opens; clicking the second row picks it and closes.
                app.dispatch(box(s));
                check(app.openSelect() == s, "clicking a select opens its popup");
                app.dispatch(row(s, 2));
                check(s->value() == "cherry", "clicking a row picks that option");
                check(app.openSelect() == nullptr, "a pick closes the popup");

                // Clicking the open box toggles shut without changing the value.
                app.dispatch(box(s));
                check(app.openSelect() == s, "the popup reopens");
                app.dispatch(box(s));
                check(app.openSelect() == nullptr, "clicking the open box closes it");
                check(s->value() == "cherry", "toggling shut keeps the value");

                // Hovering rows moves the highlight; leaving the list keeps it.
                app.dispatch(box(s));
                app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                             s->layoutX() + 5.0f,
                                             s->layoutY() + s->layoutHeight() + 0.5f * kRowH, 0));
                check(app.selectHighlight() == 0, "hovering the first row highlights it");
                app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                             s->layoutX() + 5.0f, 200.0f, 0));
                check(app.selectHighlight() == 0, "hovering off the list keeps it");
                // ... and clicking far below closes without picking.
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown,
                                             s->layoutX() + 5.0f, 200.0f, 1));
                check(app.openSelect() == nullptr, "clicking outside closes the popup");
                check(s->value() == "cherry", "closing outside keeps the value");

                // Press-drag-release picks the released row too.
                app.dispatch(box(s));
                app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                             s->layoutX() + 5.0f,
                                             s->layoutY() + s->layoutHeight() + 1.5f * kRowH, 1));
                check(app.selectHighlight() == 1, "dragging highlights the row under it");
                app.dispatch(sui::InputEvent(sui::InputKind::PointerUp,
                                             s->layoutX() + 5.0f,
                                             s->layoutY() + s->layoutHeight() + 1.5f * kRowH, 1));
                check(s->value() == "banana", "releasing over a row picks it");
                check(app.openSelect() == nullptr, "the release closes the popup");

                // Arrows move the selection while closed (value is banana).
                sui::Document& appDoc = app.document();
                appDoc.setFocused(s);
                sui::InputEvent upEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000052);
                sui::InputEvent downEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000051);
                sui::InputEvent enterEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x0D);
                sui::InputEvent escEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x1B);
                app.dispatch(upEv);
                check(s->value() == "apple", "Up moves a closed selection up");
                app.dispatch(upEv);
                check(s->value() == "apple", "Up clamps at the first option");
                app.dispatch(downEv);
                app.dispatch(downEv);
                check(s->value() == "cherry", "Down moves a closed selection down");

                // Enter opens on the selection; arrows move the highlight;
                // Enter picks it; Escape only closes.
                app.dispatch(upEv);
                check(s->value() == "banana", "back to banana for the open test");
                app.dispatch(enterEv);
                check(app.openSelect() == s, "Enter opens the popup");
                check(app.selectHighlight() == 1, "it opens highlighted on the selection");
                app.dispatch(downEv);
                check(app.selectHighlight() == 2, "Down moves the highlight");
                app.dispatch(enterEv);
                check(s->value() == "cherry", "Enter picks the highlight");
                check(app.openSelect() == nullptr, "picking closes the popup");
                app.dispatch(enterEv);
                check(app.openSelect() == s, "Enter reopens");
                app.dispatch(escEv);
                check(app.openSelect() == nullptr, "Escape closes the popup");
                check(s->value() == "cherry", "Escape keeps the value");

                // A disabled select ignores clicks and keys.
                app.dispatch(box(d));
                check(app.openSelect() == nullptr, "clicking a disabled select opens nothing");
                appDoc.setFocused(d);
                app.dispatch(downEv);
                check(d->value() == "one", "keys on a disabled select do nothing");

                // An empty select opens onto no rows and ignores keys.
                app.dispatch(box(e));
                check(app.openSelect() == e, "an empty select still opens");
                app.dispatch(escEv);
                check(app.openSelect() == nullptr, "Escape closes the empty popup");
                appDoc.setFocused(e);
                app.dispatch(downEv);
                check(app.openSelect() == nullptr, "keys on an empty select do nothing");

                // Writing the value selects the match; no match falls back to
                // the first option.
                s->setValue("apple");
                check(s->value() == "apple", "setValue selects the matching option");
                s->setValue("zzz");
                check(s->value() == "apple", "an unmatched setValue falls back to first");
            }
        }
    }

    // An open list (`size`/`multiple`) picks its clicked row: plain clicks
    // select, Ctrl toggles, Shift extends from the anchor. Arrows move the
    // selection and scroll it into view; clicking the chrome never pops up.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<select id=\"lb\" size=\"5\"><option>a1</option><option>a2</option>"
            "<option>a3</option><option>a4</option><option>a5</option></select>"
            "<select id=\"sc\" size=\"2\"><option>c1</option><option>c2</option>"
            "<option>c3</option><option>c4</option><option>c5</option></select>"
            "<select id=\"m\" multiple><option>b1</option><option>b2</option>"
            "<option>b3</option><option>b4</option><option>b5</option></select>"
            "<select id=\"ds\" size=\"2\"><option>z1</option>"
            "<option disabled>z2</option><option>z3</option></select>"
            "</body></sui>", err);
        check(doc != nullptr, "listbox test document parsed");
        if (doc) {
            sui::Element* lb = doc->getElementById("lb");
            sui::Element* sc = doc->getElementById("sc");
            sui::Element* m = doc->getElementById("m");
            sui::Element* ds = doc->getElementById("ds");
            check(lb && sc && m && ds, "listbox elements found");
            if (lb && sc && m && ds) {
                app.setDocument(std::move(doc));
                sui::Document& appDoc = app.document();

                sui::StyleResolver layoutResolver;
                layoutResolver.setRules(sui::CSSParser::parse("select{display:block}"));
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                // Rows tile the content box one line (16px) each.
                constexpr float kRowH = 16.0f;
                const auto cell = [&](sui::Element* sel, int index) {
                    return sui::InputEvent(sui::InputKind::PointerDown,
                                           sel->contentX() + 5.0f,
                                           sel->contentY() + (index + 0.5f) * kRowH, 1);
                };
                const auto marks = [&](sui::Element* sel) {
                    std::string out;
                    for (sui::Element* o : sel->selectOptions()) {
                        out += o->hasAttribute("selected") ? '1' : '0';
                    }
                    return out;
                };
                sui::InputEvent upEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000052);
                sui::InputEvent downEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000051);

                // Clicking a row selects it and focuses the select.
                app.dispatch(cell(lb, 3));
                check(lb->value() == "a4", "clicking a list row selects it");
                check(appDoc.focused() == lb, "clicking a row focuses the list");
                check(app.openSelect() == nullptr, "a list never pops up");
                app.dispatch(downEv);
                check(lb->value() == "a5", "Down moves a list selection");
                app.dispatch(downEv);
                check(lb->value() == "a5", "Down clamps at the last row");
                app.dispatch(upEv);
                app.dispatch(upEv);
                app.dispatch(upEv);
                app.dispatch(upEv);
                check(lb->value() == "a1", "Up walks back to the first row");

                // A short list scrolls: walking past the fold moves the offset,
                // walking back restores it.
                appDoc.setFocused(sc);
                app.dispatch(downEv);
                app.dispatch(downEv);
                app.dispatch(downEv);
                app.dispatch(downEv);
                check(sc->value() == "c5", "Down reaches the last row");
                check(sc->scrollTop() > 0.0f, "moving past the fold scrolls the list");
                app.dispatch(upEv);
                app.dispatch(upEv);
                app.dispatch(upEv);
                app.dispatch(upEv);
                check(sc->value() == "c1", "Up walks back to the first row");
                check(sc->scrollTop() == 0.0f, "walking back unscrolled the list");

                // Clicking the chrome (padding, not a row) opens nothing.
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown,
                                             lb->layoutX() + 2.0f,
                                             lb->layoutY() + lb->layoutHeight() * 0.5f, 1));
                check(app.openSelect() == nullptr, "list chrome never pops up");

                // Multiple: plain clicks select one, Ctrl toggles, Shift ranges.
                sui::InputEvent ctrlDown(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x400000e0);
                sui::InputEvent ctrlUp(sui::InputKind::KeyUp, 0, 0, 0, 0, 0, 0x400000e0);
                sui::InputEvent shiftDown(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x400000e1);
                sui::InputEvent shiftUp(sui::InputKind::KeyUp, 0, 0, 0, 0, 0, 0x400000e1);
                app.dispatch(cell(m, 1));
                check(marks(m) == "01000", "a multiple click selects one row");
                check(m->value() == "b2", "a multiple value reads the first mark");
                app.dispatch(ctrlDown);
                app.dispatch(cell(m, 3));
                app.dispatch(ctrlUp);
                check(marks(m) == "01010", "Ctrl+click adds to the marks");
                app.dispatch(ctrlDown);
                app.dispatch(cell(m, 1));
                app.dispatch(ctrlUp);
                check(marks(m) == "00010", "Ctrl+click again clears that mark");
                app.dispatch(cell(m, 0));
                app.dispatch(shiftDown);
                app.dispatch(cell(m, 2));
                app.dispatch(shiftUp);
                check(marks(m) == "11100", "Shift+click marks the anchor range");
                check(m->value() == "b1", "the ranged value reads its first mark");

                // Shift+arrows extend from the anchor without clicking.
                app.dispatch(shiftDown);
                app.dispatch(downEv);
                app.dispatch(shiftUp);
                check(marks(m) == "11110", "Shift+Down extends the range");
                app.dispatch(shiftDown);
                app.dispatch(upEv);
                app.dispatch(shiftUp);
                check(marks(m) == "11100", "Shift+Up shrinks it back");

                // Disabled options neither pick nor break the stride.
                appDoc.setFocused(ds);
                app.dispatch(downEv);
                check(ds->value() == "z3", "arrows skip a disabled option");
                app.dispatch(cell(ds, 1));
                check(ds->value() == "z3", "clicking a disabled option keeps the value");
            }
        }
    }

    // Date/time/color inputs edit as text (the value reads back verbatim); a
    // file control is display-only, showing the name without its path, and a
    // click focuses it like any control.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<input id=\"dt\" type=\"date\" value=\"2026-10-12\"/>"
            "<input id=\"cl\" type=\"color\" value=\"#ff0000\"/>"
            "<input id=\"fl\" type=\"file\" value=\"report.pdf\"/>"
            "</body></sui>", err);
        check(doc != nullptr, "picker test document parsed");
        if (doc) {
            sui::Element* dt = doc->getElementById("dt");
            sui::Element* cl = doc->getElementById("cl");
            sui::Element* fl = doc->getElementById("fl");
            check(dt && cl && fl, "picker elements found");
            if (dt && cl && fl) {
                app.setDocument(std::move(doc));
                sui::Document& appDoc = app.document();

                sui::StyleResolver layoutResolver;
                layoutResolver.setRules(sui::CSSParser::parse("input{display:block}"));
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);

                // A date edits like a text field, caret included.
                appDoc.setFocused(dt);
                app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "X"));
                check(dt->value() == "2026-10-12X", "typing appends to a date value");
                check(app.caretPosition() == 11, "the caret advances past the insert");
                sui::InputEvent leftEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000050);
                app.dispatch(leftEv);
                check(app.caretPosition() == 10, "arrows move a date caret");

                // A color edits its hex text; the swatch follows the value.
                appDoc.setFocused(cl);
                app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "0"));
                check(cl->value() == "#ff00000", "typing appends to a color value");

                // A file ignores typing, and a click focuses it.
                appDoc.setFocused(fl);
                app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "x"));
                check(fl->value() == "report.pdf", "a file ignores text input");
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown,
                                             fl->layoutX() + fl->layoutWidth() * 0.5f,
                                             fl->layoutY() + fl->layoutHeight() * 0.5f, 1));
                check(appDoc.focused() == fl, "clicking a file focuses it");
                check(fl->value() == "report.pdf", "clicking a file keeps its value");
            }
        }
    }

    // A `<textarea>` edits its text content as a multiline value: typing works at
    // any caret position, Return starts a new line, Home/End bound the caret to
    // the line instead of the value, and Up/Down keep the column while changing
    // lines. Pointer input maps onto the nearest line.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<textarea id=\"ta\">ab\ncd</textarea>"
            "<textarea id=\"ro\" readonly>no</textarea>"
            "<textarea id=\"dis\" disabled>no</textarea>"
            "</body></sui>", err);
        check(doc != nullptr, "textarea edit document parsed");
        if (doc) {
            sui::Element* ta = doc->getElementById("ta");
            sui::Element* ro = doc->getElementById("ro");
            sui::Element* dis = doc->getElementById("dis");
            check(ta != nullptr && ro != nullptr && dis != nullptr,
                  "textarea elements found");
            if (ta && ro && dis) {
                app.setDocument(std::move(doc));
                sui::Document& appDoc = app.document();

                check(ta->value() == "ab\ncd",
                      "textarea value keeps the newline in its text content");

                // Focus the textarea; its caret initialises on the first edit.
                appDoc.setFocused(ta);

                // Typing inserts at the caret without merging the lines.
                app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "X"));
                check(ta->value() == "ab\ncdX", "typing appends to a textarea");
                check(app.caretPosition() == 6, "caret advances past the textarea insert");

                // Return starts a new line.
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x0D));
                check(ta->value() == "ab\ncdX\n", "Return starts a new textarea line");
                check(app.caretPosition() == 7, "caret follows the inserted newline");

                // Backspace joins the lines again by deleting the newline.
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x08));
                check(ta->value() == "ab\ncdX", "backspace deletes a textarea newline");
                check(app.caretPosition() == 6, "caret lands where the newline was");

                // Left/right arrows cross the newline one byte at a time.
                sui::InputEvent leftEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000050);
                sui::InputEvent rightEv(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x4000004F);
                app.setCaretPosition(3);  // start of the second line, "cdX"
                app.dispatch(leftEv);
                check(app.caretPosition() == 2,
                      "left arrow crosses a newline into the earlier line");
                app.dispatch(rightEv);
                check(app.caretPosition() == 3,
                      "right arrow crosses back over the newline");
                app.dispatch(rightEv);
                check(app.caretPosition() == 4,
                      "right arrow moves within a textarea line");

                // Home/End are scoped to the current line in a textarea.
                app.setCaretPosition(5);
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x4000004A));
                check(app.caretPosition() == 3, "Home goes to the current line start");
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x4000004D));
                check(app.caretPosition() == 6, "End goes to the current line end");

                // Up/Down move between lines keeping the horizontal column.
                app.setCaretPosition(6);
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000052));
                check(app.caretPosition() == 2,
                      "Up moves to the end of the previous line");
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x40000051));
                check(app.caretPosition() == 5, "Down moves back keeping the column");

                // Readonly and disabled textareas reject edits, Return included.
                appDoc.setFocused(ro);
                app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "x"));
                check(ro->value() == "no", "readonly textarea ignores typing");
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0, 0, 0x0D));
                check(ro->value() == "no", "readonly textarea ignores Return");
                appDoc.setFocused(dis);
                app.dispatch(sui::InputEvent(sui::InputKind::TextInput, 0, 0, 0, 0, 0, 0, "x"));
                check(dis->value() == "no", "disabled textarea ignores typing");

                // Select-all covers the whole value, newlines included.
                appDoc.setFocused(ta);
                app.selectAll();
                check(app.hasSelection(), "textarea selectAll creates a selection");
                check(app.selectionStart() == 0, "textarea selection starts at 0");
                check(app.selectionEnd() == 6, "textarea selection covers the whole value");

                // Pointer input maps onto the nearest line, and dragging selects
                // across lines, so click-and-drag reads like a desktop app. The
                // fields are stacked as blocks so no neighbour overlaps the
                // click targets.
                sui::StyleResolver layoutResolver;
                layoutResolver.setRules(sui::CSSParser::parse("textarea{display:block}"));
                sui::LayoutInput layoutIn;
                layoutIn.resolver = &layoutResolver;
                layoutIn.viewportWidth = 400.0f;
                layoutIn.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), layoutIn);
                const std::vector<sui::Element::TextRun>& runs = ta->textRuns();
                check(runs.size() == 2 && runs[0].text == "ab" && runs[1].text == "cdX",
                      "layout split the textarea value into one run per line");
                if (runs.size() == 2) {
                    app.dispatch(sui::InputEvent(sui::InputKind::PointerDown,
                                                 runs[1].x + runs[1].width * 0.5f,
                                                 runs[1].y + runs[1].height * 0.5f, 1));
                    check(app.caretPosition() >= 3 && app.caretPosition() <= 6,
                          "clicking the second line moves the caret into it");
                    app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                                 runs[0].x + 1.0f, runs[0].y + 1.0f, 1));
                    check(ta->value() == "ab\ncdX", "dragging leaves the value unchanged");
                    check(app.hasSelection(), "dragging across lines selects");
                    check(app.selectionStart() <= 3 && app.selectionEnd() >= 3,
                          "the drag selection spans both lines");
                }
            }
        }
    }

    // `cursor` is resolved from the hovered element and handed to the platform.
    // The decision is observable without a window through Application::activeCursor.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<div id=\"hit\" style=\"cursor:pointer;width:100px;height:40px\"></div>"
            "<div id=\"miss\" style=\"width:100px;height:40px\"></div>"
            "</body></sui>", err);
        check(doc != nullptr, "cursor test document parsed");
        if (doc) {
            sui::Element* hit = doc->getElementById("hit");
            sui::Element* miss = doc->getElementById("miss");
            app.setDocument(std::move(doc));

            // Paint ops read element geometry, so the document needs a layout pass
            // before a move can land on anything.
            sui::StyleResolver layoutResolver;
            sui::LayoutInput input;
            input.resolver = &layoutResolver;
            input.viewportWidth = 400.0f;
            input.viewportHeight = 300.0f;
            sui::BlockLayout().layout(app.document().root(), input);

            check(app.activeCursor() == sui::ComputedStyle::CursorAuto,
                  "the default cursor is active before any pointer event");

            app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                         hit->layoutX() + 5.0f, hit->layoutY() + 5.0f, 0));
            check(app.activeCursor() == sui::ComputedStyle::CursorPointer,
                  "moving over a cursor:pointer element selects the pointer shape");

            app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                         miss->layoutX() + 5.0f, miss->layoutY() + 5.0f, 0));
            check(app.activeCursor() == sui::ComputedStyle::CursorAuto,
                  "moving off it returns to the default shape");
        }
    }

    // A `clip-path` masks hit-testing too: the pointer lands on whatever shows
    // through the clipped-away region instead of the clipped box.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<div id=\"clip\" style=\"width:100px;height:100px;cursor:pointer;"
            "clip-path:circle(50%)\"></div>"
            "</body></sui>", err);
        check(doc != nullptr, "clip hit document parsed");
        if (doc) {
            sui::Element* clip = doc->getElementById("clip");
            app.setDocument(std::move(doc));

            sui::StyleResolver layoutResolver;
            sui::LayoutInput input;
            input.resolver = &layoutResolver;
            input.viewportWidth = 400.0f;
            input.viewportHeight = 300.0f;
            sui::BlockLayout().layout(app.document().root(), input);

            // The 100px box sits at the body margin (8,8); its circle spans
            // the box, so the middle hits and a corner misses.
            app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                         clip->layoutX() + 50.0f,
                                         clip->layoutY() + 50.0f, 0));
            check(app.activeCursor() == sui::ComputedStyle::CursorPointer,
                  "the circle middle hits the clipped box");
            app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                         clip->layoutX() + 5.0f,
                                         clip->layoutY() + 5.0f, 0));
            check(app.activeCursor() == sui::ComputedStyle::CursorAuto,
                  "a clipped-away corner falls through to the body");
        }
    }

    // A `mask-image` fades paint but never hit-testing: a fully transparent
    // corner still lands on its box.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<div id=\"mask\" style=\"width:100px;height:100px;cursor:pointer;"
            "mask-image:linear-gradient(to right, black 50%, transparent 50%)\"></div>"
            "</body></sui>", err);
        check(doc != nullptr, "mask hit document parsed");
        if (doc) {
            sui::Element* mask = doc->getElementById("mask");
            app.setDocument(std::move(doc));

            sui::StyleResolver layoutResolver;
            sui::LayoutInput input;
            input.resolver = &layoutResolver;
            input.viewportWidth = 400.0f;
            input.viewportHeight = 300.0f;
            sui::BlockLayout().layout(app.document().root(), input);

            // The right half is fully transparent, yet it still hits.
            app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                         mask->layoutX() + mask->layoutWidth() - 5.0f,
                                         mask->layoutY() + 5.0f, 0));
            check(app.activeCursor() == sui::ComputedStyle::CursorPointer,
                  "a masked-away corner still hits its box");
        }
    }

    // One frame per window, each with its own skip state. The first frame paints;
    // an unchanged frame is correctly skipped; a second window paints on its own
    // stamp; a closed or unknown window paints nothing.
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "sui-frame-check";
        fs::remove_all(root);
        fs::create_directories(root);
        {
            std::ofstream out(root / "index.sui", std::ios::binary);
            out << "<sui><body><p id=\"where\">frame</p>"
                      "<input id=\"a\" value=\"\"/></body></sui>";
        }
        {
            std::ofstream out(root / "other.sui", std::ios::binary);
            out << "<sui><body><p id=\"where\">other</p></body></sui>";
        }
        const fs::path previous = fs::current_path();
        fs::current_path(root);
        struct RestoreFrame {
            fs::path previous;
            ~RestoreFrame() {
                std::error_code ec;
                fs::current_path(previous, ec);
            }
        } restoreFrame{previous};

        sui::Application app;
        std::string error;
        check(app.start("index.sui", error), "a headed project starts");
        if (app.platform()) {
            const sui::WindowId primary = app.platform()->primaryWindow();
            check(primary != sui::kInvalidWindow, "the started app has a primary window");
            check(app.renderFrame(primary), "the first frame paints");
            check(!app.renderFrame(primary), "an unchanged frame is skipped");

            // A second copy of the same page in its own window: each window
            // keeps its own skip state over its own page.
            const sui::WindowId other = app.openPage("index.sui", error);
            check(other != sui::kInvalidWindow && other != primary,
                  "a second headed window opens");
            check(app.platform()->windows().size() == 2, "two windows are open");
            check(app.renderFrame(other), "the second window paints on its own stamp");
            check(!app.renderFrame(other), "the second window skips when idle");
            check(!app.renderFrame(primary), "the primary stays quiet while idle");
            check(!app.renderFrame(sui::kInvalidWindow), "no window paints nothing");

            // Focus and typing stay in their own window: each window holds its
            // own elements, its own focus, and its own field values.
            sui::Element* inputA = app.pageDocument(primary)->getElementById("a");
            sui::Element* inputB = app.pageDocument(other)->getElementById("a");
            check(inputA && inputB && inputA != inputB, "each window owns its input");
            if (inputA && inputB) {
                sui::InputEvent pressA(sui::InputKind::PointerDown, inputA->layoutX() + 2.0f,
                                       inputA->layoutY() + 2.0f, 1, 0.0f, 0.0f, 0, {}, 0, 0,
                                       primary);
                app.dispatch(pressA);
                check(app.pageDocument(primary)->focused() == inputA,
                      "clicking focuses the primary window's field");
                check(app.pageDocument(other)->focused() == nullptr,
                      "the other window takes no focus from it");
                sui::InputEvent pressB(sui::InputKind::PointerDown, inputB->layoutX() + 2.0f,
                                       inputB->layoutY() + 2.0f, 1, 0.0f, 0.0f, 0, {}, 0, 0,
                                       other);
                app.dispatch(pressB);
                check(app.pageDocument(other)->focused() == inputB,
                      "clicking focuses the other window's field");
                check(app.pageDocument(primary)->focused() == inputA,
                      "the primary window keeps its own focus");
                sui::InputEvent type(sui::InputKind::TextInput, 0.0f, 0.0f, 0, 0.0f, 0.0f,
                                     0, "x", 0, 0, other);
                app.dispatch(type);
                check(inputB->value() == "x" && inputA->value().empty(),
                      "typing lands in the typed window's field only");
            }

            // A mutation repaints the window it is shown in.
            sui::Element* where = app.document().getElementById("where");
            check(where != nullptr, "the framed page is on screen");
            if (where) {
                where->setAttribute("style", "color:#ff0000");
                check(app.renderFrame(primary), "a mutated page repaints");
                check(!app.renderFrame(primary), "and then goes quiet again");
            }

            // Settle the other window after its focus and typing, so the
            // navigate below is the only thing that could wake it.
            check(app.renderFrame(other), "the other window repaints its focus");
            check(!app.renderFrame(other), "and then goes quiet");
            // Replacing the primary page leaves the other window alone: its
            // document and resolver never moved, so its next frame skips.
            check(app.navigate("other.sui"), "the primary window navigates away");
            check(app.currentPage() == "other.sui", "the primary page is the new one");
            check(app.renderFrame(primary), "the navigated window repaints");
            check(!app.renderFrame(other), "the untouched window stays quiet");

            // Closing a window drops its page with it.
            drain();
            SDL_Event close{};
            close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            close.window.windowID = SDL_GetWindowID(app.platform()->window(other));
            SDL_PushEvent(&close);
            check(app.platform()->pumpEvents(), "closing one window keeps the app up");
            check(!app.renderFrame(primary), "the surviving window stays quiet");
            check(app.pageDocument(primary) != nullptr &&
                      app.currentPage() == "other.sui",
                  "the surviving window keeps its page");
            check(app.pageDocument(other) == nullptr, "the closed window's page is gone");
            check(!app.renderFrame(other), "a closed window paints nothing");
        }
        fs::remove_all(root);
    }

    // Links navigate on press: plain same-window by default, a new window for
    // `target="_blank"`, nothing for opted-out, broken, external, or non-left
    // presses. Navigation runs before `onclick`, so a handler observes the page
    // it landed on.
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "sui-link-check";
        fs::remove_all(root);
        fs::create_directories(root / "pages");
        const auto write = [](const fs::path& at, const char* text) {
            std::ofstream out(at, std::ios::binary);
            out << text;
        };
        // Links are sized boxes: purely inline elements carry no hit-testable
        // box in this engine (the same reason `:hover` never fires on bare
        // inline text), so the press needs a box to land in.
        write(root / "index.sui",
              "<sui><body>"
              "<a id=\"self\" style=\"display:inline-block;width:80px;height:20px\" "
              "href=\"pages/settings.sui\" onclick=\"recordNavPage()\">go</a>"
              "<a id=\"blank\" style=\"display:inline-block;width:80px;height:20px\" "
              "href=\"pages/settings.sui\" target=\"_blank\">new</a>"
              "<a id=\"opt\" style=\"display:inline-block;width:80px;height:20px\" "
              "href=\"pages/settings.sui\" data-navigate=\"false\">stay</a>"
              "<a id=\"broken\" style=\"display:inline-block;width:80px;height:20px\" "
              "href=\"pages/missing.sui\">broken</a>"
              "<a id=\"ext\" style=\"display:inline-block;width:80px;height:20px\" "
              "href=\"https://example.com/\">out</a>"
              "<a id=\"mid\" style=\"display:inline-block;width:80px;height:20px\" "
              "href=\"pages/settings.sui\">middle</a>"
              "<a id=\"nest\" style=\"display:inline-block;width:80px;height:20px\" "
              "href=\"pages/settings.sui\">"
              "<b id=\"nestinner\" style=\"display:inline-block;width:40px;height:10px\">"
              "deep</b></a>"
              "</body></sui>");
        write(root / "pages" / "settings.sui",
              "<sui><body><p id=\"where\">settings</p></body></sui>");

        const fs::path previous = fs::current_path();
        fs::current_path(root);
        struct RestoreLink {
            fs::path previous;
            ~RestoreLink() {
                std::error_code ec;
                fs::current_path(previous, ec);
            }
        } restoreLink{previous};

        sui::Application app;
        std::string error;
        check(app.start("index.sui", error), "a linked project starts");
        g_navApp = &app;
        sui::script::registerHandler("recordNavPage", recordNavPage);
        const sui::WindowId primary = app.platform()->primaryWindow();
        check(app.renderFrame(primary), "the linked page frames");
        // Looks the target up fresh every time: a navigation replaces the
        // tree, so an element from before it would dangle.
        const auto press = [&](const char* id, int button, sui::WindowId window) {
            sui::Element* el = app.document().getElementById(id);
            if (!el) {
                check(false, "link target still on screen");
                return;
            }
            sui::InputEvent event(sui::InputKind::PointerDown, el->layoutX() + 2.0f,
                                  el->layoutY() + 2.0f, button, 0.0f, 0.0f, 0, {}, 0, 0,
                                  window);
            app.dispatch(event);
        };
        const auto home = [&]() {
            check(app.navigate("index.sui"), "back home");
            check(app.renderFrame(primary), "home reframed");
        };

        // A plain press replaces the window's page, and the handler already saw
        // the new one: navigation runs before `onclick`.
        press("self", 1, primary);
        check(app.currentPage() == "pages/settings.sui", "a press follows the link");
        check(g_navSeen == "pages/settings.sui", "the handler observes the new page");
        // The clicked element belongs to the tree that was just left: focus
        // must not stick to it once the new page is live.
        check(app.document().focused() == nullptr, "focus does not follow the old tree");
        home();

        // `_blank` opens instead of replacing.
        press("blank", 1, primary);
        check(app.platform()->windows().size() == 2, "blank opens a second window");
        check(app.currentPage() == "index.sui", "blank leaves the window's page");
        {
            sui::WindowId second = sui::kInvalidWindow;
            for (sui::WindowId id : app.platform()->windows()) {
                if (id != primary) {
                    second = id;
                }
            }
            sui::Element* where =
                second != sui::kInvalidWindow ? app.pageDocument(second)->getElementById("where")
                                             : nullptr;
            check(where != nullptr, "the new window shows the linked page");
        }

        // Everything else stays put.
        press("opt", 1, primary);
        check(app.currentPage() == "index.sui", "an opted-out link stays");
        press("broken", 1, primary);
        check(app.currentPage() == "index.sui" &&
                  app.document().getElementById("self") != nullptr,
              "a broken link keeps the live page");
        press("ext", 1, primary);
        check(app.currentPage() == "index.sui", "an external link stays");
        press("mid", 2, primary);
        check(app.currentPage() == "index.sui", "a non-left press stays");
        press("nestinner", 1, primary);
        check(app.currentPage() == "pages/settings.sui", "a nested press follows out");
        g_navApp = nullptr;
        fs::remove_all(root);
    }

    // Timers belong to the page that scheduled them: they fire in that page's
    // frame and never in another's. Invalidating one page never repaints
    // another, navigating retires a page's timers, and `closePage` closes the
    // caller's window.
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "sui-timer-check";
        fs::remove_all(root);
        fs::create_directories(root);
        const auto write = [](const fs::path& at, const char* text) {
            std::ofstream out(at, std::ios::binary);
            out << text;
        };
        write(root / "index.sui",
              "<sui><body><input id=\"a\" value=\"\"/></body></sui>");
        write(root / "other.sui", "<sui><body><p id=\"where\">other</p></body></sui>");

        const fs::path previous = fs::current_path();
        fs::current_path(root);
        struct RestoreTimer {
            fs::path previous;
            ~RestoreTimer() {
                std::error_code ec;
                fs::current_path(previous, ec);
            }
        } restoreTimer{previous};

        sui::Application app;
        std::string error;
        check(app.start("index.sui", error), "a timed project starts");
        const sui::WindowId primary = app.platform()->primaryWindow();
        check(app.renderFrame(primary), "the timed page frames");
        std::string timerError;
        const sui::WindowId other = app.openPage("index.sui", timerError);
        check(other != sui::kInvalidWindow, "a second timed window opens");
        check(app.renderFrame(other), "the second timed page frames");

        sui::Element* inputA = app.pageDocument(primary)->getElementById("a");
        sui::Element* inputB = app.pageDocument(other)->getElementById("a");
        check(inputA && inputB && inputA != inputB, "each timed window owns its field");
        if (!inputA || !inputB) {
            fs::remove_all(root);
        } else {
            // Arm a timer from the primary page: focusing binds it, and the
            // schedule inherits the binding.
            sui::InputEvent press(sui::InputKind::PointerDown, inputA->layoutX() + 2.0f,
                                  inputA->layoutY() + 2.0f, 1, 0.0f, 0.0f, 0, {}, 0, 0,
                                  primary);
            app.dispatch(press);
            g_timerTarget = inputA;
            g_timerFired = false;
            check(sui::script::setTimeout(fireTimerProbe, 0) != 0, "a timer arms");

            // The other page's frame leaves it alone.
            check(!app.renderFrame(other), "the other page skips while idle");
            check(!g_timerFired, "another page's frame fires nothing");
            // Its own page's frame fires it, into its own field.
            check(app.renderFrame(primary), "the owning page picks up its timer");
            check(g_timerFired, "the owning page fires its timer");
            check(inputA->value() == "timer" && inputB->value().empty(),
                  "a timer writes into its own page only");

            // Invalidating one page never repaints another.
            ::document.invalidate();
            check(!app.renderFrame(other), "an invalidation stays on its page");
            check(app.renderFrame(primary), "the invalidated page repaints");

            // Navigating retires the page's timers.
            g_timerTarget = nullptr;
            g_timerFired = false;
            check(sui::script::setTimeout(fireTimerProbe, 0) != 0, "a second timer arms");
            check(app.navigate("other.sui"), "the primary page navigates away");
            check(app.renderFrame(primary), "the navigated page paints");
            check(!g_timerFired, "navigation retires the old page's timers");

            // `closePage` closes the caller's window: bind the other one first.
            // Both pages are settled, so both frames below correctly skip.
            check(!app.renderFrame(other), "the other page stays quiet");
            check(app.closePage(), "the bound page's window closes");
            check(!app.platform()->hasWindow(other), "the other window is gone");
            check(!app.renderFrame(primary), "the primary stays quiet");
            check(app.pageDocument(other) == nullptr, "the closed page dropped");
            g_timerTarget = nullptr;
        }
        fs::remove_all(root);
    }

    // A summary toggles its <details> open on click, and back closed on the
    // next one. The tree re-lays itself out around the change.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<details id=\"d\"><summary id=\"s\">more</summary><div id=\"x\">hidden</div></details>"
            "</body></sui>",
            err);
        check(doc != nullptr, "details document parsed");
        if (doc) {
            app.setDocument(std::move(doc));

            const auto layout = [&]() {
                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);
            };
            const auto clickSummary = [&]() {
                sui::Element* s = app.document().getElementById("s");
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown,
                                             s->layoutX() + s->layoutWidth() * 0.5f,
                                             s->layoutY() + s->layoutHeight() * 0.5f, 1));
            };
            layout();
            sui::Element* x = app.document().getElementById("x");
            check(x && x->textRuns().empty(), "closed content lays out nothing");
            clickSummary();
            check(app.document().getElementById("d")->hasAttribute("open"),
                  "a press opens the details");
            layout();
            check(!app.document().getElementById("x")->textRuns().empty(),
                  "open content lays out");
            clickSummary();
            check(!app.document().getElementById("d")->hasAttribute("open"),
                  "a second press closes it again");
        }
    }

    // A drag from a textarea's grip corner writes inline dimensions off the
    // grab: both axes grow, and the tree re-lays itself out around them.
    // A horizontal-only box answers in width alone.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<textarea id=\"t\"></textarea>"
            "<div id=\"h\" style=\"resize:horizontal;overflow:auto;width:100px;height:40px\"></div>"
            "</body></sui>",
            err);
        check(doc != nullptr, "resize document parsed");
        if (doc) {
            app.setDocument(std::move(doc));

            const auto layout = [&]() {
                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);
            };
            const auto gripOf = [&](sui::Element* el) {
                return std::make_pair(el->layoutX() + el->layoutWidth() - 6.0f,
                                      el->layoutY() + el->layoutHeight() - 6.0f);
            };
            const auto drag = [&](sui::Element* el, float dx, float dy) {
                const auto [gx, gy] = gripOf(el);
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown, gx, gy, 1));
                app.dispatch(
                    sui::InputEvent(sui::InputKind::PointerMove, gx + dx, gy + dy, 1));
                app.dispatch(
                    sui::InputEvent(sui::InputKind::PointerUp, gx + dx, gy + dy, 0));
            };
            layout();
            sui::Element* t = app.document().getElementById("t");
            sui::Element* h = app.document().getElementById("h");
            check(t && h, "resize elements found");
            if (t && h) {
                const float wasW = t->layoutWidth();
                const float wasH = t->layoutHeight();
                const float wasCW = t->contentWidth();
                const float wasCH = t->contentHeight();
                const float wasHW = h->layoutWidth();
                const float wasHH = h->layoutHeight();
                drag(t, 30.0f, 20.0f);
                check(t->style().getPropertyValue("width") ==
                          std::to_string(wasCW + 30.0f) + "px",
                      "a grip drag writes the grabbed width");
                check(t->style().getPropertyValue("height") ==
                          std::to_string(wasCH + 20.0f) + "px",
                      "and the grabbed height");
                layout();
                check(t->layoutWidth() - wasW > 29.0f &&
                          t->layoutWidth() - wasW < 31.0f,
                      "the box grows wider by the drag");
                check(t->layoutHeight() - wasH > 19.0f &&
                          t->layoutHeight() - wasH < 21.0f,
                      "and taller by the drag");
                drag(h, 30.0f, 20.0f);
                layout();
                check(h->layoutWidth() - wasHW > 29.0f &&
                          h->layoutWidth() - wasHW < 31.0f,
                      "a horizontal box grows wider");
                check(std::fabs(h->layoutHeight() - wasHH) < 0.01f,
                      "but never taller");
                // Hovering the grip reports se-resize.
                {
                    const auto [gx, gy] = gripOf(t);
                    app.dispatch(
                        sui::InputEvent(sui::InputKind::PointerMove, gx, gy, 0));
                    check(app.activeCursor() ==
                              sui::ComputedStyle::CursorSeResize,
                          "a grip hover reports se-resize");
                }
            }
        }
    }

    // A non-modal dialog opens in place, stays open under Escape (which only
    // dismisses modal dialogs), and closes on request.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body><dialog id=\"d\">hi</dialog></body></sui>", err);
        check(doc != nullptr, "dialog document parsed");
        if (doc) {
            app.setDocument(std::move(doc));
            sui::Element* d = app.document().getElementById("d");
            check(d && !d->dialogOpen(), "a dialog starts shut");
            if (d) {
                d->show();
                check(d->dialogOpen(), "show opens it");
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0,
                                             0, 0x1B));
                check(d->dialogOpen(), "escape leaves a modeless dialog open");
                d->close();
                check(!d->dialogOpen(), "close shuts it");
            }
        }
    }

    // A modal dialog takes focus, traps Tab, swallows outside presses, fires
    // cancel then close on Escape, and hands focus back afterwards.
    {
        sui::Application app;
        sui::ParseError err;
        auto doc = sui::MarkupParser::parse(
            "<sui><body>"
            "<input id=\"before\" value=\"\"/>"
            "<dialog id=\"m\" oncancel=\"flagCancel()\" onclose=\"flagShut()\">"
            "<button id=\"ok\">ok</button><button id=\"away\">away</button>"
            "</dialog>"
            "</body></sui>",
            err);
        check(doc != nullptr, "modal document parsed");
        if (doc) {
            app.setDocument(std::move(doc));
            const auto layout = [&]() {
                sui::StyleResolver layoutResolver;
                sui::LayoutInput input;
                input.resolver = &layoutResolver;
                input.viewportWidth = 400.0f;
                input.viewportHeight = 300.0f;
                sui::BlockLayout().layout(app.document().root(), input);
            };
            sui::script::registerHandler("flagCancel", flagModalCancel);
            sui::script::registerHandler("flagShut", flagModalShut);
            sui::Element* m = app.document().getElementById("m");
            sui::Element* before = app.document().getElementById("before");
            sui::Element* ok = app.document().getElementById("ok");
            check(m && before && ok, "modal elements found");
            if (m && before && ok) {
                layout();
                // Focusing the field first gives the modal a place to return to.
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown,
                                             before->layoutX() + 2.0f,
                                             before->layoutY() + 2.0f, 1));
                check(app.document().focused() == before, "the field takes focus");
                m->showModal();
                layout();
                sui::BlockLayout::centerInViewport(*m, 400.0f, 300.0f);
                app.dispatch(sui::InputEvent(sui::InputKind::PointerMove,
                                             m->layoutX() + 2.0f, m->layoutY() + 2.0f,
                                             0));
                check(app.document().focused() == m, "showModal focuses the dialog");
                // An outside press changes nothing: still open, focus unmoved.
                app.dispatch(sui::InputEvent(sui::InputKind::PointerDown, 2.0f,
                                             2.0f, 1));
                check(m->dialogOpen(), "an outside press never closes");
                check(app.document().focused() == m, "nor steals focus");
                // Tab cycles the dialog's own buttons, wrapping at either end.
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0,
                                             0, 0x09));
                check(app.document().focused() == ok, "tab enters the dialog");
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0,
                                             0, 0x09));
                check(app.document().focused() != before, "tab never leaves");
                // Escape fires cancel then close, and hands focus back.
                g_modalCancel = false;
                g_modalShut = false;
                app.dispatch(sui::InputEvent(sui::InputKind::KeyDown, 0, 0, 0, 0,
                                             0, 0x1B));
                check(g_modalCancel && g_modalShut, "escape fires cancel then close");
                check(!m->dialogOpen(), "escape shuts the dialog");
                check(app.document().focused() == before, "focus returns outside");
            }
        }
    }

    // An `<iframe>` brings a document of its own: loaded through the same
    // resources, styled by its own stylesheets, laid out inside the frame
    // element's content box, and untouched by the host page's rules.
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "sui-iframe-check";
        fs::remove_all(root);
        fs::create_directories(root);
        {
            std::ofstream out(root / "index.sui", std::ios::binary);
            out << "<sui><head><link rel=\"stylesheet\" href=\"/page.css\"/></head><body>"
                   "<iframe id=\"f\" src=\"/frame.sui\"></iframe>"
                   "<iframe id=\"auto\" src=\"/frame.sui\"></iframe>"
                   "<iframe id=\"gone\" src=\"/missing.sui\"></iframe>"
                   "<div id=\"box\">host</div>"
                   "</body></sui>";
        }
        {
            std::ofstream out(root / "page.css", std::ios::binary);
            out << "body { background: #00ff00; } #f { display: block; margin: 0; border: 0; "
                   "padding: 0; width: 200px; "
                   "height: 100px; } #auto { margin: 0; } #box { width: 999px; "
                   "background: #0000ff; }";
        }
        {
            std::ofstream out(root / "frame.sui", std::ios::binary);
            out << "<sui><head><link rel=\"stylesheet\" href=\"/frame.css\"/></head>"
                   "<body><div id=\"box\">frame</div><div id=\"wide\"></div></body></sui>";
        }
        {
            std::ofstream out(root / "frame.css", std::ios::binary);
            out << "body { margin: 0; } #box { width: 50px; height: 20px; margin-left: 7px; "
                   "background: #ff0000; } #wide { width: 400px; height: 10px; "
                   "background: #0000ff; }";
        }
        const fs::path previous = fs::current_path();
        struct RestoreIframe {
            fs::path previous;
            ~RestoreIframe() {
                std::error_code ec;
                fs::current_path(previous, ec);
            }
        } restoreIframe{previous};
        fs::current_path(root);

        sui::Application app;
        std::string error;
        check(app.start("index.sui", error), "an iframe project starts");
        if (app.platform()) {
            const sui::WindowId primary = app.platform()->primaryWindow();
            check(app.renderFrame(primary), "the iframe page lays out");
            sui::Element* frameEl = app.document().getElementById("f");
            sui::Element* autoEl = app.document().getElementById("auto");
            sui::Element* goneEl = app.document().getElementById("gone");
            check(frameEl != nullptr && autoEl != nullptr && goneEl != nullptr,
                  "the frame elements exist");
            if (frameEl && autoEl && goneEl) {
                sui::Document* frame = app.frameDocument(*frameEl);
                check(frame != nullptr, "the frame document loads");
                check(app.frameDocument(*goneEl) == nullptr,
                      "a frame whose source is missing stays unloaded");
                check(autoEl->layoutWidth() == 300.0f && autoEl->layoutHeight() == 150.0f,
                      "an unsized frame keeps the replaced default size");
                if (frame) {
                    sui::Element* frameBox = frame->getElementById("box");
                    sui::Element* pageBox = app.document().getElementById("box");
                    check(frameBox != nullptr && pageBox != nullptr,
                          "both #box elements exist");
                    if (frameBox && pageBox) {
                        check(pageBox->layoutWidth() == 999.0f,
                              "the host page keeps its own #box rule");
                        check(frameBox->layoutWidth() == 50.0f &&
                                  frameBox->layoutHeight() == 20.0f,
                              "the frame lays out under its own stylesheet");
                        check(std::fabs(frameBox->layoutX() - frameEl->contentX() - 7.0f) <
                                  0.01f &&
                                  std::fabs(frameBox->layoutY() - frameEl->contentY()) < 0.01f,
                              "the frame lands inside the element's content box");
                    }
                    // Pixel proof: the dummy driver keeps the renderer's buffer
                    // readable, so what the frame painted can be inspected.
                    const auto readPixel = [&](float x, float y, int& r, int& g,
                                               int& b) -> bool {
                        SDL_Renderer* renderer = app.platform()->renderer(primary);
                        if (!renderer) {
                            return false;
                        }
                        const SDL_Rect rect{static_cast<int>(x), static_cast<int>(y), 1, 1};
                        SDL_Surface* read = SDL_RenderReadPixels(renderer, &rect);
                        if (!read) {
                            return false;
                        }
                        SDL_Surface* conv =
                            SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
                        SDL_DestroySurface(read);
                        if (!conv) {
                            return false;
                        }
                        const Uint8* px = static_cast<const Uint8*>(conv->pixels);
                        r = px[0];
                        g = px[1];
                        b = px[2];
                        SDL_DestroySurface(conv);
                        return true;
                    };
                    const float fx = frameEl->contentX();
                    const float fy = frameEl->contentY();
                    int r = 0, g = 0, b = 0;
                    const bool readable = readPixel(fx + 17.0f, fy + 10.0f, r, g, b);
                    check(readable, "the frame buffer reads back");
                    if (readable) {
                        // `#box` is the frame's own red rule; the host's blue
                        // `#box` rule must never leak into the frame's paint.
                        check(r > 200 && g < 60 && b < 60,
                              "the frame paints its own element styles");
                    }
                    if (readPixel(fx + 100.0f, fy + 60.0f, r, g, b)) {
                        check(g > 200 && r < 60 && b < 60,
                              "the frame shows the host through its transparent body");
                    } else {
                        check(false, "the frame buffer reads back");
                    }
                    // `#wide` reaches past the viewport on purpose: it must be
                    // cut at the content box, not spill across the host page.
                    if (readPixel(fx + 210.0f, fy + 25.0f, r, g, b)) {
                        check(g > 200 && r < 60 && b < 60,
                              "the frame's content is clipped to the content box");
                    } else {
                        check(false, "the frame buffer reads back");
                    }
                }
            }
        }
    }

    platform->shutdown();

    std::printf("input_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
// What the platform turns raw SDL mouse events into. The scripted SUI_MOVE hook
// feeds the engine a finished event, so it cannot catch a mapping mistake here --
// and a motion that is dropped outright leaves `:hover` silently dead, with no other
// test failing. Events are pushed onto SDL's own queue so the real pump runs.
#include "Platform/Platform.h"
#include "Core/Application/Application.h"
#include "Layout/BlockLayout.h"
#include "SUI/DOM/Document.h"
#include "SUI/DOM/Element.h"
#include "SUI/Markup/Parser.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>

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

void pushMotion(int x, int y, Uint32 buttons) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.state = buttons;
    SDL_PushEvent(&event);
}

void pushLeave() {
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
    SDL_PushEvent(&event);
}

void drain() {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
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

    // A plain hover, with nothing held: this is the case that used to be dropped, and
    // dropping it is what made `:hover` never fire.
    pushMotion(120, 80, 0);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerMove, 120.0f, 80.0f, 0,
              "a motion with no button held reports a move");

    // The same motion mid-drag. `button` is what separates the two, because `:active`
    // follows the cursor during a drag but must not stick without a press.
    pushMotion(121, 81, SDL_BUTTON_LMASK);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerMove, 121.0f, 81.0f, SDL_BUTTON_LEFT,
              "a left drag reports a move carrying the held button");

    pushMotion(122, 82, SDL_BUTTON_RMASK);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerMove, 122.0f, 82.0f, SDL_BUTTON_RIGHT,
              "a right drag reports the right button");

    pushMotion(123, 83, SDL_BUTTON_MMASK);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerMove, 123.0f, 83.0f, SDL_BUTTON_MIDDLE,
              "a middle drag reports the middle button");

    // Leaving the window must clear the hover rather than freeze it on whatever was
    // under the cursor. Off-screen is how the engine is told nothing is hovered.
    pushLeave();
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
    down.button.x = 50;
    down.button.y = 60;
    down.button.button = SDL_BUTTON_LEFT;
    SDL_PushEvent(&down);
    platform->pumpEvents();
    expectOne(*platform, sui::InputKind::PointerDown, 50.0f, 60.0f, SDL_BUTTON_LEFT,
              "a press is still a press");

    SDL_Event up{};
    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
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

    platform->shutdown();

    std::printf("input_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
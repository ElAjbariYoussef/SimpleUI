// What the platform turns raw SDL mouse events into. The scripted SUI_MOVE hook
// feeds the engine a finished event, so it cannot catch a mapping mistake here --
// and a motion that is dropped outright leaves `:hover` silently dead, with no other
// test failing. Events are pushed onto SDL's own queue so the real pump runs.
#include "Platform/Platform.h"

#include <SDL3/SDL.h>

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

    platform->shutdown();

    std::printf("input_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
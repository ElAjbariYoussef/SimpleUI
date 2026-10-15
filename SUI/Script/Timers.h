// Timers and the animation-frame hook a script drives animation with.
//
// setTimeout / setInterval take a plain `void (*)()` — the same shape as every
// script entry point — and hand back an id that clearTimeout / clearInterval
// cancel. requestAnimationFrame fires its callback on the next pumped frame,
// before layout and paint, which is the hook an animation loop re-arms each
// frame. The event loop pumps all of them through pumpTimers(); a page that
// never schedules anything costs nothing, and the two queues share one id space.
#pragma once

#include <cstdint>

namespace sui {
namespace script {

using TimerCallback = void (*)();

// One-shot after delayMs. Returns 0 for a null callback, otherwise a unique id.
std::uint64_t setTimeout(TimerCallback callback, std::uint64_t delayMs);

// Repeating every periodMs. The first fire is one period after the call.
std::uint64_t setInterval(TimerCallback callback, std::uint64_t periodMs);

void clearTimeout(std::uint64_t id);
void clearInterval(std::uint64_t id);

// Fires the callback on the next pumpTimers() call, once, before repaint.
std::uint64_t requestAnimationFrame(TimerCallback callback);
void cancelAnimationFrame(std::uint64_t id);

// Milliseconds on a monotonic clock, the way performance.now() reports them.
double now() noexcept;

// Milliseconds on the wall clock since the Unix epoch, the way Date.now() does.
double unixTimeMs() noexcept;

// Runs everything due: expired timeouts and intervals in deadline order, then
// the rAF callbacks scheduled for this frame. Returns true when anything ran,
// so the caller can treat this frame as stale. A callback may schedule more
// work and it is picked up here too; the guard keeps a self-re-arming interval
// from looping inside one pump forever.
bool pumpTimers();

// Drops every timer and frame callback a dead page owned, so a closed page's
// work never fires anywhere. The application calls this when it drops a page.
void cancelForPage(const void* page);

// Web-named readable clocks.
struct PerformanceApi {
    double now() const noexcept { return sui::script::now(); }
};
struct DateApi {
    static double now() noexcept { return sui::script::unixTimeMs(); }
};

}  // namespace script
}  // namespace sui
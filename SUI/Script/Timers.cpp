#include "SUI/Script/Timers.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace sui {
namespace script {
namespace {

double monotonicMs() noexcept {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(t).count();
}

struct Entry {
    std::uint64_t id = 0;
    TimerCallback callback = nullptr;
    double deadlineMs = 0.0;  // monotonic time of the next fire
    double periodMs = 0.0;    // 0 = one-shot
    bool cancelled = false;
};

struct Scheduler {
    std::vector<Entry> timeouts;  // sorted by deadline, earliest first
    std::vector<Entry> raf;
    std::uint64_t nextId = 1;

    void insertTimeout(Entry entry) {
        const auto at = std::upper_bound(
            timeouts.begin(), timeouts.end(), entry.deadlineMs,
            [](double deadline, const Entry& existing) {
                return deadline < existing.deadlineMs;
            });
        timeouts.insert(at, std::move(entry));
    }

    void cancel(std::uint64_t id) {
        for (Entry& entry : timeouts) {
            if (entry.id == id) {
                entry.cancelled = true;
                return;
            }
        }
        for (Entry& entry : raf) {
            if (entry.id == id) {
                entry.cancelled = true;
                return;
            }
        }
    }
};

Scheduler& scheduler() {
    // Function-local so scripts running from static initializers before main()
    // cannot schedule before this object exists.
    static Scheduler instance;
    return instance;
}

}  // namespace

std::uint64_t setTimeout(TimerCallback callback, std::uint64_t delayMs) {
    if (!callback) {
        return 0;
    }
    Scheduler& s = scheduler();
    Entry entry;
    entry.id = s.nextId++;
    entry.callback = callback;
    entry.deadlineMs = monotonicMs() + static_cast<double>(delayMs);
    const std::uint64_t id = entry.id;
    s.insertTimeout(std::move(entry));
    return id;
}

std::uint64_t setInterval(TimerCallback callback, std::uint64_t periodMs) {
    if (!callback) {
        return 0;
    }
    Scheduler& s = scheduler();
    Entry entry;
    entry.id = s.nextId++;
    entry.callback = callback;
    entry.periodMs = static_cast<double>(periodMs);
    entry.deadlineMs = monotonicMs() + static_cast<double>(periodMs);
    const std::uint64_t id = entry.id;
    s.insertTimeout(std::move(entry));
    return id;
}

void clearTimeout(std::uint64_t id) {
    scheduler().cancel(id);
}

// One id space: clearing an interval by its id is indistinguishable from
// clearing a timeout, so both spellings route to the same cancel.
void clearInterval(std::uint64_t id) {
    scheduler().cancel(id);
}

std::uint64_t requestAnimationFrame(TimerCallback callback) {
    if (!callback) {
        return 0;
    }
    Scheduler& s = scheduler();
    Entry entry;
    entry.id = s.nextId++;
    entry.callback = callback;
    const std::uint64_t id = entry.id;
    s.raf.push_back(std::move(entry));
    return id;
}

void cancelAnimationFrame(std::uint64_t id) {
    scheduler().cancel(id);
}

double now() noexcept {
    return monotonicMs();
}

double unixTimeMs() noexcept {
    const auto t = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(t).count();
}

bool pumpTimers() {
    Scheduler& s = scheduler();
    bool fired = false;

    // Timeouts first, earliest deadline first, until none are due. A callback
    // that schedules more work is picked up here too; the guard keeps a
    // self-re-arming interval from looping inside one pump forever.
    for (int guard = 0; guard < 10000; ++guard) {
        if (s.timeouts.empty()) {
            break;
        }
        const double ms = monotonicMs();
        if (s.timeouts.front().deadlineMs > ms) {
            break;
        }
        Entry entry = std::move(s.timeouts.front());
        s.timeouts.erase(s.timeouts.begin());
        if (entry.cancelled) {
            continue;
        }
        if (entry.periodMs > 0.0) {
            // Jump to the next period boundary that is still in the future: a
            // long gap catches up to one fire instead of running the interval
            // once for every missed tick.
            const double lag = ms - entry.deadlineMs;
            const std::uint64_t periods =
                static_cast<std::uint64_t>(lag / entry.periodMs) + 1;
            entry.deadlineMs += entry.periodMs * static_cast<double>(periods);
            s.insertTimeout(std::move(entry));
        }
        entry.callback();
        fired = true;
    }

    // rAF runs once per pump, before the caller repaints. A callback that
    // re-arms itself lands in the fresh queue and takes its turn next frame.
    if (!s.raf.empty()) {
        std::vector<Entry> due = std::move(s.raf);
        s.raf.clear();
        s.raf.reserve(due.size());
        for (const Entry& entry : due) {
            if (entry.cancelled) {
                continue;
            }
            entry.callback();
            fired = true;
        }
    }
    return fired;
}

}  // namespace script
}  // namespace sui
#include "SUI/Script/Timers.h"

#include "SUI/Script/Script.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace sui {
namespace script {
namespace {

// The page scheduling right now, or null with no page in context. Every entry
// remembers its page so pumping one page never fires another's timers.
const void* schedulingPage() noexcept {
    const Context* context = sui::script::context();
    return context ? context->page : nullptr;
}

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
    // The page that scheduled this entry, as an opaque token. Entries remember
    // the page whose callback scheduled them and only fire while that page is
    // the one being pumped, so a background page never acts on the page in
    // front. Null when scheduled with no page in context; those fire wherever
    // no page is active, which is the headless and test case.
    const void* owner = nullptr;
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
    entry.owner = schedulingPage();
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
    entry.owner = schedulingPage();
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
    entry.owner = schedulingPage();
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

// Drops every entry a dead page owned. The application calls this when it
// drops the page, so a closed page's timers never fire anywhere.
void cancelForPage(const void* page) {
    if (!page) {
        return;
    }
    Scheduler& s = scheduler();
    for (Entry& entry : s.timeouts) {
        if (entry.owner == page) {
            entry.cancelled = true;
        }
    }
    for (Entry& entry : s.raf) {
        if (entry.owner == page) {
            entry.cancelled = true;
        }
    }
}

bool pumpTimers() {
    Scheduler& s = scheduler();
    bool fired = false;
    // Only this page's entries fire: the pump runs inside one page's frame,
    // and an entry for another page waits for its own. Unowned entries (no
    // page in context when scheduled) fire wherever none is active.
    const void* active = schedulingPage();

    // Timeouts first, earliest deadline first, until none are due. A callback
    // that schedules more work is picked up here too; the guard keeps a
    // self-re-arming interval from looping inside one pump forever. Entries for
    // another page are left in line for its own pump.
    for (int guard = 0; guard < 10000; ++guard) {
        const double ms = monotonicMs();
        std::size_t at = s.timeouts.size();
        for (std::size_t i = 0; i < s.timeouts.size(); ++i) {
            const Entry& candidate = s.timeouts[i];
            if (candidate.deadlineMs > ms) {
                break;
            }
            if (candidate.cancelled || candidate.owner == active) {
                at = i;
                break;
            }
        }
        if (at == s.timeouts.size()) {
            break;
        }
        Entry entry = std::move(s.timeouts[at]);
        s.timeouts.erase(s.timeouts.begin() + static_cast<std::ptrdiff_t>(at));
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
            if (entry.owner != active) {
                // Another page's frame: hold it for its own pump.
                s.raf.push_back(entry);
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
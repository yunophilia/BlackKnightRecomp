#include "scheduler.h"
#include "threads.h"
#include "../cpu/cpu.h"
#include "../log.h"
#include "../platform/platform.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

struct EventType
{
    const char* name;
    SchedCallback cb;
};

struct Pending
{
    int event;
    uint64_t tb;
    uint64_t userdata;
};

static std::vector<EventType> s_types;
static std::vector<Pending> s_pending;      // guest-thread only
static std::atomic<uint64_t> s_nextDeadline{ ~0ull };
static PlatformEvent* s_timerWake = nullptr;
static PlatformEvent* s_idleWake = nullptr;

int Sched_RegisterEvent(const char* name, SchedCallback cb)
{
    s_types.push_back({ name, cb });
    return (int)s_types.size() - 1;
}

static void Recompute()
{
    uint64_t next = ~0ull;
    for (auto& p : s_pending)
        if (p.tb < next) next = p.tb;
    bool armed;
    uint64_t dec = Dec_Deadline(&armed);
    if (armed && dec < next) next = dec;
    uint64_t old = s_nextDeadline.exchange(next);
    if (next < old && s_timerWake)
        s_timerWake->Set();
}

void Sched_ScheduleAt(int event, uint64_t tb, uint64_t userdata)
{
    s_pending.push_back({ event, tb, userdata });
    Recompute();
}

void Sched_ScheduleIn(int event, uint64_t ticks, uint64_t userdata)
{
    Sched_ScheduleAt(event, Timebase_Now() + ticks, userdata);
}

void Sched_Cancel(int event)
{
    for (size_t i = 0; i < s_pending.size();)
    {
        if (s_pending[i].event == event)
            s_pending.erase(s_pending.begin() + i);
        else
            i++;
    }
    Recompute();
}

// Hooks run on the game thread before due events (e.g. completions from I/O threads).
static std::vector<void (*)()> s_pollHooks;
void Sched_AddPollHook(void (*fn)()) { s_pollHooks.push_back(fn); }

void Sched_RunDue()
{
    for (auto fn : s_pollHooks) fn();
    uint64_t now = Timebase_Now();
    for (;;)
    {
        int best = -1;
        for (size_t i = 0; i < s_pending.size(); i++)
            if (s_pending[i].tb <= now && (best < 0 || s_pending[i].tb < s_pending[best].tb))
                best = (int)i;
        if (best < 0)
            break;
        Pending p = s_pending[best];
        s_pending.erase(s_pending.begin() + best);
        s_types[p.event].cb(p.userdata, (int64_t)(now - p.tb));
    }
    Recompute();
}

void Sched_Kick()
{
    Recompute();
}

void Sched_WakeIdle()
{
    if (s_idleWake)
        s_idleWake->Set();
}

void Sched_IdleWait()
{
    // Run anything already due; otherwise sleep until the timer thread or a
    // hardware completion signals us.
    for (int spins = 0; spins < 1000; spins++)
    {
        Sched_RunDue();
        Irq_Update();
        if (g_ppcIrqPending)
            return;
        s_idleWake->Wait(2);
    }
}

// Late events made the game's audio replay stale buffers (stutter) and could delay VI
// interrupts, so the waits are precise (platform: a high-resolution timer).
static void WaitTicks(uint64_t ticks)
{
    Platform_TimerWait(*s_timerWake, ticks * 10000000ull / TB_FREQ);   // 100 ns units
}

static void TimerThread()
{
    Platform_TimerThreadSetup();
    for (;;)
    {
        uint64_t next = s_nextDeadline.load();
        uint64_t now = Timebase_Now();
        if (next <= now)
        {
            // BKR_SLOW_SAMPLE diagnostics: the game thread has not taken a due event for >3 ms
            static const bool sample = getenv("BKR_SLOW_SAMPLE") != nullptr;
            if (sample)
            {
                static uint64_t sampledFor = 0;
                if (now - next > TB_FREQ * 3 / 1000 && sampledFor != next)
                {
                    sampledFor = next;
                    void Crash_SampleSlowGuest();
                    Crash_SampleSlowGuest();
                }
            }
            g_ppcIrqPending = 1;
            s_idleWake->Set();
            // wait for the guest to consume it (or a new earlier deadline)
            WaitTicks(TB_FREQ / 2000);   // 0.5 ms
            continue;
        }
        WaitTicks(next - now);
    }
}

void Sched_Init()
{
    s_timerWake = new PlatformEvent();
    s_idleWake = new PlatformEvent();
    std::thread(TimerThread).detach();
}

#include "scheduler.h"
#include "threads.h"
#include "../cpu/cpu.h"
#include "../log.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <timeapi.h>

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
static HANDLE s_timerWake = nullptr;
static HANDLE s_idleWake = nullptr;

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
        SetEvent(s_timerWake);
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
        SetEvent(s_idleWake);
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
        WaitForSingleObject(s_idleWake, 2);
    }
}

// Waits use a high-resolution waitable timer: plain Sleep/WaitForSingleObject follow
// the system tick, which Windows 11 keeps at ~15.6 ms for minimized or throttled
// processes regardless of timeBeginPeriod. Late events made the game's audio replay
// stale buffers (stutter) and could delay VI interrupts.
static HANDLE s_hrTimer = nullptr;

static void WaitTicks(uint64_t ticks)
{
    if (s_hrTimer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(ticks * 10000000ull / TB_FREQ);   // relative, 100 ns units
        if (due.QuadPart > -1) due.QuadPart = -1;
        SetWaitableTimerEx(s_hrTimer, &due, 0, nullptr, nullptr, nullptr, 0);
        HANDLE hs[2] = { s_timerWake, s_hrTimer };
        WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        CancelWaitableTimer(s_hrTimer);
    }
    else
        WaitForSingleObject(s_timerWake, (DWORD)std::max<uint64_t>(1, ticks * 1000 / TB_FREQ));
}

static void TimerThread()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    s_hrTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s_hrTimer) LOG_WARN("scheduler: no high-resolution timer, timing may be coarse");
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
            SetEvent(s_idleWake);
            // wait for the guest to consume it (or a new earlier deadline)
            WaitTicks(TB_FREQ / 2000);   // 0.5 ms
            continue;
        }
        WaitTicks(next - now);
    }
}

void Sched_Init()
{
    s_timerWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_idleWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(TimerThread).detach();
}

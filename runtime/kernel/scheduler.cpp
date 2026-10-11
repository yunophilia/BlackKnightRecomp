#include "scheduler.h"
#include "threads.h"
#include "../cpu/cpu.h"
#include "../log.h"

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

void Sched_RunDue()
{
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

static void TimerThread()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    for (;;)
    {
        uint64_t next = s_nextDeadline.load();
        uint64_t now = Timebase_Now();
        if (next <= now)
        {
            g_ppcIrqPending = 1;
            SetEvent(s_idleWake);
            // wait for the guest to consume (or a new earlier deadline)
            WaitForSingleObject(s_timerWake, 1);
            continue;
        }
        uint64_t ticks = next - now;
        double ms = (double)ticks * 1000.0 / (double)TB_FREQ;
        if (ms > 2.0)
            WaitForSingleObject(s_timerWake, (DWORD)(ms - 1.0));
        else
            YieldProcessor(), Sleep(0);
    }
}

void Sched_Init()
{
    s_timerWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_idleWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(TimerThread).detach();
}

#pragma once
#include <cstdint>

// Timed hardware events. Callbacks always run on the guest thread (from
// ppc_check_irq / idle wait), never on the timer thread.
typedef void (*SchedCallback)(uint64_t userdata, int64_t lateTicks);

int Sched_RegisterEvent(const char* name, SchedCallback cb);
void Sched_ScheduleAt(int event, uint64_t tb, uint64_t userdata = 0);
void Sched_ScheduleIn(int event, uint64_t ticks, uint64_t userdata = 0);
void Sched_Cancel(int event);
void Sched_RunDue();

void Sched_Init();
void Sched_Kick();
void Sched_IdleWait();
void Sched_WakeIdle();
// Runs fn on the game thread at the start of every Sched_RunDue (I/O completions).
void Sched_AddPollHook(void (*fn)());

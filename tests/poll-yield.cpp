#include <windows.h>
#include <cassert>
static LONGLONG clock_ticks;
static ULONGLONG coarse_ticks;
static bool frequency_ok, create_ok, set_ok, wait_ok;
static int creates, closes, yields, sleeps, sets, waits;
static BOOL WINAPI TestFrequency(LARGE_INTEGER* v) { v->QuadPart=1000000; return frequency_ok; }
static BOOL WINAPI TestCounter(LARGE_INTEGER* v) { v->QuadPart=clock_ticks; return TRUE; }
static ULONGLONG WINAPI TestTicks() { return coarse_ticks; }
static HANDLE WINAPI TestCreate(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD flags, DWORD access) {
    assert(flags==CREATE_WAITABLE_TIMER_HIGH_RESOLUTION);
    assert(access==(TIMER_MODIFY_STATE|SYNCHRONIZE));
    ++creates; return create_ok ? reinterpret_cast<HANDLE>(1) : nullptr;
}
static BOOL WINAPI TestClose(HANDLE h) { assert(h); ++closes; return TRUE; }
static BOOL WINAPI TestYield() { ++yields; return TRUE; }
static void WINAPI TestSleep(DWORD ms) { assert(ms==1); ++sleeps; }
static BOOL WINAPI TestSet(HANDLE, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE cb, LPVOID, BOOL resume) {
    assert(due->QuadPart==-10000 && period==0 && !cb && !resume); ++sets; return set_ok;
}
static DWORD WINAPI TestWait(HANDLE, DWORD ms) { assert(ms==10); ++waits; return wait_ok ? WAIT_OBJECT_0 : WAIT_FAILED; }
#define QueryPerformanceFrequency TestFrequency
#define QueryPerformanceCounter TestCounter
#define GetTickCount64 TestTicks
#define CreateWaitableTimerExW TestCreate
#define CloseHandle TestClose
#define SwitchToThread TestYield
#define Sleep TestSleep
#define SetWaitableTimer TestSet
#define WaitForSingleObject TestWait
#include "../src/poll-yield.hpp"
static void Reset() {
    clock_ticks=0; coarse_ticks=0; frequency_ok=create_ok=set_ok=wait_ok=true;
    creates=closes=yields=sleeps=sets=waits=0;
}
int main() {
    Reset();
    { PollYield p; p.Start(); clock_ticks=999; p.Pause(); assert(yields==1 && sets==0);
      clock_ticks=1000; p.Pause(); assert(sets==1 && waits==1 && sleeps==0);
      p.Start(); p.Pause(); assert(yields==2 && creates==1); }
    assert(closes==1);
    Reset(); create_ok=false;
    { PollYield p; p.Start(); p.Pause(); ++coarse_ticks; p.Pause(); assert(yields==1 && sleeps==1 && sets==0); }
    assert(closes==0);
    Reset(); set_ok=false;
    { PollYield p; p.Start(); clock_ticks=1000; ++coarse_ticks; p.Pause(); p.Pause();
      assert(sets==1 && waits==0 && closes==1 && sleeps==2); }
    assert(closes==1);
    Reset(); wait_ok=false;
    { PollYield p; p.Start(); clock_ticks=1000; p.Pause(); p.Pause();
      assert(sets==1 && waits==1 && closes==1 && yields==2); }
    assert(closes==1);
    Reset(); frequency_ok=false;
    { PollYield p; p.Start(); p.Pause(); assert(creates==0 && yields==1); }
    assert(closes==0);
}

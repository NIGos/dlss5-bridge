#pragma once
// Reused by each Vulkan worker. A VkEvent has no waitable Win32 handle.
// Yield for the first millisecond, then poll with a local high-resolution
// one-shot timer: Sleep(1) may oversleep when the process uses a coarse clock.
class PollYield {
    HANDLE timer_ = nullptr;
    LONGLONG deadline_ = 0, ticks_per_ms_ = 0;
    ULONGLONG start_ = 0;
public:
    PollYield() {
        LARGE_INTEGER f = {};
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
            ticks_per_ms_ = (f.QuadPart + 999) / 1000;
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
    }
    ~PollYield() { if (timer_) CloseHandle(timer_); }
    PollYield(const PollYield&) = delete;
    PollYield& operator=(const PollYield&) = delete;
    void Start() {
        LARGE_INTEGER now = {}; QueryPerformanceCounter(&now);
        deadline_ = now.QuadPart + ticks_per_ms_;
        start_ = GetTickCount64();
    }
    void Pause() {
        if (timer_) {
            LARGE_INTEGER now = {}; QueryPerformanceCounter(&now);
            if (now.QuadPart < deadline_) { SwitchToThread(); return; }
            LARGE_INTEGER due = {}; due.QuadPart = -10000; // relative 1 ms
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(timer_, 10) == WAIT_OBJECT_0) return;
            CloseHandle(timer_); timer_ = nullptr;
        }
        // Keep the original polling behavior if the timer is unsupported or fails.
        if (GetTickCount64() == start_) SwitchToThread(); else Sleep(1);
    }
};

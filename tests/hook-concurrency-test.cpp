// Automated test suite exercising:
// 1. Concurrent trampoline invocations with TrampolineCallGuard
// 2. In-flight call tracking and non-destructive draining
// 3. Module unload / decommit with MH_RetireHook without touching unmapped memory
// 4. Module reload and re-hooking without stale record collisions
// 5. Orphan-on-timeout safety (trampolines are not freed while in use)

#include "../src/dlss5-bridge.cpp"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

#define TEST_EXPECT(cond) do { \
    if (!(cond)) { \
        printf("FAILED line %d: %s\n", __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

typedef int (*PFN_TestFunc)(int a, int b);
static Hook g_test_hook;
static std::atomic<int> g_detour_calls{0};

// Standard x64 function machine code (26 bytes):
// 48 89 4c 24 08    mov [rsp+8], rcx
// 48 89 54 24 10    mov [rsp+16], rdx
// 48 83 ec 28       sub rsp, 28h
// 8b 44 24 30       mov eax, [rsp+48] ; a
// 03 44 24 38       add eax, [rsp+56] ; b
// 48 83 c4 28       add rsp, 28h
// c3                ret
static const BYTE kTestFuncCode[] = {
    0x48, 0x89, 0x4C, 0x24, 0x08,
    0x48, 0x89, 0x54, 0x24, 0x10,
    0x48, 0x83, 0xEC, 0x28,
    0x8B, 0x44, 0x24, 0x30,
    0x03, 0x44, 0x24, 0x38,
    0x48, 0x83, 0xC4, 0x28,
    0xC3
};

static int DetourFunc(int a, int b)
{
    TrampolineCallGuard guard;
    g_detour_calls.fetch_add(1, std::memory_order_relaxed);
    auto fwd = reinterpret_cast<PFN_TestFunc>(g_test_hook.original ? g_test_hook.original : g_test_hook.target);
    return fwd(a, b) + 1000;
}

static void TestBasicHook()
{
    printf("CASE: Basic HookInstall, trampoline call and HookRemove\n");
    void *mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    TEST_EXPECT(mem != nullptr);
    memcpy(mem, kTestFuncCode, sizeof(kTestFuncCode));
    auto func = reinterpret_cast<PFN_TestFunc>(mem);

    // Initial unhooked call:
    TEST_EXPECT(func(3, 7) == 10);

    // Hook installation:
    g_test_hook = {};
    TEST_EXPECT(HookInstall(g_test_hook, mem, reinterpret_cast<void *>(&DetourFunc)));
    TEST_EXPECT(g_test_hook.active);
    TEST_EXPECT(g_test_hook.original != nullptr);

    // Detour call through trampoline:
    g_detour_calls = 0;
    TEST_EXPECT(func(3, 7) == 1010);
    TEST_EXPECT(g_detour_calls == 1);

    // Unhook:
    HookRemove(g_test_hook);
    TEST_EXPECT(!g_test_hook.active);
    TEST_EXPECT(func(3, 7) == 10);

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("PASS\n");
}

static void TestConcurrencyAndDraining()
{
    printf("CASE: Concurrent trampoline invocations and in-flight tracking\n");
    void *mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    TEST_EXPECT(mem != nullptr);
    memcpy(mem, kTestFuncCode, sizeof(kTestFuncCode));
    auto func = reinterpret_cast<PFN_TestFunc>(mem);

    g_test_hook = {};
    TEST_EXPECT(HookInstall(g_test_hook, mem, reinterpret_cast<void *>(&DetourFunc)));
    g_detour_calls = 0;

    const int kThreads = 8;
    const int kCallsPerThread = 5000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([func, kCallsPerThread]() {
            for (int i = 0; i < kCallsPerThread; ++i)
            {
                int res = func(1, 2);
                TEST_EXPECT(res == 1003);
            }
        });
    }

    for (auto &th : workers)
        th.join();

    TEST_EXPECT(g_detour_calls == kThreads * kCallsPerThread);
    TEST_EXPECT(InterlockedCompareExchange(&g_in_flight_trampoline_calls, 0, 0) == 0);

    // Now test concurrent draining while calls are actively occurring:
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> continuous_workers;
    for (int t = 0; t < 4; ++t)
    {
        continuous_workers.emplace_back([func, &stop_flag]() {
            while (!stop_flag.load(std::memory_order_relaxed))
            {
                func(5, 5);
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Stop continuous callers:
    stop_flag.store(true, std::memory_order_relaxed);
    for (auto &th : continuous_workers)
        th.join();

    // Verify all in-flight calls have drained:
    TEST_EXPECT(WaitForInFlightTrampolineCalls(2000));
    TEST_EXPECT(InterlockedCompareExchange(&g_in_flight_trampoline_calls, 0, 0) == 0);

    HookRemove(g_test_hook);
    VirtualFree(mem, 0, MEM_RELEASE);
    printf("PASS\n");
}

static void TestModuleUnloadAndRetire()
{
    printf("CASE: Module unload decommits memory; HookRetire frees trampoline without writing unmapped memory\n");
    void *mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    TEST_EXPECT(mem != nullptr);
    memcpy(mem, kTestFuncCode, sizeof(kTestFuncCode));
    auto func = reinterpret_cast<PFN_TestFunc>(mem);

    g_test_hook = {};
    TEST_EXPECT(HookInstall(g_test_hook, mem, reinterpret_cast<void *>(&DetourFunc)));
    TEST_EXPECT(func(10, 20) == 1030);

    // Simulate module unload by decommitting the target memory pages (just like FreeLibrary unmapping a DLL):
    TEST_EXPECT(VirtualFree(mem, 4096, MEM_DECOMMIT));

    // Target memory is now DECOMMITTED. Calling MH_RemoveHook would fail VirtualProtect / write to dead memory.
    // HookRetire (MH_RetireHook) must cleanly retire the hook and free the trampoline buffer:
    HookRetire(g_test_hook);
    TEST_EXPECT(!g_test_hook.active);
    TEST_EXPECT(g_test_hook.target == nullptr);
    TEST_EXPECT(g_test_hook.original == nullptr);

    // Free the reserved address space:
    VirtualFree(mem, 0, MEM_RELEASE);
    printf("PASS\n");
}

static void TestModuleReload()
{
    printf("CASE: Module reload reallocates memory and re-installs hook cleanly\n");
    void *mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    TEST_EXPECT(mem != nullptr);
    memcpy(mem, kTestFuncCode, sizeof(kTestFuncCode));
    auto func = reinterpret_cast<PFN_TestFunc>(mem);

    g_test_hook = {};
    // HookInstall must succeed and have no collision with the previously retired hook:
    TEST_EXPECT(HookInstall(g_test_hook, mem, reinterpret_cast<void *>(&DetourFunc)));
    TEST_EXPECT(func(50, 50) == 1100);

    HookRemove(g_test_hook);
    TEST_EXPECT(func(50, 50) == 100);

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("PASS\n");
}

static void TestOrphanOnTimeoutSafety()
{
    printf("CASE: Orphan-on-timeout safety (trampoline is NOT freed while calls are in flight)\n");
    void *mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    TEST_EXPECT(mem != nullptr);
    memcpy(mem, kTestFuncCode, sizeof(kTestFuncCode));

    g_test_hook = {};
    TEST_EXPECT(HookInstall(g_test_hook, mem, reinterpret_cast<void *>(&DetourFunc)));

    // Artificially simulate an in-flight call held by a long-running frame:
    InterlockedIncrement(&g_in_flight_trampoline_calls);

    // A wait with a short timeout must return false:
    TEST_EXPECT(!WaitForInFlightTrampolineCalls(50));

    // Attempting HookRemove during timeout must NOT call MH_RemoveHook (it logs and returns):
    void *orig_target = g_test_hook.target;
    void *orig_trampoline = g_test_hook.original;
    HookRemove(g_test_hook);

    // Target and original trampoline must NOT have been destroyed while in flight:
    TEST_EXPECT(g_test_hook.target == orig_target);
    TEST_EXPECT(g_test_hook.original == orig_trampoline);

    // Now release the in-flight hold:
    InterlockedDecrement(&g_in_flight_trampoline_calls);
    TEST_EXPECT(WaitForInFlightTrampolineCalls(50));

    // Now that in-flight is 0, HookRemove succeeds:
    g_test_hook.active = true; // re-arm flag for normal remove
    HookRemove(g_test_hook);
    TEST_EXPECT(g_test_hook.target == nullptr);

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("PASS\n");
}

int main()
{
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_hook_cs);
    InitializeCriticalSection(&g_ngx_cs);
    InitializeCriticalSection(&g_bridge_cs);
    g_ngx_cs_ready = true;
    strcpy_s(g_log_path, "NUL");
    MH_Initialize();

    TestBasicHook();
    TestConcurrencyAndDraining();
    TestModuleUnloadAndRetire();
    TestModuleReload();
    TestOrphanOnTimeoutSafety();

    MH_Uninitialize();
    DeleteCriticalSection(&g_bridge_cs);
    DeleteCriticalSection(&g_ngx_cs);
    DeleteCriticalSection(&g_hook_cs);
    DeleteCriticalSection(&g_log_cs);

    printf("ALL HOOK CONCURRENCY AND LIFECYCLE TESTS PASSED!\n");
    return 0;
}

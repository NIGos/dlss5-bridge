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

static Hook *g_active_test_hook = &g_test_hook;

static int DetourFunc(int a, int b)
{
    TrampolineCallGuard guard;
    g_detour_calls.fetch_add(1, std::memory_order_relaxed);
    Hook *h = g_active_test_hook ? g_active_test_hook : &g_test_hook;
    auto fwd = reinterpret_cast<PFN_TestFunc>(h->original ? h->original : h->target);
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
    printf("CASE: Module unload decommits memory; HookRetire retains trampoline without writing unmapped memory\n");
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

static void TestDeferredRetirementUnderTimeout()
{
    printf("CASE: ForgetUnloadedLayer defers retirement without wiping record; drains cleanly after in-flight calls finish\n");
    void *mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    TEST_EXPECT(mem != nullptr);
    memcpy(mem, kTestFuncCode, sizeof(kTestFuncCode));

    EnterCriticalSection(&g_hook_cs);
    g_layer_count = 1;
    g_layer[0] = {};
    g_layer[0].mod = static_cast<HMODULE>(mem);
    TEST_EXPECT(HookInstall(g_layer[0].eval, mem, reinterpret_cast<void *>(&DetourFunc)));
    LeaveCriticalSection(&g_hook_cs);

    // Hold an in-flight call to simulate a render frame actively running:
    InterlockedIncrement(&g_in_flight_trampoline_calls);

    // Module unloads; ForgetUnloadedLayer runs under loader lock:
    InterlockedExchangePointer(&g_layer_modules[0], mem);
    ForgetUnloadedLayer(mem);
    ProcessPendingRetirements();

    // Verify hooks are deactivated but record and pointers are PRESERVED:
    EnterCriticalSection(&g_hook_cs);
    TEST_EXPECT(g_layer[0].pending_retirement == true);
    TEST_EXPECT(g_layer[0].eval.active == false);
    TEST_EXPECT(g_layer[0].mod == static_cast<HMODULE>(mem));
    TEST_EXPECT(g_layer[0].eval.target == mem);
    TEST_EXPECT(g_layer[0].eval.original != nullptr);
    LeaveCriticalSection(&g_hook_cs);

    // Worker attempts background drain while calls are still in flight:
    ProcessPendingRetirements();

    // Must NOT wipe layer while in flight:
    EnterCriticalSection(&g_hook_cs);
    TEST_EXPECT(g_layer[0].pending_retirement == true);
    TEST_EXPECT(g_layer[0].mod == static_cast<HMODULE>(mem));
    TEST_EXPECT(g_layer[0].eval.target == mem);
    LeaveCriticalSection(&g_hook_cs);

    // Now in-flight call completes:
    InterlockedDecrement(&g_in_flight_trampoline_calls);
    TEST_EXPECT(WaitForInFlightTrampolineCalls(50));

    // Worker attempts background drain now that calls are 0:
    ProcessPendingRetirements();

    // Layer must now be fully retired and cleared:
    EnterCriticalSection(&g_hook_cs);
    TEST_EXPECT(g_layer[0].pending_retirement == false);
    TEST_EXPECT(g_layer[0].mod == nullptr);
    TEST_EXPECT(g_layer[0].eval.target == nullptr);
    TEST_EXPECT(g_layer[0].eval.original == nullptr);
    g_layer_count = 0;
    LeaveCriticalSection(&g_hook_cs);

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("PASS\n");
}

static void TestReloadAtSameAddressWithPendingRetirement()
{
    printf("CASE: Module reloads at same address while pending retirement; old hook finalized without collision\n");
    void *mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    TEST_EXPECT(mem != nullptr);
    memcpy(mem, kTestFuncCode, sizeof(kTestFuncCode));
    auto func = reinterpret_cast<PFN_TestFunc>(mem);

    g_active_test_hook = &g_layer[0].eval;
    EnterCriticalSection(&g_hook_cs);
    g_layer_count = 1;
    g_layer[0] = {};
    g_layer[0].mod = static_cast<HMODULE>(mem);
    TEST_EXPECT(HookInstall(g_layer[0].eval, mem, reinterpret_cast<void *>(&DetourFunc)));
    LeaveCriticalSection(&g_hook_cs);

    TEST_EXPECT(func(10, 20) == 1030);

    // Module unloads while in-flight calls are active; ForgetUnloadedLayer defers retirement:
    InterlockedIncrement(&g_in_flight_trampoline_calls);
    InterlockedExchangePointer(&g_layer_modules[0], mem);
    ForgetUnloadedLayer(mem);
    ProcessPendingRetirements();
    TEST_EXPECT(g_layer[0].pending_retirement == true);

    // SUB-CASE 1: Attempt reload while in-flight call is STILL OUTSTANDING (> 0).
    // The drain must fail/timeout, preserving the pending record and deferring re-hooking.
    EnterCriticalSection(&g_hook_cs);
    bool reload_deferred = false;
    for (LONG k = 0; k < g_layer_count; ++k)
    {
        if (g_layer[k].mod == static_cast<HMODULE>(mem) && g_layer[k].pending_retirement)
        {
            if (WaitForInFlightTrampolineCalls(50))
            {
                HookRetire(g_layer[k].eval);
                HookRetire(g_layer[k].eval_c);
                HookRetire(g_layer[k].create);
                HookRetire(g_layer[k].vk_eval);
                HookRetire(g_layer[k].vk_eval_c);
                HookRetire(g_layer[k].vk_create);
                HookRetire(g_layer[k].vk_create1);
                g_layer[k] = {};
            }
            else
            {
                reload_deferred = true;
            }
        }
    }
    TEST_EXPECT(reload_deferred == true);
    TEST_EXPECT(g_layer[0].pending_retirement == true);
    TEST_EXPECT(g_layer[0].mod == static_cast<HMODULE>(mem));
    TEST_EXPECT(g_layer[0].eval.target == mem);
    TEST_EXPECT(g_layer[0].eval.original != nullptr);
    LeaveCriticalSection(&g_hook_cs);

    // SUB-CASE 2: Now the outstanding in-flight call completes:
    InterlockedDecrement(&g_in_flight_trampoline_calls);
    TEST_EXPECT(WaitForInFlightTrampolineCalls(50));

    // Subsequent reload pass now successfully drains and finalizes retirement:
    EnterCriticalSection(&g_hook_cs);
    for (LONG k = 0; k < g_layer_count; ++k)
    {
        if (g_layer[k].mod == static_cast<HMODULE>(mem) && g_layer[k].pending_retirement)
        {
            if (WaitForInFlightTrampolineCalls(2000))
            {
                HookRetire(g_layer[k].eval);
                HookRetire(g_layer[k].eval_c);
                HookRetire(g_layer[k].create);
                HookRetire(g_layer[k].vk_eval);
                HookRetire(g_layer[k].vk_eval_c);
                HookRetire(g_layer[k].vk_create);
                HookRetire(g_layer[k].vk_create1);
                g_layer[k] = {};
            }
        }
    }
    TEST_EXPECT(g_layer[0].pending_retirement == false);
    TEST_EXPECT(g_layer[0].mod == nullptr);

    // Simulate Windows PE loader mapping fresh module bytes at the reloaded base address:
    memcpy(mem, kTestFuncCode, sizeof(kTestFuncCode));

    // Now install fresh hook on the reloaded module at the same address:
    g_layer[0].mod = static_cast<HMODULE>(mem);
    TEST_EXPECT(HookInstall(g_layer[0].eval, mem, reinterpret_cast<void *>(&DetourFunc)));
    LeaveCriticalSection(&g_hook_cs);

    TEST_EXPECT(func(20, 30) == 1050);

    EnterCriticalSection(&g_hook_cs);
    HookRemove(g_layer[0].eval);
    g_layer[0] = {};
    g_layer_count = 0;
    LeaveCriticalSection(&g_hook_cs);

    g_active_test_hook = &g_test_hook;
    VirtualFree(mem, 0, MEM_RELEASE);
    printf("PASS\n");
}


static void TestLoaderNotificationDoesNotWaitForMinHook()
{
    printf("CASE: loader unload notification does not wait on another MinHook instance\n");
    wchar_t name[64];
    swprintf_s(name, L"minhook_multihook_%08X", GetCurrentProcessId());
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
    TEST_EXPECT(mutex != nullptr);
    HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    TEST_EXPECT(held && release);
    void *mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    TEST_EXPECT(mem);
    memcpy(mem, kTestFuncCode, sizeof(kTestFuncCode));
    g_layer_count = 1; g_layer[0] = {};
    g_layer[0].mod = static_cast<HMODULE>(mem);
    TEST_EXPECT(HookInstall(g_layer[0].eval, mem, reinterpret_cast<void *>(&DetourFunc)));
    InterlockedExchangePointer(&g_layer_modules[0], mem);
    std::thread holder([&] {
        WaitForSingleObject(mutex, INFINITE); SetEvent(held);
        WaitForSingleObject(release, 2000); // Watchdog lets a regression fail instead of hanging CI.
        ReleaseMutex(mutex);
    });
    TEST_EXPECT(WaitForSingleObject(held, 5000) == WAIT_OBJECT_0);
    auto nt = GetModuleHandleW(L"ntdll.dll");
    auto lock = reinterpret_cast<LONG (NTAPI *)(ULONG, ULONG *, ULONG_PTR *)>(GetProcAddress(nt,"LdrLockLoaderLock"));
    auto unlock = reinterpret_cast<LONG (NTAPI *)(ULONG, ULONG_PTR)>(GetProcAddress(nt,"LdrUnlockLoaderLock"));
    TEST_EXPECT(lock && unlock);
    ULONG disposition = 0; ULONG_PTR cookie = 0;
    TEST_EXPECT(lock(0,&disposition,&cookie) >= 0);
    const ULONGLONG start = GetTickCount64();
    ForgetUnloadedLayer(mem);
    const ULONGLONG elapsed = GetTickCount64() - start;
    TEST_EXPECT(unlock(0,cookie) >= 0);
    SetEvent(release); holder.join();
    TEST_EXPECT(elapsed < 1000);
    TEST_EXPECT(g_layer_unloaded[0] == 1);
    TEST_EXPECT(g_layer[0].eval.original != nullptr);
    TEST_EXPECT(ProcessPendingRetirements());
    TEST_EXPECT(g_layer[0].mod == nullptr);
    g_layer_count = 0;
    VirtualFree(mem,0,MEM_RELEASE);
    CloseHandle(mutex); CloseHandle(held); CloseHandle(release);
    printf("PASS\n");
}

static HMODULE g_scan_fixture;
static bool g_scan_released, g_scan_detached, g_scan_detached_under_lock;
static FARPROC (WINAPI *g_real_get_proc)(HMODULE, LPCSTR);
static void ScanFixtureDetached()
{
    g_scan_detached = true;
    // This deterministic single-thread test only owns this section in the scan.
    g_scan_detached_under_lock = g_hook_cs.RecursionCount != 0;
}
static FARPROC WINAPI ScanGetProc(HMODULE module, LPCSTR name)
{
    if (module == g_scan_fixture && !g_scan_released &&
        reinterpret_cast<uintptr_t>(name) > 65535 &&
        strcmp(name, "NVSDK_NGX_D3D11_EvaluateFeature") == 0)
    {
        // The production scanner has acquired its temporary reference. Release
        // the host reference now, making the scanner responsible for final unload.
        g_scan_released = true;
        FreeLibrary(g_scan_fixture);
    }
    return g_real_get_proc(module, name);
}
static void TestScanReleasesOutsideHookLock()
{
    puts("CASE: production scan releases its final DLL reference outside hook lock");
    g_scan_fixture = LoadLibraryW(L"lifetime-a.dll");
    TEST_EXPECT(g_scan_fixture);
    auto set = reinterpret_cast<void (*)(void (*)())>(GetProcAddress(g_scan_fixture,"SetDetachCallback"));
    TEST_EXPECT(set); set(ScanFixtureDetached);
    void *target = reinterpret_cast<void *>(&GetProcAddress);
    TEST_EXPECT(MH_CreateHook(target,reinterpret_cast<void *>(&ScanGetProc),
        reinterpret_cast<void **>(&g_real_get_proc)) == MH_OK);
    TEST_EXPECT(MH_EnableHook(target) == MH_OK);
    g_shutting_down = false;
    TryInstallHooks();
    g_shutting_down = true;
    TEST_EXPECT(MH_RemoveHook(target) == MH_OK);
    TEST_EXPECT(g_scan_released && g_scan_detached);
    TEST_EXPECT(!g_scan_detached_under_lock);
    puts("PASS");
}

static void TestBusyScanIsRescheduled()
{
    puts("CASE: a scan losing the hook lock keeps a retry pending");
    HANDLE held = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE release = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    TEST_EXPECT(held && release);
    std::thread holder([&] {
        EnterCriticalSection(&g_hook_cs); SetEvent(held);
        WaitForSingleObject(release,5000); LeaveCriticalSection(&g_hook_cs);
    });
    TEST_EXPECT(WaitForSingleObject(held,5000) == WAIT_OBJECT_0);
    g_shutting_down = false;
    InterlockedExchange(&g_scan_pending,0);
    TryInstallHooks();
    const LONG retry = InterlockedCompareExchange(&g_scan_pending,0,0);
    g_shutting_down = true;
    SetEvent(release); holder.join();
    CloseHandle(held); CloseHandle(release);
    TEST_EXPECT(retry == 1);
    puts("PASS");
}

static MH_STATUS WINAPI FailRetirement(LPVOID) { return MH_ERROR_MUTEX_FAILURE; }
static void TestRetirementFailurePreservesRecord()
{
    puts("CASE: failed MinHook retirement preserves the pending layer for retry");
    void *mem = VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    TEST_EXPECT(mem); memcpy(mem,kTestFuncCode,sizeof(kTestFuncCode));
    g_layer_count=1; g_layer[0]={}; g_layer[0].mod=static_cast<HMODULE>(mem);
    TEST_EXPECT(HookInstall(g_layer[0].eval,mem,reinterpret_cast<void *>(&DetourFunc)));
    InterlockedExchangePointer(&g_layer_modules[0],mem);
    void *retire = reinterpret_cast<void *>(&MH_RetireHook);
    void *unused = nullptr;
    TEST_EXPECT(MH_CreateHook(retire,reinterpret_cast<void *>(&FailRetirement),&unused)==MH_OK);
    TEST_EXPECT(MH_EnableHook(retire)==MH_OK);
    ForgetUnloadedLayer(mem);
    const bool finished = ProcessPendingRetirements();
    const bool retained = g_layer[0].pending_retirement && g_layer[0].mod==mem &&
        g_layer[0].eval.original && g_layer[0].eval.target==mem;
    TEST_EXPECT(MH_RemoveHook(retire)==MH_OK);
    TEST_EXPECT(!finished && retained);
    TEST_EXPECT(ProcessPendingRetirements());
    TEST_EXPECT(g_layer[0].mod==nullptr);
    g_layer_count=0; VirtualFree(mem,0,MEM_RELEASE);
    puts("PASS");
}

static void TestAbandonedMutexFailureReleasesOwnership()
{
    puts("CASE: an abandoned MinHook mutex is not retained after API failure");
    wchar_t name[64]; swprintf_s(name,L"minhook_multihook_%08X",GetCurrentProcessId());
    HANDLE mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,name);
    TEST_EXPECT(mutex);
    std::thread abandon([&] {
        TEST_EXPECT(WaitForSingleObject(mutex,5000)==WAIT_OBJECT_0);
        // Deliberately exit while owning it: Windows marks the mutex abandoned.
    });
    abandon.join();
    const MH_STATUS status=MH_SetThreadFreezeMethod(MH_FREEZE_METHOD_ORIGINAL);
    DWORD next=WAIT_FAILED;
    std::thread observer([&] {
        next=WaitForSingleObject(mutex,0);
        if (next==WAIT_OBJECT_0 || next==WAIT_ABANDONED) ReleaseMutex(mutex);
    });
    observer.join();
    if (next==WAIT_TIMEOUT) ReleaseMutex(mutex); // Clean up the pre-fix failure.
    CloseHandle(mutex);
    TEST_EXPECT(status==MH_ERROR_MUTEX_FAILURE);
    TEST_EXPECT(next==WAIT_OBJECT_0);
    puts("PASS");
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_hook_cs);
    InitializeCriticalSection(&g_ngx_cs);
    InitializeCriticalSection(&g_bridge_cs);
    g_ngx_cs_ready = true;
    g_shutting_down = true;
    strcpy_s(g_log_path, "NUL");
    MH_Initialize();

    TestBasicHook();
    TestConcurrencyAndDraining();
    TestModuleUnloadAndRetire();
    TestModuleReload();
    TestOrphanOnTimeoutSafety();
    TestDeferredRetirementUnderTimeout();
    TestReloadAtSameAddressWithPendingRetirement();
    TestLoaderNotificationDoesNotWaitForMinHook();
    TestScanReleasesOutsideHookLock();
    TestBusyScanIsRescheduled();
    TestRetirementFailurePreservesRecord();
    TestAbandonedMutexFailureReleasesOwnership();
    // Actual DLL/loader-lock lifetime is covered by module-lifetime-test.

    MH_Uninitialize();
    DeleteCriticalSection(&g_bridge_cs);
    DeleteCriticalSection(&g_ngx_cs);
    DeleteCriticalSection(&g_hook_cs);
    DeleteCriticalSection(&g_log_cs);

    printf("ALL HOOK CONCURRENCY AND LIFECYCLE TESTS PASSED!\n");
    return 0;
}

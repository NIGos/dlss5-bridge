#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main()
{
    auto nt = GetModuleHandleW(L"ntdll.dll");
    auto lock = reinterpret_cast<LONG (NTAPI *)(ULONG, ULONG *, ULONG_PTR *)>(GetProcAddress(nt, "LdrLockLoaderLock"));
    auto unlock = reinterpret_cast<LONG (NTAPI *)(ULONG, ULONG_PTR)>(GetProcAddress(nt, "LdrUnlockLoaderLock"));
    CHECK(lock && unlock);
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE detached = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(gate && done && detached);
    // Exercise startup/probe unload repeatedly, before the worker can start.
    for (int i = 0; i < 20; ++i)
    {
        ResetEvent(gate); ResetEvent(done); ResetEvent(detached);
        HMODULE module = LoadLibraryW(L"lifetime-a.dll"); CHECK(module);
        auto start = reinterpret_cast<HANDLE (*)(HANDLE,HANDLE,HANDLE)>(GetProcAddress(module, "Start")); CHECK(start);
        ULONG disposition = 0; ULONG_PTR cookie = 0;
        CHECK(lock(0, &disposition, &cookie) >= 0);
        HANDLE worker = start(gate, done, detached); CHECK(worker);
        CHECK(FreeLibrary(module)); // Release the host's only reference.
        CHECK(WaitForSingleObject(detached, 0) == WAIT_TIMEOUT);
        CHECK(WaitForSingleObject(worker, 0) == WAIT_TIMEOUT);
        CHECK(unlock(0, cookie) >= 0);
        SetEvent(gate);
        CHECK(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0);
        CHECK(WaitForSingleObject(done, 0) == WAIT_OBJECT_0);
        CHECK(WaitForSingleObject(detached, 0) == WAIT_OBJECT_0);
        CloseHandle(worker);
    }
    puts("PASS: 20 real DLL unloads with worker startup under loader lock");
    CloseHandle(gate); CloseHandle(done); CloseHandle(detached);

    // Two separately linked MinHook instances, same target. Remove in both orders.
    BYTE code[] = {0x89,0xC8,0x01,0xD0,0x90,0x90,0x90,0x90,0xC3};
    void *target = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); CHECK(target);
    memcpy(target, code, sizeof(code));
    auto call = reinterpret_cast<int (*)(int,int)>(target);
    for (int reverse = 0; reverse < 2; ++reverse)
    {
        HMODULE modules[2] = {LoadLibraryW(L"lifetime-a.dll"), LoadLibraryW(L"lifetime-b.dll")};
        CHECK(modules[0] && modules[1]);
        using Fn = BOOL (*)(void *);
        Fn remove[2];
        for (int i=0;i<2;++i)
        {
            auto install = reinterpret_cast<Fn>(GetProcAddress(modules[i], "Install"));
            remove[i] = reinterpret_cast<Fn>(GetProcAddress(modules[i], "Remove"));
            CHECK(install && remove[i]); CHECK(install(target));
        }
        CHECK(call(4,5) == 209);
        CHECK(remove[reverse](target)); CHECK(FreeLibrary(modules[reverse]));
        CHECK(call(4,5) == 109);
        CHECK(remove[1-reverse](target)); CHECK(FreeLibrary(modules[1-reverse]));
        CHECK(call(4,5) == 9);
    }
    VirtualFree(target,0,MEM_RELEASE);
    puts("PASS: two DLL MinHook chaining and removal in both orders");

    // Installed-hook lifetime policy: host release cannot unload pinned code.
    HMODULE pinned = LoadLibraryW(L"lifetime-a.dll"); CHECK(pinned);
    HANDLE pinnedDetached = CreateEventW(nullptr,TRUE,FALSE,nullptr); CHECK(pinnedDetached);
    auto pin = reinterpret_cast<BOOL (*)(HANDLE)>(GetProcAddress(pinned,"Pin")); CHECK(pin);
    CHECK(pin(pinnedDetached)); CHECK(FreeLibrary(pinned));
    CHECK(WaitForSingleObject(pinnedDetached,0) == WAIT_TIMEOUT);
    puts("PASS: active-hook module remains mapped after host FreeLibrary");
    // Leave this handle valid for the pinned DLL's process-exit notification.
    return 0;
}

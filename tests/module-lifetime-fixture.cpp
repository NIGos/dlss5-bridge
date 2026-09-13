#include "../src/module-lifetime.h"
#include <MinHook.h>
static HANDLE gate, finished, detached;
static int (*original)(int, int);
static int Detour(int a, int b) { return original(a, b) + 100; }
static DWORD WINAPI Worker(LPVOID owner)
{
    WaitForSingleObject(gate, INFINITE);
    SetEvent(finished);
    FreeLibraryAndExitThread(static_cast<HMODULE>(owner), 0);
}
extern "C" __declspec(dllexport) HANDLE Start(HANDLE wait, HANDLE done, HANDLE unload)
{
    gate = wait; finished = done; detached = unload;
    return StartModuleWorker(Worker);
}
extern "C" __declspec(dllexport) BOOL Pin(HANDLE unload)
{
    detached = unload;
    return PinHookModule(reinterpret_cast<const void *>(&Pin));
}
extern "C" __declspec(dllexport) BOOL Install(void *target)
{
    return MH_Initialize() == MH_OK &&
        MH_CreateHook(target, reinterpret_cast<void *>(&Detour), reinterpret_cast<void **>(&original)) == MH_OK &&
        MH_EnableHook(target) == MH_OK;
}
extern "C" __declspec(dllexport) BOOL Remove(void *target)
{
    return MH_RemoveHook(target) == MH_OK && MH_Uninitialize() == MH_OK;
}
BOOL WINAPI DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH && detached) SetEvent(detached);
    return TRUE;
}

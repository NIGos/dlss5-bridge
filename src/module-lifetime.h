#pragma once
#include <windows.h>

// Acquire before CreateThread: DLL_THREAD_ATTACH may be delayed by the loader
// lock, but the entry point must already be protected against FreeLibrary.
// The worker receives the owned module and must end with FreeLibraryAndExitThread.
static HANDLE StartModuleWorker(LPTHREAD_START_ROUTINE entry)
{
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(entry), &owner)) return nullptr;
    HANDLE thread = CreateThread(nullptr, 0, entry, owner, 0, nullptr);
    if (!thread) FreeLibrary(owner);
    return thread;
}

// Installed detours can be entered independently of ReShade's addon references.
// Keep their code and state until process exit. Runtime config still works;
// replacing/unloading an active bridge requires restarting the host.
static bool PinHookModule(const void *address)
{
    HMODULE owner = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(address), &owner) != FALSE;
}

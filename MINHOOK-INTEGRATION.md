# MinHook integration

This work completes the MinHook contribution in PR #42. It replaces the bridge's
manual NGX entry-point patching with relocated trampolines and retains the
contributor's commits.

Each scan worker acquires a module reference before its thread is created and
releases it with `FreeLibraryAndExitThread`. Windows may delay thread startup
under the loader lock; the worker's code stays mapped during that delay.

Before publishing an NGX hook, the bridge pins its own module until process exit.
There is no timed join or MinHook teardown in `DllMain`. **Once hooks are active,
restart the game to replace or unload the bridge.** Runtime configuration changes
still work. A probe that never attempts NGX hook installation is not pinned by this policy.

Unload notifications publish atomic markers only for tracked modules. The worker
retires hooks outside the loader callback, retaining state while calls remain in
flight. Module references protect export inspection and hook installation against
concurrent unload. Retired executable buffers are retained until process exit;
this trades a small allocation per retired hook for avoiding stale trampoline reuse.

MinHook is compiled into the addon; no extra runtime DLL is required. Its source
revisions and local changes are recorded in `src/minhook/PROVENANCE.md`.

Validation commands:

```
src\build.cmd
tests\build-hook-concurrency-test.cmd
tests\build-module-lifetime-test.cmd
tests\build-d3d11-conversion-test.cmd --warp
tests\build-d3d11-histories-test.cmd
tests\build-vulkan-fg-device-test.cmd
```

The lifetime test uses real DLLs and the Windows loader lock, including 20
unload/reload cycles before worker startup, process-lifetime pinning, and two
independent MinHook instances removed in either order. The notification test
holds the shared MinHook mutex on another thread while delivering an unload
notification under the loader lock.

These tests do not prove compatibility with every mod or fix every Frame
Generation issue. No performance improvement is claimed.

Local results on 2026-09-13: addon build; eight hook/lifecycle cases; real DLL
lifetime and chaining suite; D3D11 history ownership; 18 Vulkan FG CPU cases;
four WARP pixel cases all passed. D3D11 debug-layer validation was unavailable
and is **skipped**, not passed. Background Gym runs passed D3D11 consumer
(300/300 evaluates), unequal split views (128/128 evaluates), and Vulkan smoke
(120/120 evaluates), with neural rendering active. No game was launched.
The D3D11 runs preceded the final unrelated-unload scheduling guard; that guard
was included in the CPU notification suite and Vulkan run.

See [the follow-up audit](MINHOOK-AUDIT.md) for three additional edge-case fixes
and their failing-before/passing-after regression tests.

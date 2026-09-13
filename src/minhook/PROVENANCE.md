# Vendored MinHook

The hook engine, buffer allocator and trampoline builder match
[m417z/minhook at 8af6b4acae5a9388fd742b56fa79ece89d96f823](https://github.com/m417z/minhook/tree/8af6b4acae5a9388fd742b56fa79ece89d96f823),
with these local differences (ignoring UTF-8 BOMs and line endings):

- `hook.c`: `TEXT("ntdll.dll")` works in both ANSI and Unicode builds; added
  `MH_RetireHook` / `MH_RetireHookEx` remove records for unmapped targets without
  touching target memory or freeing executable buffers. Those buffers remain
  allocated until process exit in the bridge. Retirement runs outside loader notifications.
- `hook.c`: on `WAIT_ABANDONED`, release the mutex ownership granted by Windows
  before returning `MH_ERROR_MUTEX_FAILURE`. All guarded APIs use the same helper.
- `MinHook.h`: declarations/documentation for retirement; omitted an upstream
  comment about a possible future thread-freeze method.
- `src/hde/*`: the complete HDE directory instead matches
  [TsudaKageyu/minhook v1.3.4, c3fcafdc10146beb5919319d0683e44e3c30d537](https://github.com/TsudaKageyu/minhook/tree/c3fcafdc10146beb5919319d0683e44e3c30d537/src/hde).
  Its decoder is used as a consistent set; no newer m417z decoder changes are mixed in.

The bridge uses the default documented thread enumeration method. It does not
enable the optional `NtGetNextThread` optimization. This integration makes no
claim of improved FPS or universal compatibility with third-party detours.

`tests/build-module-lifetime-test.cmd` tests two independently loaded copies of
this engine on the same target and removes them in both orders. The hook tests
also cover concurrent forwarding, retirement and address reuse.

Original licensing is retained in `LICENSE.txt` and `AUTHORS.txt`.

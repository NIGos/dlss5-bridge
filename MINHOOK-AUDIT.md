# Follow-up audit, 2026-09-13

Reviewed the integration merged in `9a7df3f80a570b6d8787672e149f5cf559202a8e`.
Found and corrected four edge cases before publishing pre8.

| Finding | Correction | Regression evidence |
| --- | --- | --- |
| The scanner's last DLL reference could be released while holding `g_hook_cs`, running foreign detach code under the bridge lock. | Keep scan references until after releasing the lock. | A real fixture DLL releases its host reference during the production export scan. Its detach callback observes the lock held before the fix and released afterwards. |
| Losing the hook lock between retirement and installation could discard a scan request. With no hooks installed, there might be no evaluate callback to retry it. | Leave the scan flag pending when installation cannot acquire the lock. | A separate thread holds the section; the production scanner failed to request a retry before the fix and requests one afterwards. |
| Retirement cleared the layer even when MinHook returned an error. | Preserve failed hook records and keep the layer pending until retirement succeeds; both retirement paths share the same helper. | Injected `MH_ERROR_MUTEX_FAILURE` into the real retirement call. State was lost before the fix; it is retained afterwards and the subsequent successful retry clears it. |
| MinHook returned an error on an abandoned mutex without releasing the ownership granted by Windows. Other callers could remain blocked. | All guarded APIs release abandoned ownership before returning the error; this does not attempt to recover potentially inconsistent state. | A thread exits owning the real mutex. Before the fix a second observer cannot acquire it after the API fails; afterwards it can. |

All four tests were run before their respective corrections and failed at the
assertion for the reported condition. All now pass as part of the full **12-case
hook/lifecycle suite**. These are reproduced state/lifetime defects, not claims
that a particular game's crash was reproduced.

Additional verification:

- Real DLL lifetime fixture: 20 unload/reload cycles under the loader lock;
  two separate MinHook DLLs removed in both orders; module pinning. Passed.
- Vulkan FG routing: all 18 CPU cases passed, including deferred unload.
- D3D11 Gym consumer: 300/300 evaluates, NR active, no stand-down, normal exit.
  Output hash `80C474E0175760DD`, identical to the earlier integration run.
- Vulkan Gym display scenario: 600/600 evaluates; changes between 1920x1080 and
  2560x1440, HDR on/off accepted, five runtime teardowns and four re-arms,
  NR active, no stand-down or pool exhaustion, normal exit.
- Build succeeds. CI also runs history ownership, WARP pixels and download verification.

No game was opened and no game installation changed. The display test verifies
resource/runtime transitions, not perceptual HDR quality. This is not a measured
performance comparison, exhaustive mod-compatibility test, or proof that every
Frame Generation issue is resolved. Replacing an active bridge still requires a
game restart. No release is published by this audit.

The GPU runs preceded the isolated abandoned-mutex error-path correction; the
full CPU and real-DLL suites cover both normal and abandoned mutex paths after it.

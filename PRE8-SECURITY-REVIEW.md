# pre8 security review — 2026-09-13

Scope: source changes from v1.4.13-pre7 to ecd1b00674020a1e8c76a9cb653a1a21d11676a0, sensitive API use in the addon source, build workflow, vendored MinHook, and the published v1.4.13-pre8 addon.

Result: no evidence of malicious behavior found in the checks performed. This is a targeted source and binary review plus one antivirus scan, not proof of absence of malware.

## Artifact

SHA-256: `c4c8b5bc4b26b2b3f3bf2767cdb708546d62f7d0bbb63d24e940c736da9efe26`; 546304 bytes. The repository verifier confirms exact agreement with the published GitHub release. GitHub build attestation was verified during installation. This is the same artifact installed in BG3.

Microsoft Defender custom scan, remediation disabled, on the artifact directory returned exit 0 and “found no threats”. Defender was enabled, with signatures dated 2026-09-13 06:41 local time. No exclusions or protection settings were changed.

## Source and build review

The pre7-to-pre8 changes concern NGX hooks, worker/module lifetime, tests, build inputs, and documentation. Searches for network/download APIs, external process execution, credential access, keyboard/clipboard capture and registry persistence did not identify an implementation of those behaviors in addon code. Sensitive calls were checked in context: registry access reads NVIDIA's NGX path; file operations handle configuration, logs, and DLL/version/hash inspection; thread suspension/context changes implement in-process hooks. MinHook's documented enumeration filters to the current process.

The release PE imports USER32, ADVAPI32, bcrypt, VERSION and KERNEL32. Imports and dynamically resolved function names inspected are consistent with the documented graphics, configuration, diagnostic and hook paths. There are no direct networking imports. Import absence alone cannot exclude hidden behavior.

The build script compiles checked-in C/C++ and resources with MSVC and Windows libraries. It contains no download or post-build payload execution. The CI uses commit-pinned GitHub actions, read-only permissions in the build job, and a separate main-only attestation job. The new test build scripts compile local fixtures and run them; their hook behavior is confined to test processes. The separate download-verifier script intentionally contacts the GitHub API and does not load the addon.

## Finding: incorrect provenance reference, corrected

The provenance document named m417z/minhook master commit `8af6b4acae5a9388fd742b56fa79ece89d96f823`. A fresh comparison shows the vendored engine matches **multihook commit `4f18d1809a81e6c700bcc4a21aeb43ade72bbf8b`**, with exactly the documented local changes.

After normalizing BOMs and line endings, buffer.c, buffer.h, trampoline.c and trampoline.h match that commit. hook.c differs only in the documented ANSI/Unicode fix, retirement functions and abandoned-mutex handling. MinHook.h differs only in retirement declarations and the omitted upstream comment. All seven HDE files match TsudaKageyu/minhook v1.3.4 commit `c3fcafdc10146beb5919319d0683e44e3c30d537`.

The wrong commit reference was a documentation error, not an unexplained payload or a change to the released binary. PROVENANCE.md now names the verified revision.

## Limits

No multi-engine online scan or isolated behavioral malware run was completed. Windows Sandbox is present, but checking its feature state required elevation; no sandbox was started and no test was represented as isolated. Previous Gym runs are functional tests, not malware analysis. No game was started for this review. Third-party neural runtime, ReShade and other game addons are outside this artifact review. Existing local DLL-loading paths still rely on the trustworthiness of the driver/game environment. No full reverse engineering or byte-reproducible independent rebuild was performed.

# Verify a Bridge download

Use [NIGos/dlss5-bridge on GitHub](https://github.com/NIGos/dlss5-bridge/releases)
as the source for both the addon and these instructions. `dlss5bridge.com` is
not operated or endorsed by this project. A matching filename or version
number does not establish where a file came from.

**Check before copying the addon into the game folder or starting the game.**
Verification does not require loading the addon, running an installer, or
administrator privileges.

## Compare SHA-256 manually

In PowerShell, replace the path with your downloaded file:

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath 'C:\Users\YourName\Downloads\dlss5-bridge.addon64'
```

Compare the whole hash, ignoring letter case, with the matching release below.
These values were checked against downloaded official GitHub assets on
9 September 2026. **The tag must match the release you intended to download.**

| Release | Asset bytes | SHA-256 of `dlss5-bridge.addon64` |
| --- | ---: | --- |
| [v1.4.12 — stable](https://github.com/NIGos/dlss5-bridge/releases/tag/v1.4.12) | 508928 | `4f2acecc1026ae89ac0b92767be66ceea2662ad0ef88710b89c7da7840d548d4` |
| [v1.4.13-pre3](https://github.com/NIGos/dlss5-bridge/releases/tag/v1.4.13-pre3) | 508928 | `3dc81b261377c936c22cf8589d96cb0be8b18b8de90d1cea5ba83e9bca763717` |
| [v1.4.13-pre2](https://github.com/NIGos/dlss5-bridge/releases/tag/v1.4.13-pre2) | 508928 | `2a3e8b74df4fb837841eeebb456cda9cff67ad5b782b14a7b886d90d8d4c333b` |
| [v1.4.13-pre1](https://github.com/NIGos/dlss5-bridge/releases/tag/v1.4.13-pre1) | 508928 | `59bbc7b111d766c9a325870ba694084da76fbd96ba5aa48a8f0348d2062b6403` |
| [v1.3.0 — old, reference only](https://github.com/NIGos/dlss5-bridge/releases/tag/v1.3.0) | 414720 | `589bae7e5391b27d993351ed042f67f7322f6fb62a240e5271b9dfc2df79fc97` |

The v1.3.0 hash belongs to the official addon, not to the ZIP offered by the
unaffiliated website. This does not recommend installing an old release.

## Automated comparison

Download and review [tools/Verify-Bridge.ps1](tools/Verify-Bridge.ps1) from this
repository. It uses PowerShell 5.1 or newer and contacts the GitHub API over
HTTPS for the SHA-256 and size of the exact release asset. No login is needed.
It does not download or execute the addon, install anything, or trust the
version string inside the file.

From the folder containing that script, run:

```powershell
.\Verify-Bridge.ps1 -Path 'C:\Users\YourName\Downloads\dlss5-bridge.addon64' -ReleaseTag v1.4.12
```

For another release, supply its tag explicitly, for example `v1.4.13-pre3`.
If PowerShell blocks the downloaded script, use the manual hash comparison
above; weakening execution policy is not required for verification.

- **MATCH** / exit code 0: the file's SHA-256 and size match that published
  asset at verification time.
- **NOT VERIFIED** / nonzero exit: do not load it. A mismatch can mean a
  modified or damaged file, or simply the wrong version. Network errors,
  GitHub rate limits, missing releases and missing digests also fail verification;
  none is treated as a successful check.

## What this protects

The trusted reference is this GitHub repository and its HTTPS API. A hash
copied from the same untrusted download site does not establish authenticity.
This check covers the selected file only: it does not validate other files
in a ZIP, scan for malware, or ensure an old release is current. Rebuilding
the source locally can produce a different hash without malicious changes.

An internal "not tampered with" badge would not provide the same protection:
an attacker could replace both the addon and its check, and the addon would
already be loaded by the time the badge appears. These instructions do not
claim that the addon can prevent a repackaged version from being distributed.

The releases listed above are currently unsigned. Publisher signing and
verification by a trusted tool or loader before loading would add protection;
this SHA-256 check is not an Authenticode signature.

For maintainers: run the offline regression check with
`powershell -NoProfile -File tools\Test-Verify-Bridge.ps1`.

# Verify a Bridge download

Use [NIGos/dlss5-bridge on GitHub](https://github.com/NIGos/dlss5-bridge/releases)
as the source for both the addon and these instructions. `dlss5bridge.com` is
not operated or endorsed by this project. A matching filename or version
number does not establish where a file came from.

The Bridge download is **`dlss5-bridge.addon64`**. We do not distribute a Bridge
installer or ask you to disable antivirus protection or add exclusions.
ReShade's installer is a separate download from [reshade.me](https://reshade.me/).

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
| [v1.4.13-pre5](https://github.com/NIGos/dlss5-bridge/releases/tag/v1.4.13-pre5) | 530944 | `100fa93e597df0803a7edff24d89b47dbbaee11fd3a06d26ce8107e317a66b71` |
| [v1.4.13-pre4](https://github.com/NIGos/dlss5-bridge/releases/tag/v1.4.13-pre4) | 513024 | `710e5354c42ae3491c9523576c82805d0c42383836525f3d3d02a31ffa9f9d2f` |
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

For another release, supply its tag explicitly, for example `v1.4.13-pre5`.
If PowerShell blocks the downloaded script, use the manual hash comparison
above; weakening execution policy is not required for verification.

- **MATCH** / exit code 0: the file's SHA-256 and size match that published
  asset at verification time.
- **NOT VERIFIED** / nonzero exit: do not load it. A mismatch can mean a
  modified or damaged file, or simply the wrong version. Network errors,
  GitHub rate limits, missing releases and missing digests also fail verification;
  none is treated as a successful check.

## Verify an immutable release

**All seven releases in the table above are immutable and have verified release
attestations.** Pre4 and pre5 were published as immutable releases. The five older
releases became immutable when their notes were updated on 9 September 2026;
their original tags, publication dates and addon hashes were preserved.

With a current GitHub CLI, replace `TAG` and the file path with the intended
release and your local addon:

```powershell
gh release verify TAG --repo github.com/NIGos/dlss5-bridge
gh release verify-asset TAG 'C:\path\dlss5-bridge.addon64' --repo github.com/NIGos/dlss5-bridge
```

Both commands must succeed. For any release outside the table, check its actual
immutability status; these commands do not apply to mutable releases or GitHub's
automatically generated source archives. The attestation authenticates the files
now locked to that release, without proving how the older binaries were built.
The PowerShell checker remains a SHA-256/size comparison and does not verify an
attestation.

## Verify where a CI build came from

**Pre4 and pre5 have GitHub Actions build provenance.** Their addons come from
the official build workflow on GitHub-hosted runners; pre4 was the first release
published this way.

| Release | Source commit | Build run |
| --- | --- | --- |
| pre5 | [c37b670b4ce98fd412c52867666018c1f4d02745](https://github.com/NIGos/dlss5-bridge/commit/c37b670b4ce98fd412c52867666018c1f4d02745) | [34406985478](https://github.com/NIGos/dlss5-bridge/actions/runs/34406985478) |
| pre4 | [efc5cac43adac2cca98a163afacee5ac1b16917e](https://github.com/NIGos/dlss5-bridge/commit/efc5cac43adac2cca98a163afacee5ac1b16917e) | [34342819589](https://github.com/NIGos/dlss5-bridge/actions/runs/34342819589) |

The five older releases in the table (**v1.4.12, pre1, pre2, pre3 and v1.3.0**)
**do not have CI build provenance**. Their release attestations do not add it
retroactively.

To verify pre5 with a current GitHub CLI, replace only the file path:

```powershell
gh attestation verify 'C:\path\dlss5-bridge.addon64' --repo NIGos/dlss5-bridge --hostname github.com --signer-workflow NIGos/dlss5-bridge/.github/workflows/verify-download-checker.yml --source-ref refs/heads/main --source-digest c37b670b4ce98fd412c52867666018c1f4d02745 --deny-self-hosted-runners
```

This checks that the file was attested by the named build workflow for that
source commit on a GitHub-hosted runner. Release verification above answers a
different question: whether the file matches what was published for a specific
immutable release. Use both checks for a release that provides both attestations.
These checks read the file without loading it. A missing or failed attestation
does not count as verified; older releases were not built with this workflow.
See the [GitHub CLI documentation](https://cli.github.com/manual/gh_attestation_verify)
for the verification options.

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

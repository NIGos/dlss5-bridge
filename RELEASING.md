# Publish an immutable release

Release immutability is enabled. **Upload every final asset to a draft before
publishing.** After publication, assets and the release tag cannot be replaced.
Use a new version for a correction; do not delete/recreate an old release to
retrofit immutability. Existing releases retain their original status.

1. Build from the intended reviewed commit. Run the relevant Gym checks and
   `powershell -NoProfile -File tools\Test-Verify-Bridge.ps1`. Confirm that the
   binary version, release tag and intended commit agree. Do not package unrelated
   addons, NVIDIA model DLLs, installers or files from third-party bundles.
2. If publisher signing is configured in future, sign and timestamp the final
   binary, then verify the expected publisher's signature. **Hash after signing.**
   Signing is not currently configured; do not claim that an unsigned build is signed.
3. Calculate SHA-256 for every final asset. Record the addon hash, exact byte size
   and intended source commit in the release notes. Prepare `SHA256SUMS.txt` with
   one line per final downloadable file, excluding the checksum file itself.
4. Create and push the intended version tag once. Confirm it resolves remotely
   to the intended commit. Create a **draft** release for that existing tag,
   with the correct prerelease/latest settings, notes and every final asset.
   With the CLI, use `gh release create ... --draft --verify-tag --repo github.com/NIGos/dlss5-bridge`.
   Do not publish an empty release and upload its addon afterward.
5. Before publishing, compare the draft's GitHub asset digests and sizes with the
   local final files. Review its tag/commit and complete asset list. If anything
   differs or is missing, keep the release in draft and correct it.
6. Publish the reviewed draft. Confirm that GitHub reports it as immutable and
   that it has the intended stable/prerelease/latest status. Verify its attestation
   and the local final addon using the commands below. A failed or unavailable
   attestation is not a successful verification; do not announce success while it
   is unresolved.
7. Update the version-specific SHA-256 reference in `VERIFYING-DOWNLOADS.md` and
   link the release from the README. Keep the security warning and official URLs.

For the actual newly published tag, replace `TAG` and the local path:

```powershell
gh release verify TAG --repo github.com/NIGos/dlss5-bridge
gh release verify-asset TAG 'C:\path\dlss5-bridge.addon64' --repo github.com/NIGos/dlss5-bridge
```

Use a current GitHub CLI with these commands. Specify the full GitHub host and
repository, even when running from another checkout. These are native GitHub
release attestations; no locally generated signing key is required.

Repository controls also prevent force-pushing/deleting `main` and moving or
deleting `v*` tags. Normal commits and new tags are allowed. Review pull requests
and CI results before applying changes; workflow tokens default to read-only,
and external contributor workflows need approval.

Sources: [immutable releases](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases),
[release verification](https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/secure-your-dependencies/verify-release-integrity).

# Publish an immutable release

Release immutability is enabled. **Upload every final asset to a draft before
publishing.** After publication, assets and the release tag cannot be replaced.
Use a new version for a correction; do not delete/recreate an old release to
retrofit immutability.

Check an existing release's actual `immutable` status before and after editing
it. In this repository, updating the published notes on 9 September 2026
finalized immutability for **v1.4.12, v1.4.13-pre1, v1.4.13-pre2, v1.4.13-pre3
and v1.3.0**, without replacing their tags or binaries. Their release attestations
were verified. Do not assume an older release will remain mutable after an edit:
confirm its tag and complete asset list first. This locks the existing files;
it does not create provenance for their historical builds. New releases must
still be complete drafts before publication.

1. Prepare the release through a pull request. Review the diff and require the
   build and verification checks to pass before merging. The project has one
   maintainer; this is a recorded review and CI check, not independent approval.
   Run the relevant Gym checks for code changes; the hosted build does not test
   games or GPU behavior. Confirm that the binary version and intended release
   tag agree. Compare the source against the release being replaced: a
   prerelease may contain fixes on a separate branch that are not yet on `main`.
2. Use the successful **Build and verify** run for the exact merged commit on
   `main`, including its **Attest main build** job. Download that run's
   `dlss5-bridge-COMMIT_SHA` artifact into a clean staging folder. It contains the
   addon, `SHA256SUMS.txt` and `BUILD-INFO.txt`. Verify the addon using the build
   provenance command below, supplying the full commit SHA. Check that the run,
   `BUILD-INFO.txt`, checksum and intended tag all identify the same source and
   file. Do not substitute a local rebuild or a pull request artifact.
   The binary is published as `dlss5-bridge.addon64`; do not package unrelated
   addons, NVIDIA model DLLs, installers or files from third-party bundles.
3. If publisher signing is configured in future, sign and timestamp the final
   binary, then verify the expected publisher's signature. **Hash after signing.**
   Signing is not currently configured; do not claim that an unsigned build is signed.
   Signing changes the binary, so the signing workflow must also attest the final
   signed file; an attestation for the unsigned file will no longer match it.
4. Record the final addon hash, exact byte size, source commit and successful
   build-run link in the release notes. Recheck the CI checksum before uploading.
   If adding any release assets, include their hashes in `SHA256SUMS.txt`, with
   one line per file excluding the checksum file itself.
5. Create and push the intended version tag once. Confirm it resolves remotely
   to the intended commit. Create a **draft** release for that existing tag,
   with the correct prerelease/latest settings, notes and every final asset.
   With the CLI, use `gh release create ... --draft --verify-tag --repo github.com/NIGos/dlss5-bridge`.
   Do not publish an empty release and upload its addon afterward.
6. Before publishing, compare the draft's GitHub asset digests and sizes with the
   local final files. Review its tag/commit and complete asset list. If anything
   differs or is missing, keep the release in draft and correct it.
7. Publish the reviewed draft. Confirm that GitHub reports it as immutable and
   that it has the intended stable/prerelease/latest status. Verify its attestation
   and the local final addon using the commands below. A failed or unavailable
   attestation is not a successful verification; do not announce success while it
   is unresolved.
8. Update the version-specific SHA-256 reference in `VERIFYING-DOWNLOADS.md` and
   link the release from the README. Keep the security warning and official URLs.

## Verify the build before publication

With a current GitHub CLI, replace `COMMIT_SHA` with the full commit from the
successful `main` run and use the downloaded addon's path:

```powershell
gh attestation verify 'C:\path\dlss5-bridge.addon64' --repo NIGos/dlss5-bridge --hostname github.com --signer-workflow NIGos/dlss5-bridge/.github/workflows/verify-download-checker.yml --source-ref refs/heads/main --source-digest COMMIT_SHA --deny-self-hosted-runners
```

This verifies the file against this repository's named workflow and exact source
commit on a GitHub-hosted runner. The build comes from `main`; the release tag
must point to that same commit. If verification fails, do not publish the file
as a verified CI build. Rerunning the build can produce a different binary;
always verify and publish the artifact from the selected successful run.

## Verify the published release

For the intended immutable release, replace `TAG` and the local path:

```powershell
gh release verify TAG --repo github.com/NIGos/dlss5-bridge
gh release verify-asset TAG 'C:\path\dlss5-bridge.addon64' --repo github.com/NIGos/dlss5-bridge
```

Use a current GitHub CLI with these commands. Specify the full GitHub host and
repository, even when running from another checkout. These are native GitHub
release attestations; no locally generated signing key is required.

Repository controls require a pull request and the successful **Build and
verifier checks** check from GitHub Actions before merging into `main`. The
branch must be up to date and review conversations resolved. Required human
approvals are zero, so the sole maintainer can merge their own checked pull
request. There are no bypass actors. Force-pushing/deleting `main` and moving
or deleting `v*` tags are also blocked; new tags remain allowed. Workflow tokens
default to read-only, and external contributor workflows need approval.

Sources: [immutable releases](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases),
[release verification](https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/secure-your-dependencies/verify-release-integrity),
[build provenance verification](https://cli.github.com/manual/gh_attestation_verify).

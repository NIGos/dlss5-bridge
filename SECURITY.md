# Security and authentic downloads

The official project is [NIGos/dlss5-bridge on GitHub](https://github.com/NIGos/dlss5-bridge).
Downloads are published on its [Releases page](https://github.com/NIGos/dlss5-bridge/releases).
The domain `dlss5bridge.com` is not owned, operated or endorsed by this project.
See [#30](https://github.com/NIGos/dlss5-bridge/issues/30) for the reported incident.

Bridge is distributed as **`dlss5-bridge.addon64`**, without a Bridge installer.
It never asks users to disable antivirus protection or add exclusions. ReShade
has its own legitimate installer at [reshade.me](https://reshade.me/), separate
from the Bridge download.

Follow [Verify a Bridge download](VERIFYING-DOWNLOADS.md) before loading a file.
A filename, version number, copied project page or in-game badge does not prove
authenticity. A hash match checks the selected file, not other files in a package.
Neither a hash nor a signature is a malware scan.

## Reporting

For a vulnerability in the project, use
[Report a vulnerability](https://github.com/NIGos/dlss5-bridge/security/advisories/new).
This reaches the maintainers privately. Include the affected version, impact
and a minimal reproduction, without passwords, tokens or personal information.
Do not attach live malware or sensitive crash dumps to a public issue.

For a suspicious download site, report its URL, the filename received, download
time and any existing hash or antivirus finding. Use the private channel when
the evidence contains sensitive details. Do not run the file to collect evidence.
For the already reported domain, keep public updates in #30.

A suspected infection is separate from a Bridge bug. Stop running the package;
if there is active unauthorized behavior, disconnect the affected computer and
use a trusted device to secure affected accounts and seek incident-response help.

## Release integrity

Release immutability was enabled on **9 September 2026**. The existing releases
**v1.4.12, v1.4.13-pre1, v1.4.13-pre2, v1.4.13-pre3 and v1.3.0** were also made
immutable that day when their notes were updated with a download warning.
Their original tags, publication dates and addon files were preserved, and their
release attestations were verified. Their assets and associated tags are now
locked. Check any other release's actual status instead of assuming from its version.

The attestation identifies what GitHub published for this repository and tag.
It is not an independent audit of the source or proof that a binary is harmless.
Build provenance is a separate attestation that links a file to the source
commit and GitHub Actions workflow that produced it. The five releases above
have release attestations, but **no CI build provenance or Authenticode publisher
signature**. Locking their existing files does not establish how they were
originally built. Follow the verification instructions for the specific release.
We do not use a self-signed certificate or an internal integrity badge as a
substitute for publisher authentication.

Maintainers must follow [the release procedure](RELEASING.md). Protect the
maintainer's GitHub account with strong two-factor authentication or a passkey,
keep recovery material private, and use narrowly scoped credentials. Credentials
and signing keys must never be committed or included in a release.

The MIT license permits redistribution under its terms. These measures protect
the official channel and help users identify its files; they cannot prevent a
third party from copying the project or creating an unrelated website.

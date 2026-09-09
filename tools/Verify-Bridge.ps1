# Read this script from the official repository before running it.
# Verifies a file without loading it. Requires PowerShell 5.1+ and HTTPS access to GitHub.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9][A-Za-z0-9.-]*)?$')]
    [string]$ReleaseTag
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

try {
    $file = Get-Item -LiteralPath $Path
    if ($file -isnot [System.IO.FileInfo]) { throw 'Select a file, not a directory.' }

    # The repository and asset are fixed; neither comes from the file being checked.
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/NIGos/dlss5-bridge/releases/tags/$ReleaseTag" `
        -Headers @{ Accept = 'application/vnd.github+json'; 'User-Agent' = 'DLSS5-Bridge-Verify' } `
        -TimeoutSec 30 -MaximumRedirection 0
    if ($release.draft -or $release.tag_name -cne $ReleaseTag) {
        throw 'GitHub did not return the requested published release.'
    }
    $assets = @($release.assets | Where-Object { $_.name -ceq 'dlss5-bridge.addon64' })
    if ($assets.Count -ne 1) { throw 'Expected exactly one official Bridge asset.' }
    $asset = $assets[0]
    if ($asset.state -ne 'uploaded' -or $asset.digest -cnotmatch '^sha256:[0-9a-fA-F]{64}$' -or $asset.size -le 0) {
        throw 'GitHub has no usable SHA-256 and size for this asset.'
    }

    # Hold a read-only stream that prevents concurrent writes during hashing.
    $stream = [System.IO.File]::Open($file.FullName, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        $size = $stream.Length
        $actual = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash
    } finally {
        $stream.Dispose()
    }
    $expected = $asset.digest.Substring(7)
    if ($size -ne $asset.size -or $actual -ine $expected) {
        throw "DOES NOT MATCH $ReleaseTag. The file may be modified, corrupt, or from a different version. Do not load it."
    }
    Write-Output "MATCH: exact SHA-256 and size of $ReleaseTag on NIGos/dlss5-bridge."
    Write-Output "SHA-256: $($actual.ToLowerInvariant())"
    Write-Output 'This checks this file only; it is not a malware scan or an update check.'
    exit 0
} catch {
    Write-Error "NOT VERIFIED: $($_.Exception.Message)" -ErrorAction Continue
    exit 1
}

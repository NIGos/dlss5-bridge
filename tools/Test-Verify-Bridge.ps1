# Offline regression check; no DLL is loaded and no network request is made.
$ErrorActionPreference = 'Stop'
$verify = Join-Path $PSScriptRoot 'Verify-Bridge.ps1'
$testDir = Join-Path ([IO.Path]::GetTempPath()) ('bridge-verify-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDir | Out-Null
$sample = Join-Path $testDir 'sample [1].addon64'
$output = Join-Path $testDir 'output.txt'
[IO.File]::WriteAllBytes($sample, [byte[]](1, 2, 3, 4))
$digest = (Get-FileHash -LiteralPath $sample -Algorithm SHA256).Hash.ToLowerInvariant()
$scenario = ''

function Invoke-RestMethod {
    param($Uri, $Headers, $TimeoutSec, $MaximumRedirection)
    if ($Uri -cne 'https://api.github.com/repos/NIGos/dlss5-bridge/releases/tags/v1.4.12' -or $MaximumRedirection -ne 0) {
        throw 'Unexpected endpoint or redirect policy.'
    }
    if ($scenario -eq 'network-error') { throw 'Simulated network failure.' }
    $asset = [pscustomobject]@{ name = 'dlss5-bridge.addon64'; state = 'uploaded'; digest = "sha256:$digest"; size = 4 }
    $release = [pscustomobject]@{ tag_name = 'v1.4.12'; draft = $false; assets = @($asset) }
    switch ($scenario) {
        'no-digest' { $asset.digest = $null }
        'bad-digest' { $asset.digest = 'sha256:1234' }
        'no-asset' { $release.assets = @() }
        'duplicate-asset' { $release.assets = @($asset, $asset) }
        'wrong-name' { $asset.name = 'installer.exe' }
        'wrong-size' { $asset.size = 5 }
        'not-uploaded' { $asset.state = 'new' }
        'wrong-tag' { $release.tag_name = 'v1.3.0' }
        'draft' { $release.draft = $true }
    }
    return $release
}

try {
    foreach ($case in @('match', 'modified', 'no-digest', 'bad-digest', 'no-asset', 'duplicate-asset',
            'wrong-name', 'wrong-size', 'not-uploaded', 'wrong-tag', 'draft', 'network-error', 'missing-file', 'directory')) {
        $scenario = $case
        $bytes = if ($case -eq 'modified') { [byte[]](1, 2, 3, 5) } else { [byte[]](1, 2, 3, 4) }
        [IO.File]::WriteAllBytes($sample, $bytes)
        $inputFile = if ($case -eq 'missing-file') { Join-Path $testDir 'missing.addon64' } elseif ($case -eq 'directory') { $testDir } else { $sample }
        & $verify -Path $inputFile -ReleaseTag 'v1.4.12' *> $output
        $code = $LASTEXITCODE
        $message = Get-Content -LiteralPath $output -Raw
        $expectedCode = if ($case -eq 'match') { 0 } else { 1 }
        if ($code -ne $expectedCode -or (($message -match '(?m)^MATCH:') -ne ($case -eq 'match'))) {
            throw "Failed $case (exit $code): $message"
        }
        Write-Output "PASS $case"
    }
    $rejected = $false
    try { & $verify -Path $sample -ReleaseTag '../latest' *> $output } catch { $rejected = $true }
    if (-not $rejected) { throw 'An invalid release tag was accepted.' }
    Write-Output 'PASS invalid-tag'
} finally {
    # Remove only the two known temporary files and our now-empty directory.
    foreach ($tempFile in @($sample, $output)) {
        if (Test-Path -LiteralPath $tempFile) { Remove-Item -LiteralPath $tempFile -Force }
    }
    Remove-Item -LiteralPath $testDir
}

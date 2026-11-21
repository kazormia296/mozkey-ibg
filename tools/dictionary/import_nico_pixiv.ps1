param(
    [ValidateSet("", "ncaq", "Niccori250k")]
    [string]$Source = ""
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "daily_source_lock.ps1")

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$OutDir = Join-Path $RepoRoot "src\data\dictionary_koyasi\generated\nico_pixiv"
$OutFile = Join-Path $OutDir "dic-nico-intersection-pixiv-google.txt"
$SourceId = "dic-nico-intersection-pixiv"
$PayloadId = "dictionary"

# Daily builds must never resolve a moving branch.  The reviewed source,
# revision, URL and SHA-256 all come from tools/dictionary/daily_sources.lock.json,
# which is the single lock shared with the Linux and native release pipelines.
$LockedSource = Get-DailyLockedSource -Id $SourceId
$Payload = Get-DailyLockedPayload -SourceId $SourceId -PayloadId $PayloadId
$Revision = [string]$LockedSource.revision
$RepositoryMatch = [regex]::Match(
    [string]$LockedSource.repository,
    '^https://github\.com/(?<owner>[A-Za-z0-9_.-]+)/dic-nico-intersection-pixiv\.git$')
if (-not $RepositoryMatch.Success -or $Revision -notmatch '^[0-9a-f]{40}$') {
    throw "Invalid pinned Nico/Pixiv repository or revision"
}
$LockedOwner = $RepositoryMatch.Groups['owner'].Value
$Repository = "$LockedOwner/dic-nico-intersection-pixiv"
if (-not $Source) {
    $Source = $LockedOwner
}
if ($Source -ne $LockedOwner) {
    throw "Only $LockedOwner is supported by the reviewed daily lock. Update lock via separate review."
}
$ExpectedHash = ([string]$Payload.sha256).ToUpperInvariant()
$Url = [string]$Payload.url
$ExpectedUrl = "https://raw.githubusercontent.com/$Repository/$Revision/$($Payload.path)"
if ($Url -ne $ExpectedUrl) {
    throw "Pinned Nico/Pixiv URL does not match the locked repository and revision: $Url"
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$TempFile = Join-Path $OutDir ("dic-nico-intersection-pixiv-google.txt." + [guid]::NewGuid().ToString("N") + ".tmp")

Write-Host "Source: $Source"
Write-Host "Upstream: SHA256-locked commit"
Write-Host "Resolved revision: $Revision"
Write-Host "Downloading: $Url"
Write-Host "Output: $OutFile"

try {
    Invoke-WebRequest -Uri $Url -OutFile $TempFile -UseBasicParsing
    $Bytes = (Get-Item -LiteralPath $TempFile).Length
    if ($Bytes -lt 5000000) {
        throw "Unexpectedly small nico/pixiv dictionary: $Bytes bytes"
    }
    # Reject truncated or unrecognized content before replacing last known good data.
    $Header = @(Get-Content -LiteralPath $TempFile -Encoding UTF8 -TotalCount 8)
    if (-not (@($Header | Where-Object { $_ -eq '# name: dic-nico-intersection-pixiv' }).Count -gt 0)) {
        throw 'Downloaded file does not look like dic-nico-intersection-pixiv.'
    }
    $ActualHash = (Get-FileHash -LiteralPath $TempFile -Algorithm SHA256).Hash
    if ($ActualHash -ne $ExpectedHash) {
        throw "Nico/Pixiv pinned SHA256 mismatch: expected $ExpectedHash got $ActualHash"
    }
    Assert-DailyLockedFile -SourceId $SourceId -PayloadId $PayloadId -Path $TempFile
    $LineCount = (Get-Content -LiteralPath $TempFile -Encoding UTF8 | Measure-Object -Line).Lines
    if ($LineCount -lt 80000 -or $LineCount -gt 250000) {
        throw "Unexpected dictionary line count: $LineCount"
    }
    Move-Item -LiteralPath $TempFile -Destination $OutFile -Force
} finally {
    if (Test-Path -LiteralPath $TempFile) {
        Remove-Item -LiteralPath $TempFile -Force
    }
}

# Record the actual revision and hash for build traceability without freezing
# subsequent imports. This file is under the Git-ignored generated directory.
$Provenance = [ordered]@{
    source = $Source
    repository = $Repository
    upstream_ref = $Revision
    resolved_revision = $Revision
    download_url = $Url
    sha256 = $ActualHash
    bytes = $Bytes
    lines = $LineCount
    retrieved_at_utc = [DateTime]::UtcNow.ToString('o')
}
$ManifestPath = Join-Path $OutDir 'dic-nico-intersection-pixiv-source.json'
$ManifestText = $Provenance | ConvertTo-Json -Depth 3
[System.IO.File]::WriteAllText($ManifestPath, $ManifestText + "`n", (New-Object System.Text.UTF8Encoding($false)))

Write-Host ''
Write-Host 'Downloaded and checked:'
Write-Host "  lines: $LineCount"
Write-Host "  bytes: $Bytes"
Write-Host "  sha256: $ActualHash"
Write-Host "  revision: $Revision"
Write-Host "  source-manifest: $ManifestPath"

param(
  [switch]$SkipDownload
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "daily_source_lock.ps1")

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$OutDir = Join-Path $RepoRoot "src\data\dictionary_koyasi\generated\personal_names"
$Bz2 = Join-Path $OutDir "mozcdic-ut-personal-names.txt.bz2"
$Txt = Join-Path $OutDir "mozcdic-ut-personal-names.txt"
$SourceId = "mozcdic-ut-personal-names"
$PayloadId = "dictionary_bz2"

# The reviewed repository, revision, URL and SHA-256 all come from
# tools/dictionary/daily_sources.lock.json, the single lock shared with the
# Linux and native release pipelines.
$LockedSource = Get-DailyLockedSource -Id $SourceId
$Payload = Get-DailyLockedPayload -SourceId $SourceId -PayloadId $PayloadId
$Revision = [string]$LockedSource.revision
if ([string]$LockedSource.repository -ne 'https://github.com/utuhiro78/mozcdic-ut-personal-names.git' -or
    $Revision -notmatch '^[0-9a-f]{40}$') {
    throw "Invalid pinned personal-names repository or revision"
}
$ExpectedHash = ([string]$Payload.sha256).ToUpperInvariant()
$Url = [string]$Payload.url
if ($Url -ne "https://raw.githubusercontent.com/utuhiro78/mozcdic-ut-personal-names/$Revision/$($Payload.path)") {
    throw "Pinned personal-names URL does not match the locked repository and revision: $Url"
}
$TxtDownload = $Txt + "." + [guid]::NewGuid().ToString('N') + ".tmp"

New-Item -ItemType Directory -Force $OutDir | Out-Null

if (-not $SkipDownload) {
  Write-Host "Downloading SHA256-locked personal names dictionary..."
  Write-Host "  $Url"
  $Tmp = $Bz2 + "." + [guid]::NewGuid().ToString('N') + ".tmp"
  try {
    Invoke-WebRequest -Uri $Url -OutFile $Tmp -UseBasicParsing
    if ((Get-FileHash -LiteralPath $Tmp -Algorithm SHA256).Hash -ne $ExpectedHash) {
      throw "Pinned personal names SHA256 mismatch"
    }
    Assert-DailyLockedFile -SourceId $SourceId -PayloadId $PayloadId -Path $Tmp
    Move-Item -LiteralPath $Tmp -Destination $Bz2 -Force
  } finally {
    if (Test-Path -LiteralPath $Tmp) { Remove-Item -LiteralPath $Tmp -Force }
  }
} else {
  Write-Host "Skipping download; verifying pinned archive."
}

if (-not (Test-Path -LiteralPath $Bz2 -PathType Leaf)) {
  throw "bz2 file does not exist: $Bz2"
}
if ((Get-FileHash -LiteralPath $Bz2 -Algorithm SHA256).Hash -ne $ExpectedHash) {
  throw "Pinned personal names archive differs from lock; refusing to unpack"
}

Assert-DailyLockedFile -SourceId $SourceId -PayloadId $PayloadId -Path $Bz2

$env:MOZKEY_PERSONAL_NAMES_BZ2 = (Resolve-Path $Bz2).Path
$env:MOZKEY_PERSONAL_NAMES_TXT = $TxtDownload

$PythonCode = "import bz2, os, pathlib; src = pathlib.Path(os.environ['MOZKEY_PERSONAL_NAMES_BZ2']); dst = pathlib.Path(os.environ['MOZKEY_PERSONAL_NAMES_TXT']); dst.write_bytes(bz2.decompress(src.read_bytes())); print(f'wrote: {dst}'); print(f'bytes: {dst.stat().st_size}')"

try {
  python -c $PythonCode
  if ($LASTEXITCODE -ne 0) {
    throw "failed to decompress mozcdic-ut personal names dictionary."
  }
  Assert-DailyLockedFile -SourceId $SourceId -PayloadId "dictionary_txt" -Path $TxtDownload
  Move-Item -LiteralPath $TxtDownload -Destination $Txt -Force
} finally {
  if (Test-Path -LiteralPath $TxtDownload) {
    Remove-Item -LiteralPath $TxtDownload -Force
  }
}

Write-Host ""
Write-Host "Personal names dictionary is ready:"
Write-Host "  $Txt"

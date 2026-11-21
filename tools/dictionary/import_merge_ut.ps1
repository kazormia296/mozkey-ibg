param(
    [ValidateSet("sample", "safe", "daily", "rich", "max")]
    [string]$Profile = "sample",
    [int]$SampleLines = 5000,
    [string]$BashPath = ""
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "daily_source_lock.ps1")

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$WorkRoot = Join-Path $RepoRoot "dist\dictionary\merge-ut"
$OutDir = Join-Path $RepoRoot "src\data\dictionary_koyasi\generated"
# Pin the upstream source used by both local builds and GitHub Actions.
# tools/dictionary/daily_sources.lock.json is the single reviewed lock shared
# with the Linux and native release pipelines; no revision is duplicated here.
# Bumping the locked revision requires revalidating the managed patch.
$MergeSource = Get-DailyLockedSource -Id "merge-ut-dictionaries"
$UpstreamRevision = [string]$MergeSource.revision
$ExpectedMergeScriptBlob = "778e4620117aa0eda85f7572a40413e2783cef9a"
$UpstreamUrl = [string]$MergeSource.repository
if ($UpstreamUrl -ne "https://github.com/utuhiro78/merge-ut-dictionaries.git" -or
    $UpstreamRevision -notmatch '^[0-9a-f]{40}$') {
    throw "Invalid pinned merge-ut repository or revision"
}
$RunRoot = Join-Path $WorkRoot ("run-" + [guid]::NewGuid().ToString("N"))
$MergeRepo = Join-Path $RunRoot "merge-ut-dictionaries"
$PatchPath = Join-Path $PSScriptRoot "patches\merge_ut_mozkey_local.patch"
$InputLockFile = $DailySourceLockPath
$PinTool = Join-Path $PSScriptRoot "pin_dictionary_inputs.py"
if (-not (Test-Path -LiteralPath $InputLockFile -PathType Leaf)) { throw "Missing dictionary lock: $InputLockFile" }
if (-not (Test-Path -LiteralPath $PinTool -PathType Leaf)) { throw "Missing dictionary lock tool: $PinTool" }
$WikiSourceId = "jawiki-dump-index"
$WikiPayloadId = "multistream_index"
$WikiSource = Get-DailyLockedSource -Id $WikiSourceId
$WikiPayload = Get-DailyLockedPayload -SourceId $WikiSourceId -PayloadId $WikiPayloadId
$WikiSnapshot = [string]$WikiSource.version
$WikiFileName = [string]$WikiPayload.path
$WikiUrl = [string]$WikiPayload.url
if ($WikiSnapshot -notmatch '^[0-9]{8}$' -or
    $WikiFileName -ne "jawiki-$WikiSnapshot-pages-articles-multistream-index.txt.bz2" -or
    $WikiUrl -ne "https://dumps.wikimedia.org/jawiki/$WikiSnapshot/$WikiFileName") {
    throw "Wikipedia must use a dated snapshot, not latest"
}
# The children that make.sh may clone.  Each is replaced by an exact revision
# fetch plus a SHA-256 check of its dictionary archive.
$PinnedChildren = [ordered]@{
    place_names    = "mozcdic-ut-place-names"
    sudachidict    = "mozcdic-ut-sudachidict"
    personal_names = "mozcdic-ut-personal-names"
}

if ($Profile -eq "safe") {
    $EffectiveProfile = "sample"
} else {
    $EffectiveProfile = $Profile
}

Write-Host "Repo root: $RepoRoot"
Write-Host "Work root: $WorkRoot"
Write-Host "Output dir: $OutDir"
Write-Host "Profile: $EffectiveProfile"
Write-Host "Upstream revision: $UpstreamRevision"
Write-Host "Isolated work root: $RunRoot"

New-Item -ItemType Directory -Force $WorkRoot | Out-Null
New-Item -ItemType Directory -Force $OutDir | Out-Null

function Test-IsWindowsHost {
    if ($PSVersionTable.ContainsKey("Platform")) {
        return $PSVersionTable.Platform -eq "Win32NT"
    }

    return $env:OS -eq "Windows_NT"
}

function Test-BashPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        return $false
    }

    try {
        $Output = & $Path -lc "printf MOZC_BASH_OK" 2>$null
        return ($LASTEXITCODE -eq 0 -and $Output -eq "MOZC_BASH_OK")
    } catch {
        return $false
    }
}

# A Bash executable alone is not a complete POSIX build environment.
# Choose the matching usr/bin directory (MSYS2 / Git for Windows) and check
# the actual utilities BEFORE cloning dependencies or replacing outputs.
function Get-BashToolsDirectory {
    param([string]$Candidate)

    $BashDirectory = Split-Path -Parent $Candidate
    if (-not (Test-IsWindowsHost)) { return $BashDirectory }

    $ParentDirectory = Split-Path -Parent $BashDirectory
    if ((Split-Path -Leaf $BashDirectory) -ieq 'bin' -and
        (Split-Path -Leaf $ParentDirectory) -ieq 'usr') {
        return $BashDirectory
    }

    $SiblingUsrBin = Join-Path $ParentDirectory 'usr\bin'
    if (Test-Path -LiteralPath $SiblingUsrBin -PathType Container) {
        return $SiblingUsrBin
    }
    return $BashDirectory
}

function Test-BashToolchain {
    param([string]$Candidate)
    if (-not (Test-BashPath $Candidate)) { return $false }

    $PreviousPath = $env:PATH
    try {
        if (Test-IsWindowsHost) {
            $ToolsDirectory = Get-BashToolsDirectory -Candidate $Candidate
            $env:PATH = "$ToolsDirectory;$PreviousPath"
        }
        # No login shell: profile startup files may replace the inherited PATH.
        $Probe = 'for utility in rm mv cat bzip2 sha256sum git python; do command -v "$utility" >/dev/null 2>&1 || exit 22; done'
        & $Candidate --noprofile --norc -c $Probe 2>$null | Out-Null
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    } finally {
        $env:PATH = $PreviousPath
    }
}

function Find-Bash {
    param(
        [string]$RequestedBashPath = ""
    )

    if ($RequestedBashPath) {
        if (Test-BashToolchain $RequestedBashPath) {
            return (Resolve-Path $RequestedBashPath).Path
        }
        throw "Specified bash lacks required tools (rm, mv, cat, bzip2, sha256sum, git, python): $RequestedBashPath"
    }

    if (Test-IsWindowsHost) {
        $BashCandidates = @(
            (Join-Path $RepoRoot "src\third_party\msys64\usr\bin\bash.exe"),
            (Join-Path $RepoRoot "src\third_party\msys64\bin\bash.exe"),
            "C:\Program Files\Git\bin\bash.exe",
            "C:\Program Files\Git\usr\bin\bash.exe",
            "C:\Program Files (x86)\Git\bin\bash.exe",
            "C:\Program Files (x86)\Git\usr\bin\bash.exe"
        )

        foreach ($Candidate in $BashCandidates) {
            if (Test-BashToolchain $Candidate) {
                return (Resolve-Path $Candidate).Path
            }
        }

        $PathBashes = @(Get-Command bash.exe -ErrorAction SilentlyContinue)
        foreach ($Command in $PathBashes) {
            $Source = $Command.Source

            # Do not select WSL bash launchers blindly. They depend on the
            # default WSL distribution and may resolve to docker-desktop,
            # which is not a normal Linux user environment.
            if ($Source -like "$env:WINDIR\System32\bash.exe" -or
                $Source -like "$env:LOCALAPPDATA\Microsoft\WindowsApps\bash.exe") {
                # WSL is not the MSYS/Git Bash environment used by make.sh.
                continue
            }

            if (Test-BashToolchain $Source) {
                return $Source
            }
        }

        throw "No complete Bash toolchain found (requires rm, mv, cat, bzip2, sha256sum, git, python). Check MSYS2 or Git for Windows usr\bin."
    }

    $Command = Get-Command bash -ErrorAction SilentlyContinue
    if ($Command -and (Test-BashToolchain $Command.Source)) {
        return $Command.Source
    }

    foreach ($Candidate in @("/bin/bash", "/usr/bin/bash")) {
        if (Test-BashToolchain $Candidate) {
            return $Candidate
        }
    }

    throw "No complete Bash toolchain found (requires rm, mv, cat, bzip2, sha256sum, git, python)."
}

$BashPath = Find-Bash -RequestedBashPath $BashPath
$BashToolsDirectory = Get-BashToolsDirectory -Candidate $BashPath
Write-Host "Using bash: $BashPath"
Write-Host "Using Bash tool directory: $BashToolsDirectory"
Write-Host "Required shell tools: rm, mv, cat, bzip2, sha256sum, git, python (preflight PASS)"

# Verify/copy/download the EXACT dated Wikipedia snapshot before building.
# The hash in daily_sources.lock.json is the only acceptable content identity.
$WikiCacheDir = Join-Path $RepoRoot "dist\dictionary\locked-sources"
$WikiIndexPath = Join-Path $WikiCacheDir $WikiFileName
New-Item -ItemType Directory -Force $WikiCacheDir | Out-Null
if (Test-Path -LiteralPath $WikiIndexPath -PathType Leaf) {
    # A cache with the wrong SHA-256 is never overwritten silently.
    Assert-DailyLockedFile -SourceId $WikiSourceId -PayloadId $WikiPayloadId -Path $WikiIndexPath
} else {
    # Do not touch generated dictionaries if a stable snapshot is unavailable.
    $WikiTemp = $WikiIndexPath + "." + [guid]::NewGuid().ToString("N") + ".part"
    try {
        Write-Host "Downloading SHA256-locked Wikipedia index: $WikiUrl"
        Invoke-WebRequest -Uri $WikiUrl -OutFile $WikiTemp -UseBasicParsing `
            -UserAgent "Mozkey-dictionary-reproducibility/1.0"
        Assert-DailyLockedFile -SourceId $WikiSourceId -PayloadId $WikiPayloadId -Path $WikiTemp
        Move-Item -LiteralPath $WikiTemp -Destination $WikiIndexPath
    } finally {
        if (Test-Path -LiteralPath $WikiTemp) {
            Remove-Item -LiteralPath $WikiTemp -Force
        }
    }
}
$WikiIndexPath = (Resolve-Path -LiteralPath $WikiIndexPath).Path
if (-not (Test-Path -LiteralPath $WikiIndexPath -PathType Leaf)) {
    throw "Pinned Wikipedia index file does not exist: $WikiIndexPath"
}
Write-Host "Locked Wikipedia index: $WikiIndexPath"
# Do not pull into the old dist/dictionary/merge-ut/merge-ut-dictionaries checkout:
# that checkout can have intentional local patches and generated assets.
# A disposable checkout prevents dirty upstream state from affecting CI or
# the developer's local dictionary preparation.
if (-not (Test-Path -LiteralPath $PatchPath -PathType Leaf)) {
    throw "Managed Mozkey patch is missing: $PatchPath"
}

New-Item -ItemType Directory -Force $RunRoot | Out-Null
& git -c core.autocrlf=false init -q $MergeRepo
if ($LASTEXITCODE -ne 0) { throw "git init failed." }
& git -C $MergeRepo remote add origin $UpstreamUrl
if ($LASTEXITCODE -ne 0) { throw "git remote add failed." }
& git -C $MergeRepo -c core.autocrlf=false fetch --depth 1 origin $UpstreamRevision
if ($LASTEXITCODE -ne 0) { throw "git fetch failed for pinned revision $UpstreamRevision." }
& git -C $MergeRepo -c core.autocrlf=false checkout -q --detach FETCH_HEAD
if ($LASTEXITCODE -ne 0) { throw "git checkout failed." }

$ActualRevision = (& git -C $MergeRepo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $ActualRevision -ne $UpstreamRevision) {
    throw "Upstream revision mismatch: $ActualRevision"
}

# The lock also pins the tree, so a revision cannot be re-pointed silently.
Assert-DailyLockedGitRevision -SourceId "merge-ut-dictionaries" -RepositoryPath $MergeRepo

$MakePath = Join-Path $MergeRepo "src\merge\make.sh"
if (-not (Test-Path $MakePath)) {
    throw "make.sh was not found: $MakePath"
}

$MergeScript = Join-Path $MergeRepo "src\merge\merge_dictionaries.py"
# Verify the pristine upstream files before the managed patch touches them.
Assert-DailyLockedFile -SourceId "merge-ut-dictionaries" -PayloadId "make_script" -Path $MakePath
Assert-DailyLockedFile -SourceId "merge-ut-dictionaries" -PayloadId "merge_script" -Path $MergeScript
$ActualBlob = (& git -C $MergeRepo hash-object -- $MergeScript).Trim()
if ($LASTEXITCODE -ne 0 -or $ActualBlob -ne $ExpectedMergeScriptBlob) {
    throw "Upstream merge_dictionaries.py changed: $ActualBlob. Review patch before continuing."
}

& git -C $MergeRepo apply --unidiff-zero --check $PatchPath
if ($LASTEXITCODE -ne 0) { throw "Managed Mozkey patch does not apply cleanly." }
& git -C $MergeRepo apply --unidiff-zero $PatchPath
if ($LASTEXITCODE -ne 0) { throw "Managed Mozkey patch application failed." }
Write-Host "Managed merge_dictionaries.py patch applied: PASS"

$ContractTest = Join-Path $PSScriptRoot "tests\test_merge_ut_patch.py"
& python $ContractTest --script $MergeScript
if ($LASTEXITCODE -ne 0) { throw "Patched merge_dictionaries.py contract test failed." }
Write-Host "Mozkey dictionary patch contract: PASS"

$Flags = @{
    alt_cannadic   = $false
    edict2         = $false
    jawiki         = $false
    neologd        = $false
    personal_names = $false
    place_names    = $false
    skk_jisyo      = $false
    sudachidict    = $false
    generate_latest = $false
}

switch ($EffectiveProfile) {
    "sample" {
        # Minimal and reproducible first profile.
        $Flags.place_names = $true
        $Flags.sudachidict = $true
    }

    "daily" {
        # Current daily baseline.
        # Keep this conservative until we add cost profiling.
        $Flags.place_names = $true
        $Flags.sudachidict = $true
    }

    "rich" {
        # Broad recall profile, but not full legacy/GPL-heavy set.
        $Flags.jawiki = $true
        $Flags.neologd = $true
        $Flags.personal_names = $true
        $Flags.place_names = $true
        $Flags.sudachidict = $true
    }

    "max" {
        # Experimental full-recall profile.
        $Flags.alt_cannadic = $true
        $Flags.edict2 = $true
        $Flags.jawiki = $true
        $Flags.neologd = $true
        $Flags.personal_names = $true
        $Flags.place_names = $true
        $Flags.skk_jisyo = $true
        $Flags.sudachidict = $true
    }

    default {
        throw "Unknown profile: $EffectiveProfile"
    }
}

Write-Host "Enabled dictionaries:"
foreach ($Key in $Flags.Keys | Sort-Object) {
    if ($Key -eq "generate_latest") {
        continue
    }

    if ($Flags[$Key]) {
        Write-Host "  + $Key"
    }
}

Write-Host "Disabled dictionaries:"
foreach ($Key in $Flags.Keys | Sort-Object) {
    if ($Key -eq "generate_latest") {
        continue
    }

    if (-not $Flags[$Key]) {
        Write-Host "  - $Key"
    }
}

function Set-MakeFlag {
    param(
        [string]$Content,
        [string]$Name,
        [bool]$Enabled
    )

    if ($Enabled) {
        $Replacement = "$Name=`"true`""
    } else {
        $Replacement = "#$Name=`"true`""
    }

    $Pattern = "(?m)^#?" + [System.Text.RegularExpressions.Regex]::Escape("$Name=`"true`"") + "(?=\r?$)"
    $MatchCount = [System.Text.RegularExpressions.Regex]::Matches($Content, $Pattern).Count
    if ($MatchCount -ne 1) {
        throw "Expected one make.sh flag '$Name', found $MatchCount. Upstream layout changed."
    }
    return [System.Text.RegularExpressions.Regex]::Replace($Content, $Pattern, $Replacement)
}

function Set-PinnedGitClone {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Content,
        [Parameter(Mandatory = $true)]
        [string]$SourceId,
        [Parameter(Mandatory = $true)]
        [string]$Directory,
        [Parameter(Mandatory = $true)]
        [string]$PayloadId
    )

    $Source = Get-DailyLockedSource -Id $SourceId
    $Payload = Get-DailyLockedPayload -SourceId $SourceId -PayloadId $PayloadId
    if ([string]$Source.repository -ne "https://github.com/utuhiro78/$Directory.git" -or
        [string]$Source.revision -notmatch '^[0-9a-f]{40}$') {
        throw "Unexpected child repository identity or revision for $SourceId"
    }
    $Original = "git clone --depth 1 $($Source.repository)"
    $CloneCount = ($Content.Length - $Content.Replace($Original, "").Length) / $Original.Length
    if ($CloneCount -ne 1) {
        throw "Expected exactly one clone command for $SourceId, found $CloneCount"
    }
    $Replacement = @(
        "git init -q $Directory",
        "git -C $Directory config core.autocrlf false",
        "git -C $Directory remote add origin $($Source.repository)",
        "git -C $Directory fetch --depth 1 origin $($Source.revision)",
        "git -C $Directory checkout -q --detach FETCH_HEAD",
        "test `"`$(git -C $Directory rev-parse HEAD)`" = `"$($Source.revision)`" || exit 1",
        "printf '%s  %s\n' '$($Payload.sha256)' '$Directory/$($Payload.path)' | sha256sum -c - || exit 1"
    ) -join "`n"
    return $Content.Replace($Original, $Replacement)
}

Write-Host "Patching make.sh for profile: $EffectiveProfile"

$Content = Get-Content -Raw -Encoding UTF8 $MakePath

foreach ($Key in $Flags.Keys) {
    $Content = Set-MakeFlag -Content $Content -Name $Key -Enabled $Flags[$Key]
}

# The following edits affect ONLY the disposable merge-ut checkout.
# Replace moving child clones with exact Git revisions and archive digests.
foreach ($ChildDirectory in $PinnedChildren.Values) {
    $Content = Set-PinnedGitClone -Content $Content `
        -SourceId $ChildDirectory `
        -Directory $ChildDirectory `
        -PayloadId "dictionary_bz2"
}

# Upstream make.sh does not stop reliably on failures (e.g. missing bzip2).
# Add fail-fast behavior only to this disposable copy; never patch upstream.
$FirstNewline = $Content.IndexOf("`n")
if ($FirstNewline -lt 0 -or -not $Content.StartsWith('#!')) {
    throw "Unexpected make.sh header; cannot enable fail-fast safely."
}
$Content = $Content.Substring(0, $FirstNewline + 1) +
    "set -eo pipefail`n" + $Content.Substring($FirstNewline + 1)

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($MakePath, $Content, $Utf8NoBom)

# Disable merge-ut's latest-Wikipedia RSS lookup in favor of the pinned index.
# This reuses the pin tool's reviewed transformation; the child clones were
# already locked above from daily_sources.lock.json.
$PatchWikiCode = "import pathlib, sys; sys.path.insert(0, sys.argv[1]); import pin_dictionary_inputs as pin; path = pathlib.Path(sys.argv[2]); path.write_text(pin.patch_merge_python(path.read_text(encoding='utf-8')), encoding='utf-8', newline=chr(10))"
& python -c $PatchWikiCode $PSScriptRoot $MergeScript
if ($LASTEXITCODE -ne 0) { throw "Failed to lock Wikipedia index code" }
Write-Host "Pinned dependent clones and Wikipedia code: PASS"

$MergeDir = Join-Path $MergeRepo "src\merge"

# The managed patch reads Mozkey's own dictionary, never google/mozc master.
$DictionaryOssDir = Join-Path $RepoRoot "src\data\dictionary_oss"
if (-not (Test-Path -LiteralPath (Join-Path $DictionaryOssDir "id.def") -PathType Leaf)) {
    throw "Mozkey id.def is missing: $DictionaryOssDir"
}
$BaseDictFiles = @(Get-ChildItem -LiteralPath $DictionaryOssDir -File -Filter "dictionary0*.txt" | Sort-Object Name)
if ($BaseDictFiles.Count -eq 0) { throw "Mozkey base dictionaries not found: $DictionaryOssDir" }
$PreviousDictionaryDir = [Environment]::GetEnvironmentVariable("MOZKEY_DICTIONARY_OSS_DIR", "Process")
$PreviousWikiIndexPath = [Environment]::GetEnvironmentVariable("MOZKEY_JAWIKI_INDEX_BZ2", "Process")
$env:MOZKEY_JAWIKI_INDEX_BZ2 = $WikiIndexPath
$env:MOZKEY_DICTIONARY_OSS_DIR = (Resolve-Path -LiteralPath $DictionaryOssDir).Path

Write-Host "Running make.sh with Mozkey base dictionary..."
$PreviousPath = $env:PATH
Push-Location $MergeDir
try {
    if (Test-IsWindowsHost) {
        # This must be the usr/bin belonging to the SAME MSYS/Git Bash runtime.
        $env:PATH = "$BashToolsDirectory;$PreviousPath"
    }
    # Preserve the verified PATH; a login shell can unexpectedly replace it.
    # No nested bare 'bash': it could resolve to the WSL launcher.
    & $BashPath --noprofile --norc "./make.sh"
    if ($LASTEXITCODE -ne 0) {
        throw "make.sh failed with exit code $LASTEXITCODE. Work tree retained: $RunRoot"
    }
} finally {
    Pop-Location
    $env:PATH = $PreviousPath
    [Environment]::SetEnvironmentVariable("MOZKEY_DICTIONARY_OSS_DIR", $PreviousDictionaryDir, "Process")
    [Environment]::SetEnvironmentVariable("MOZKEY_JAWIKI_INDEX_BZ2", $PreviousWikiIndexPath, "Process")
}

# Fail before replacing existing output if any dependent source is unexpected.
foreach ($ChildFlag in $PinnedChildren.Keys) {
    if (-not $Flags[$ChildFlag]) { continue }
    $ChildName = $PinnedChildren[$ChildFlag]
    $ChildPath = Join-Path $MergeDir $ChildName
    Assert-DailyLockedGitRevision -SourceId $ChildName -RepositoryPath $ChildPath
    Assert-DailyLockedFile -SourceId $ChildName -PayloadId "dictionary_bz2" -Path (Join-Path $ChildPath "$ChildName.txt.bz2")
}
$Generated = Join-Path $MergeDir "mozcdic-ut.txt"
if (-not (Test-Path $Generated)) {
    throw "mozcdic-ut.txt was not generated."
}

if ($EffectiveProfile -eq "sample") {
    # Keep legacy file names used in the previous step.
    $OutFile = Join-Path $OutDir "mozcdic-ut-safe.txt"
    $SampleFile = Join-Path $OutDir "mozcdic-ut-sample.txt"
} else {
    $OutFile = Join-Path $OutDir "mozcdic-ut-$EffectiveProfile.txt"
    $SampleFile = Join-Path $OutDir "mozcdic-ut-$EffectiveProfile-sample.txt"
}

# Validate the source BEFORE touching the previously generated dictionary.
# A failing 'cat ... > mozcdic-ut.txt' creates an empty file even if cat fails.
$Size = (Get-Item -LiteralPath $Generated).Length
if ($Size -le 0) {
    throw "Generated dictionary is empty; existing output left intact: $Generated"
}
$LineCount = (Get-Content -LiteralPath $Generated -Encoding UTF8 | Measure-Object -Line).Lines
if ($LineCount -le 0) {
    throw "Generated dictionary has no entries; existing output left intact: $Generated"
}
$FirstLine = Get-Content -LiteralPath $Generated -Encoding UTF8 -TotalCount 1
if (@($FirstLine -split "`t").Count -ne 5) {
    throw "Generated dictionary has invalid first entry; existing output left intact: $Generated"
}

# Stage both files under generated/ and commit them only after validation.
$StageSuffix = "." + [guid]::NewGuid().ToString('N') + '.tmp'
$StagedFull = Join-Path $OutDir ((Split-Path -Leaf $OutFile) + $StageSuffix)
$StagedSample = $null
try {
    Copy-Item -LiteralPath $Generated -Destination $StagedFull -ErrorAction Stop
    if ((Get-Item -LiteralPath $StagedFull).Length -ne $Size) {
        throw "Staged dictionary size mismatch."
    }
    if ($SampleLines -gt 0) {
        [string[]]$Sample = @(Get-Content -LiteralPath $StagedFull -Encoding UTF8 -TotalCount $SampleLines)
        if ($Sample.Count -le 0) {
            throw "Generated sample dictionary would be empty."
        }
        $StagedSample = Join-Path $OutDir ((Split-Path -Leaf $SampleFile) + $StageSuffix)
        [System.IO.File]::WriteAllLines($StagedSample, $Sample, $Utf8NoBom)
        if ((Get-Item -LiteralPath $StagedSample).Length -le 0) {
            throw "Staged sample dictionary is empty."
        }
    }
    # Generated data can now replace previously valid files.
    Move-Item -LiteralPath $StagedFull -Destination $OutFile -Force
    if ($StagedSample) {
        Move-Item -LiteralPath $StagedSample -Destination $SampleFile -Force
    }
} finally {
    if (Test-Path -LiteralPath $StagedFull) {
        Remove-Item -LiteralPath $StagedFull -Force
    }
    if ($StagedSample -and (Test-Path -LiteralPath $StagedSample)) {
        Remove-Item -LiteralPath $StagedSample -Force
    }
}

Write-Host ""
Write-Host "Generated full dictionary:"
Write-Host "  $OutFile"
Write-Host "Lines:"
Write-Host "  $LineCount"
Write-Host "Bytes:"
Write-Host "  $Size"

if ($SampleLines -gt 0) {
    $SampleLineCount = (Get-Content -Encoding UTF8 $SampleFile | Measure-Object -Line).Lines
    $SampleSize = (Get-Item $SampleFile).Length

    Write-Host ""
    Write-Host "Generated sample dictionary:"
    Write-Host "  $SampleFile"
    Write-Host "Lines:"
    Write-Host "  $SampleLineCount"
    Write-Host "Bytes:"
    Write-Host "  $SampleSize"
}

# Metadata is recorded inside generated/ so CI's existing artifact step keeps it.
$IdDef = Join-Path $DictionaryOssDir "id.def"
$Inputs = @($IdDef) + @($BaseDictFiles | ForEach-Object { $_.FullName })
$InputHashes = @(
    foreach ($InputPath in $Inputs) {
        [ordered]@{
            path = [IO.Path]::GetFileName($InputPath)
            sha256 = (Get-FileHash -LiteralPath $InputPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
)
$Dependents = @(
    foreach ($DependentName in $PinnedChildren.Values) {
        $DependentPath = Join-Path $MergeDir $DependentName
        if (Test-Path -LiteralPath (Join-Path $DependentPath ".git")) {
            $DependentRevision = (& git -C $DependentPath rev-parse HEAD).Trim()
            if ($LASTEXITCODE -ne 0) { throw "Unable to record revision: $DependentName" }
            [ordered]@{ name = $DependentName; revision = $DependentRevision }
        }
    }
)
$Manifest = [ordered]@{
    upstream_repository = $UpstreamUrl
    upstream_revision = $UpstreamRevision
    upstream_merge_script_blob = $ExpectedMergeScriptBlob
    mozkey_patch_sha256 = (Get-FileHash -LiteralPath $PatchPath -Algorithm SHA256).Hash.ToLowerInvariant()
    daily_input_lock_sha256 = (Get-FileHash -LiteralPath $InputLockFile -Algorithm SHA256).Hash.ToLowerInvariant()
    wikipedia_snapshot = $WikiSnapshot
    wikipedia_sha256 = (Get-FileHash -LiteralPath $WikiIndexPath -Algorithm SHA256).Hash.ToLowerInvariant()
    wikipedia_url = $WikiUrl
    dictionary_inputs = $InputHashes
    dependent_repositories = $Dependents
    output_file = [IO.Path]::GetFileName($OutFile)
    output_sha256 = (Get-FileHash -LiteralPath $OutFile -Algorithm SHA256).Hash.ToLowerInvariant()
    output_lines = $LineCount
    generated_at_utc = [DateTime]::UtcNow.ToString("o")
}
$ManifestPath = Join-Path $OutDir "merge-ut-source.json"
[IO.File]::WriteAllText(
    $ManifestPath,
    (($Manifest | ConvertTo-Json -Depth 6) + "`n"),
    $Utf8NoBom
)
Write-Host "Manifest: $ManifestPath"

# Only the per-run checkout is deleted. Never reset/clean the old external repo.
Remove-Item -LiteralPath $RunRoot -Recurse -Force
Write-Host "Isolated work tree cleaned: $RunRoot"

Write-Host "Done."

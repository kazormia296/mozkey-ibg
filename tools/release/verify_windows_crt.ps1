# Copyright 2026 The Mozkey Authors

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("x64", "arm64")]
  [string]$Architecture,

  [Parameter(Mandatory = $false)]
  [string]$DestinationRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$requiredDlls = @(
  "msvcp140.dll",
  "msvcp140_1.dll",
  "msvcp140_2.dll",
  "vcruntime140.dll",
  "vcruntime140_1.dll"
)
$expectedToolsetDirectoryPattern = '^Microsoft\.VC\d+\.CRT$'

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
  throw "vswhere.exe was not found."
}

$installations = @(
  & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Where-Object {
      $_ -and (Test-Path -LiteralPath $_ -PathType Container)
    }
)
$vcvarsallCandidates = @(
  foreach ($installation in $installations) {
    $candidate = Join-Path $installation "VC\Auxiliary\Build\vcvarsall.bat"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      (Resolve-Path -LiteralPath $candidate).Path
    }
  }
)
$redistRoots = @(
  foreach ($vcvarsall in $vcvarsallCandidates) {
    $command = "`"$vcvarsall`" $Architecture >nul && set VCToolsRedistDir"
    $environmentLines = @(& cmd.exe /d /s /c $command)
    if ($LASTEXITCODE -ne 0) {
      throw "vcvarsall failed for ${Architecture}: $vcvarsall"
    }
    foreach ($line in $environmentLines) {
      if ($line -match '^VCToolsRedistDir=(.+)$') {
        $Matches[1].Trim()
        break
      }
    }
  }
)
$redistRoots = @(
  $redistRoots |
    Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) } |
    ForEach-Object { (Resolve-Path -LiteralPath $_).Path } |
    Select-Object -Unique
)
if ($redistRoots.Count -ne 1) {
  throw "Expected exactly one Visual Studio VCToolsRedistDir for $Architecture, found $($redistRoots.Count)."
}
$sourceRoot = $redistRoots[0]
$expectedRedistVersion = Split-Path -Leaf $sourceRoot
if ($expectedRedistVersion -notmatch '^\d+\.\d+\.\d+$') {
  throw "VCToolsRedistDir is not a versioned CRT root: $sourceRoot"
}
$expectedFileVersion = "14.52.37029.0"
$architectureRoot = Join-Path $sourceRoot $Architecture
if (-not (Test-Path -LiteralPath $architectureRoot -PathType Container)) {
  throw "Pinned CRT architecture directory is missing: $architectureRoot"
}
$sourceCandidates = @(
  Get-ChildItem -LiteralPath $architectureRoot -Directory |
    Where-Object { $_.Name -match $expectedToolsetDirectoryPattern } |
    ForEach-Object { $_.FullName }
)
if ($sourceCandidates.Count -ne 1) {
  throw "Expected exactly one pinned Microsoft CRT source directory, found $($sourceCandidates.Count)."
}
$sourceRoot = $sourceCandidates[0]
$sourceToolsetDirectory = Split-Path -Leaf $sourceRoot

if ($sourceToolsetDirectory -notmatch $expectedToolsetDirectoryPattern -or
    $sourceRoot -notmatch "\\$Architecture\\$sourceToolsetDirectory$") {
  throw "CRT source directory has an unexpected architecture or toolset: $sourceRoot"
}
if ([string]::IsNullOrWhiteSpace($DestinationRoot)) {
  $DestinationRoot = $sourceRoot
}

New-Item -ItemType Directory -Force $DestinationRoot | Out-Null
foreach ($dll in $requiredDlls) {
  $source = Join-Path $sourceRoot $dll
  if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Pinned CRT file is missing: $source"
  }

  $signature = Get-AuthenticodeSignature -LiteralPath $source
  if ($signature.Status -ne "Valid" -or
      $signature.SignerCertificate.Subject -notmatch "(?i)Microsoft") {
    throw "Pinned CRT file is not signed by Microsoft: $source"
  }

  $fileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($source).FileVersion
  if ($fileVersion -ne $expectedFileVersion) {
    throw "Pinned CRT file has unexpected version: $source ($fileVersion)"
  }

  $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
  $destination = Join-Path $DestinationRoot $dll
  if ([IO.Path]::GetFullPath($source) -ne [IO.Path]::GetFullPath($destination)) {
    Copy-Item -LiteralPath $source -Destination $destination -Force
  }
  $destinationHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($sourceHash -ne $destinationHash) {
    throw "CRT copy hash mismatch: $dll"
  }

  Write-Host "$dll version=$fileVersion sha256=$sourceHash signer=$($signature.SignerCertificate.Subject)"
}

Write-Host "Prepared exact Microsoft CRT $expectedRedistVersion/$sourceToolsetDirectory for $Architecture"

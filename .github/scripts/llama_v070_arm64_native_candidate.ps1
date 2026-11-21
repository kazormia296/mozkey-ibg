#requires -Version 7.0
# Isolated native ARM64 candidate build and smoke test for Mozkey's pinned llama.cpp.
# Does not install, stage, or modify the packaged Mozkey runtime.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$RepositoryRoot,
    [Parameter(Mandatory=$true)][string]$OutputRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$UpstreamCommit = '620d2225cc436ab0b001e3200e590e3b286ef9ab'
$PatchHash = '6DB5C11B2DA8415D6B37200EC6AA4F3FDCDDE8EFB1E7A71375FE331A7B0E829A'
$PatchedVocabHash = '2CEF3BDD758D9F2EB4CB80C1FC149C75461E9E32AF06E03D67D99E36FF69BBED'
$ModelRevision = 'c67e03e07d215c869f591b274c1631170d3e11fe'
$ModelHash = '29C223D4C23327B80FD13EBB5AB2555057A46317997D5DA391584FFBEF0DB673'
$ModelUrl = "https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf/resolve/$ModelRevision/ggml-model-Q5_K_M.gguf?download=true"

function SHA256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [Convert]::ToHexString($sha.ComputeHash($stream)) }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Assert-Hash([string]$Path,[string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing required file: $Path" }
    $actual = SHA256 $Path
    if ($actual -ne $Expected) { throw "SHA256 mismatch: $Path`nExpected=$Expected`nActual=$actual" }
    return $actual
}
function Invoke-Checked([string]$Caption,[scriptblock]$Command) {
    Write-Host "===== $Caption ====="
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Caption exited with $LASTEXITCODE" }
}
function PEMachine([string]$FilePath) {
    $s=[IO.File]::OpenRead($FilePath)
    $reader=[IO.BinaryReader]::new($s)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Missing MZ header' }
        [void]$s.Seek(0x3C,[IO.SeekOrigin]::Begin)
        $offset=$reader.ReadInt32()
        if ($offset -lt 64 -or $offset+6 -gt $s.Length) { throw 'Invalid PE offset' }
        [void]$s.Seek($offset,[IO.SeekOrigin]::Begin)
        if ($reader.ReadUInt32() -ne 0x4550) { throw 'Missing PE signature' }
        return $reader.ReadUInt16()
    } finally { $reader.Dispose() }
}
function LocalPort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    try { $listener.Start(); return [int]$listener.LocalEndpoint.Port }
    finally { $listener.Stop() }
}

$RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if ($OutputRoot.StartsWith(($RepositoryRoot.TrimEnd('\')+'\'),[StringComparison]::OrdinalIgnoreCase) -or
    $OutputRoot.TrimEnd('\') -eq $RepositoryRoot.TrimEnd('\')) {
    throw 'OutputRoot must be separate from Mozkey repository'
}
if (Test-Path -LiteralPath $OutputRoot) { throw 'Refusing to overwrite an existing candidate output directory' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$evidence=Join-Path $OutputRoot 'evidence'
$candidateDir=Join-Path $OutputRoot 'candidate'
$source=Join-Path $OutputRoot 'llama.cpp'
$build=Join-Path $OutputRoot 'build-arm64'
New-Item -ItemType Directory -Force -Path $evidence,$candidateDir | Out-Null
$report=Join-Path $evidence 'RESULT.txt'
$transcript=Join-Path $evidence 'audit.log'
$lines=[Collections.Generic.List[string]]::new()
$passed=$false
$server=$null
$transcribing=$false
try {
    Start-Transcript -LiteralPath $transcript | Out-Null
    $transcribing=$true
    Write-Host '===== NATIVE ARM64 LLAMA v0.7.0 CANDIDATE ====='
    $lines.Add('WorkflowVersion=v2-native-arm64-vs2026-msvc-native')
    $lines.Add('NativeCPUFeaturePortability=UNVERIFIED_RUNNER_SPECIFIC')
    $lines.Add('UpstreamCommitExpected='+$UpstreamCommit)
    if ($env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { throw "Native ARM64 runner required: $env:PROCESSOR_ARCHITECTURE" }
    $lines.Add('RunnerArchitecture=ARM64')
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'CMake not found' }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git not found' }
    $cmakeVersion = (& cmake --version | Select-Object -First 1)
    $lines.Add("CMakeVersion=$cmakeVersion")
    $patch=Join-Path $RepositoryRoot 'src/mac/installer/zenz_runtime/patches/llama-vocab-gpt2-small-japanese-char.patch'
    $lines.Add('TokenizerPatchSHA256='+(Assert-Hash $patch $PatchHash))
    $head=(& git -C $RepositoryRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Git identity check failed' }
    $lines.Add("MozkeyHEAD=$head")
    $changed=@(& git -C $RepositoryRoot status --porcelain)
    if ($LASTEXITCODE -ne 0 -or $changed.Count -ne 0) { throw 'Mozkey worktree not clean; refusing to continue' }

    Invoke-Checked 'CLONE EXACT UPSTREAM TAG' {
        & git -c core.autocrlf=false clone --depth 1 --filter=blob:none --single-branch --branch v0.7.0 'https://github.com/ggml-org/llama.cpp.git' $source
    }
    $commit=(& git -C $source rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $commit -ne $UpstreamCommit) { throw "Pinned upstream commit mismatch: $commit" }
    $lines.Add('UpstreamCommitVerified='+$commit)
    $parser=Join-Path $source 'common/chat-peg-parser.cpp'
    if (-not [regex]::IsMatch([IO.File]::ReadAllText($parser),'pending_tool_call\.reset\(\);\s*current_tool\s*=\s*nullptr\s*;')) {
        throw 'Upstream CVE parser fix not found'
    }
    $lines.Add('UpstreamParserSecurityFix=PASS')
    Invoke-Checked 'CHECK TRACKED MOZKEY TOKENIZER PATCH' { & git -C $source apply --check $patch }
    Invoke-Checked 'APPLY TRACKED MOZKEY TOKENIZER PATCH' { & git -C $source apply $patch }
    $vocab=Join-Path $source 'src/llama-vocab.cpp'
    $lines.Add('PatchedVocabSHA256='+(Assert-Hash $vocab $PatchedVocabHash))
    Invoke-Checked 'CHECK ISOLATED PATCH DIFF' { & git -C $source diff --check }
    $paths=@(& git -C $source diff --name-only)
    if ($LASTEXITCODE -ne 0 -or $paths.Count -ne 1 -or $paths[0] -ne 'src/llama-vocab.cpp') {
        throw 'Unexpected modified upstream source paths'
    }
    $lines.Add('SourceChanges=TRACKED_TOKENIZER_PATCH_ONLY')

    # The Windows 11 VS2026 ARM64 runner builds natively; this bypasses the local
    # x64 -> ARM64 cross compilation and its MSBuild SDK availability failure.
    $cmakeArgs=@('-S',$source,'-B',$build,'-G','Visual Studio 18 2026','-A','ARM64',
        '-DGGML_STATIC=ON','-DBUILD_SHARED_LIBS=OFF','-DGGML_NATIVE=ON',
        '-DGGML_OPENMP=OFF','-DGGML_BLAS=OFF','-DGGML_CPU_KLEIDIAI=OFF',
        '-DGGML_CUDA=OFF','-DGGML_VULKAN=OFF',
        '-DLLAMA_BUILD_COMMON=ON','-DLLAMA_BUILD_EXAMPLES=OFF','-DLLAMA_BUILD_TESTS=OFF',
        '-DLLAMA_BUILD_SERVER=ON','-DLLAMA_BUILD_TOOLS=ON',
        '-DLLAMA_BUILD_MTMD=OFF','-DLLAMA_BUILD_UI=OFF',
        '-DLLAMA_USE_PREBUILT_UI=OFF','-DLLAMA_OPENSSL=OFF')
    Invoke-Checked 'CONFIGURE NATIVE ARM64 VS2026' { & cmake @cmakeArgs }
    $cacheText=[IO.File]::ReadAllText((Join-Path $build 'CMakeCache.txt'))
    if ($cacheText -notmatch '(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=ARM64\s*$') { throw 'CMake platform is not ARM64' }
    if ($cacheText -notmatch '(?m)^CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\s*$') { throw 'CMake generator mismatch' }
    $lines.Add('CMakeArchitecture=ARM64')
    $lines.Add('CMakeCompilerDetection=PASS')
    Invoke-Checked 'BUILD LLAMA-SERVER NATIVE ARM64 RELEASE' {
        & cmake --build $build --config Release --target llama-server --parallel 4
    }
    $binary=Join-Path $build 'bin/Release/llama-server.exe'
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Missing ARM64 binary: $binary" }
    $machine=PEMachine $binary
    if ($machine -ne 0xAA64) { throw ('Unexpected PE Machine 0x{0:X4}' -f $machine) }
    $lines.Add('CandidatePEMachine=0xAA64')
    $lines.Add('CandidateSHA256='+(SHA256 $binary))
    Copy-Item -LiteralPath $binary -Destination (Join-Path $candidateDir 'llama-server.exe')
    $lines.Add('Build=PASS_NATIVE_ARM64')

    $dumpbin=Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($null -ne $dumpbin) {
        $deps=@(& $dumpbin.Source /DEPENDENTS $binary 2>&1)
        if ($LASTEXITCODE -ne 0) { throw 'dumpbin /DEPENDENTS failed' }
        $deps | Set-Content -LiteralPath (Join-Path $evidence 'dependencies.txt') -Encoding utf8
        if (($deps -join "`n") -match '(?i)(?:ggml|llama|libomp|vulkan|cudart|libcrypto)\S*\.dll') {
            throw 'Forbidden external inference-engine DLL dependency'
        }
        $lines.Add('ForbiddenThirdPartyDllDependencies=NONE_DETECTED')
    } else {
        $lines.Add('DependencyAudit=SKIPPED_DUMPBIN_UNAVAILABLE')
    }
    Invoke-Checked 'RUN NATIVE ARM64 LLAMA-SERVER VERSION' { & $binary --version }

    Write-Host '===== DOWNLOAD EXACT MODEL FOR NATIVE SMOKE (NOT PACKAGED) ====='
    $model=Join-Path $OutputRoot 'zenz-v3.2-small-Q5_K_M.gguf'
    Invoke-WebRequest -Uri $ModelUrl -OutFile $model -MaximumRedirection 10 -TimeoutSec 900
    $lines.Add('ModelSHA256='+(Assert-Hash $model $ModelHash))
    $port=LocalPort
    $apiKey='mozkey-ci-'+[guid]::NewGuid().ToString('N')
    $stdout=Join-Path $evidence 'llama-stdout.log'
    $stderr=Join-Path $evidence 'llama-stderr.log'
    $serverArgs=@('-m',('"'+$model+'"'),'-c','256','-t','4','--parallel','1','--cache-ram','0',
        '--host','127.0.0.1','--port',"$port",'--api-key',$apiKey)
    $server=Start-Process -FilePath $binary -ArgumentList $serverArgs -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $baseUrl="http://127.0.0.1:$port"
    $ready=$false
    for($attempt=0;$attempt -lt 450;$attempt++) {
        if($server.HasExited) { throw "Native llama-server exited during load: $($server.ExitCode)" }
        try {
            $r=Invoke-WebRequest -Uri "$baseUrl/health" -TimeoutSec 3 -SkipHttpErrorCheck
            if([int]$r.StatusCode -eq 200) { $ready=$true; break }
        } catch {}
        Start-Sleep -Milliseconds 400
    }
    if (-not $ready) { throw 'Native ARM64 llama-server health timeout' }
    $lines.Add('NativeHealth=PASS')
    $stop=@(0..15 | ForEach-Object { [string][char](0xEE00+$_) }) + @("`n","`r")
    $vectors=@(
        @{Name='dentist';Context='歯が痛いので';Kana='ハイシャ';Expected='歯医者'},
        @{Name='scrap_car';Context='車が壊れたので';Kana='ハイシャ';Expected='廃車'},
        @{Name='japanese_input';Context='パソコンで';Kana='ニホンゴニュウリョク';Expected='日本語入力'},
        @{Name='smartphone_count';Context='';Kana='スマートフォンイチダイ';Expected='スマートフォン1台'},
        @{Name='yen_100';Context='代金は';Kana='ヒャクエンデス';Expected='100円です'},
        @{Name='greeting';Context='';Kana='オハヨウゴザイマス';Expected='おはようございます'}
    )
    $authProbe=@{prompt=([string][char]0xEE02+[string][char]0xEE00+'ニホンゴ'+[string][char]0xEE01);n_predict=8;temperature=0} | ConvertTo-Json -Compress
    $unauthorized=Invoke-WebRequest -Method POST -Uri "$baseUrl/completion" -Body $authProbe -ContentType 'application/json' -SkipHttpErrorCheck -TimeoutSec 30
    if ([int]$unauthorized.StatusCode -ne 401) { throw "Unauthenticated completion was not rejected: HTTP $($unauthorized.StatusCode)" }
    $lines.Add('ApiKeyUnauthorizedAccess=HTTP401_PASS')
    $review=[Collections.Generic.List[string]]::new()
    $semanticFailureCount=0
    foreach($v in $vectors) {
        $prompt=[string][char]0xEE02+$v.Context+[string][char]0xEE00+$v.Kana+[string][char]0xEE01
        $body=[ordered]@{prompt=$prompt;n_predict=64;temperature=0.0;top_k=1;top_p=1.0;stream=$false;cache_prompt=$true;stop=$stop} | ConvertTo-Json -Depth 6 -Compress
        $resp=Invoke-RestMethod -Method POST -Uri "$baseUrl/completion" -Body $body -ContentType 'application/json' -Headers @{Authorization=('Bearer '+$apiKey)} -TimeoutSec 120
        $text=[string]$resp.content
        $ok=($text -ceq [string]$v.Expected)
        $review.Add("$($v.Name): pass=$ok expected=$($v.Expected) actual=$text")
        Write-Host ("$($v.Name): $text [expected=$($v.Expected), pass=$ok]")
        if (-not $ok) { $semanticFailureCount++ }
    }
    $review | Set-Content -LiteralPath (Join-Path $evidence 'semantic-gate.txt') -Encoding utf8
    $lines.Add(('NativeZenzSemantic={0}/{1}_EXACT' -f ($vectors.Count - $semanticFailureCount), $vectors.Count))
    if ($semanticFailureCount -gt 0) { throw "$semanticFailureCount native semantic comparison(s) differed from x64 reference; inspect semantic-gate.txt before treating as regression" }
    $lines.Add('NativeARM64Candidate=PASS')
    $lines.Add('MSIInstallOrStaging=NOT_PERFORMED')
    $passed=$true
}
catch {
    $lines.Add('Failure='+$_.Exception.Message)
    Write-Host ('ERROR: '+$_.Exception.Message) -ForegroundColor Red
}
finally {
    if($null -ne $server) {
        try { if(-not $server.HasExited) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue } } catch {}
        try { $server.Dispose() } catch {}
    }
    if($passed) { $lines.Insert(0,'MOZKEY_LLAMA_V070_NATIVE_ARM64_CANDIDATE=PASS') }
    else { $lines.Insert(0,'MOZKEY_LLAMA_V070_NATIVE_ARM64_CANDIDATE=FAIL') }
    $lines.Add('MozkeySourceAndInstalledRuntime=NOT_MODIFIED')
    $lines.Add('OutputDirectory='+$OutputRoot)
    $lines | Set-Content -LiteralPath $report -Encoding utf8
    if($transcribing) { Stop-Transcript | Out-Null }
    Write-Host '===== RESULT ====='
    $lines | ForEach-Object { Write-Host $_ }
}
if(-not $passed) { exit 1 }

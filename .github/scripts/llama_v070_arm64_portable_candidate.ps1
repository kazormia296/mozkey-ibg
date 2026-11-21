#requires -Version 7.0
# Isolated portable-baseline ARM64 ClangCL build and smoke test for pinned llama.cpp.
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
    Write-Host '===== PORTABLE-BASELINE ARM64 LLAMA v0.7.0 EXPERIMENT ====='
    $lines.Add('WorkflowVersion=v1-arm64-clangcl-portable-experiment')
    $lines.Add('PortabilityPolicy=ARMV8_A_BASELINE_BUILD_NOT_YET_MULTIDEVICE_VALIDATED')
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
    $lines.Add('ExpectedToolset=ClangCL')

    # MSVC requires GGML_NATIVE=ON on ARM64, which probes runner-local CPU features.
    # Use ClangCL to build an explicitly specified ARMv8-A baseline instead.
    # This is a candidate experiment, NOT multi-device release certification.
    $cmakeArgs=@('-S',$source,'-B',$build,'-G','Visual Studio 18 2026','-A','ARM64','-T','ClangCL',
        '-DGGML_STATIC=ON','-DBUILD_SHARED_LIBS=OFF','-DGGML_NATIVE=OFF',
        '-DGGML_CPU_ARM_ARCH=armv8-a','-DGGML_CPU_ALL_VARIANTS=OFF',
        '-DGGML_OPENMP=OFF','-DGGML_BLAS=OFF','-DGGML_CPU_KLEIDIAI=OFF',
        '-DGGML_CUDA=OFF','-DGGML_VULKAN=OFF',
        '-DLLAMA_BUILD_COMMON=ON','-DLLAMA_BUILD_EXAMPLES=OFF','-DLLAMA_BUILD_TESTS=OFF',
        '-DLLAMA_BUILD_SERVER=ON','-DLLAMA_BUILD_TOOLS=ON',
        '-DLLAMA_BUILD_MTMD=OFF','-DLLAMA_BUILD_UI=OFF',
        '-DLLAMA_USE_PREBUILT_UI=OFF','-DLLAMA_OPENSSL=OFF')
    Invoke-Checked 'CONFIGURE ARM64 CLANGCL ARMV8-A BASELINE' { & cmake @cmakeArgs }
    $cacheText=[IO.File]::ReadAllText((Join-Path $build 'CMakeCache.txt'))
    if ($cacheText -notmatch '(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=ARM64\s*$') { throw 'CMake platform is not ARM64' }
    if ($cacheText -notmatch '(?m)^CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\s*$') { throw 'CMake generator mismatch' }
    if ($cacheText -notmatch '(?m)^CMAKE_GENERATOR_TOOLSET:INTERNAL=ClangCL(?:,.*)?\s*$') { throw 'CMake did not select ClangCL toolset' }
    if ($cacheText -notmatch '(?m)^GGML_NATIVE:[^=]*=OFF\s*$') { throw 'GGML_NATIVE is not OFF' }
    if ($cacheText -notmatch '(?m)^GGML_CPU_ARM_ARCH:[^=]*=armv8-a\s*$') { throw 'ARMv8-A baseline not recorded by CMake' }
    if ($cacheText -notmatch '(?m)^BUILD_SHARED_LIBS:[^=]*=OFF\s*$') { throw 'Shared library build unexpectedly enabled' }
    $compilerInfo = @(Get-ChildItem -LiteralPath (Join-Path $build 'CMakeFiles') -Recurse -Filter CMakeCXXCompiler.cmake -File)
    if ($compilerInfo.Count -ne 1) { throw 'Expected one CMake C++ compiler identification file' }
    $compilerText=[IO.File]::ReadAllText($compilerInfo[0].FullName)
    if ($compilerText -notmatch 'set\(CMAKE_CXX_COMPILER_ID "Clang"\)') { throw 'CXX compiler is not Clang' }
    if ($compilerText -notmatch 'set\(CMAKE_CXX_COMPILER_FRONTEND_VARIANT "MSVC"\)') { throw 'CXX compiler is not MSVC-like Clang' }
    $lines.Add('CMakeArchitecture=ARM64')
    $lines.Add('CMakeCxxCompiler=ClangCL')
    $lines.Add('CMakeCompilerDetection=PASS')
    $lines.Add('GGML_NATIVE=OFF')
    $lines.Add('GGML_CPU_ARM_ARCH=armv8-a')
    Invoke-Checked 'BUILD ARM64 BASELINE CLANGCL RELEASE' {
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

    # Parse ordinary AND delay import tables with a separately checked,
    # standard-library-only PE parser.  Never silently skip the dependency gate.
    $python=Get-Command python.exe -ErrorAction Stop
    $peAudit=Join-Path $RepositoryRoot '.github/scripts/llama_v070_arm64_pe_import_audit.py'
    if (-not (Test-Path -LiteralPath $peAudit -PathType Leaf)) { throw 'Tracked PE audit script missing' }
    $peReport=Join-Path $evidence 'pe-imports.json'
    Invoke-Checked 'AUDIT PE IMPORTS INCLUDING DELAY-LOAD DLLS' {
        & $python.Source $peAudit $binary $peReport
    }
    $deps=Get-Content -LiteralPath $peReport -Raw | ConvertFrom-Json
    if ([int]$deps.machine -ne 43620) { throw 'PE dependency parser did not confirm ARM64' }
    if ($deps.policy -ne 'OS_OR_MSVC_CRT_ONLY') { throw 'PE dependency audit policy mismatch' }
    $lines.Add('DependencyAudit=PASS_NORMAL_AND_DELAY_IMPORTS')
    $lines.Add('ImportedDLLs='+((@($deps.imports)+@($deps.delay_imports) | Sort-Object -Unique) -join ','))
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
    $lines.Add('PortableARM64Candidate=PASS_ON_ONE_ARM64_RUNNER')
    $lines.Add('MSIInstallOrStaging=NOT_PERFORMED')
    $lines.Add('MultiDeviceCpuCompatibility=NOT_TESTED')
    $passed=$true
}
catch {
    $lines.Add('Failure='+$_.Exception.Message)
    Write-Host ('ERROR: '+$_.Exception.Message) -ForegroundColor Red
}
finally {
    # Keep native CMake's explanation in the artifact, especially if
    # the ClangCL toolset is absent or fails during configure.
    foreach ($fileSpec in @(
        @{Name='CMakeConfigureLog.yaml'; Path=(Join-Path $build 'CMakeFiles/CMakeConfigureLog.yaml')},
        @{Name='CMakeCache.txt'; Path=(Join-Path $build 'CMakeCache.txt')}
    )) {
        try {
            if (Test-Path -LiteralPath $fileSpec.Path -PathType Leaf) {
                Copy-Item -LiteralPath $fileSpec.Path -Destination (Join-Path $evidence $fileSpec.Name) -ErrorAction Stop
            }
        } catch { Write-Host ('Evidence copy warning: ' + $_.Exception.Message) }
    }
    if($null -ne $server) {
        try { if(-not $server.HasExited) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue } } catch {}
        try { $server.Dispose() } catch {}
    }
    if($passed) { $lines.Insert(0,'MOZKEY_LLAMA_V070_ARM64_PORTABLE_EXPERIMENT=PASS') }
    else { $lines.Insert(0,'MOZKEY_LLAMA_V070_ARM64_PORTABLE_EXPERIMENT=FAIL') }
    $lines.Add('MozkeySourceAndInstalledRuntime=NOT_MODIFIED')
    $lines.Add('OutputDirectory='+$OutputRoot)
    $lines | Set-Content -LiteralPath $report -Encoding utf8
    if($transcribing) { Stop-Transcript | Out-Null }
    Write-Host '===== RESULT ====='
    $lines | ForEach-Object { Write-Host $_ }
}
if(-not $passed) { exit 1 }

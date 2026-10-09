# Standalone ASR distribution; no OCR, FFmpeg, downloader or application UI.
param(
    [string]$BuildDir = 'build-asr-cuda',
    [string]$DistDir = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Resolve-ProjectPath([string]$value) {
    if ([IO.Path]::IsPathRooted($value)) { return [IO.Path]::GetFullPath($value) }
    return [IO.Path]::GetFullPath((Join-Path $root $value))
}
$build = Resolve-ProjectPath $BuildDir
$cache = Join-Path $build 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) { $cache = Join-Path (Split-Path -Parent $build) 'CMakeCache.txt' }
$text = Get-Content -LiteralPath $cache -Raw
if ($text -notmatch '(?m)^ENABLE_WHISPER:BOOL=ON\r?$') { throw 'Build requires ENABLE_WHISPER=ON' }
$mode = [regex]::Match($text, '(?m)^ASR_RUNTIME:STRING=(cpu|cuda)\r?$').Groups[1].Value
if (-not $mode) { throw 'Missing ASR_RUNTIME in build cache' }
if (-not $DistDir) { $DistDir = if ($mode -eq 'cuda') { 'dist-asr-gpu' } else { 'dist-asr' } }
$dist = Resolve-ProjectPath $DistDir
$whisper = [regex]::Match($text, '(?m)^WHISPER_DIR:PATH=([^\r\n]+)').Groups[1].Value
if (-not $whisper) { throw 'Missing WHISPER_DIR' }
$entries = @(
    @((Join-Path $build 'asr.dll'), 'asr.dll'),
    @((Join-Path $build 'asr_cli.exe'), 'asr_cli.exe'),
    @((Join-Path $build 'asr_example.exe'), 'asr_example.exe'),
    @((Join-Path $root 'sdk/asr.py'), 'asr.py'),
    @((Join-Path $root 'sdk/asr.hpp'), 'asr.hpp'),
    @((Join-Path $root 'src/asr_api.hpp'), 'asr_api.hpp'),
    @((Join-Path $root 'sdk/examples/asr_example.cpp'), 'asr_example.cpp'),
    @((Join-Path $root 'docs/ASR.md'), 'README-ASR.md'),
    @((Join-Path $root 'docs/ASR-UTF8.md'), 'ASR-UTF8.md'),
    @((Join-Path $root 'docs/ASR-REPETITION.md'), 'ASR-REPETITION.md'),
    @((Join-Path $whisper 'LICENSE'), 'Whisper-LICENSE')
)
$ggmlLicense = Join-Path $whisper 'ggml/LICENSE'
if (-not (Test-Path -LiteralPath $ggmlLicense)) { $ggmlLicense = Join-Path $whisper 'LICENSE' }
$entries += ,@($ggmlLicense, 'GGML-LICENSE')
foreach ($name in @('ggml-small-q5_1.bin', 'ggml-silero-v6.2.0.bin', 'MODEL.txt', 'VAD-MODEL.txt', 'LICENSE', 'Silero-LICENSE')) {
    $entries += ,@((Join-Path $root "models/whisper/$name"), "models/whisper/$name")
}
if ($mode -eq 'cuda') {
    $runtime = [regex]::Match($text, '(?m)^ASR_CUDA_DIR:PATH=([^\r\n]+)').Groups[1].Value
    if (-not $runtime) { throw 'Missing ASR_CUDA_DIR' }
    Get-ChildItem -LiteralPath $runtime -File |
        Where-Object { $_.Extension -eq '.dll' -or $_.Name -match 'LICENSE|^runtime.json$' } |
        ForEach-Object { $entries += ,@($_.FullName, $_.Name) }
}
# Preflight every input before updating the output directory.
foreach ($entry in $entries) {
    if (-not (Test-Path -LiteralPath $entry[0] -PathType Leaf)) { throw "Missing SDK file: $($entry[0])" }
}
if ($mode -eq 'cpu' -and (Test-Path -LiteralPath (Join-Path $dist 'ggml-cuda.dll'))) {
    throw 'Use a separate clean CPU output directory'
}
foreach ($entry in $entries) {
    $destination = Join-Path $dist $entry[1]
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $entry[0] -Destination $destination -Force
}
$manifest = @{ sdk = 'asr'; version = '0.3'; runtime = $mode; model = 'ggml-small-q5_1.bin'; vad_model = 'ggml-silero-v6.2.0.bin'; files = @{} }
foreach ($entry in $entries) {
    $manifest.files[$entry[1]] = (Get-FileHash -LiteralPath (Join-Path $dist $entry[1]) -Algorithm SHA256).Hash
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $dist 'asr-sdk.json') -Encoding UTF8
Write-Host "ASR SDK ready: $dist (CPU$(if($mode -eq 'cuda'){'/CUDA'}))"

# CPU: powershell -File tools/make_dist.ps1 -BuildDir build-cpu
# GPU: powershell -File tools/make_dist.ps1 -Runtime cuda -BuildDir build-gpu
param(
    [ValidateSet('cpu', 'cuda')][string]$Runtime = 'cpu',
    [string]$BuildDir = 'build',
    [string]$DistDir = '',
    [string]$CudaDllDir = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Resolve-ProjectPath([string]$value) {
    if ([IO.Path]::IsPathRooted($value)) { return [IO.Path]::GetFullPath($value) }
    return [IO.Path]::GetFullPath((Join-Path $root $value))
}
if (-not $DistDir) { $DistDir = if ($Runtime -eq 'cuda') { 'dist-gpu' } else { 'dist' } }
$build = Resolve-ProjectPath $BuildDir
$dist = Resolve-ProjectPath $DistDir
$cache = Join-Path $build 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) { $cache = Join-Path (Split-Path -Parent $build) 'CMakeCache.txt' }
if (-not (Test-Path -LiteralPath $cache)) { throw "Missing build configuration: $cache" }
$cacheText = Get-Content -LiteralPath $cache -Raw
$runtimeMatch = [regex]::Match($cacheText, '(?m)^OCR_RUNTIME:STRING=([^\r\n]+)')
if (-not $runtimeMatch.Success -or $runtimeMatch.Groups[1].Value -ne $Runtime) {
    throw "Build runtime does not match -Runtime $Runtime. Reconfigure CMake first."
}
$ortMatch = [regex]::Match($cacheText, '(?m)^ONNXRUNTIME_DIR:PATH=([^\r\n]+)')
if (-not $ortMatch.Success) { throw 'ONNXRUNTIME_DIR missing from CMake cache' }
$ort = Resolve-ProjectPath $ortMatch.Groups[1].Value
$cudaMatch = [regex]::Match($cacheText, '(?m)^OCR_CUDA_DLL_DIR:PATH=([^\r\n]+)')
if (-not $CudaDllDir -and $cudaMatch.Success) { $CudaDllDir = $cudaMatch.Groups[1].Value }
if ($Runtime -eq 'cuda') {
    foreach ($name in @('onnxruntime_providers_shared.dll', 'onnxruntime_providers_cuda.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $ort "lib\$name"))) { throw "GPU runtime missing $name" }
    }
}
if ($CudaDllDir -and $Runtime -ne 'cuda') { throw '-CudaDllDir requires -Runtime cuda' }
if ($CudaDllDir -and -not (Test-Path -LiteralPath (Resolve-ProjectPath $CudaDllDir))) {
    throw "CUDA DLL directory does not exist: $CudaDllDir"
}
$files = @('ocr.dll', 'ppocr_onnx.exe', 'ocrwatch.exe', 'ocrmon.exe',
           'region_picker.exe', 'screen_point_picker.exe', 'api_demo.exe', 'gpu_example.exe')
foreach ($name in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $name))) { throw "Build output missing $name" }
}
$models = @('v6_det_tiny.onnx', 'v6_rec_tiny.onnx', 'v6_tiny_dict.txt',
            'ch_PP-OCRv4_det_mobile.onnx', 'ch_PP-OCRv4_rec_mobile.onnx', 'ppocr_keys_v1.txt')
foreach ($name in $models) {
    if (-not (Test-Path -LiteralPath (Join-Path $root "models\onnx\$name"))) { throw "Model missing $name" }
}
if ($Runtime -eq 'cpu' -and (Test-Path -LiteralPath $dist)) {
    if (Get-ChildItem -LiteralPath $dist -Filter '*cuda*.dll') {
        throw 'Output contains CUDA DLLs; use a separate CPU distribution directory'
    }
}
New-Item -ItemType Directory -Force -Path $dist | Out-Null
foreach ($name in $files) {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $dist -Force
}
Get-ChildItem -LiteralPath (Join-Path $ort 'lib') -Filter '*.dll' -File |
    Copy-Item -Destination $dist -Force
foreach ($name in @('LICENSE', 'ThirdPartyNotices.txt')) {
    if (Test-Path -LiteralPath (Join-Path $ort $name)) {
        Copy-Item -LiteralPath (Join-Path $ort $name) -Destination (Join-Path $dist "ONNXRuntime-$name") -Force
    }
}
if ($CudaDllDir) {
    Get-ChildItem -LiteralPath (Resolve-ProjectPath $CudaDllDir) -Filter '*.dll' -File |
        Copy-Item -Destination $dist -Force
    $licenses = Join-Path (Resolve-ProjectPath $CudaDllDir) 'licenses'
    if (Test-Path -LiteralPath $licenses) {
        Copy-Item -LiteralPath $licenses -Destination (Join-Path $dist 'cuda-licenses') -Recurse -Force
    }
    $packages = Join-Path (Resolve-ProjectPath $CudaDllDir) 'packages.txt'
    if (Test-Path -LiteralPath $packages) {
        Copy-Item -LiteralPath $packages -Destination (Join-Path $dist 'cuda-packages.txt') -Force
    }
}
Copy-Item -LiteralPath (Join-Path $root 'sdk\ocr.hpp') -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root 'src\ocr_api.hpp') -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $dist -Force
$docs = Join-Path $dist 'docs'
New-Item -ItemType Directory -Force -Path $docs | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'docs\GPU.md') -Destination $docs -Force
foreach ($name in @('example.cpp', 'gpu_example.cpp')) {
    Copy-Item -LiteralPath (Join-Path $root "sdk\examples\$name") -Destination $dist -Force
}
$modelDir = Join-Path $dist 'models\onnx'
New-Item -ItemType Directory -Force -Path $modelDir | Out-Null
foreach ($name in $models) {
    Copy-Item -LiteralPath (Join-Path $root "models\onnx\$name") -Destination $modelDir -Force
}
Set-Content -LiteralPath (Join-Path $dist 'runtime.txt') -Value "runtime=$Runtime" -Encoding UTF8
Write-Host "Distribution ready: $dist ($Runtime)"

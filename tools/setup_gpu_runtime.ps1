# Download only into this project; no machine-wide installation or PATH changes.
param([string]$Version = '1.23.2', [switch]$WithCudaDependencies)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Version must be x.y.z' }
$root = Split-Path -Parent $PSScriptRoot
$archiveName = "onnxruntime-win-x64-gpu-$Version"
$cache = Join-Path $root '.source'
$destination = Join-Path $root 'third_party\onnxruntime-gpu'
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$zip = Join-Path $cache "$archiveName.zip"
$uri = "https://github.com/microsoft/onnxruntime/releases/download/v$Version/$archiveName.zip"
if (-not (Test-Path -LiteralPath $zip)) {
    Write-Host "Downloading $uri"
    Invoke-WebRequest -Uri $uri -OutFile $zip -UseBasicParsing
}
Expand-Archive -LiteralPath $zip -DestinationPath $cache -Force
$package = Join-Path $cache $archiveName
foreach ($file in @('include\onnxruntime_c_api.h', 'lib\onnxruntime.dll',
                   'lib\onnxruntime_providers_shared.dll', 'lib\onnxruntime_providers_cuda.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $file))) {
        throw "Invalid ONNX Runtime GPU package: missing $file"
    }
}
New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($name in @('include', 'lib')) {
    Copy-Item -LiteralPath (Join-Path $package $name) -Destination $destination -Recurse -Force
}
foreach ($name in @('LICENSE', 'ThirdPartyNotices.txt', 'VERSION_NUMBER')) {
    if (Test-Path -LiteralPath (Join-Path $package $name)) {
        Copy-Item -LiteralPath (Join-Path $package $name) -Destination $destination -Force
    }
}
Write-Host "GPU runtime ready: $destination"
Write-Host 'CUDA/cuDNN are separate prerequisites; see docs/GPU.md.'
if ($WithCudaDependencies) {
    # Pinned Windows x64 wheels. These also support RTX 50-series GPUs.
    $pythonDir = Join-Path $cache 'cuda-python'
    $cudaDir = Join-Path $root 'third_party\cuda-dlls'
    $packageVersions = 'cuda-runtime=12.9.79;cublas=12.9.1.4;cufft=11.4.1.4;cudnn=9.9.0.52;nvrtc=12.9.86'
    $manifest = Join-Path $cudaDir 'packages.txt'
    $expected = @('cudart64_12.dll', 'cublas64_12.dll', 'cublasLt64_12.dll',
                  'cufft64_11.dll', 'cudnn64_9.dll', 'nvrtc64_120_0.dll')
    $ready = $true
    if (-not (Test-Path -LiteralPath $manifest) -or
        (Get-Content -LiteralPath $manifest -Raw).Trim() -ne $packageVersions) { $ready = $false }
    foreach ($name in $expected) {
        if (-not (Test-Path -LiteralPath (Join-Path $cudaDir $name))) { $ready = $false }
    }
    if (-not $ready) {
        python -m pip install --upgrade --target $pythonDir --no-deps --no-warn-script-location `
            nvidia-cuda-runtime-cu12==12.9.79 nvidia-cublas-cu12==12.9.1.4 `
            nvidia-cufft-cu12==11.4.1.4 nvidia-cudnn-cu12==9.9.0.52 nvidia-cuda-nvrtc-cu12==12.9.86
        if ($LASTEXITCODE -ne 0) { throw 'CUDA dependency download failed' }
        New-Item -ItemType Directory -Force -Path $cudaDir | Out-Null
        Get-ChildItem -LiteralPath $pythonDir -Recurse -Filter '*.dll' -File |
            Copy-Item -Destination $cudaDir -Force
        $licenses = Join-Path $cudaDir 'licenses'
        New-Item -ItemType Directory -Force -Path $licenses | Out-Null
        foreach ($package in Get-ChildItem -LiteralPath $pythonDir -Directory -Filter '*.dist-info') {
            if (Test-Path -LiteralPath (Join-Path $package.FullName 'licenses')) {
                Copy-Item -LiteralPath (Join-Path $package.FullName 'licenses') `
                    -Destination (Join-Path $licenses $package.Name) -Recurse -Force
            }
            foreach ($name in @('License.txt', 'LICENSE.txt', 'LICENSE')) {
                if (Test-Path -LiteralPath (Join-Path $package.FullName $name)) {
                    Copy-Item -LiteralPath (Join-Path $package.FullName $name) `
                        -Destination (Join-Path $licenses "$($package.Name)-$name") -Force
                }
            }
        }
        [IO.File]::WriteAllText($manifest, $packageVersions + "`r`n")
    }
    Write-Host "Local CUDA DLLs ready: $cudaDir"
    Write-Host 'Configure with -DOCR_CUDA_DLL_DIR=third_party/cuda-dlls (or an absolute path).'
}

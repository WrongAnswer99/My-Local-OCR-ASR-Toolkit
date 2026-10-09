# Prepare source/model inside the project. No Python installation or system PATH changes.
param([ValidateSet('https://huggingface.co','https://hf-mirror.com')][string]$ModelHost='https://huggingface.co')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$cache=Join-Path $root '.source\whisper'
$source=Join-Path $root 'third_party\whisper.cpp'
$models=Join-Path $root 'models\whisper'
$version='1.9.5'
$modelName='ggml-small-q5_1.bin'
$modelSha='ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb'
New-Item -ItemType Directory -Force -Path $cache,$models | Out-Null
function Download([string]$url,[string]$destination){
    Write-Host "Downloading $url"
    # Keep TLS certificate validation; disable only unavailable Windows
    # revocation retrieval. Write to a partial file until curl succeeds.
    & curl.exe --ssl-no-revoke -sS -L --fail --retry 2 --max-time 1800 $url -o ($destination+'.partial')
    if($LASTEXITCODE -ne 0){throw "Download failed: $url"}
    Move-Item -LiteralPath ($destination+'.partial') -Destination $destination -Force
}
$marker=Join-Path $source 'OCRtest-version.txt'
if(-not (Test-Path -LiteralPath $marker) -or ([IO.File]::ReadAllText($marker).Trim() -ne $version)){
    if(Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt')){
        throw 'Whisper source already exists without the matching version marker; use a clean source directory or verify it manually.'
    }
    $archive=Join-Path $cache "v$version-complete.zip"
    if(-not (Test-Path -LiteralPath $archive)){Download "https://codeload.github.com/ggml-org/whisper.cpp/zip/refs/tags/v$version" $archive}
    Expand-Archive -LiteralPath $archive -DestinationPath $cache -Force
    $package=Join-Path $cache "whisper.cpp-$version"
    New-Item -ItemType Directory -Force -Path $source | Out-Null
    Get-ChildItem -LiteralPath $package -Force | Copy-Item -Destination $source -Recurse -Force
    [IO.File]::WriteAllText($marker,$version+"`r`n")
}
$model=Join-Path $models $modelName
if(-not (Test-Path -LiteralPath $model)){Download "$ModelHost/ggerganov/whisper.cpp/resolve/main/$modelName`?download=true" $model}
if((Get-FileHash -LiteralPath $model -Algorithm SHA256).Hash.ToLowerInvariant() -ne $modelSha){
    throw 'Whisper model SHA-256 mismatch; do not use the model.'
}
$modelLicense=Join-Path $models 'LICENSE'
if(-not (Test-Path -LiteralPath $modelLicense)){
    $licenseJson=Join-Path $cache 'openai-license.json'
    if(-not (Test-Path -LiteralPath $licenseJson)){Download 'https://api.github.com/repos/openai/whisper/contents/LICENSE' $licenseJson}
    $license=Get-Content -LiteralPath $licenseJson -Raw -Encoding UTF8 | ConvertFrom-Json
    [IO.File]::WriteAllBytes($modelLicense,[Convert]::FromBase64String($license.content))
}
[IO.File]::WriteAllText((Join-Path $models 'MODEL.txt'),"model=$modelName`r`nsha256=$modelSha`r`nsource=https://huggingface.co/ggerganov/whisper.cpp`r`n")
Write-Host "Whisper $version and multilingual small Q5_1 model ready."
Write-Host 'Configure with -DENABLE_WHISPER=ON; Set ASR_RUNTIME=cpu or cuda; CUDA also needs tools/setup_whisper_gpu.ps1.'

& (Join-Path $PSScriptRoot 'setup_whisper_vad.ps1') -ModelHost $ModelHost

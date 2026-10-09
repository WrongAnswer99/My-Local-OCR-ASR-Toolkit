param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = Split-Path -Parent $PSScriptRoot
$cache = Join-Path $root '.source/whisper-cuda'
$runtime = Join-Path $root 'third_party/whisper-cuda'
New-Item -ItemType Directory -Force -Path $cache, $runtime | Out-Null

$marker = Join-Path $runtime 'runtime.json'
if (Test-Path -LiteralPath $marker) {
    $installed = Get-Content -LiteralPath $marker -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($installed.whisper.tag -eq 'b5454' -and $installed.cublas.version -eq '12.9.1.4' -and $installed.cudart.version -eq '12.9.79' -and $installed.files) {
        $valid = $true
        foreach ($entry in $installed.files.PSObject.Properties) {
            $file = Join-Path $runtime $entry.Name
            if (-not (Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.Value) { $valid = $false; break }
        }
        if ($valid) { Write-Host "GPU runtime already prepared and file hashes verified: $runtime"; return }
    }
}
function Get-PinnedArchive([string]$Name, [string]$Url, [string]$Sha256) {
    $path = Join-Path $cache $Name
    if (Test-Path -LiteralPath $path) {
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Sha256) { return $path }
        throw "Cached archive checksum mismatch: $path"
    }
    Write-Host "Downloading $Name..."
    $temporary = $path + '.partial'
    & curl.exe -sS -L --fail --retry 2 --ssl-no-revoke --connect-timeout 30 --max-time 1800 -o $temporary $Url
    if ($LASTEXITCODE -ne 0) { throw "Download failed: $Name" }
    if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Sha256) {
        throw "Downloaded archive checksum mismatch: $Name"
    }
    Move-Item -LiteralPath $temporary -Destination $path -Force
    return $path
}
function Copy-ArchiveEntries([string]$Path, [string]$Pattern, [string]$LicenseName) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        foreach ($entry in $archive.Entries) {
            $name = [IO.Path]::GetFileName($entry.FullName)
            if ($name -match $Pattern) {
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $runtime $name), $true)
            } elseif ($LicenseName -and $name -match '^LICENSE(?:\.txt)?$') {
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $runtime $LicenseName), $true)
            }
        }
    } finally { $archive.Dispose() }
}

# Official Windows CUDA build for whisper.cpp 1.9.5; the semantic tag has no assets.
$whisperUrl = 'https://github.com/ggml-org/whisper.cpp/releases/download/b5454/whisper-bin-win-cuda-12.4.0-x64.zip'
$whisperSha = 'afef0b881c500958921c3f5523b50e59ee2ec9b6f5cbd25b324c51ed308a957a'
$whisper = Get-PinnedArchive 'whisper-bin-win-cuda-12.4.0-x64.zip' $whisperUrl $whisperSha
Copy-ArchiveEntries $whisper '^(whisper-cli\.exe|whisper\.dll|ggml.*\.dll|cublas.*\.dll|cudart.*\.dll|nvrtc.*\.dll)$' ''

# CUDA 12.9 components support this machine's RTX 5060 (Blackwell).
$cublasUrl = 'https://developer.download.nvidia.com/compute/cuda/redist/libcublas/windows-x86_64/libcublas-windows-x86_64-12.9.1.4-archive.zip'
$cublasSha = 'd534d98b0b453a98914dbf3adf47d7e84b55037abf02f87466439e1dcef581ed'
$cublas = Get-PinnedArchive 'libcublas-12.9.1.4.zip' $cublasUrl $cublasSha
Copy-ArchiveEntries $cublas '^cublas(?:Lt)?64_12\.dll$' 'NVIDIA-cuBLAS-LICENSE.txt'
$cudartUrl = 'https://developer.download.nvidia.com/compute/cuda/redist/cuda_cudart/windows-x86_64/cuda_cudart-windows-x86_64-12.9.79-archive.zip'
$cudartSha = '179e9c43b0735ffe67207b3da556eb5a0c50f3047961882b7657d3b822d34ef8'
$cudart = Get-PinnedArchive 'cuda-cudart-12.9.79.zip' $cudartUrl $cudartSha
Copy-ArchiveEntries $cudart '^cudart64_12\.dll$' 'NVIDIA-CUDA-runtime-LICENSE.txt'
$license = Join-Path $root 'dist/Whisper-LICENSE'
if (Test-Path -LiteralPath $license) { Copy-Item -LiteralPath $license -Destination (Join-Path $runtime 'Whisper-LICENSE') -Force }
$metadata = @{
    whisper = @{ tag = 'b5454'; url = $whisperUrl; sha256 = $whisperSha }
    cublas = @{ version = '12.9.1.4'; url = $cublasUrl; sha256 = $cublasSha }
    cudart = @{ version = '12.9.79'; url = $cudartUrl; sha256 = $cudartSha }
}
$metadata.files = @{}
Get-ChildItem -LiteralPath $runtime -File | Where-Object { $_.Extension -in @('.dll', '.exe') } | ForEach-Object {
    $metadata.files[$_.Name] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runtime 'runtime.json') -Encoding UTF8
$version = & (Join-Path $runtime 'whisper-cli.exe') --version
if ($LASTEXITCODE -ne 0) { throw 'Whisper CUDA executable cannot start' }
Write-Host "$version"
Write-Host "GPU runtime ready: $runtime. Prepare the model separately with tools/setup_whisper.ps1."

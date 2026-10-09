# Download only the pinned lightweight VAD model; recognition stays offline.
param([ValidateSet('https://huggingface.co','https://hf-mirror.com')][string]$ModelHost='https://huggingface.co')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$directory=Join-Path $root 'models/whisper'
New-Item -ItemType Directory -Force -Path $directory | Out-Null
$name='ggml-silero-v6.2.0.bin'
$sha='2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987'
$revision='9ffd54a'
$model=Join-Path $directory $name
if (-not (Test-Path -LiteralPath $model)) {
    $partial=$model+'.partial'
    $hosts=@($ModelHost)
    if ($ModelHost -eq 'https://huggingface.co') { $hosts += 'https://hf-mirror.com' }
    $downloaded=$false
    foreach ($hostUrl in $hosts) {
        $url="$hostUrl/ggml-org/whisper-vad/resolve/$revision/$name"
        & curl.exe --ssl-no-revoke -sS -L --fail --connect-timeout 15 --max-time 300 -o $partial $url
        if ($LASTEXITCODE -eq 0) { $downloaded=$true; break }
    }
    if (-not $downloaded) { throw 'VAD download failed; try -ModelHost https://hf-mirror.com' }
    if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha) { throw 'VAD model SHA-256 mismatch' }
    Move-Item -LiteralPath $partial -Destination $model
}
if ((Get-FileHash -LiteralPath $model -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha) { throw 'VAD model SHA-256 mismatch' }
Copy-Item -LiteralPath (Join-Path $root 'licenses/Silero-VAD-LICENSE') -Destination (Join-Path $directory 'Silero-LICENSE') -Force
@("model=$name","sha256=$sha","revision=$revision","source=https://huggingface.co/ggml-org/whisper-vad","license=MIT") |
    Set-Content -LiteralPath (Join-Path $directory 'VAD-MODEL.txt') -Encoding UTF8
Write-Host 'Silero VAD model ready; its file hash was verified.'

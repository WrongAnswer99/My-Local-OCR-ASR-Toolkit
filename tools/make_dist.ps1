# make_dist.ps1 - 生成对外分发目录 ./dist(含 dll、头文件、默认模型)。
# 用法: 先构建主工程, 再运行本脚本:
#   cmake --build build -j 8
#   powershell -ExecutionPolicy Bypass -File tools\make_dist.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
$onnx = Join-Path $root 'models\onnx'

function Copy-One([string]$src, [string]$dst) {
    Copy-Item -LiteralPath $src -Destination $dst -Force
    if (-not (Test-Path -LiteralPath $dst)) {
        throw "copy failed: $src -> $dst"
    }
}

# 1) 确保 dist 存在(不整目录删除, 逐个文件覆盖, 避免个别环境对删除的干扰)
New-Item -ItemType Directory -Force -Path $dist | Out-Null

# 2) 运行时与头文件
Copy-One (Join-Path $root 'build\ocr.dll') (Join-Path $dist 'ocr.dll')
Copy-One (Join-Path $root 'third_party\onnxruntime\lib\onnxruntime.dll') (Join-Path $dist 'onnxruntime.dll')
Copy-One (Join-Path $root 'sdk\ocr.hpp') (Join-Path $dist 'ocr.hpp')
Copy-One (Join-Path $root 'src\ocr_api.hpp') (Join-Path $dist 'ocr_api.hpp')
# 文档与最小用法示例(对方直接照着 example.cpp 改即可)
Copy-One (Join-Path $root 'README.md') (Join-Path $dist 'README.md')
Copy-One (Join-Path $root 'sdk\examples\example.cpp') (Join-Path $dist 'example.cpp')

# 3) 默认模型(v6 tiny 三件 + 可选 v4 mobile 三件)
$m = Join-Path $dist 'models\onnx'
New-Item -ItemType Directory -Force -Path $m | Out-Null
$want = '^(v6_det_tiny|v6_rec_tiny|v6_tiny_dict|' +
        'ch_PP-OCRv4_det_mobile|ch_PP-OCRv4_rec_mobile|ppocr_keys_v1)' +
        '\.(onnx|txt)$'
$copied = 0
foreach ($f in Get-ChildItem $onnx -File | Where-Object { $_.Name -match $want }) {
    Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $m $f.Name) -Force
    $copied++
}
if ($copied -ne 6) { throw "模型文件复制不完整: 期望6个, 实际 $copied" }
# 清理 dist\models\onnx 里不在目标列表内的旧/多余模型(如 server/cls 大模型), 保持目录干净
Get-ChildItem $m -File | Where-Object { $_.Name -notmatch $want } |
    Remove-Item -Force

# 4) 校验
Write-Host '=== dist 内容 ==='
Get-ChildItem $dist -Recurse -File | ForEach-Object {
    '{0,10:N1} KB  {1}' -f ($_.Length / 1KB),
        ($_.FullName.Substring($dist.Length + 1))
}
$total = (Get-ChildItem $dist -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ('total: {0:N1} MB' -f ($total / 1MB))

# 兜底: 个别环境(实时防护)可能在脚本运行中移除刚写入的 dll, 末尾确保 ocr.dll 在位
if (-not (Test-Path -LiteralPath (Join-Path $dist 'ocr.dll'))) {
    Copy-One (Join-Path $root 'build\ocr.dll') (Join-Path $dist 'ocr.dll')
    Write-Host '[warn] ocr.dll was removed during the run, re-copied at end'
}

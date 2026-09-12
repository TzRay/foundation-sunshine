param(
    [string]$Output = 'build/ds5-sidecar-package.json'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build'))
$outputPath = [System.IO.Path]::GetFullPath((Join-Path $root $Output))
$buildPrefix = $buildRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
if (-not $outputPath.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "拒绝在工作区 build 目录外写入 DualSense 清单：$outputPath"
}

# 此组件由上游提交 645465c5 对应的官方发布构建，哈希和大小已通过官方 Release 资产核验。
$manifest = [ordered]@{
    schema = 1
    component_version = '1.2.0'
    protocol = 1
    target = 'win-x64-self-contained'
    license = 'GPL-3.0-only'
    asset_name = 'Sunshine.Ds5Sidecar.x64.zip'
    download_url = 'https://github.com/AlkaidLab/foundation-sunshine/releases/download/v2026.911.85650.%E6%9D%82%E9%B1%BC/Sunshine.Ds5Sidecar.x64.zip'
    sha256 = '8ed32d2d135cc21c887dcc9b19221830cd4790b8167e165c6b01f4d41ff65423'
    size = 44011192
}

$directory = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($outputPath, ($manifest | ConvertTo-Json) + [Environment]::NewLine, $utf8NoBom)
Write-Host "已写入与上游基线匹配的 DualSense 固定清单：$outputPath"

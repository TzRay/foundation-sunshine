# 使用隔离副本验证分发清单，并执行上游要求的真实组件自检。
param(
    [string]$RuntimeDirectory = 'build/ds5-sidecar-runtime'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$relative = "build/fork-packaging-test-$([guid]::NewGuid().ToString('N'))"
$testRoot = [System.IO.Path]::GetFullPath((Join-Path $root $relative))
$buildPrefix = [System.IO.Path]::GetFullPath((Join-Path $root 'build')) + [System.IO.Path]::DirectorySeparatorChar
if (-not $testRoot.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw '打包测试目录必须位于当前工作区 build 目录内。'
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw "打包回归检查失败：$Message"
    }
}

try {
    $runtime = Join-Path $testRoot 'runtime'
    $sourceRuntime = Join-Path $root $RuntimeDirectory
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRuntime 'Sunshine.Ds5Sidecar.exe') -PathType Leaf)) {
        throw '未找到完整的 DualSense 运行时，请先运行 scripts/build-ds5-sidecar.ps1。'
    }
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    Copy-Item -LiteralPath $sourceRuntime -Destination $runtime -Recurse

    $arguments = @{
        RuntimeDirectory = "$relative/runtime"
        PackageDirectory = "$relative/package"
        ManifestPath = "$relative/manifest.json"
    }
    $script = Join-Path $PSScriptRoot 'package-ds5-sidecar.ps1'
    $tag = 'fork-test-1.杂鱼'
    $repository = 'example/sunshine-fork'
    & $script @arguments -ReleaseTag $tag -Repository $repository
    $manifest = Get-Content -LiteralPath (Join-Path $testRoot 'manifest.json') -Raw -Encoding utf8 | ConvertFrom-Json
    $archive = Join-Path $testRoot 'package/Sunshine.Ds5Sidecar.x64.zip'
    $expectedUrl = "https://github.com/$repository/releases/download/$([uri]::EscapeDataString($tag))/Sunshine.Ds5Sidecar.x64.zip"
    Assert-True ($manifest.download_url -ceq $expectedUrl) '下载地址未正确绑定 Fork、标签或中文标签编码。'
    Assert-True ($manifest.sha256 -ceq (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()) '组件哈希不匹配。'
    Assert-True ($manifest.size -eq (Get-Item -LiteralPath $archive).Length) '组件大小不匹配。'

    # 不发布的 PR / 本地构建不得产生一个看似有效但实际不存在的地址。
    & $script @arguments -ReleaseTag '' -Repository ''
    $manifest = Get-Content -LiteralPath (Join-Path $testRoot 'manifest.json') -Raw -Encoding utf8 | ConvertFrom-Json
    Assert-True ($manifest.download_url -ceq '') '未发布构建必须保留空下载地址。'

    foreach ($invalidRepository in @('', 'https://github.com/example/repo', '../repo')) {
        $rejected = $false
        try {
            & $script @arguments -ReleaseTag $tag -Repository $invalidRepository
        }
        catch {
            $rejected = $_.Exception.Message -like '*owner/repo*'
        }
        Assert-True $rejected "未拒绝无效仓库名称：$invalidRepository"
    }

    $pinnedPath = Join-Path $testRoot 'pinned.json'
    & (Join-Path $PSScriptRoot 'write-pinned-ds5-manifest.ps1') -Output "$relative/pinned.json"
    $pinned = Get-Content -LiteralPath $pinnedPath -Raw -Encoding utf8 | ConvertFrom-Json
    Assert-True ($pinned.download_url -like 'https://github.com/AlkaidLab/foundation-sunshine/releases/download/*') '固定组件必须使用控制面板信任的上游发布地址。'
    Assert-True ($pinned.component_version -ceq '1.3.0') '固定组件版本发生意外变化。'
    Assert-True ($pinned.sha256 -ceq 'af0b2f2a897aee5281e602e9190825fc0e2a089266bfacce266f003ebcc87dee') '固定组件哈希发生意外变化。'
    Assert-True ($pinned.size -eq 79235186) '固定组件大小发生意外变化。'
    Write-Host 'Fork 打包回归检查通过：仓库、标签、哈希、大小、未发布构建、错误输入及官方固定组件。'
}
finally {
    # 只清理本次创建的、已经校验过工作区边界的测试目录。
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

# 非管理员场景:用环境变量让 loader 看到 SwiftShader。
# 用法:  . .\tools\vulkan_env.ps1     (注意前面的点,要在当前 shell 生效)
#
# 管理员身份下 loader 会忽略这些变量,那时请用 register_swiftshader_icd.ps1。

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)  # MagicBlocks\tools -> 仓库根

$icd = Join-Path $repo 'third_party\_install\swiftshader-master\release\vk_swiftshader_icd.json'
if (-not (Test-Path $icd)) {
    $found = Get-ChildItem -Path (Join-Path $repo 'third_party\_install') -Recurse `
                -Filter 'vk_swiftshader_icd.json' -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if ($found) { $icd = $found.FullName }
}

if (-not (Test-Path $icd)) {
    Write-Error "找不到 vk_swiftshader_icd.json,请先跑 tools\build-deps.py"
}
$icd = (Resolve-Path $icd).Path
$dir = Split-Path -Parent $icd

$env:VK_DRIVER_FILES = $icd
$env:VK_ICD_FILENAMES = $icd          # 旧名,兼容老 loader
if ($env:PATH -notlike "*$dir*") { $env:PATH = "$dir;$env:PATH" }

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($isAdmin) {
    Write-Warning "当前是管理员:loader 会忽略 VK_DRIVER_FILES,请用 register_swiftshader_icd.ps1"
}

Write-Host "VK_DRIVER_FILES = $icd"
Write-Host "验证: MagicBlocks\build\magicblocks-l0-cli.exe device"

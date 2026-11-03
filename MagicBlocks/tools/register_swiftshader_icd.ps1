# 把 third_party 里自带的 SwiftShader 注册给 Vulkan loader。
#
# 为什么需要提权:以管理员身份运行时,loader **忽略** VK_DRIVER_FILES /
# VK_ICD_FILENAMES 环境变量,只认 HKLM\SOFTWARE\Khronos\Vulkan\Drivers。
# 非管理员场景请改用 vulkan_env.ps1(设 VK_DRIVER_FILES)。
#
# 本脚本只写注册表,指向 third_party/_install/swiftshader 里的 ICD JSON,
# **不修改** third_party 内任何文件内容。

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)  # MagicBlocks\tools -> 仓库根
$icd = Join-Path $repo 'third_party\_install\swiftshader-master\release\vk_swiftshader_icd.json'

if (-not (Test-Path $icd)) {
    $icd = Join-Path $repo 'third_party\_install\swiftshader-master-windows-release\vk_swiftshader_icd.json'
}
if (-not (Test-Path $icd)) {
    $found = Get-ChildItem -Path (Join-Path $repo 'third_party\_install') -Recurse `
                -Filter 'vk_swiftshader_icd.json' -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if (-not $found) {
        Write-Error "找不到 vk_swiftshader_icd.json,请先跑 tools\\build-deps.py 拉取 swiftshader"
    }
    $icd = $found.FullName
}

$icd = (Resolve-Path $icd).Path
Write-Host "ICD: $icd"

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Warning "当前不是管理员。loader 会读 VK_DRIVER_FILES,请改用 tools\vulkan_env.ps1"
    Write-Host "（若仍要注册表方式,请以管理员身份重跑本脚本）"
}

$key = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
if (-not (Test-Path $key)) {
    New-Item -Path $key -Force | Out-Null
    Write-Host "创建 $key"
}

# loader 约定:值名是 ICD 的绝对路径,DWORD 0 表示启用。
New-ItemProperty -Path $key -Name $icd -PropertyType DWord -Value 0 -Force | Out-Null
Write-Host "已注册 SwiftShader ICD (值为 0 = 启用)"

# 让 vk_swiftshader.dll / vulkan-1.dll 能被找到
$root = Split-Path -Parent $icd
if ($env:PATH -notlike "*$root*") {
    Write-Host "提示:运行时需要 $root 在 PATH 里,或把 vulkan-1.dll 拷到 exe 同级目录"
}

Write-Host "验证: list-vulkan-devices 或跑 magicblocks-l0-cli device"
Write-Host "撤销: Remove-ItemProperty -Path '$key' -Name '$icd'"

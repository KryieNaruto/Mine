# 运行 MagicBlocks 测试（自动配置 SwiftShader ICD）
#
# 为什么需要这个脚本:
#   以管理员身份运行时,Vulkan loader **忽略** VK_DRIVER_FILES / VK_ICD_FILENAMES,
#   只认注册表 HKLM\SOFTWARE\Khronos\Vulkan\Drivers。若 SwiftShader 的注册项
#   丢失(换盘符、重装依赖、从别的机器 clone),所有需要设备能力的测试会一起
#   失败并报「require_software_rasterizer=true 但没有软件光栅设备」——
#   这看起来像代码 bug,其实是环境问题,极易误判。
#
# 本脚本在**子进程环境**里设置 VK_DRIVER_FILES 后跑 ctest,不写注册表、
# 不需要管理员权限,也不污染当前 shell。
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\MagicBlocks\tools\run_tests.ps1
#   powershell ... -File .\MagicBlocks\tools\run_tests.ps1 -Filter 'Pool.*'
#   powershell ... -File .\MagicBlocks\tools\run_tests.ps1 -BuildFirst
param(
  [string]$Filter = "",
  [switch]$BuildFirst
)

$ErrorActionPreference = 'Stop'

$toolsDir = $PSScriptRoot
$mb = Split-Path -Parent $toolsDir
$repo = Split-Path -Parent $mb

if ($BuildFirst) {
  & (Join-Path $toolsDir 'build.ps1') -Target magicblocks_test
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# ---- 定位 SwiftShader ICD JSON ----
$icd = $null
$candidates = @(
  (Join-Path $repo 'third_party\_install\swiftshader-master\release\vk_swiftshader_icd.json'),
  (Join-Path $repo 'third_party\_install\swiftshader-master-windows-release\vk_swiftshader_icd.json')
)
foreach ($c in $candidates) {
  if (Test-Path $c) { $icd = (Resolve-Path $c).Path; break }
}
if (-not $icd) {
  $found = Get-ChildItem -Path (Join-Path $repo 'third_party\_install') -Recurse `
             -Filter 'vk_swiftshader_icd.json' -ErrorAction SilentlyContinue |
           Select-Object -First 1
  if ($found) { $icd = $found.FullName }
}
if (-not $icd) {
  Write-Error "找不到 vk_swiftshader_icd.json;请先跑 tools\build-deps.py 拉取 swiftshader"
}

$env:VK_DRIVER_FILES = $icd
$env:VK_ICD_FILENAMES = $icd
Write-Host "SwiftShader ICD: $icd"

$exe = Join-Path $mb 'build\magicblocks_test.exe'
if (-not (Test-Path $exe)) {
  Write-Error "测试可执行文件不存在: $exe (用 -BuildFirst 先构建)"
}

$argsList = @()
if ($Filter) { $argsList += "--gtest_filter=$Filter" }
& $exe @argsList
exit $LASTEXITCODE

<#
.SYNOPSIS
    t6 UI 隔离性门控的一键驾驶台:建导出面 → 建 DLL/导入库 → 跑 check_ui_isolation.py

.DESCRIPTION
    老板的判据只有一句:"UI 只依赖 DLL + 一个 include 头文件。UI 更新和 SDK 更新无关。"
    这个脚本把这句话变成可以**失败**的机器检查。它做四件事:

      1) 建导出面(export_include):
         只有一个目录树 —— `<导出面>/magicblocks/mb_capi.h`。
         默认**复制**自 SDK 源码树;`-ExportFromCmake` 时改为解析 CMake 的
         PUBLIC/INTERFACE include 目录再拼出来(证明的不是我拷了什么,而是构建
         系统**声称**会导给消费者什么)。两种来源最后都只有 1 个头才算通过 ——
         CMake 那一档若把 `include/` 整棵导出去,派生出来的导出面就会有 73 个头,
         GATE_UI_EXPORT 立刻红。

      2) 确保被测的"UI 链接目标"存在:
         优先用 t5 产出的 `magicblocks.dll` + 导入库 `magicblocks.lib`。
         若还没有 DLL(t5 未完成),用主工程的静态库 `magicblocks.lib` 生成
         **桌面桩 DLL**:`src/capi/mb_capi.cpp` + `src/android/{scene_renderer,jni_bridge}.cpp`
         + 静态库(静态库里的对象会按需被拉进来),一次性产出真正的动态库。
         ⚠ 这一步证明的是"UI 能只靠 DLL + 头编译、链接、运行",**不覆盖**
           "DLL 里真的只有 mb_capi 在导出" —— 那是 GATE_SYMBOLS 的活,它看的是
           真正的导出表。桩 DLL 的符号表同样要检查:26 个 C 入口必须在。
           两者会各跑一遍,报告里分开写。

      3) 跑 `MagicBlocks/tools/check_ui_isolation.py`(全部门控 + 元测试)。

      4) 额外跑既有门控(可选,由 -SkipLegacy 关闭):
           python tools\check_layer_purity.py
           python MagicBlocks\tools\tests\test_check_layer_purity.py

.PARAMETER BuildDir
    构建目录(相对 MagicBlocks/ 或绝对路径)。团队规矩:每个 teammate 用自己的。
    默认 build-uisplit。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File MagicBlocks\tools\ui_isolation.ps1
    powershell -ExecutionPolicy Bypass -File MagicBlocks\tools\ui_isolation.ps1 -ExportFromCmake
#>
param(
  [string]$BuildDir = "build-uisplit",
  [string]$ExportInclude = "",
  [string]$Lib = "",
  [switch]$ExportFromCmake,
  [switch]$NeedDllOnly,      # 只做前两步(建导出面/建 DLL),不跑门控:被测试当夹具用
  [string]$Mode = "all",
  [switch]$SkipLegacy,
  [switch]$SkipDll,
  [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$mb   = Split-Path -Parent $here
$repo = Split-Path -Parent $mb
if ([System.IO.Path]::IsPathRooted($BuildDir)) { $bd = $BuildDir } else { $bd = Join-Path $mb $BuildDir }
if (-not (Test-Path $bd)) { New-Item -ItemType Directory -Path $bd -Force | Out-Null }
$work = Join-Path $bd 'ui_isolation'
if (-not (Test-Path $work)) { New-Item -ItemType Directory -Path $work -Force | Out-Null }

function Say($msg) { if (-not $Quiet) { Write-Host "[t6] $msg" } }

# ---------------------------------------------------------------- MSVC env
$vcvars = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat' -ErrorAction SilentlyContinue |
          Select-Object -First 1
if (-not $vcvars) { throw "vcvars64.bat not found (need Visual Studio C++ toolchain)" }
$envDump = & cmd.exe /c ('"' + $vcvars.FullName + '" >nul 2>&1 && set')
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
    $name = $Matches[1]; $value = $Matches[2]
    if ($name -ieq 'Path') { if ($env:PATH -notlike "*$value*") { $env:PATH = "$value;$env:PATH" }; continue }
    if ($name -ieq 'PSModulePath') { continue }
    Set-Item -Path "env:$name" -Value $value
  }
}

# ============================================================================
# 第 1 步:建导出面
# ============================================================================
if (-not $ExportInclude) { $ExportInclude = Join-Path $work 'export_include' }
function Reset-Dir($p) { if (Test-Path $p) { Remove-Item -Recurse -Force $p }; New-Item -ItemType Directory -Path $p -Force | Out-Null }

Reset-Dir $ExportInclude
$cmakePublicDirs = @()
if ($ExportFromCmake) {
  # 解析主工程 CMakeLists 里 magicblocks 目标的 PUBLIC / INTERFACE include 目录。
  # 这里**刻意不做任何筛选**:CMake 说导出什么,导出面就是什么。若它导出整棵
  # include/,导出面就会有 73 个内部头,门控会红 —— 这正是我们要的。
  $cmakeText = Get-Content (Join-Path $mb 'CMakeLists.txt') -Raw
  foreach ($m in [regex]::Matches($cmakeText, 'target_include_directories\s*\(\s*(\S+)([^)]*)\)', 'Singleline')) {
    $target = $m.Groups[1].Value; $rest = $m.Groups[2].Value
    if ($target -ne 'magicblocks') { continue }
    $mode = $null
    foreach ($tok in ($rest -split '\s+')) {
      if ($tok -match '^(PUBLIC|INTERFACE|PRIVATE|SYSTEM|BEFORE|AFTER)$') { $mode = $Matches[1]; continue }
      if ($mode -in @('PUBLIC','INTERFACE') -and $tok) { $cmakePublicDirs += $tok }
    }
  }
  if (-not $cmakePublicDirs) { throw "CMakeLists 里找不到 magicblocks 的 PUBLIC/INTERFACE include 目录" }
  Say ("CMake PUBLIC/INTERFACE include 目录: " + ($cmakePublicDirs -join ', '))
  foreach ($d in $cmakePublicDirs) {
    # CMake 的相对路径以 MagicBlocks/ 为基准;变量形式只认 ${CMAKE_CURRENT_SOURCE_DIR}
    $resolved = $d.Replace('${CMAKE_CURRENT_SOURCE_DIR}', $mb).Replace('\', '/')
    if ($resolved -match '^[$]') { Say "跳过无法解析的 CMake 变量路径: $d"; continue }
    $abs = if ([System.IO.Path]::IsPathRooted($resolved)) { $resolved } else { Join-Path $mb $resolved }
    if (-not (Test-Path $abs)) { Say "PUBLIC 目录不存在(忽略): $abs"; continue }
    Say "把 CMake 导出的目录整棵复制进导出面: $abs"
    Copy-Item -Recurse -Force (Join-Path $abs '*') $ExportInclude -ErrorAction SilentlyContinue
  }
} else {
  # 默认来源:**源码树里那个唯一的契约头**。
  $src = Join-Path $mb 'include\magicblocks\mb_capi.h'
  if (-not (Test-Path $src)) { throw "mb_capi.h 不存在: $src" }
  $dstDir = Join-Path $ExportInclude 'magicblocks'
  New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
  Copy-Item $src (Join-Path $dstDir 'mb_capi.h') -Force
  Say "导出面复制自源码树: $src"
}
$hdrCount = (Get-ChildItem -Recurse -File -Include *.h,*.hpp,*.inl $ExportInclude | Measure-Object).Count
Say "导出面共 $hdrCount 个头文件: $ExportInclude"
if ($hdrCount -eq 0) { throw "导出面为空 —— 拒绝继续(空导出面会让门控假绿)" }

# ============================================================================
# 第 2 步:确保"UI 链接目标"
# ============================================================================
$dllPath = ""
$implLib = ""
$dllOrigin = ""
if (-not $Lib) {
  # 1) t5 的真正产物
  $cand = Get-ChildItem -Path $bd -Filter 'magicblocks.dll' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($cand) {
    $dllPath = $cand.FullName
    $dllOrigin = 't5 构建产物(magicblocks.dll)'
    # ⚠️ --lib 必须是**导入库 .lib**,不能是 .dll:
    #   check_ui_isolation.py 用 llvm-nm --extern-only 读符号名,而 llvm-nm 对 PE
    #   动态库**不输出任何符号**(实测 `llvm-nm magicblocks.dll` → "no symbols",
    #   exit=0)。把 .dll 当 --lib 传进去,导出面唯一的硬检查 GATE_SYMBOLS 会拿到
    #   空符号集,于是 27 个入口全被报成"缺"—— 看起来像没导出,其实是工具选错了。
    #   导入库 .lib 里同时有裸名和 __imp_ 前缀名,llvm-nm 读得到。
    $impCand = Get-ChildItem -Path $bd -Filter 'magicblocks.lib' -Recurse -ErrorAction SilentlyContinue |
               Where-Object { $_.DirectoryName -eq $cand.DirectoryName } | Select-Object -First 1
    if (-not $impCand) {
      $impCand = Get-ChildItem -Path $bd -Filter 'magicblocks.lib' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    $implLib = if ($impCand) { $impCand.FullName } else { $dllPath }
  }
}
else { $dllPath = (Resolve-Path $Lib).Path; $dllOrigin = '命令行指定'; $implLib = $dllPath }

if (-not $dllPath -and -not $SkipDll) {
  # 2) 没有 DLL:t5 还没完成 → 用静态库 + C ABI 实现 TU 现造一个桌面 DLL
  $staticLib = Join-Path $bd 'magicblocks.lib'
  if (-not (Test-Path $staticLib)) { throw "既没有 magicblocks.dll,也没有静态库 $staticLib(先跑 tools\build.ps1 -Target all -BuildDir $BuildDir)" }
  $capiDir = Join-Path $work 'stub_dll'
  Reset-Dir $capiDir
  $sources = @(
    (Join-Path $mb 'src\capi\mb_capi.cpp'),
    (Join-Path $mb 'src\android\scene_renderer.cpp'),
    (Join-Path $mb 'src\android\jni_bridge.cpp')
  )
  foreach ($s in $sources) { if (-not (Test-Path $s)) { throw "缺少 C ABI 实现 TU: $s" } }
  # jni_bridge.cpp 的 JNIEnv 成员函数在真机上由 libart 提供、桌面上由
  # tests/capistub/jni_stub.cpp 提供。桩 DLL **必须**把替身链进去,否则
  # 9 个 JNIEnv::* 符号解析不了 —— 那不是 UI 隔离的问题,是桌面替身的必修项。
  # (主工程里同样的做法见 tests/capi_smoke/CMakeLists.txt 的说明。)
  $jniStub = Join-Path $mb 'tests\capistub\jni_stub.cpp'
  if (-not (Test-Path $jniStub)) { throw "缺少桌面 JNI 替身: $jniStub" }
  $sources += $jniStub
  $objDir = Join-Path $capiDir 'obj'; New-Item -ItemType Directory -Path $objDir -Force | Out-Null
  $objs = @()
  foreach ($s in $sources) {
    $o = Join-Path $objDir (([System.IO.Path]::GetFileNameWithoutExtension($s)) + '.obj')
    # 与主工程同参数(/MD /O2 /std:c++20 /utf-8):MSVC 不同 Runtime 混链会在链接期炸,
    # 那是环境噪声,不是本门控要证明的东西。
    & cl /nologo /std:c++20 /EHsc /utf-8 /MD /O2 /DNDEBUG /c $s "/Fo$o" /I (Join-Path $mb 'include') /I (Join-Path $mb 'src') /I (Join-Path $mb 'tests\capistub') /I $bd /DMB_CAPI_BUILDING_LIBRARY /DVK_USE_PLATFORM_WIN32_KHR | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "编译 C ABI 实现 TU 失败: $s" }
    $objs += $o
  }
  $outDll = Join-Path $capiDir 'magicblocks.dll'
  $outLib = Join-Path $capiDir 'magicblocks.lib'
  # vulkan-1.lib:主工程通过 find_package(Vulkan) 拿到,优先级:环境变量 VULKAN_SDK
  # → 依赖池 third_party/_install。找不到就报明确错误,不要静默生成半成品 DLL。
  $vulkanLib = $null
  $vkCands = @()
  if ($env:VULKAN_SDK) { $vkCands += (Join-Path $env:VULKAN_SDK 'Lib\vulkan-1.lib') }
  $vkCands += (Join-Path $repo 'third_party\_install\VulkanSdk\release\Lib\vulkan-1.lib')
  foreach ($c in $vkCands) { if (Test-Path $c) { $vulkanLib = $c; break } }
  if (-not $vulkanLib) {
    $found = Get-ChildItem -Path (Join-Path $repo 'third_party\_install') -Filter 'vulkan-1.lib' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $vulkanLib = $found.FullName }
  }
  if (-not $vulkanLib) { throw "找不到 vulkan-1.lib(设 VULKAN_SDK 或确认依赖池中有 Vulkan SDK)" }
  Say "vulkan-1.lib: $vulkanLib"
  $linkArgs = @('/nologo','/DLL',"/OUT:$outDll","/IMPLIB:$outLib",'/SUBSYSTEM:WINDOWS') + $objs + @($staticLib, $vulkanLib)
  & link @linkArgs | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "生成桌面桩 DLL 失败(link 退出码 $LASTEXITCODE)" }
  $dllPath = $outDll
  $implLib = $outLib
  $dllOrigin = '桌面桩 DLL(静态库 + C ABI TU 现造;t5 未产出 SHARED 时使用)'
  Say "已生成: $outDll"
}
Say "被测 UI 链接目标: $dllPath ($dllOrigin)"

if ($NeedDllOnly) {
  [pscustomobject]@{ ExportInclude = $ExportInclude; Dll = $dllPath; ImportLib = $implLib; Origin = $dllOrigin } | ConvertTo-Json
  exit 0
}

# ============================================================================
# 第 3 步:门控
# ============================================================================
$fail = 0
Say "=== check_ui_isolation.py --mode $Mode ==="
& python (Join-Path $here 'check_ui_isolation.py') --export-include $ExportInclude --lib $implLib --build-dir $bd --mode $Mode
$isoExit = $LASTEXITCODE
if ($isoExit -ne 0) { $fail++ }

if (-not $SkipLegacy) {
  Say "=== tools/check_layer_purity.py (G3) ==="
  & python (Join-Path $repo 'tools\check_layer_purity.py')
  if ($LASTEXITCODE -ne 0) { $fail++; Say "G3 门控红(exit=$LASTEXITCODE)。注意:预期会剩 16 处既有违规(t9 处理)" }

  Say "=== MagicBlocks/tools/tests/test_check_layer_purity.py (G3 自测) ==="
  & python (Join-Path $here 'tests\test_check_layer_purity.py')
  if ($LASTEXITCODE -ne 0) { $fail++; Say "G3 自测红(exit=$LASTEXITCODE)" }
}

if ($fail -gt 0) { Say "总计 $fail 组门控未通过"; exit 1 }
Say "全部门控通过"
exit 0

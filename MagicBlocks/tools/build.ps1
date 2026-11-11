param(
  [string]$Target = "",
  [switch]$Configure,
  # 独立构建目录. Team rule (2026-11-11): every teammate MUST use its own
  # directory (`build-fsk`, `build-msaa`, ...) -- concurrent ninja runs in a
  # shared dir produce `C1083 Permission denied` on .obj files that look like
  # source errors but are pure lock contention. Relative to the MagicBlocks dir.
  [string]$BuildDir = "build",
  # Re-run CMake configure. gtest_discover_tests only refreshes at configure
  # time, so a newly added test file will NOT be visible to `--gtest_filter`
  # without this -- it silently "passes" by matching zero tests otherwise.
  [switch]$RefreshTests
)

# Import MSVC environment (vcvars64) so INCLUDE/LIB are set for cl.exe.
# NOTE: without this, every TU fails with `C1083 cannot open cstdint` -- that
# is a missing INCLUDE env var, NOT a code error. Always build through here.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$mb = Join-Path $repo 'MagicBlocks'
if ([System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDirPath = $BuildDir
} else {
  $BuildDirPath = Join-Path $mb $BuildDir
}

$vcvars = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat' -ErrorAction SilentlyContinue |
          Select-Object -First 1
if (-not $vcvars) { throw "vcvars64.bat not found (need Visual Studio C++ toolchain)" }

$cmdLine = '"' + $vcvars.FullName + '" >nul 2>&1 && set'
$envDump = & cmd.exe /c $cmdLine
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
    $name = $Matches[1]
    $value = $Matches[2]
    if ($name -ieq 'Path') {
      if ($env:PATH -notlike "*$value*") { $env:PATH = "$value;$env:PATH" }
      continue
    }
    if ($name -ieq 'PSModulePath') { continue }
    Set-Item -Path "env:$name" -Value $value
  }
}

if ($Configure -or $RefreshTests -or -not (Test-Path (Join-Path $BuildDirPath 'build.ninja'))) {
  Write-Host "[build] cmake configure -> $BuildDirPath"
  & cmake -S $mb -B $BuildDirPath -G Ninja -DCMAKE_BUILD_TYPE=Release
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  $needsRepair = $true
}

# --- 修复增量构建的 header 依赖跟踪 -------------------------------------------
# 症状: `ninja -t deps` 对每个 .obj 都报 `#deps 0`,于是改一个头文件不触发任何
# 重编(只有全量重建才能反映改动)。产物本身正确,坑在增量。
#
# 根因: CMake 的 CMAKE_DETERMINE_MSVC_SHOWINCLUDES_PREFIX() 执行
# cl.exe /showIncludes 时用 ENCODING AUTO 读到了正确的前缀,但在写
# rules.ninja 时又经过一次错误的代码页转码,落盘成了乱码
#   msvc_deps_prefix = <e5 a8 a9 e3 84 a6 ...>   (二次编码的产物)
# 而 ninja 从 cl.exe 管道里读到的其实是正确的 UTF-8
#   <e6 b3 a8 e6 84 8f ...>                      ("注意: 包含文件:  ")
# 两边字节永不相等 -> 一行都匹配不上 -> deps 恒为 0。
# 非中文 Windows 上前缀本就是 ASCII,不会触发,所以这个坑只在本地化系统上出现。
#
# 修法: 落盘乱码与正确字节之间有一个**完全可逆**的变换,不需要知道系统语言:
#   <乱码文本>.encode(系统代码页) == <正确 UTF-8 字节>
# 实测: '\u5a09\u3126\u5270...'.encode('gbk') == '注意...'.encode('utf-8')
# 所以直接从 rules.ninja 读出那一行、按系统代码页重编码写回即可。
function Repair-MsvcDepsPrefix {
  param([string]$BuildDirPath)

  $rules = Join-Path $BuildDirPath 'CMakeFiles/rules.ninja'
  if (-not (Test-Path $rules)) { return }

  $marker = 'msvc_deps_prefix = '
  $markerBytes = [System.Text.Encoding]::ASCII.GetBytes($marker)
  $bytes = [System.IO.File]::ReadAllBytes($rules)

  # 按 \n 切行(保留行尾 \r 不裁,原样回写)
  $lines = New-Object 'System.Collections.Generic.List[byte[]]'
  $start = 0
  for ($i = 0; $i -le $bytes.Length; $i++) {
    if ($i -eq $bytes.Length -or $bytes[$i] -eq 10) {
      $len = $i - $start
      $seg = New-Object 'byte[]' $len
      if ($len -gt 0) { [Array]::Copy($bytes, $start, $seg, 0, $len) }
      $lines.Add($seg)
      $start = $i + 1
    }
  }

  $out = New-Object 'System.Collections.Generic.List[byte]'
  $changed = $false
  $cp = [System.Text.Encoding]::GetEncoding(0)

  for ($i = 0; $i -lt $lines.Count; $i++) {
    $ln = $lines[$i]
    $emit = $ln
    if ($ln.Length -gt $markerBytes.Length) {
      $same = $true
      for ($k = 0; $k -lt $markerBytes.Length; $k++) {
        if ($ln[$k] -ne $markerBytes[$k]) { $same = $false; break }
      }
      if ($same) {
        $tailLen = $ln.Length - $markerBytes.Length
        $tail = New-Object 'byte[]' $tailLen
        [Array]::Copy($ln, $markerBytes.Length, $tail, 0, $tailLen)
        $text = [System.Text.Encoding]::UTF8.GetString($tail).TrimEnd("`r")
        try {
          $cand = $cp.GetBytes($text)
          # 只有结果确实是合法 UTF-8(cl.exe 实际输出的形态)时才替换
          if ($cand.Length -gt 0 -and -not ([System.Text.Encoding]::UTF8.GetString($cand) -match [char]0xFFFD)) {
            $newLn = New-Object 'System.Collections.Generic.List[byte]'
            $newLn.AddRange($markerBytes)
            $newLn.AddRange([byte[]]$cand)
            if ($tail.Length -gt 0 -and $tail[$tail.Length - 1] -eq 13) { $newLn.Add(13) }
            $emit = [byte[]]$newLn.ToArray()
            if (-not [System.Linq.Enumerable]::SequenceEqual([byte[]]$ln, [byte[]]$emit)) { $changed = $true }
          }
        } catch { }
      }
    }
    $out.AddRange([byte[]]$emit)
    if ($i -lt $lines.Count - 1) { $out.Add(10) }
  }

  if ($changed) {
    [System.IO.File]::WriteAllBytes($rules, $out.ToArray())
    Write-Host "[build] 已修正 msvc_deps_prefix 编码 -> 增量重编可正确跟踪头文件依赖"
  }
}

# 每次 build 都检查(configure 会重写 rules.ninja;没 configure 时它也已是被修正的状态,
# 函数内部对"已经正确"的情况不改文件,避免无谓刷新 mtime 导致 ninja 重新读图)
Repair-MsvcDepsPrefix -BuildDirPath $BuildDirPath

$cargs = @('--build', $BuildDirPath)
if ($Target) { $cargs += @('--target', $Target) }
& cmake @cargs
exit $LASTEXITCODE

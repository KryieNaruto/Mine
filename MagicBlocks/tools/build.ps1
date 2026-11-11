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
}

$cargs = @('--build', $BuildDirPath)
if ($Target) { $cargs += @('--target', $Target) }
& cmake @cargs
exit $LASTEXITCODE

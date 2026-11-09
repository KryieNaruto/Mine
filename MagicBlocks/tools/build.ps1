param(
  [string]$Target = "",
  [switch]$Configure
)

# Import MSVC environment (vcvars64) so INCLUDE/LIB are set for cl.exe.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$mb = Join-Path $repo 'MagicBlocks'
$BuildDir = Join-Path $mb 'build'

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

if ($Configure) {
  & cmake -S $mb -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$cargs = @('--build', $BuildDir)
if ($Target) { $cargs += @('--target', $Target) }
& cmake @cargs
exit $LASTEXITCODE

# MagicBlocks asset fetch: copy from RayTracing res and verify against assets/assets.lock.json
#
# Design (see docs/superpowers/specs/2026-11-11-magicblocks-ui-assets-inventory.md section 2.3):
#   - no large binaries in git (res is ~205 MB total);
#   - only assets.lock.json (path + bytes + sha256) is versioned;
#   - runtime locates assets via --asset-root or MB_ASSET_ROOT, and fails loudly when absent.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File MagicBlocks/tools/fetch-assets.ps1
#   powershell ... -Source D:\some\res -Dest MagicBlocks\assets\res
#
# Exit: 0 all present and verified, 1 a file is missing or mismatched, 2 usage/environment error.
param(
  [string]$Source = "",
  [string]$Dest = ""
)

$ErrorActionPreference = 'Stop'

$mb = Split-Path -Parent $PSScriptRoot
$lockPath = Join-Path $mb 'assets\assets.lock.json'
if (-not (Test-Path $lockPath)) {
  Write-Host "ERROR: missing manifest: $lockPath"
  exit 2
}

$lock = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json

if (-not $Source) {
  if ($env:MB_ASSET_SRC) { $Source = $env:MB_ASSET_SRC } else { $Source = $lock.source_root }
}
if (-not $Dest) { $Dest = Join-Path $mb 'assets\res' }

if (-not (Test-Path $Source)) {
  Write-Host "ERROR: source directory not found: $Source"
  Write-Host "       pass -Source, or set MB_ASSET_SRC."
  exit 2
}

Write-Host "fetch source: $Source"
Write-Host "fetch dest  : $Dest"

$failed = 0
foreach ($f in $lock.files) {
  $rel = $f.path -replace '/', '\'
  $src = Join-Path $Source $rel
  $dst = Join-Path $Dest $rel
  $dstDir = Split-Path -Parent $dst

  if (-not (Test-Path $src)) {
    Write-Host "MISSING  $($f.path)"
    $failed++
    continue
  }

  if (-not (Test-Path $dstDir)) {
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
  }

  # Idempotent: skip the copy when the destination already verifies.
  $needCopy = $true
  if (Test-Path $dst) {
    $item = Get-Item -LiteralPath $dst
    if ($item.Length -eq $f.bytes) {
      $h = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToLower()
      if ($h -eq $f.sha256) { $needCopy = $false }
    }
  }

  if ($needCopy) {
    # Copy, never move: the source belongs to another project's install tree.
    Copy-Item -LiteralPath $src -Destination $dst -Force
  }

  $len = (Get-Item -LiteralPath $dst).Length
  $hash = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToLower()
  if ($len -ne $f.bytes -or $hash -ne $f.sha256) {
    Write-Host "MISMATCH $($f.path)"
    Write-Host "         expected $($f.bytes) bytes / $($f.sha256.Substring(0,16))"
    Write-Host "         actual   $len bytes / $($hash.Substring(0,16))"
    $failed++
  } else {
    Write-Host "OK       $($f.path)"
  }
}

if ($failed -ne 0) {
  Write-Host "FAILED: $failed file(s) did not verify"
  exit 1
}

Write-Host "All assets in place. Set MB_ASSET_ROOT=$Dest or pass --asset-root to the CLI."
exit 0

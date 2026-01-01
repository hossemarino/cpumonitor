# Builds help\CCM.chm from help\CCM.hhp.
# Requires Microsoft HTML Help Workshop (hhc.exe).

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$hhcCandidates = @(
  "C:\\Program Files (x86)\\HTML Help Workshop\\hhc.exe",
  "C:\\Program Files\\HTML Help Workshop\\hhc.exe"
)

$hhc = $null
foreach ($c in $hhcCandidates) {
  if (Test-Path $c) { $hhc = $c; break }
}

if (-not $hhc) {
  $cmd = Get-Command hhc.exe -ErrorAction SilentlyContinue
  if ($cmd) { $hhc = $cmd.Source }
}

if (-not $hhc) {
  throw "hhc.exe not found. Install 'Microsoft HTML Help Workshop' and re-run."
}

$chm = Join-Path $root 'CCM.chm'
if (Test-Path $chm) {
  try {
    Remove-Item -Force $chm
  } catch {
    throw "Cannot overwrite '$chm'. Close any open CHM viewers and re-run. ($($_.Exception.Message))"
  }
}

$output = & $hhc (Join-Path $root 'CCM.hhp') 2>&1 | Out-String
$output | Write-Host

if ($output -match 'Compilation stopped\.' -or $output -match 'HHC\d+:\s*Error:') {
  throw "CHM build failed (hhc.exe reported errors). See output above."
}

if (-not (Test-Path $chm)) {
  throw "CHM build did not produce: $chm"
}

Write-Host "Built: $chm"

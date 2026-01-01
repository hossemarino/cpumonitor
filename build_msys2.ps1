param(
  # Build mode:
  # - both   : build CCM.exe and CCM_all.exe (default)
  # - all    : build CCM_all.exe only (static runtime)
  # - normal : build CCM.exe only
  [ValidateSet('both', 'all', 'normal')]
  [string]$Build,

  # Clean build outputs (removes the ./build folder contents)
  [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$gccCandidates = @(
  "C:\\msys64\\ucrt64\\bin\\gcc.exe",
  "C:\\msys64\\mingw64\\bin\\gcc.exe",
  "C:\\msys64\\clang64\\bin\\clang.exe"
)

$cc = $null
foreach ($c in $gccCandidates) {
  if (Test-Path $c) { $cc = $c; break }
}

if (-not $cc) {
  $cmd = Get-Command gcc -ErrorAction SilentlyContinue
  if ($cmd) { $cc = $cmd.Source }
}

if (-not $cc) {
  throw "No suitable compiler found. Install MSYS2 (ucrt64/mingw64) and ensure gcc is available."
}

New-Item -ItemType Directory -Force -Path build | Out-Null

if ($Clean) {
  if (Test-Path .\build) {
    Get-ChildItem -Force .\build | Remove-Item -Force -Recurse -ErrorAction SilentlyContinue
  }
  Write-Host "Cleaned: $root\build"
  exit 0
}

$srcAll = Get-ChildItem -Path .\src -Filter *.c | ForEach-Object { $_.FullName }

$normalName = $env:CCM_EXE
if (-not $normalName) { $normalName = "CCM.exe" }

if ($env:HTOP_STYLE_EXE) {
  # Legacy env var name (single-output build)
  $normalName = $env:HTOP_STYLE_EXE
}

if ($env:CCM_SAFE_EXE) {
  # Legacy from old safe/driver split; treat as normal output name.
  $normalName = $env:CCM_SAFE_EXE
}

$allName = $env:CCM_ALL_EXE
if (-not $allName) { $allName = "CCM_all.exe" }

if (-not $Build) {
  if ($env:CCM_BUILD_MODE) {
    $Build = $env:CCM_BUILD_MODE
  } else {
    # Default: build both, with CCM_all.exe being the "shipping" artifact.
    $Build = 'both'
  }
}

$defines = @(
  "-DUNICODE",
  "-D_UNICODE",
  "-DWIN32_LEAN_AND_MEAN",
  "-DNOMINMAX"
)

$libs = @(
  "-ld2d1",
  "-ldwrite",
  "-ldxgi",
  "-lole32",
  "-loleaut32",
  "-lwbemuuid",
  "-lpdh",
  "-lpowrprof",
  "-ladvapi32",
  "-lpsapi",
  "-liphlpapi",
  "-lws2_32",
  "-luuid",
  "-lgdi32",
  "-luser32"
)

$providerSrc = @(
  (Join-Path $root "tools\ccm_sensor_provider.c"),
  (Join-Path $root "src\wmi_sensors.c")
)

function Invoke-BuildProvider {
  param(
    [string]$outFile
  )
  $outExe = Join-Path $root ("build\\" + $outFile)
  & $cc -std=c17 -O2 @defines $providerSrc -o $outExe -municode "-Wl,-subsystem,windows" @libs
  if ($LASTEXITCODE -ne 0) {
    throw "Provider build failed with exit code $LASTEXITCODE"
  }
  Write-Host "Built: $outExe"
}

function Invoke-BuildExe {
  param(
    [string]$outFile,
    [string[]]$extraDefines,
    [string[]]$extraLinkFlags
  )
  $outExe = Join-Path $root ("build\\" + $outFile)
  & $cc -std=c17 -O2 @defines @extraDefines @srcAll -o $outExe -municode "-Wl,-subsystem,windows" @extraLinkFlags @libs
  if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
  }
  Write-Host "Built: $outExe"
}

if ($Build -eq 'normal' -or $Build -eq 'both') {
  Invoke-BuildExe -outFile $normalName -extraDefines @("-DCCM_SAFE_MODE=1") -extraLinkFlags @()
}

if ($Build -eq 'all' -or $Build -eq 'both') {
  # Try to produce a single-file-ish EXE (no MSYS2 runtime DLL dependencies).
  # Windows system DLLs (d2d1.dll, dwrite.dll, etc.) are still required.
  $staticFlags = @(
    "-static",
    "-static-libgcc"
  )

  if ($Build -eq 'both') {
    try {
      Invoke-BuildExe -outFile $allName -extraDefines @("-DCCM_SAFE_MODE=1") -extraLinkFlags $staticFlags
    } catch {
      Write-Warning "Static build (CCM_all.exe) failed; keeping CCM.exe only. Error: $($_.Exception.Message)"
    }
  } else {
    Invoke-BuildExe -outFile $allName -extraDefines @("-DCCM_SAFE_MODE=1") -extraLinkFlags $staticFlags
  }
}

# Always build the bundled sample provider if sources exist.
if (Test-Path $providerSrc[0]) {
  Invoke-BuildProvider -outFile "CCM_sensor_provider.exe"
}

 

param(
  [string]$ObsRoot = "",
  [string]$Config = "RelWithDebInfo",
  [switch]$WithPdb
)

$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
  $here = Split-Path -Parent $PSCommandPath
  return (Resolve-Path (Join-Path $here "..")).Path
}

function Test-ObsLayout([string]$root) {
  if (-not (Test-Path $root)) { return $false }
  if (Test-Path (Join-Path $root "obs-plugins\64bit")) { return $true }
  if (Test-Path (Join-Path $root "bin\64bit")) { return $true } # typical OBS distribution root
  return $false
}

function Resolve-ObsRoot([string]$repoRoot, [string]$override) {
  if ($override -and (Test-ObsLayout $override)) {
    return (Resolve-Path $override).Path
  }

  $candidates = @(
    (Join-Path $repoRoot "..\obs-studio\build_x64\rundir\RelWithDebInfo"),
    (Join-Path $repoRoot "..\obs-studio\build_x64\rundir\Release"),
    "$env:OBS_PORTABLE_DIR",
    "C:\Program Files\obs-studio",
    "C:\Program Files (x86)\obs-studio"
  ) | Where-Object { $_ -and $_.Trim().Length -gt 0 }

  foreach ($c in $candidates) {
    if (Test-ObsLayout $c) {
      return (Resolve-Path $c).Path
    }
  }

  throw "OBS root not found. Pass -ObsRoot 'C:\Path\To\obs-studio' (or set OBS_PORTABLE_DIR)."
}

function Resolve-ObsPluginDir([string]$obsRoot) {
  $dir1 = Join-Path $obsRoot "obs-plugins\64bit"
  if (Test-Path $dir1) { return $dir1 }

  # fallback for odd layouts
  $dir2 = Join-Path $obsRoot "bin\64bit\obs-plugins\64bit"
  if (Test-Path $dir2) { return $dir2 }

  # create standard dir if OBS root looks valid
  if (Test-Path (Join-Path $obsRoot "bin\64bit")) {
    New-Item -ItemType Directory -Force -Path $dir1 | Out-Null
    return $dir1
  }

  throw "Can't resolve OBS plugin directory under: $obsRoot"
}

function Copy-IfNewer([string]$src, [string]$dstDir) {
  $dst = Join-Path $dstDir (Split-Path -Leaf $src)
  $needs = $true

  if (Test-Path $dst) {
    $s = Get-Item $src
    $d = Get-Item $dst
    if ($s.Length -eq $d.Length -and $s.LastWriteTimeUtc -le $d.LastWriteTimeUtc) {
      $needs = $false
    }
  }

  if ($needs) {
    try {
      Copy-Item -Force -Path $src -Destination $dst
      Write-Host ("updated  " + (Split-Path -Leaf $src))
    } catch [System.IO.IOException] {
      Write-Host ("locked   " + (Split-Path -Leaf $src) + " (close OBS to update)")
    }
  } else {
    Write-Host ("skipped  " + (Split-Path -Leaf $src))
  }
}

$repoRoot = Resolve-RepoRoot
$buildDir = Join-Path $repoRoot ("build_x64\" + $Config)
if (-not (Test-Path $buildDir)) {
  throw "Build output folder not found: $buildDir. Build first (cmake --build --preset windows-x64)."
}

$obs = Resolve-ObsRoot -repoRoot $repoRoot -override $ObsRoot
$obsPlugins = Resolve-ObsPluginDir -obsRoot $obs

$files = @(
  "sayo_obs_plugin.dll",
  "abseil_dll.dll",
  "cares.dll",
  "libcrypto-3-x64.dll",
  "libprotobuf.dll",
  "libssl-3-x64.dll",
  "re2.dll",
  "samplerate.dll",
  "z.dll"
)

if ($WithPdb) {
  $files += "sayo_obs_plugin.pdb"
}

Write-Host ("build: " + $buildDir)
Write-Host ("obs:   " + $obs)
Write-Host ("to:    " + $obsPlugins)
Write-Host ""

foreach ($f in $files) {
  $src = Join-Path $buildDir $f
  if (Test-Path $src) {
    Copy-IfNewer -src $src -dstDir $obsPlugins
  } else {
    Write-Host ("missing " + $f)
  }
}

Write-Host ""
Write-Host "Done. If OBS is running, restart OBS to reload the plugin DLLs."


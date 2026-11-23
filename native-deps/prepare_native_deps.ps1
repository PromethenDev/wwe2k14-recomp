# Prepare the native renderer's private copy of plume and reblue-XenosRecomp (work\native-deps).
#
# C:\Dev\native-deps is shared with WWE 2K15 PC: this script never writes there. It copies plume and
# reblue-XenosRecomp from it (no download), resets the copies to the commits WWE '12 PC pins and applies
# port\patches in WWE '12 PC's order, so one plume builds the Vulkan, Direct3D 12 and Direct3D 11
# executables:
#   plume:              plume-reblue, plume-pipeline-cache (WWE 2K15 PC), plume-d3d12, plume-d3d11,
#                       plume-fullscreen-exclusive, plume-vk-dxgi-present, plume-vk-dxgi-present-active
#                       (the last three: Vulkan frames through a DXGI flip swap chain; *-REFUTED patches are
#                       kept as records only and never applied)
#   reblue-XenosRecomp: xenosrecomp-reblue-wwe12 (WWE '12 PC's: the 2K15 one plus the DXIL depth flip)
# then builds XenosRecomp.exe (REBLUE_RECOMP), which turns the game's shaders into the build-time caches
# of wwe13_d3d12.exe / wwe13_d3d11.exe.
#
# Called by build.ps1 -Native (needs the MSVC + LLVM environment it sets up). Done again only when a
# patch changes (stamp file in each copy).
param(
  [string]$SourceDir = "C:\Dev\native-deps",
  [string]$DestDir = ""
)
$ErrorActionPreference = "Stop"
$port = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent $port
if (-not $DestDir) { $DestDir = Join-Path $repo "work\native-deps" }
$patches = Join-Path $port "patches"

function Invoke-Native([string]$exe, [string[]]$argv) {
  # Windows PowerShell 5.1 turns a native program's stderr into errors: judge by the exit code only.
  $ErrorActionPreference = "Continue"
  & $exe @argv 2>&1 | ForEach-Object { Write-Host "$_" }
  if ($LASTEXITCODE) { throw "$exe $($argv -join ' ') failed ($LASTEXITCODE)" }
}

function Get-Stamp([string[]]$names) {
  ($names | ForEach-Object { (Get-FileHash (Join-Path $patches $_) -Algorithm SHA256).Hash }) -join ";"
}

# Copy <name> from the shared folder (without its top build folder), reset it to <commit>, apply <names>.
function Prepare-Repo([string]$name, [string]$commit, [string[]]$names) {
  $src = Join-Path $SourceDir $name
  $dst = Join-Path $DestDir $name
  $stampFile = Join-Path $dst ".wwe13_patches"
  $stamp = "$commit;" + (Get-Stamp $names)
  if ((Test-Path $stampFile) -and ((Get-Content $stampFile -Raw).Trim() -eq $stamp)) {
    Write-Host "$name ready ($dst)"
    return $false
  }
  if (-not (Test-Path (Join-Path $src ".git"))) { throw "$src not found (plume and reblue-XenosRecomp of WWE 2K15 PC)" }
  Write-Host "== $name : copy from $src"
  New-Item -ItemType Directory -Force $dst | Out-Null
  # /MIR keeps the copy identical to the source, but leaves our own build folder alone (/XD).
  robocopy $src $dst /MIR /XD (Join-Path $src "build") (Join-Path $dst "build") /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy $src failed ($LASTEXITCODE)" }
  # Our copy only: back to the pinned commit, without the shared folder's local changes.
  Invoke-Native git @("-C", $dst, "checkout", "-q", "-f", $commit)
  Invoke-Native git @("-C", $dst, "clean", "-q", "-f", "-d", "-e", "build", "-e", ".wwe13_patches")
  foreach ($p in $names) {
    Write-Host "   apply $p"
    Invoke-Native git @("-C", $dst, "apply", "--whitespace=nowarn", (Join-Path $patches $p))
  }
  Set-Content -LiteralPath $stampFile -Value $stamp -Encoding ascii
  return $true
}

Prepare-Repo "plume" "e0c8871" @("plume-reblue.patch", "plume-pipeline-cache.patch", "plume-d3d12.patch",
  "plume-d3d11.patch", "plume-fullscreen-exclusive.patch", "plume-vk-dxgi-present.patch",
  "plume-vk-dxgi-present-active.patch") | Out-Null
$xrChanged = Prepare-Repo "reblue-XenosRecomp" "339af41" @("xenosrecomp-reblue-wwe12.patch")

$xr = Join-Path $DestDir "reblue-XenosRecomp"
$xrBuild = Join-Path $xr "build"
$xrExe = Join-Path $xrBuild "XenosRecomp\XenosRecomp.exe"
if ($xrChanged -or -not (Test-Path $xrExe)) {
  Write-Host "== XenosRecomp.exe (REBLUE_RECOMP)"
  # CXXFLAGS adds to CMake's default flags (/EHsc included) where CMAKE_CXX_FLAGS would replace them.
  $env:CXXFLAGS = "-DREBLUE_RECOMP"
  try {
    Invoke-Native cmake @("-S", $xr, "-B", $xrBuild, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
      "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl")
  } finally { Remove-Item Env:CXXFLAGS -ErrorAction SilentlyContinue }
  Invoke-Native cmake @("--build", $xrBuild, "--target", "XenosRecomp")
}
Write-Host "XenosRecomp ready ($xrExe)"

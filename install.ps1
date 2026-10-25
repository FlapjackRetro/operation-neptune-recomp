<#
  Operation Neptune static recompilation - installer.
  Usage:   powershell -ExecutionPolicy Bypass -File install.ps1 [-GameDir C:\DOS\Neptune] [-Msys C:\msys64]
  Needs the ORIGINAL game files (ON.EXE and NEP00x.DAT). Nothing from the game is uploaded or redistributed:
  the C code is generated on this PC from your own ON.EXE.
  Installs (if missing): MSYS2 + gcc/cmake/ninja/SDL2/python/git, and fetches the pcrecomp toolbox.
  Result: play.bat next to this script.
#>
param(
  [string]$GameDir = "",
  [string]$Msys = "C:\msys64"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# 1. find the game files: -GameDir, else this folder's parent (recomp\ lives inside the game folder), else this folder
if (-not $GameDir) {
  foreach ($c in @((Split-Path -Parent $root), $root, (Get-Location).Path)) { if (Test-Path (Join-Path $c "ON.EXE")) { $GameDir = $c; break } }
}
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir "ON.EXE"))) {
  throw "ON.EXE not found. Re-run with -GameDir <folder containing ON.EXE and NEP000.DAT>."
}
$GameDir = (Resolve-Path $GameDir).Path
$exe = Join-Path $GameDir "ON.EXE"
if (-not (Test-Path (Join-Path $GameDir "NEP000.DAT"))) { Write-Warning "NEP000.DAT not found in $GameDir - the game needs its .DAT files to run." }
Write-Host "Game files: $GameDir"

# 2. MSYS2
$bash = Join-Path $Msys "usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
  Write-Host "MSYS2 not found at $Msys - installing with winget..."
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { throw "winget is unavailable. Install MSYS2 from https://www.msys2.org to $Msys and re-run." }
  winget install --id MSYS2.MSYS2 -e --accept-package-agreements --accept-source-agreements --location $Msys
  if (-not (Test-Path $bash)) { throw "MSYS2 install did not produce $bash. Install it manually from https://www.msys2.org and re-run." }
}
$env:MSYSTEM = "MINGW64"; $env:CHERE_INVOKING = "1"
function Msh([string]$cmd) {
  & $bash -lc $cmd
  if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $cmd" }
}
function Unix([string]$p) { (& $bash -lc "cygpath -u '$p'").Trim() }

# 3. packages
Write-Host "Installing build tools (pacman)..."
Msh "pacman -S --needed --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-python git"

# 4. pcrecomp toolbox (lifter/disassembler), kept inside this folder
$pc = Join-Path $root ".pcrecomp"
if (-not (Test-Path (Join-Path $pc "tools"))) {
  Write-Host "Fetching pcrecomp..."
  Msh "git clone --depth 1 https://github.com/sp00nznet/pcrecomp.git '$(Unix $pc)'"
}

# 5. lift + build
Write-Host "Lifting and building (a few minutes)..."
$r = Unix $root; $e = Unix $exe; $d = Unix $GameDir
Msh "cd '$r' && PCRECOMP_HOME='$r/.pcrecomp' sh tools/build_all.sh '$e' '$d'"

# 6. launcher
$play = Join-Path $root "play.bat"
@"
@echo off
rem Launches the recompiled Operation Neptune. Set NEP_AUDIO=speaker below for PC-speaker sound.
set PATH=$Msys\mingw64\bin;%PATH%
set NEP_VGA=1
"%~dp0build\nep.exe" "$exe" "$GameDir" %*
"@ | Set-Content -Encoding ASCII $play
Write-Host ""
Write-Host "Done. Run play.bat to start the game." -ForegroundColor Green

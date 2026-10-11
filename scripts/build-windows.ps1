#Requires -Version 7
<#
.SYNOPSIS
  Builds skynat-engine for Windows x64 and packages it into dist/win32-x64/.

.DESCRIPTION
  1. Clones OBS Studio $ObsTag (unmodified) into <work>/obs-studio, once.
  2. Configures OBS headless with its own "windows-x64" preset (Visual Studio 17 2022,
     x64, Windows SDK 10.0.22621); OBS's CMake downloads its prebuilt obs-deps.
  3. Builds libobs, the Direct3D 11 renderer and only the plugins the engine loads.
  4. Builds skynat-engine against that libobs.
  5. Packages dist/win32-x64/:
       skynat-engine.exe, obs.dll, libobs-d3d11.dll, w32-pthreads.dll, the obs-deps DLLs,
       the encoder test helpers (obs-nvenc-test.exe, obs-qsv-test.exe, obs-amf-test.exe — the
       plugins look for them next to the executable),
       obs-plugins/64bit/<module>.dll, data/libobs/, data/obs-plugins/<module>/,
       COPYING, NOTICE.md, README.md, SOURCE.txt, licenses/

  Written for GitHub Actions "windows-2022" runners (Visual Studio 2022, CMake 3.28+,
  PowerShell 7). Run from any folder:  pwsh scripts/build-windows.ps1

.PARAMETER ObsTag     OBS Studio release tag (default 31.0.4 — see NOTICE.md).
.PARAMETER Work       Work folder for the OBS checkout and build trees
                      (default: $env:SKYNAT_ENGINE_WORK, else ../skynat-engine-work).
.PARAMETER Config     Build configuration (default Release).
.PARAMETER Jobs       Parallel build jobs (default 4).
.PARAMETER Smoke      Also start the packaged engine once and wait for its "ready" event.
#>
[CmdletBinding()]
param(
  [string]$ObsTag = '31.0.4',
  [string]$Work = '',
  [ValidateSet('Release', 'RelWithDebInfo')]
  [string]$Config = 'Release',
  [int]$Jobs = 4,
  [switch]$Smoke
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$Root = Split-Path -Parent $PSScriptRoot
if (-not $Work) {
  $Work = if ($env:SKYNAT_ENGINE_WORK) { $env:SKYNAT_ENGINE_WORK } else { Join-Path (Split-Path -Parent $Root) 'skynat-engine-work' }
}
$ObsSrc = Join-Path $Work 'obs-studio'
$ObsBuild = Join-Path $ObsSrc 'build_x64'
$EngineBuild = Join-Path $Work 'engine-build-x64'
$Out = Join-Path $Root 'dist/win32-x64'

$ObsTargets = @(
  'libobs', 'libobs-d3d11', 'image-source', 'obs-transitions', 'obs-filters', 'obs-ffmpeg',
  'obs-outputs', 'obs-x264', 'win-dshow', 'obs-nvenc', 'obs-qsv11',
  'obs-nvenc-test', 'obs-qsv-test', 'obs-amf-test'
)
$Plugins = @(
  'image-source', 'obs-transitions', 'obs-filters', 'obs-ffmpeg', 'obs-outputs', 'obs-x264',
  'win-dshow', 'obs-nvenc', 'obs-qsv11'
)
$Helpers = @('obs-nvenc-test.exe', 'obs-qsv-test.exe', 'obs-amf-test.exe')

function Step([string]$Text) { Write-Host "`n== $Text" -ForegroundColor Cyan }

function Invoke-Native {
  param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @())
  Write-Host "> $Exe $($Arguments -join ' ')"
  & $Exe @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

# ---- tools ---------------------------------------------------------------------
$cmakeVersion = [version]((cmake --version | Select-Object -First 1) -replace '[^0-9.]', '')
if ($cmakeVersion -lt [version]'3.28') { throw "CMake 3.28+ is needed (found $cmakeVersion)" }
Step "skynat-engine for Windows x64, OBS $ObsTag, work folder $Work"
New-Item -ItemType Directory -Force -Path $Work | Out-Null

# ---- 1. OBS source ---------------------------------------------------------------
if (-not (Test-Path (Join-Path $ObsSrc '.git'))) {
  Step "cloning OBS Studio $ObsTag"
  Invoke-Native git @('clone', '--depth', '1', '--branch', $ObsTag, '--recurse-submodules', '--shallow-submodules',
    'https://github.com/obsproject/obs-studio', $ObsSrc)
}
$haveTag = (git -C $ObsSrc describe --tags --exact-match 2>$null)
if ($haveTag -ne $ObsTag) { throw "$ObsSrc is at '$haveTag', expected $ObsTag (delete it to re-clone)" }
if (git -C $ObsSrc status --porcelain --untracked-files=no) {
  throw "$ObsSrc has local changes — the engine is built from unmodified OBS"
}
if (Get-ChildItem -Path (Join-Path $Root 'patches') -Filter '*.patch' -ErrorAction SilentlyContinue) {
  throw 'patches/ is not empty; apply them deliberately and record them in NOTICE.md first'
}

# ---- 2. configure OBS (headless) ---------------------------------------------------
Step 'configuring OBS (windows-x64 preset, headless)'
Push-Location $ObsSrc
try {
  Invoke-Native cmake @(
    '--preset', 'windows-x64', '-B', $ObsBuild,
    '-DENABLE_UI=OFF', '-DENABLE_SCRIPTING=OFF', '-DENABLE_BROWSER=OFF', '-DENABLE_WEBSOCKET=OFF',
    '-DENABLE_VST=OFF', '-DENABLE_AJA=OFF', '-DENABLE_DECKLINK=OFF', '-DENABLE_VIRTUALCAM=OFF',
    '-DENABLE_VLC=OFF', '-DENABLE_FREETYPE=OFF', '-DENABLE_WEBRTC=OFF', '-DENABLE_NEW_MPEGTS_OUTPUT=OFF',
    '-DENABLE_SERVICE_UPDATES=OFF', '-DENABLE_COMPAT_UPDATES=OFF', '-DENABLE_NVAFX=OFF', '-DENABLE_NVVFX=OFF',
    '-DENABLE_CCACHE=OFF', '-DCMAKE_COMPILE_WARNING_AS_ERROR=OFF'
  )
} finally { Pop-Location }

# ---- 3. build libobs + renderer + plugins ------------------------------------------
Step "building $($ObsTargets -join ', ')"
Invoke-Native cmake (@('--build', $ObsBuild, '--config', $Config, '--parallel', "$Jobs", '--target') + $ObsTargets)

# ---- 4. build the engine ------------------------------------------------------------
Step 'building skynat-engine'
$pthreadsDir = Join-Path $ObsBuild 'deps/w32-pthreads'
Invoke-Native cmake @(
  '-S', $Root, '-B', $EngineBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64',
  "-Dlibobs_DIR=$(Join-Path $ObsBuild 'libobs')", "-Dw32-pthreads_DIR=$pthreadsDir"
)
Invoke-Native cmake @('--build', $EngineBuild, '--config', $Config, '--parallel', "$Jobs")

# ---- 5. package ------------------------------------------------------------------------
Step "packaging into $Out"
$Rundir = Join-Path $ObsBuild "rundir/$Config"
$Bin = Join-Path $Rundir 'bin/64bit'
$Deps = Get-ChildItem -Path (Join-Path $ObsSrc '.deps') -Directory -Filter 'obs-deps-*-x64' | Select-Object -First 1
if (-not $Deps) { throw "obs-deps for x64 not found under $ObsSrc/.deps" }

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
$pluginOut = Join-Path $Out 'obs-plugins/64bit'
New-Item -ItemType Directory -Force -Path $pluginOut, (Join-Path $Out 'data/obs-plugins'), (Join-Path $Out 'licenses') | Out-Null

Copy-Item (Join-Path $EngineBuild "$Config/skynat-engine.exe") $Out
foreach ($f in @('obs.dll', 'libobs-d3d11.dll', 'w32-pthreads.dll')) {
  $src = Join-Path $Bin $f
  if (-not (Test-Path $src)) { throw "missing $src" }
  Copy-Item $src $Out
}
foreach ($f in $Helpers) {
  $src = Join-Path $Bin $f
  if (Test-Path $src) { Copy-Item $src $Out } else { Write-Warning "$f not built; that encoder family will not be detected" }
}
# Third-party DLLs (FFmpeg, x264, mbedTLS, SRT, RIST, zlib, ...) — as OBS ships them.
Get-ChildItem -Path (Join-Path $Deps.FullName 'bin') -Filter '*.dll' | ForEach-Object { Copy-Item $_.FullName $Out }

foreach ($p in $Plugins) {
  $src = Join-Path $Rundir "obs-plugins/64bit/$p.dll"
  if (-not (Test-Path $src)) { throw "plugin not built: $src" }
  Copy-Item $src $pluginOut
  $data = Join-Path $Rundir "data/obs-plugins/$p"
  if (Test-Path $data) { Copy-Item -Recurse $data (Join-Path $Out 'data/obs-plugins') }
}
$libobsData = Join-Path $Rundir 'data/libobs'
if (-not (Test-Path $libobsData)) { throw "libobs data not found: $libobsData" }
Copy-Item -Recurse $libobsData (Join-Path $Out 'data')

Copy-Item (Join-Path $Root 'COPYING'), (Join-Path $Root 'NOTICE.md'), (Join-Path $Root 'README.md') $Out
$licenses = Join-Path $Deps.FullName 'licenses'
if (Test-Path $licenses) { Copy-Item -Recurse (Join-Path $licenses '*') (Join-Path $Out 'licenses') }
Copy-Item (Join-Path $ObsSrc 'COPYING') (Join-Path $Out 'licenses/OBS-Studio-COPYING')
$engineRev = (git -C $Root rev-parse --short HEAD 2>$null)
if (-not $engineRev) { $engineRev = 'unknown' }
@"
skynat-engine is free software under the GNU General Public License,
version 2 or (at your option) any later version. See COPYING.

It is built from OBS Studio's libobs (https://github.com/obsproject/obs-studio,
tag $ObsTag, unmodified) and the OBS project's prebuilt dependencies
(https://github.com/obsproject/obs-deps). Licences of the third-party
libraries are in licenses/.

Complete corresponding source code: https://github.com/skynatlabs/skynat-engine
(engine revision $engineRev). On request, Skynat will also provide the source
on a physical medium for three years from the date you received this program.
"@ | Set-Content -Encoding utf8 (Join-Path $Out 'SOURCE.txt')

$version = & (Join-Path $Out 'skynat-engine.exe') --version
if ($LASTEXITCODE -ne 0) { throw 'the packaged engine does not run' }
$size = '{0:N1} MB' -f ((Get-ChildItem -Recurse -File $Out | Measure-Object -Sum Length).Sum / 1MB)
Step "done: $version, $size in $Out"

# ---- optional smoke test: start it, wait for "ready", close stdin --------------------------
if ($Smoke) {
  Step 'smoke test'
  $psi = [System.Diagnostics.ProcessStartInfo]::new((Join-Path $Out 'skynat-engine.exe'), '--log-level warning')
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $proc = [System.Diagnostics.Process]::Start($psi)
  $stdout = $proc.StandardOutput.BaseStream
  $header = [byte[]]::new(5)
  $read = 0
  $deadline = [DateTime]::UtcNow.AddSeconds(30)
  while ($read -lt 5 -and [DateTime]::UtcNow -lt $deadline) {
    $n = $stdout.Read($header, $read, 5 - $read)
    if ($n -le 0) { break }
    $read += $n
  }
  if ($read -lt 5) { throw "no message from the engine: $($proc.StandardError.ReadToEnd())" }
  $length = [BitConverter]::ToUInt32($header, 0)
  $body = [byte[]]::new($length)
  $got = 0
  while ($got -lt $length) { $n = $stdout.Read($body, $got, $length - $got); if ($n -le 0) { break }; $got += $n }
  $json = [System.Text.Encoding]::UTF8.GetString($body, 0, $got)
  Write-Host $json
  $proc.StandardInput.Close()   # the engine exits when its stdin closes
  if (-not $proc.WaitForExit(10000)) { $proc.Kill(); throw 'engine did not exit after stdin closed' }
  if ($header[4] -ne 2 -or $json -notmatch '"event":"(ready|error)"') { throw 'unexpected first message' }
  if ($json -match '"event":"error"') { Write-Warning 'engine reported an error at start-up (no GPU on this runner?)' }
  Write-Host "exit code $($proc.ExitCode)"
}

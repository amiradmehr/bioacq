<#
.SYNOPSIS
    Build BioAcq.exe (packaged Release) and assemble a self-contained folder + zip.

.DESCRIPTION
    Output:
      dist\windows\BioAcq\BioAcq.exe     double-click to start the GUI (no console window)
      dist\BioAcq-windows-x64.zip        the same folder, zipped

    The folder holds the Qt runtime (windeployqt, release, no translations, only
    the platform and style plugins the app uses),
    the offscreen platform plugin (for --screenshot without a display), the
    BrainFlow core DLLs and the MSVC runtime DLLs (app-local, so no VC++
    redistributable install is needed).

    Requirements: an MSVC x64 developer environment (cl.exe on PATH, e.g. the
    "x64 Native Tools" prompt or ilammy/msvc-dev-cmd), CMake, Ninja, Qt 6 for
    msvc2022_64 with Qt Serial Port, and BrainFlow built by
    scripts\build_brainflow.ps1.

.EXAMPLE
    scripts\package_windows.ps1 -QtPrefix C:\Qt\6.10.2\msvc2022_64
#>
param(
    [string]$QtPrefix = $(if ($env:QT_PREFIX) { $env:QT_PREFIX } else { $env:QT_ROOT_DIR }),
    [string]$BrainflowRoot = $(if ($env:BRAINFLOW_ROOT) { $env:BRAINFLOW_ROOT } else { Join-Path $env:USERPROFILE 'brainflow' }),
    [string]$BuildDir = $(if ($env:BIOACQ_BUILD_DIR) { $env:BIOACQ_BUILD_DIR } else { Join-Path $env:USERPROFILE '.local\build\bioacq-package-windows' }),
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$RootDir = Split-Path -Parent $PSScriptRoot
$DistRoot = Join-Path $RootDir 'dist'
$AppDir = Join-Path $DistRoot 'windows\BioAcq'
$Zip = Join-Path $DistRoot 'BioAcq-windows-x64.zip'

function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}

if (-not $QtPrefix -or -not (Test-Path (Join-Path $QtPrefix 'bin\windeployqt.exe'))) {
    throw "Qt not found: pass -QtPrefix <...\msvc2022_64> (or set QT_PREFIX / QT_ROOT_DIR)"
}
if (-not (Test-Path (Join-Path $BrainflowRoot 'inc\board_shim.h'))) {
    throw "BrainFlow not found in $BrainflowRoot (run scripts\build_brainflow.ps1 or pass -BrainflowRoot)"
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'cl.exe not on PATH: run from an x64 Native Tools / Developer PowerShell'
}
if (-not $env:VCToolsRedistDir) { throw 'VCToolsRedistDir not set: run from an x64 Native Tools / Developer PowerShell' }

# ------------------------------------------------------------------ build
if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }
$generator = if (Get-Command ninja -ErrorAction SilentlyContinue) { @('-G', 'Ninja') } else { @('-G', 'NMake Makefiles') }
Write-Host "==> configuring $BuildDir"
Invoke-Checked cmake (@('-S', $RootDir, '-B', $BuildDir) + $generator + @(
    '-DCMAKE_BUILD_TYPE=Release',
    '-DBIOACQ_PACKAGED=ON',
    "-DBRAINFLOW_ROOT=$BrainflowRoot",
    "-DQT_PREFIX=$QtPrefix",
    "-DCMAKE_PREFIX_PATH=$QtPrefix"))
Write-Host '==> building'
Invoke-Checked cmake @('--build', $BuildDir, '--config', 'Release', '--parallel')
$exe = Join-Path $BuildDir 'BioAcq.exe'
if (-not (Test-Path $exe)) { throw "build produced no $exe" }

# ------------------------------------------------------------------ deploy
Write-Host "==> assembling $AppDir"
if (Test-Path $AppDir) { Remove-Item -Recurse -Force $AppDir }
New-Item -ItemType Directory -Force -Path $AppDir | Out-Null
Copy-Item $exe $AppDir

$windeployqt = Join-Path $QtPrefix 'bin\windeployqt.exe'
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue' # Windows PowerShell 5.1 turns native stderr into terminating errors
$help = (& $windeployqt --help 2>&1 | Out-String)
$ErrorActionPreference = $prevEap
$deployArgs = @('--release', '--no-translations', '--no-compiler-runtime', '--no-opengl-sw', '--no-system-d3d-compiler')
if ($help -match '--no-system-dxc-compiler') { $deployArgs += '--no-system-dxc-compiler' }
if ($help -match '--no-quick-import') { $deployArgs += '--no-quick-import' }
# The app draws everything with QPainter and uses no TLS, image formats, SVG icons or touch input.
if ($help -match '--skip-plugin-types') { $deployArgs += @('--skip-plugin-types', 'generic,iconengines,imageformats,networkinformation,tls') }
Invoke-Checked $windeployqt ($deployArgs + @((Join-Path $AppDir 'BioAcq.exe')))

# offscreen platform plugin: --selftest / --screenshot with QT_QPA_PLATFORM=offscreen
$platforms = Join-Path $AppDir 'platforms'
New-Item -ItemType Directory -Force -Path $platforms | Out-Null
Copy-Item (Join-Path $QtPrefix 'plugins\platforms\qoffscreen.dll') $platforms

# BrainFlow core DLLs (board-specific DLLs are only loaded for other boards)
foreach ($dll in 'BoardController', 'DataHandler', 'MLModule') {
    Copy-Item (Join-Path $BrainflowRoot "lib\$dll.dll") $AppDir
}

# MSVC runtime, app-local
$crt = Get-ChildItem -Directory (Join-Path $env:VCToolsRedistDir 'x64') -Filter 'Microsoft.VC*.CRT' |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $crt) { throw "no Microsoft.VC*.CRT folder under $env:VCToolsRedistDir\x64" }
Copy-Item (Join-Path $crt.FullName '*.dll') $AppDir
Write-Host "   MSVC runtime from $($crt.FullName)"

# licences of what we ship
$licenses = Join-Path $AppDir 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item (Join-Path $RootDir 'resources\fonts\*-OFL.txt') $licenses
Copy-Item (Join-Path $RootDir 'third_party\brainflow\README.md') (Join-Path $licenses 'BrainFlow-patch-README.md')

# ------------------------------------------------------------------ zip
if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path $AppDir -DestinationPath $Zip -CompressionLevel Optimal
$size = [math]::Round((Get-Item $Zip).Length / 1MB, 1)
Write-Host "==> $Zip ($size MB)"
Get-ChildItem -Recurse $AppDir | Where-Object { -not $_.PSIsContainer } |
    Sort-Object FullName | ForEach-Object { '{0,10:N0}  {1}' -f $_.Length, $_.FullName.Substring($AppDir.Length + 1) }

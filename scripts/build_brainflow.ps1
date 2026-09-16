<#
.SYNOPSIS
    Build and install the patched BrainFlow that bioacq links against (Windows, MSVC 2022 x64).

.DESCRIPTION
    Clones BrainFlow tag 5.23.0 (commit 7994b54b), applies
    third_party\brainflow\emotibit-ancillary-upsample.patch, builds Release and
    installs to the prefix (inc\ + lib\, where lib\ holds BoardController.dll /
    .lib, DataHandler, MLModule and the static Brainflow.lib C++ binding).

    BrainFlow defaults to the static MSVC runtime (/MT); it is built here with
    the dynamic runtime (/MD) instead, because its static C++ binding is linked
    into BioAcq.exe together with Qt, which uses /MD.

    Run from a "x64 Native Tools" / Developer PowerShell (cl.exe on PATH) to
    build with Ninja; otherwise the Visual Studio 2022 generator is used.

.EXAMPLE
    scripts\build_brainflow.ps1                       # installs to %USERPROFILE%\brainflow
    scripts\build_brainflow.ps1 -Prefix C:\deps\brainflow
#>
param(
    [string]$Prefix = (Join-Path $env:USERPROFILE 'brainflow'),
    [string]$SourceDir = '',
    [string]$BuildDir = '',
    [string]$Repo = 'https://github.com/brainflow-dev/brainflow.git'
)

$ErrorActionPreference = 'Stop'
$Tag = '5.23.0'
$Commit = '7994b54b'
$Marker = 'patched (stream_gui_cpp)'
$RootDir = Split-Path -Parent $PSScriptRoot
$Patch = Join-Path $RootDir 'third_party\brainflow\emotibit-ancillary-upsample.patch'

if (-not $SourceDir) { $SourceDir = Join-Path $env:USERPROFILE ".local\src\brainflow-$Tag" }
if (-not $BuildDir) { $BuildDir = Join-Path $SourceDir 'build-bioacq' }

function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}

foreach ($tool in 'git', 'cmake') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found on PATH" }
}

if (-not (Test-Path (Join-Path $SourceDir '.git'))) {
    Write-Host "==> cloning BrainFlow $Tag into $SourceDir"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $SourceDir) | Out-Null
    # autocrlf off: keep LF line endings so the patch applies byte for byte
    Invoke-Checked git @('-c', 'core.autocrlf=false', '-c', 'advice.detachedHead=false', 'clone', '--depth', '1',
        '--branch', $Tag, $Repo, $SourceDir)
}

$head = (& git -C $SourceDir rev-parse HEAD).Trim()
if (-not $head.StartsWith($Commit)) { throw "$SourceDir is at $head, expected tag $Tag ($Commit)" }

$emotibitCpp = Join-Path $SourceDir 'src\board_controller\emotibit\emotibit.cpp'
if (Select-String -Path $emotibitCpp -SimpleMatch $Marker -Quiet) {
    Write-Host '==> patch already applied'
} else {
    Write-Host "==> applying $(Split-Path -Leaf $Patch)"
    Invoke-Checked git @('-C', $SourceDir, '-c', 'core.autocrlf=false', 'apply', '--ignore-whitespace', $Patch)
}
if (-not (Select-String -Path $emotibitCpp -SimpleMatch $Marker -Quiet)) { throw 'patch marker missing after apply' }

$common = @(
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$Prefix",
    "-DBRAINFLOW_VERSION=$Tag",
    '-DMSVC_RUNTIME=dynamic',
    '-DBRAINFLOW_COPY_TO_PACKAGE_DIRS=OFF',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
)
$haveCl = [bool](Get-Command cl.exe -ErrorAction SilentlyContinue)
$haveNinja = [bool](Get-Command ninja -ErrorAction SilentlyContinue)
if ($haveCl -and $haveNinja) {
    $generator = @('-G', 'Ninja')
} else {
    $generator = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
}

Write-Host "==> configuring ($BuildDir, $($generator[1]))"
Invoke-Checked cmake (@('-S', $SourceDir, '-B', $BuildDir) + $generator + $common)
Write-Host '==> building'
Invoke-Checked cmake @('--build', $BuildDir, '--config', 'Release', '--parallel')
Write-Host "==> installing to $Prefix"
Invoke-Checked cmake @('--install', $BuildDir, '--config', 'Release')

foreach ($f in 'inc\board_shim.h', 'lib\BoardController.dll', 'lib\BoardController.lib', 'lib\DataHandler.dll',
    'lib\MLModule.dll', 'lib\Brainflow.lib') {
    if (-not (Test-Path (Join-Path $Prefix $f))) { throw "install incomplete: $f missing under $Prefix" }
}
Write-Host "BrainFlow $Tag (patched) installed in $Prefix"

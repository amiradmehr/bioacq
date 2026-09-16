<#
.SYNOPSIS
    Run BioAcq.exe in a command-line mode, wait for it (with a watchdog), show its output and
    return its exit code.

.DESCRIPTION
    BioAcq.exe is a GUI-subsystem program: PowerShell and cmd do not wait for it and do not
    capture its output by default. This wrapper starts it with redirected stdout/stderr,
    waits up to -TimeoutSec, prints both streams and exits with the program's exit code
    (124 on timeout).

.EXAMPLE
    scripts\run_cli_windows.ps1 -Exe dist\windows\BioAcq\BioAcq.exe -Arguments '--selftest'
    scripts\run_cli_windows.ps1 -Exe dist\windows\BioAcq\BioAcq.exe -Arguments '--screenshot','shot.png' -TimeoutSec 90
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string[]]$Arguments = @(),
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path $Exe).Path
$tmp = [System.IO.Path]::GetTempPath()
$tag = [System.Guid]::NewGuid().ToString('N')
$outFile = Join-Path $tmp "bioacq-$tag.out.txt"
$errFile = Join-Path $tmp "bioacq-$tag.err.txt"

# Quote arguments that contain spaces (Start-Process joins them with blanks).
$quoted = $Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
Write-Host "==> $exePath $($quoted -join ' ')"
$startArgs = @{
    FilePath               = $exePath
    NoNewWindow            = $true
    PassThru               = $true
    RedirectStandardOutput = $outFile
    RedirectStandardError  = $errFile
}
if ($quoted) { $startArgs.ArgumentList = $quoted }
$p = Start-Process @startArgs
$null = $p.Handle # keeps ExitCode available after exit
$finished = $p.WaitForExit($TimeoutSec * 1000)
if (-not $finished) {
    try { $p.Kill() } catch { }
    $p.WaitForExit(5000) | Out-Null
}

if (Test-Path $outFile) { Get-Content $outFile | Write-Host; Remove-Item -Force $outFile -ErrorAction SilentlyContinue }
if (Test-Path $errFile) {
    $err = Get-Content $errFile
    if ($err) { Write-Host '--- stderr ---'; $err | Write-Host }
    Remove-Item -Force $errFile -ErrorAction SilentlyContinue
}

if (-not $finished) {
    Write-Host "TIMEOUT after $TimeoutSec s"
    exit 124
}
$code = $p.ExitCode
Write-Host "exit code $code"
exit $code

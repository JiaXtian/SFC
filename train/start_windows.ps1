# Windows compatibility wrapper.
# The main PowerShell pipeline lives in start.ps1.

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$mainScript = Join-Path $scriptDir "start.ps1"

if (-not (Test-Path -LiteralPath $mainScript -PathType Leaf)) {
    Write-Host "ERROR: missing start.ps1 beside start_windows.ps1" -ForegroundColor Red
    exit 1
}

& $mainScript @args
exit $LASTEXITCODE

param()

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $ProjectRoot

$LogFile = Join-Path -Path $ProjectRoot -ChildPath "server.log"

function Write-Log {
    param([string]$Message)
    "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $Message" | Out-File -FilePath $LogFile -Append -Encoding utf8
}

$Python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $Python) {
    $Python = (Get-Command python3 -ErrorAction SilentlyContinue).Source
}
if (-not $Python) {
    Write-Log "ERROR: python not found in PATH"
    Start-Sleep -Seconds 30
    exit 1
}

Write-Log "Starting Copilot Usage Display Server from $ProjectRoot"
Write-Log "Using Python: $Python"

$env:COPILOT_USAGE_DEBUG = "true"

if (-not $env:COPILOT_USAGE_SERIAL_PORT) {
    Write-Log "COPILOT_USAGE_SERIAL_PORT not set — sending '0' to stdin to disable serial"
    "0" | & $Python "host\server.py" >> $LogFile 2>&1
} else {
    Write-Log "Using serial port: $env:COPILOT_USAGE_SERIAL_PORT"
    & $Python "host\server.py" >> $LogFile 2>&1
}

$ExitCode = $LASTEXITCODE
Write-Log "Server exited with code $ExitCode"
exit $ExitCode

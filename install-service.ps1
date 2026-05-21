#Requires -RunAsAdministrator

$TaskName = "CopilotUsageDisplayServer"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ScriptPath = Join-Path -Path $ProjectDir -ChildPath "start-server.ps1"
$Desc = "Starts the Copilot Usage Display HTTP server at boot/logon"

Write-Host "=== Install Copilot Usage Display Server as Scheduled Task ==="
Write-Host "Task name:  $TaskName"
Write-Host "Script:     $ScriptPath"
Write-Host ""

$envVar = [Environment]::GetEnvironmentVariable("COPILOT_USAGE_SERIAL_PORT", "User")
if (-not $envVar) {
    Write-Host "NOTE: COPILOT_USAGE_SERIAL_PORT is not set."
    Write-Host "  - Serial output to ESP32 will be disabled."
    Write-Host "  - To enable, set the user env var first:"
    Write-Host "      [Environment]::SetEnvironmentVariable('COPILOT_USAGE_SERIAL_PORT', 'COM3', 'User')"
    Write-Host ""
}

$PowerShellExe = "powershell.exe"
$Arguments = "-WindowStyle Hidden -ExecutionPolicy Bypass -File `"$ScriptPath`""

try {
    $Action = New-ScheduledTaskAction -Execute $PowerShellExe -Argument $Arguments
    $Trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $Settings = New-ScheduledTaskSettingsSet `
        -RestartCount 3 `
        -RestartInterval (New-TimeSpan -Minutes 1) `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -StartWhenAvailable `
        -Compatibility Win8
    $Principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited

    Register-ScheduledTask -TaskName $TaskName `
        -Action $Action `
        -Trigger $Trigger `
        -Settings $Settings `
        -Principal $Principal `
        -Description $Desc `
        -Force

    Write-Host "SUCCESS: Scheduled task '$TaskName' created."
    Write-Host "The server will start automatically at your next logon (~1 min delay)."
    Write-Host "Log file: $ProjectDir\server.log"
    Write-Host ""
    Write-Host "To start immediately without logging out:"
    Write-Host "  Start-ScheduledTask -TaskName '$TaskName'"
    Write-Host ""
    Write-Host "To remove: .\uninstall-service.ps1"
} catch {
    Write-Host "ERROR: $_"
    exit 1
}

#Requires -RunAsAdministrator

$TaskName = "CopilotUsageDisplayServer"

Write-Host "=== Uninstall Copilot Usage Display Server Scheduled Task ==="

try {
    $Task = Get-ScheduledTask -TaskName $TaskName -ErrorAction Stop
    Write-Host "Found task '$TaskName' (state: $($Task.State))."
    Write-Host "Stopping and removing..."
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "SUCCESS: Scheduled task '$TaskName' removed."
} catch [Microsoft.PowerShell.Cmdletization.Cim.CimJobException] {
    Write-Host "Task '$TaskName' not found. Nothing to uninstall."
} catch {
    Write-Host "ERROR: $_"
    exit 1
}

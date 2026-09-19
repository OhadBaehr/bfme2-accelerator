# Fires the UAC prompt for inject_run.ps1.
$d = $PSScriptRoot
$script = Join-Path $d 'inject_run.ps1'
$args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script)
Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $args
Write-Output "UAC prompt issued for inject_run.ps1"

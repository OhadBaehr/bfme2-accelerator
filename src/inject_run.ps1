# Runs inject.exe (elevated) and records the result next to it.
$d = $PSScriptRoot
$out = Join-Path $d 'inject_result.txt'
Set-Location $d
$r = & (Join-Path $d 'inject.exe') (Join-Path $d 'aotr_accel.dll') 2>&1
$txt = ($r | Out-String) + "`nexit code: $LASTEXITCODE"
Set-Content -Path $out -Value $txt -Encoding UTF8
Write-Host $txt
Start-Sleep -Seconds 4

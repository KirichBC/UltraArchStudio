param([int]$RootProcessId, [string]$Output)
$ErrorActionPreference = 'Stop'
while ((Get-Process -Id $RootProcessId -ErrorAction SilentlyContinue) -and -not (Test-Path -LiteralPath (Join-Path $Output 'stop-monitor'))) {
    Start-Sleep -Seconds 2
}
wpr.exe -status collectors -details | Set-Content -LiteralPath (Join-Path $Output 'etw-final-status.txt') -Encoding utf8
wpr.exe -stop (Join-Path $Output 'process-events.etl') | Set-Content -LiteralPath (Join-Path $Output 'etw-stop.txt') -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw 'WPR trace stop failed' }

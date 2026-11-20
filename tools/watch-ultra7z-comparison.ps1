param([int]$RootProcessId, [string]$Folder, [string]$Output)
$ErrorActionPreference = 'Stop'
$seen = @{}
$descendants = @{$RootProcessId = $true}
$counter = 0
while (-not (Test-Path -LiteralPath (Join-Path $Output 'stop-monitor'))) {
    $processes = @(Get-CimInstance Win32_Process -Filter "Name = '7z.exe' OR Name = '7za.exe' OR Name = '7zz.exe' OR Name = 'Ultra7z Archive Optimizer.exe'")
    foreach ($process in $processes) {
        if ($descendants.ContainsKey([int]$process.ParentProcessId)) {
            $descendants[[int]$process.ProcessId] = $true
            $key = [string]$process.ProcessId + ':' + [string]$process.CreationDate
            if (-not $seen.ContainsKey($key)) {
                $seen[$key] = $true
                [ordered]@{observedUtc=[DateTime]::UtcNow.ToString('o');pid=$process.ProcessId;parent=$process.ParentProcessId;exe=$process.ExecutablePath;command=$process.CommandLine;created=$process.CreationDate} | ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $Output 'child-commands.jsonl') -Encoding utf8
            }
        }
    }
    if (($counter % 5) -eq 0) {
        $files = @(Get-ChildItem -LiteralPath $Folder -Recurse -File -ErrorAction SilentlyContinue | Select-Object FullName,Length,LastWriteTimeUtc)
        [ordered]@{observedUtc=[DateTime]::UtcNow.ToString('o');files=$files} | ConvertTo-Json -Depth 4 -Compress | Add-Content -LiteralPath (Join-Path $Output 'folder-samples.jsonl') -Encoding utf8
    }
    $counter++
    if (-not (Get-Process -Id $RootProcessId -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Seconds 2
}

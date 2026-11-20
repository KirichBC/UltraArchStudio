[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$ToolsBin,
    [Parameter(Mandatory)][string]$Destination,
    [ValidateRange(1,3)][int]$Repeats = 3,
    [switch]$CollectSystemCounters
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows) { throw 'Run natively on Windows in PowerShell 7.' }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$ToolsBin = (Resolve-Path -LiteralPath $ToolsBin).Path
$seven = Join-Path $ToolsBin '7z.exe'
if (-not (Test-Path -LiteralPath $seven -PathType Leaf)) { throw '7z.exe missing.' }
$Destination = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $Destination) { throw 'Destination must be new; existing evidence will not be overwritten.' }
[void][IO.Directory]::CreateDirectory($Destination)
function Save-Json($Object, [string]$Path) {
    $Object | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $Path -Encoding utf8
}
function Native-Args([string[]]$Values) {
    ($Values | ForEach-Object { '"' + (($_ -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"' }) -join ' '
}
function Manifest([string]$Root) {
    $files = @(Get-ChildItem -LiteralPath $Root -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{path=[IO.Path]::GetRelativePath($Root,$_.FullName).Replace('\','/');bytes=$_.Length;
                   sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
    $directories = @(Get-ChildItem -LiteralPath $Root -Recurse -Directory | ForEach-Object {
        [IO.Path]::GetRelativePath($Root,$_.FullName).Replace('\','/')
    } | Sort-Object)
    [ordered]@{files=$files;directories=$directories}
}
function Check-Manifest($Expected, $Actual) {
    if (($Expected | ConvertTo-Json -Depth 10 -Compress) -cne ($Actual | ConvertTo-Json -Depth 10 -Compress)) {
        throw 'Independent extraction path/size/SHA/directory manifest mismatch.'
    }
}
function Counter-Snapshot {
    if (-not $CollectSystemCounters) { return $null }
    try {
        @(Get-Counter -Counter '\PhysicalDisk(_Total)\Disk Write Bytes/sec','\Memory\Pages Output/sec' -ErrorAction Stop |
            Select-Object -ExpandProperty CounterSamples | ForEach-Object {
                [ordered]@{path=$_.Path;value=$_.CookedValue;timestamp=$_.Timestamp.ToUniversalTime().ToString('o')}
            })
    } catch { [ordered]@{unavailable=$_.Exception.Message} }
}
$fixedTime = [datetime]'2020-01-02T03:04:06Z'
$rows = [Collections.Generic.List[object]]::new()
$cases = [Collections.Generic.List[object]]::new()
foreach ($name in @('text','mixed','pe')) {
    $root = Join-Path $Destination "fixtures/$name"
    [void][IO.Directory]::CreateDirectory((Join-Path $root 'empty'))
    if ($name -eq 'text') {
        [IO.File]::WriteAllText((Join-Path $root 'text.txt'), ('UltraArch RAM probe repeatable text. ' * 100000), [Text.UTF8Encoding]::new($false))
    } elseif ($name -eq 'mixed') {
        [IO.File]::WriteAllText((Join-Path $root 'данные-日本語.txt'), ('Mixed structured content 0123456789. ' * 43000), [Text.UTF8Encoding]::new($false))
        $noise = [byte[]]::new(1500000)
        [Random]::new(4201).NextBytes($noise)
        [IO.File]::WriteAllBytes((Join-Path $root 'noise.bin'), $noise)
    } else {
        foreach ($pair in @(@('System32','cmd-x64.exe'),@('SysWOW64','cmd-x86.exe'))) {
            $source = Join-Path $env:WINDIR "$($pair[0])/cmd.exe"
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required native PE fixture missing: $source" }
            Copy-Item -LiteralPath $source -Destination (Join-Path $root $pair[1])
        }
    }
    Get-ChildItem -LiteralPath $root -Recurse | ForEach-Object { $_.LastWriteTimeUtc = $fixedTime }
    $expected = Manifest $root
    Save-Json $expected (Join-Path $Destination "fixtures/$name-manifest.json")
    $archive = Join-Path $Destination "fixtures/$name.7z"
    Push-Location -LiteralPath $root
    try { & $seven a -y -t7z -mx=1 $archive '.' *> (Join-Path $Destination "fixtures/$name-pack.log")
        if ($LASTEXITCODE -ne 0) { throw "Fixture packing failed: $name" }
    } finally { Pop-Location }
    $cases.Add([ordered]@{name=$name;root=$root;archive=$archive;manifest=$expected})
}
Save-Json ([ordered]@{
    schema=1;startedUtc=[datetime]::UtcNow.ToString('o');exe=$Exe;
    exeSha256=(Get-FileHash -LiteralPath $Exe).Hash;tools=$ToolsBin;
    sevenZipSha256=(Get-FileHash -LiteralPath $seven).Hash;
    dllSha256=(Get-FileHash -LiteralPath (Join-Path $ToolsBin '7z.dll')).Hash;
    cache='OS cache retained; sequential paired runs; no cache flush. First observed pair may be colder.';
    repeats=$Repeats;cpuThreads=4;workers=2;ramMiB=8192;ramGroupLimitMiB=8;ramTemporaryBudgetMiB=512;
    inputArchives=@($cases | ForEach-Object { [ordered]@{name=$_.name;sha256=(Get-FileHash -LiteralPath $_.archive).Hash} });
    physicalWrites='Not measured per run. Optional counters are whole-system point-in-time rates, not attributed bytes.'
}) (Join-Path $Destination 'environment.json')
foreach ($case in $cases) {
    for ($repeat=1; $repeat -le $Repeats; ++$repeat) {
        # Alternate order to reduce systematic first/second cache bias.
        $modes = if ($repeat % 2) { @('disk','ram') } else { @('ram','disk') }
        foreach ($mode in $modes) {
            $run = Join-Path $Destination "$($case.name)-$repeat-$mode"
            [void][IO.Directory]::CreateDirectory($run)
            $output = Join-Path $run 'output.7z'; $reportPath = Join-Path $run 'report.json'
            $cli = @('--cli','--input',$case.archive,'--output',$output,'--tools',$ToolsBin,
                '--temp',(Join-Path $run 'scratch'),'--profile','deep','--search','fast','--no-grouping',
                '--lzma-dictionary','4','--workers','2','--cpu-threads','4','--ram','8192',
                '--ram-group-limit','8','--ram-temp-budget','512','--verifier','dual',
                '--timeout','60','--content','exact','--report',$reportPath)
            if ($mode -eq 'ram') { $cli += '--ram-candidates' }
            Save-Json $cli (Join-Path $run 'arguments.json')
            $before = Counter-Snapshot
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $process = Start-Process -FilePath $Exe -ArgumentList (Native-Args $cli) -WorkingDirectory $run `
                -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $run 'stdout.log') `
                -RedirectStandardError (Join-Path $run 'stderr.log')
            $watch.Stop(); $after = Counter-Snapshot
            if ($process.ExitCode -ne 0) { throw "Benchmark failed: $run (exit $($process.ExitCode))" }
            $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
            $restore = Join-Path $run 'independent-restored'
            $verifyWatch = [Diagnostics.Stopwatch]::StartNew()
            & $seven x -y "-o$restore" $output *> (Join-Path $run 'independent-extract.log')
            if ($LASTEXITCODE -ne 0) { throw "Independent extraction failed: $run" }
            $actual = Manifest $restore; Check-Manifest $case.manifest $actual; $verifyWatch.Stop()
            Save-Json $actual (Join-Path $run 'independent-manifest.json')
            $parentPeak = $null
            try { if ($process.PeakWorkingSet64 -gt 0) { $parentPeak = $process.PeakWorkingSet64 } } catch {}
            $candidatePeak = @($report.results | ForEach-Object { $_.peakRamMB } | Measure-Object -Maximum).Maximum
            $row = [ordered]@{case=$case.name;repeat=$repeat;mode=$mode;seconds=$watch.Elapsed.TotalSeconds;
                bytes=(Get-Item -LiteralPath $output).Length;sha256=(Get-FileHash -LiteralPath $output).Hash;
                candidateFileBytes=$report.temporaryIO.candidateFileBytes;probeFileBytes=$report.temporaryIO.probeFileBytes;
                probeInputFileBytes=$report.temporaryIO.probeInputFileBytes;ramCandidates=$report.temporaryIO.ramCandidates;
                ramInputMaterializedBytes=$report.temporaryIO.ramInputMaterializedBytes;
                ramProbes=$report.temporaryIO.ramProbes;ramInputBytes=$report.inputStorage.ramBytes;
                diskInputBytes=$report.inputStorage.diskBytes;parentPeakWorkingSetBytes=$parentPeak;
                maximumCandidateReportedPeakMB=$candidatePeak;guardedCpuSeconds=$report.guardedChildCpuSeconds;
                independentShaPassed=$true;independentVerifySeconds=$verifyWatch.Elapsed.TotalSeconds;
                systemRatesBefore=$before;systemRatesAfter=$after}
            Save-Json $row (Join-Path $run 'measurement.json'); $rows.Add($row)
            Save-Json @($rows.ToArray()) (Join-Path $Destination 'measurements.json')
            $rows.ToArray() | Select-Object case,repeat,mode,seconds,bytes,candidateFileBytes,probeFileBytes,probeInputFileBytes,
                ramInputMaterializedBytes,ramCandidates,ramProbes,ramInputBytes,diskInputBytes,parentPeakWorkingSetBytes,maximumCandidateReportedPeakMB,
                guardedCpuSeconds,independentShaPassed | Export-Csv -LiteralPath (Join-Path $Destination 'measurements.csv') -NoTypeInformation -Encoding utf8
            Write-Host "$($case.name) #$repeat $mode : $($row.bytes) bytes / $([math]::Round($row.seconds,3)) sec / SHA passed"
        }
    }
}
$pairs = foreach ($case in $cases) {
    for ($repeat=1; $repeat -le $Repeats; ++$repeat) {
        $disk = $rows | Where-Object { $_.case -eq $case.name -and $_.repeat -eq $repeat -and $_.mode -eq 'disk' }
        $ram = $rows | Where-Object { $_.case -eq $case.name -and $_.repeat -eq $repeat -and $_.mode -eq 'ram' }
        [ordered]@{case=$case.name;repeat=$repeat;ramMinusDiskBytes=$ram.bytes-$disk.bytes;
            ramMinusDiskSeconds=$ram.seconds-$disk.seconds;
            artifactLogicalBytesAvoided=($disk.candidateFileBytes+$disk.probeFileBytes+$disk.probeInputFileBytes)-
                                       ($ram.candidateFileBytes+$ram.probeFileBytes+$ram.probeInputFileBytes);
            trackedLogicalBytesAvoided=($disk.diskInputBytes+$disk.candidateFileBytes+$disk.probeFileBytes+$disk.probeInputFileBytes+$disk.ramInputMaterializedBytes)-
                                      ($ram.diskInputBytes+$ram.candidateFileBytes+$ram.probeFileBytes+$ram.probeInputFileBytes+$ram.ramInputMaterializedBytes)}
    }
}
Save-Json @($pairs) (Join-Path $Destination 'pairs.json')
Write-Host "Evidence retained: $Destination"

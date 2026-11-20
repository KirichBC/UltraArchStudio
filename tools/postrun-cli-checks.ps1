#Requires -Version 7.0
param([Parameter(Mandatory)][string]$PackageDirectory,
      [Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference='Stop'
$package=(Resolve-Path -LiteralPath $PackageDirectory).Path
$evidence=[IO.Path]::GetFullPath($EvidenceDirectory)
if(Test-Path -LiteralPath $evidence){throw 'Choose a new evidence directory'}
New-Item -ItemType Directory -Path $evidence | Out-Null
$inputPath=Join-Path $evidence 'LICENSE'
[IO.File]::WriteAllText($inputPath,('Deterministic CLI contract. '+[Environment]::NewLine)*128)
$exe=Join-Path $package 'UltraArchStudio.exe'
function Invoke-Cli([string[]]$Arguments,[string]$Log) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$exe
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true
    $start.RedirectStandardError=$true
    foreach($item in $Arguments){$start.ArgumentList.Add($item)}
    $process=[Diagnostics.Process]::new()
    $process.StartInfo=$start
    try {
        $process.Start() | Out-Null
        $stdout=$process.StandardOutput.ReadToEndAsync()
        $stderr=$process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        [IO.File]::WriteAllText($Log,$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult())
        return $process.ExitCode
    } finally {$process.Dispose()}
}
$base=@('--cli','--input',$inputPath,'--tools',(Join-Path $package 'bin'),
        '--ram','256','--workers','1','--cpu-threads','1','--no-grouping')
$cases=@(
    @{name='full-before-adaptive';flags=@('--search','full','--adaptive');mode=2},
    @{name='full-after-adaptive';flags=@('--adaptive','--search','full');mode=2},
    @{name='legacy-adaptive';flags=@('--adaptive');mode=1},
    @{name='balanced-explicit';flags=@('--search','balanced','--probe-passes','4',
        '--probe-sample-mb','256','--probe-budget','3600','--probe-timeout','600',
        '--full-attempt-limit','2');mode=1})
$rows=@()
foreach($case in $cases) {
    $report=Join-Path $evidence ($case.name+'.json')
    $output=Join-Path $evidence ($case.name+'.7z')
    $arguments=$base+@('--output',$output,'--report',$report)+$case.flags
    $code=Invoke-Cli $arguments (Join-Path $evidence ($case.name+'.log'))
    if($code){throw "CLI case failed: $($case.name)"}
    $result=Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if($result.settings.searchMode -ne $case.mode -or $result.stateName -ne 'Completed') {
        throw "CLI policy mismatch: $($case.name)"
    }
    $rows+=@{name=$case.name;passed=$true;searchMode=$result.settings.searchMode}
}
$invalid=@(
    @('--probe-sample-mb','0'),@('--probe-sample-mb','257'),
    @('--probe-budget','0'),@('--probe-timeout','601'),@('--probe-passes','5'),
    @('--full-attempt-limit','10001'),@('--probe-passes','-1'),
    @('--probe-sample-mb','25junk'),@('--probe-budget','18446744073709551616'))
$index=0
foreach($bad in $invalid) {
    $arguments=$base+@('--what-if')+$bad
    $code=Invoke-Cli $arguments (Join-Path $evidence ('invalid-'+$index+'.log'))
    if($code -eq 0){throw "Invalid CLI accepted: $bad"}
    $rows+=@{name=($bad -join ' ');passed=$true;expectedRejection=$true}
    ++$index
}
$rows | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $evidence 'summary.json') -Encoding utf8NoBOM
Write-Host 'CLI precedence, visible budgets and numeric rejection contracts passed.'

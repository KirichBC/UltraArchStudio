#Requires -Version 7.0
param([string]$PackageDirectory=(Split-Path $PSScriptRoot -Parent),[switch]$KeepEvidence)
$ErrorActionPreference='Stop'
$package=(Resolve-Path $PackageDirectory).Path
$evidence=Join-Path ([IO.Path]::GetTempPath()) ('UltraArch-проверка-日本語-'+[Guid]::NewGuid().ToString('N'))
try {
    New-Item $evidence -ItemType Directory | Out-Null
    $copy=Join-Path $evidence 'Приложение 日本語'
    Copy-Item $package $copy -Recurse
    $manifest=Get-Content (Join-Path $copy 'bin/tool-manifest.json') -Raw | ConvertFrom-Json
    $signatures=@()
    foreach($tool in $manifest.tools) {
        $path=Join-Path (Join-Path $copy 'bin') $tool.file
        if((Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $tool.sha256){throw 'Tool identity mismatch'}
        $signature=Get-AuthenticodeSignature $path
        $signatures+=@{file=$tool.file;status=[string]$signature.Status;subject=$signature.SignerCertificate.Subject}
    }
    $signatures | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $evidence 'authenticode.json') -Encoding utf8NoBOM
    Push-Location ([IO.Path]::GetTempPath())
    try {
        foreach($name in @('UltraArchTests.exe','UltraArchVerificationTests.exe','UltraArchSearchSelectionTests.exe','UltraArchCostModelTests.exe','UltraArchContentSimilarityTests.exe','UltraArchRamPolicyTests.exe','UltraArchRamIntegrationTests.exe','UltraArchRamInputTests.exe','UltraArchRamEngineTests.exe','UltraArchRamFinalizationTests.exe','UltraArchIntegrationTests.exe')) {
            $test=Join-Path $copy "tests/$name"
            if(-not(Test-Path $test -PathType Leaf)){throw "Missing developer test: $test"}
            & $test
            if($LASTEXITCODE){throw "$name failed: $LASTEXITCODE"}
        }
        $input=Join-Path $evidence 'Вход\sample.txt';New-Item (Split-Path $input) -ItemType Directory | Out-Null
        [IO.File]::WriteAllText($input,('Windows package smoke '+[Environment]::NewLine)*512)
        $report=Join-Path $evidence 'run-report.json'
        $output=Join-Path $evidence 'output.7z'
        $process=Start-Process -FilePath (Join-Path $copy 'UltraArchStudio.exe') -ArgumentList @('--cli','--input',('"'+$input+'"'),'--output',('"'+$output+'"'),'--report',('"'+$report+'"'),'--verifier','dual','--ram','256','--workers','1','--cpu-threads','1') -PassThru -Wait -WindowStyle Hidden
        if($process.ExitCode -ne 0 -or -not(Test-Path $output)){throw 'CLI from alternate working directory failed'}
        $result=Get-Content $report -Raw | ConvertFrom-Json
        if($result.stateName -ne 'Completed'){throw 'CLI report is not Completed'}
    } finally {Pop-Location}
    Write-Host 'Installed Unicode-path package / alternate CWD / dual verifier suites passed.'
    Write-Host 'Manual GUI: DPI 100/150/200%, move monitors, minimize/tray, close/cancel/pause/skip/assemble, long paths, sessions, previews. See docs/windows-validation.md.'
    Write-Host "Evidence: $evidence"
    if(-not $KeepEvidence){Remove-Item $evidence -Recurse -Force}
} catch {
    Write-Host "Failed smoke evidence retained: $evidence"
    throw
}

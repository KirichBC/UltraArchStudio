#Requires -Version 7.0
param([ValidateSet('Debug','Release')][string]$Configuration='Release', [switch]$FaultTests, [switch]$IPO)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $preset=if($FaultTests){'windows-faults'}else{'windows'}
    $args=@('--preset',$preset)
    if($IPO){$args+='-DULTRAARCH_IPO=ON'}
    & cmake @args
    if($LASTEXITCODE){throw 'Configure failed. Install VS 2022 C++ x64 and Windows SDK.'}
    $buildPreset=if($FaultTests){'windows-faults'}else{'windows-'+$Configuration.ToLowerInvariant()}
    & cmake --build --preset $buildPreset
    if($LASTEXITCODE){throw 'Build failed'}
    & ctest --preset $buildPreset
    if($LASTEXITCODE){throw 'Tests failed'}
    if($FaultTests){return}
    $destination='release-windows-'+$Configuration.ToLowerInvariant()+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss')
    & "$PSScriptRoot/package.ps1" -BuildDirectory 'build/windows' -Configuration $Configuration -Destination $destination
    & "$PSScriptRoot/windows-smoke.ps1" -PackageDirectory (Join-Path $root ($destination+'/developer'))
    if($LASTEXITCODE){throw 'Installed package smoke failed'}
    Write-Host "Packages: $destination"
} finally {Pop-Location}

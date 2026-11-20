#Requires -Version 7.0
param([string]$BuildDirectory='build/windows',[string]$Configuration='Release',[string]$Destination='release-windows')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$buildPath=[IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
$destinationPath=[IO.Path]::GetFullPath((Join-Path $root $Destination))
function Assert-ProjectChild([string]$path) {
    $resolved=[IO.Path]::GetFullPath($path)
    $prefix=[IO.Path]::GetFullPath($root).TrimEnd('\')+'\'
    if(-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {
        throw "Package target must stay inside the project: $resolved"
    }
}
Assert-ProjectChild $destinationPath
if(Test-Path $destinationPath){throw 'Destination already exists; choose a new directory'}
$stage=$destinationPath+'.stage-'+[Guid]::NewGuid().ToString('N')
Assert-ProjectChild $stage
try {
    foreach($kind in @('portable','developer')) {
        $path=Join-Path $stage $kind
        & cmake --install $buildPath --config $Configuration --prefix $path --component Runtime
        if($LASTEXITCODE){throw 'Runtime install failed'}
        if($kind -eq 'developer') {
            & cmake --install $buildPath --config $Configuration --prefix $path --component Developer
            if($LASTEXITCODE){throw 'Developer install failed'}
            $symbols=Join-Path $buildPath $Configuration
            Get-ChildItem $symbols -Filter '*.pdb' -ErrorAction SilentlyContinue | ForEach-Object {
                New-Item (Join-Path $path 'symbols') -ItemType Directory -Force | Out-Null
                Copy-Item $_.FullName (Join-Path $path 'symbols')
            }
        }
        $manifest=Get-ChildItem $path -File -Recurse | Sort-Object FullName | ForEach-Object {
            $relative=[IO.Path]::GetRelativePath($path,$_.FullName).Replace('\','/')
            '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$relative
        }
        $manifest | Set-Content (Join-Path $path 'SHA256SUMS.txt') -Encoding utf8NoBOM
        Compress-Archive -Path (Join-Path $path '*') -DestinationPath (Join-Path $stage ('UltraArchStudio-Win64-'+$kind+'.zip'))
    }
    Get-ChildItem -LiteralPath $stage -Filter '*.zip' -File | Sort-Object Name | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$_.Name
    } | Set-Content (Join-Path $stage 'SHA256SUMS.txt') -Encoding utf8NoBOM
    Assert-ProjectChild $stage
    Move-Item -LiteralPath $stage -Destination $destinationPath
    Write-Host "Packages: $destinationPath"
} finally {if(Test-Path -LiteralPath $stage){Assert-ProjectChild $stage;Remove-Item -LiteralPath $stage -Recurse -Force}}

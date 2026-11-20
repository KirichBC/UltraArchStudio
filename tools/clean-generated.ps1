#requires -Version 7.0
<#
.SYNOPSIS
Inventories an explicit list of obsolete generated artifacts. Default: dry run.
.EXAMPLE
pwsh -NoProfile -File tools/clean-generated.ps1
.EXAMPLE
pwsh -NoProfile -File tools/clean-generated.ps1 -IncludeVerifiedRestorations -DeleteGenerated
Run deletion manually only after reviewing the dry-run inventory. Source, canonical
comparison inputs/evidence, latest packages and active MSVC build trees are preserved.
#>
[CmdletBinding()]
param([switch]$DeleteGenerated, [switch]$IncludeVerifiedRestorations)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$rootPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd([IO.Path]::DirectorySeparatorChar)
$rootPrefix = $rootPath + [IO.Path]::DirectorySeparatorChar
$comparison = 'validation/ultra7z-comparison-2026-10-08-no-ttf'

function Assert-NoLinkedAncestors([string]$Path) {
    $current = Get-Item -LiteralPath $Path -Force
    while ($null -ne $current) {
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing reparse point: $($current.FullName)"
        }
        $current = if ($current -is [IO.DirectoryInfo]) { $current.Parent } else { $current.Directory }
    }
}

function Resolve-WorkspacePath([string]$Relative) {
    $resolved = [IO.Path]::GetFullPath((Join-Path $rootPath $Relative))
    if (-not $resolved.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes workspace: $resolved"
    }
    return $resolved
}

# Traverse one level at a time, checking attributes before descending into a directory.
# Get-ChildItem -Recurse can traverse an unexpected junction before it is rejected.
function Get-SafeInventory([string]$Path) {
    Assert-NoLinkedAncestors $Path
    $target = Get-Item -LiteralPath $Path -Force
    if (-not $target.PSIsContainer) { throw "Expected a generated directory: $Path" }
    $queue = [Collections.Generic.Stack[string]]::new()
    $queue.Push($Path)
    [long]$bytes = 0
    [long]$files = 0
    while ($queue.Count -gt 0) {
        foreach ($item in Get-ChildItem -LiteralPath $queue.Pop() -Force) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing nested reparse point: $($item.FullName)"
            }
            if ($item.PSIsContainer) { $queue.Push($item.FullName) }
            else { $bytes += $item.Length; $files++ }
        }
    }
    return @{ bytes = $bytes; files = $files }
}

Assert-NoLinkedAncestors $rootPath
$protected = @(
    '.git', 'src', 'tests', 'tools', 'vendor', 'bin', 'resources', 'licenses',
    'build/windows', 'build/windows-faults', 'build/dependencies',
    'release/msvc-2026-10-09/selection-final-debug',
    'release/msvc-2026-10-09/selection-final-release',
    "$comparison/input-original.7z", "$comparison/reference-manifest.json", "$comparison/reference"
)
$protectedAbsolute = @($protected | ForEach-Object { Resolve-WorkspacePath $_ })
$targets = @(
    'release/msvc-2026-10-09/debug',
    'release/msvc-2026-10-09/modes-debug',
    'release/msvc-2026-10-09/modes-release',
    'release/msvc-2026-10-09/ordering-debug',
    'release/msvc-2026-10-09/ordering-release',
    'release/msvc-2026-10-09/probes-debug',
    'release/msvc-2026-10-09/probes-release',
    'release/msvc-2026-10-09/release',
    'release/msvc-2026-10-09/selection-release',
    'build/windows-mingw', 'build/installed-msvc-debug', 'build/installed-msvc-release',
    'build/password-probe', 'build/long-cwd-probe'
)
$proofs = @{}
if ($IncludeVerifiedRestorations) {
    $manifestPath = Resolve-WorkspacePath "$comparison/reference-manifest.json"
    $baselinePath = Resolve-WorkspacePath "$comparison/input-original.7z"
    $referencePath = Resolve-WorkspacePath "$comparison/reference"
    foreach ($path in @($manifestPath, $baselinePath, $referencePath)) { Assert-NoLinkedAncestors $path }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $baselineSha = (Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash
    $verifiedCopies = @(
        @{ directory = "$comparison/ultra7z-extreme2-01/independent-restored"; proof = "$comparison/ultra7z-extreme2-01/verified-result.json" },
        @{ directory = "$comparison/ultraarch-extreme-01/independent-restored"; proof = "$comparison/ultraarch-extreme-01/measurement.json" }
    )
    foreach ($copy in $verifiedCopies) {
        $proofPath = Resolve-WorkspacePath $copy.proof
        Assert-NoLinkedAncestors $proofPath
        $proof = Get-Content -LiteralPath $proofPath -Raw | ConvertFrom-Json
        if ($proof.inputSha256 -ne $baselineSha -or
            $proof.independentVerification.perFileSha256Matched -ne $true -or
            $proof.independentVerification.files -ne @($manifest.files).Count) {
            throw "Missing matching successful verification: $proofPath"
        }
        $targets += $copy.directory
        $proofs[$copy.directory] = $copy.proof
    }
}

# Preflight the entire allowlist before removing anything.
$inventory = @()
foreach ($relative in $targets) {
    $absolute = Resolve-WorkspacePath $relative
    foreach ($saved in $protectedAbsolute) {
        if ($absolute.Equals($saved, [StringComparison]::OrdinalIgnoreCase) -or
            $absolute.StartsWith($saved + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
            $saved.StartsWith($absolute + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Target intersects a protected path: $absolute"
        }
    }
    $entry = [ordered]@{ relativePath = $relative; absolutePath = $absolute; bytes = 0L; files = 0L; status = 'missing'; proof = $null }
    if ($proofs.ContainsKey($relative)) { $entry.proof = $proofs[$relative] }
    if (Test-Path -LiteralPath $absolute) {
        $counts = Get-SafeInventory $absolute
        $entry.bytes = $counts.bytes
        $entry.files = $counts.files
        $entry.status = 'planned'
    }
    $inventory += [pscustomobject]$entry
}
$validationPath = Resolve-WorkspacePath 'validation'
Assert-NoLinkedAncestors $validationPath
$reportPath = Join-Path $validationPath ("cleanup-generated-{0}.json" -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff'))
$report = [ordered]@{
    schema = 1; utc = [DateTime]::UtcNow.ToString('o'); root = $rootPath
    mode = $(if ($DeleteGenerated) { 'manual-delete' } else { 'dry-run' })
    protected = $protected; plannedBytes = [long](($inventory | Measure-Object bytes -Sum).Sum)
    removedBytes = 0L; entries = $inventory; completed = $false
}
function Save-Report { $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8NoBOM }
Save-Report
try {
    if ($DeleteGenerated) {
        foreach ($entry in $inventory) {
            if ($entry.status -ne 'planned') { continue }
            # Recheck just before deletion; this cannot eliminate all concurrent filesystem races.
            $now = Get-SafeInventory $entry.absolutePath
            if ($now.bytes -ne $entry.bytes -or $now.files -ne $entry.files) {
                throw "Target changed after inventory: $($entry.absolutePath)"
            }
            $entry.status = 'removing'
            Save-Report
            Remove-Item -LiteralPath $entry.absolutePath -Recurse -Force -ErrorAction Stop
            $entry.status = 'removed'
            $report.removedBytes += $entry.bytes
            Save-Report
        }
    }
    $report.completed = $true
} finally { Save-Report }
$inventory | Format-Table relativePath, bytes, files, status -AutoSize
Write-Host "Planned: $($report.plannedBytes) bytes; removed: $($report.removedBytes) bytes."
Write-Host "Report: $reportPath"
if (-not $DeleteGenerated) { Write-Host 'Dry run: nothing deleted. Manual deletion requires -DeleteGenerated.' }

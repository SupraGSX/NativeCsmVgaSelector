# Actual installer functions on temporary files; no Storage cmdlet or disk access.
# Run under Windows PowerShell 5.1 or PowerShell 7. SPDX-License-Identifier: GPL-3.0-only
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '../Installers/Windows/Installer.ps1'
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$names = @('Stop-Installer', 'Copy-FileDurable', 'Move-LoaderFile', 'Install-Efi')
foreach ($definition in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
    if ($definition.Name -in $names) {
        $text = $definition.Extent.Text
        if ($definition.Name -eq 'Copy-FileDurable') { $text = $text.Replace('function Copy-FileDurable(', 'function Actual-CopyFile(') }
        if ($definition.Name -eq 'Move-LoaderFile') { $text = $text.Replace('function Move-LoaderFile(', 'function Actual-MoveFile(') }
        Invoke-Expression $text
    }
}
function Read-Host { return 'B' }
function Install-PciDatabase { }
function Copy-FileDurable([string]$Source, [string]$Destination) {
    if ($Destination.EndsWith('.tmp') -and $script:Case -eq 'partial-stage') {
        [IO.File]::WriteAllText($Destination, 'partial')
        throw 'Injected disk-full/partial staging write'
    }
    Actual-CopyFile $Source $Destination
    if ($Destination.EndsWith('.tmp') -and $script:Case -eq 'wrong-stage') {
        [IO.File]::WriteAllText($Destination, 'changed source content')
    }
}
function Move-LoaderFile([string]$Source, [string]$Destination) {
    ++$script:MoveCount
    if (($script:Case -eq 'commit-failure' -and $script:MoveCount -eq 2) -or
        ($script:Case -eq 'restore-failure' -and $script:MoveCount -ge 2)) {
        throw 'Injected move failure'
    }
    Actual-MoveFile $Source $Destination
    if ($script:Case -eq 'final-hash-failure' -and $script:MoveCount -eq 2) {
        [IO.File]::WriteAllText($Destination, 'injected corruption')
    }
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('ncv-loader-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $payload = Join-Path $root 'source.efi'
    [IO.File]::WriteAllText($payload, 'complete new synthetic loader')
    $hash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
    $count = 0
    foreach ($script:Case in @('success', 'partial-stage', 'wrong-stage', 'commit-failure', 'final-hash-failure', 'restore-failure')) {
        $volume = Join-Path $root $script:Case
        $boot = Join-Path $volume 'EFI/BOOT'
        New-Item -ItemType Directory -Path $boot -Force | Out-Null
        $destination = Join-Path $boot 'BOOTX64.EFI'
        [IO.File]::WriteAllText($destination, 'complete old synthetic loader')
        $script:MoveCount = 0; $failed = $false
        $failureDetail = ''
        try { Install-Efi $volume $payload $hash } catch { $failed = $true; $failureDetail = $_.ToString() }
        if ($script:Case -eq 'success') {
            if ($failed -or [IO.File]::ReadAllText($destination) -cne 'complete new synthetic loader') { throw "Successful replacement failed: $failureDetail" }
        } elseif ($script:Case -eq 'restore-failure') {
            if (-not $failed) { throw 'Expected recovery failure' }
            $previous = @(Get-ChildItem -LiteralPath $boot -Force | Where-Object Name -Like '*.previous')
            if ($previous.Count -ne 1 -or [IO.File]::ReadAllText($previous[0].FullName) -cne 'complete old synthetic loader') { throw 'Recoverable previous loader lost' }
        } else {
            if (-not $failed -or [IO.File]::ReadAllText($destination) -cne 'complete old synthetic loader') { throw "Old loader not preserved/restored: $script:Case" }
        }
        $backups = @(Get-ChildItem -LiteralPath $boot | Where-Object Name -Like 'BOOTX64-backup-*.efi')
        if ($backups.Count -ne 1 -or [IO.File]::ReadAllText($backups[0].FullName) -cne 'complete old synthetic loader') { throw 'Verified backup lost' }
        if (@(Get-ChildItem -LiteralPath $boot -Force | Where-Object Name -Like '*.tmp').Count -ne 0) { throw 'Temporary staging file leaked' }
        ++$count
    }
    Write-Output "PASS $count Windows loader staging/commit/rollback scenarios"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}

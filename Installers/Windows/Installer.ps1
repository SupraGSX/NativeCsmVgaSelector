[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$InstallerDirectory = $PSScriptRoot,
    [switch]$WaitOnExit
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Stop-Installer([string]$Message) { throw $Message }

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Format-Capacity([UInt64]$Bytes) { return ('{0:N2} GiB' -f ($Bytes / 1GB)) }

function Get-SinglePartitionForDrive([char]$DriveLetter) {
    $partitions = @(Get-Partition -DriveLetter $DriveLetter -ErrorAction Stop)
    if ($partitions.Count -ne 1) {
        Stop-Installer "Could not map drive $DriveLetter`: to exactly one partition."
    }
    return $partitions[0]
}

function Get-DiskPartitions([int]$DiskNumber) {
    # The filtered cmdlet throws ObjectNotFound for a valid, unpartitioned disk.
    # Enumerate successfully first, then filter; actual enumeration errors still stop us.
    return @(Get-Partition -ErrorAction Stop | Where-Object { $_.DiskNumber -eq $DiskNumber })
}

function Test-EligibleUsbDisk($Disk) {
    try {
        if (($null -eq $Disk) -or ($Disk.BusType.ToString() -ne 'USB')) { return $false }
        if ($Disk.IsOffline -or $Disk.IsReadOnly -or $Disk.IsBoot -or $Disk.IsSystem) { return $false }
        if (($Disk.Number -eq $script:OperatingSystemDisk) -or
            ($Disk.Number -eq $script:InstallerSourceDisk)) { return $false }
        foreach ($partition in (Get-DiskPartitions $Disk.Number)) {
            if ($partition.IsBoot -or $partition.IsSystem -or
                $partition.IsReadOnly -or $partition.IsOffline) { return $false }
        }
        return $true
    } catch {
        return $false
    }
}

function Get-EligibleUsbDisks {
    $eligible = @()
    foreach ($disk in @(Get-Disk -ErrorAction Stop | Sort-Object Number)) {
        if (Test-EligibleUsbDisk $disk) { $eligible += $disk }
    }
    return $eligible
}

function Get-PartitionVolumes($Partition) { return @($Partition | Get-Volume -ErrorAction SilentlyContinue) }

function Get-PartitionMountPaths($Partition) {
    $paths = @()
    foreach ($path in @($Partition.AccessPaths)) {
        if ((-not [string]::IsNullOrWhiteSpace($path)) -and
            (-not $path.StartsWith('\\?\Volume{', [StringComparison]::OrdinalIgnoreCase))) {
            $paths += [string]$path
        }
    }
    return $paths
}

function Test-DiskHasMountedVolume([int]$DiskNumber) {
    foreach ($partition in (Get-DiskPartitions $DiskNumber)) {
        if (@(Get-PartitionMountPaths $partition).Count -ne 0) { return $true }
    }
    return $false
}

function Show-DiskSummary($Disk) {
    $partitions = @(Get-DiskPartitions $Disk.Number)
    $mounted = if (Test-DiskHasMountedVolume $Disk.Number) { 'yes' } else { 'no' }
    Write-Host ('Disk {0}: {1}; {2}; partitions={3}; mounted volumes={4}' -f
        $Disk.Number, $Disk.FriendlyName, (Format-Capacity $Disk.Size),
        $partitions.Count, $mounted)
}

function Select-UsbDisk([string]$Prompt) {
    $disks = @(Get-EligibleUsbDisks)
    if ($disks.Count -eq 0) { Stop-Installer 'No eligible USB physical disk was found.' }
    Write-Host ''
    Write-Host $Prompt
    for ($i = 0; $i -lt $disks.Count; $i++) {
        Write-Host -NoNewline ('[{0}] ' -f ($i + 1))
        Show-DiskSummary $disks[$i]
    }
    Write-Host '[C] Cancel'
    $answer = (Read-Host 'Selection').Trim()
    if ($answer -match '^[Cc]$') { return $null }
    $index = 0
    if ((-not [int]::TryParse($answer, [ref]$index)) -or
        ($index -lt 1) -or ($index -gt $disks.Count)) {
        Stop-Installer 'Invalid USB-disk selection.'
    }
    return $disks[$index - 1]
}

function New-DiskSnapshot($Disk) {
    return [pscustomobject]@{
        Number = [int]$Disk.Number
        UniqueId = [string]$Disk.UniqueId
        FriendlyName = [string]$Disk.FriendlyName
        Size = [UInt64]$Disk.Size
    }
}

function Assert-SameSafeUsbDisk($Snapshot, [bool]$RequireUniqueId) {
    $current = Get-Disk -Number $Snapshot.Number -ErrorAction Stop
    if (-not (Test-EligibleUsbDisk $current)) {
        Stop-Installer 'The selected physical disk no longer passes USB safety checks.'
    }
    if (($current.Number -ne $Snapshot.Number) -or
        ([string]$current.FriendlyName -cne $Snapshot.FriendlyName) -or
        ([UInt64]$current.Size -ne $Snapshot.Size)) {
        Stop-Installer 'The selected physical disk identity changed.'
    }
    if ($RequireUniqueId -and [string]::IsNullOrWhiteSpace($Snapshot.UniqueId)) {
        Stop-Installer 'The selected disk has no stable UniqueId; destructive mode is refused.'
    }
    if ((-not [string]::IsNullOrWhiteSpace($Snapshot.UniqueId)) -and
        ([string]$current.UniqueId -cne $Snapshot.UniqueId)) {
        Stop-Installer 'The selected physical disk UniqueId changed.'
    }
    return $current
}

function Assert-NoReparseComponents([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($fullPath)
    $current = $root
    foreach ($component in ($fullPath.Substring($root.Length) -split '[\\/]')) {
        if ($component.Length -eq 0) { continue }
        $current = Join-Path $current $component
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Stop-Installer "Installer-package path contains a reparse point: $current"
        }
    }
}

function Test-HasDriveLetter($VolumeOrPartition) {
    # Storage cmdlets can return a NUL char, not $null, for an unassigned letter.
    return ([string]$VolumeOrPartition.DriveLetter -cmatch '^[A-Za-z]$')
}

function Get-FreeDriveAccessPath {
    $used = @(Get-Volume -ErrorAction Stop | Where-Object { Test-HasDriveLetter $_ } |
        ForEach-Object { "$($_.DriveLetter):\" })
    for ($code = 90; $code -ge 68; $code--) {
        $path = "$([char]$code):\"
        if ($used -notcontains $path) { return $path }
    }
    Stop-Installer 'No temporary drive letter is available.'
}

function Get-SourceHash([string]$SourcePath, [string]$SourceName, [string]$PackageDirectory) {
    if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
        Stop-Installer "Package file is missing: $SourceName"
    }
    $sourceItem = Get-Item -LiteralPath $SourcePath -Force -ErrorAction Stop
    if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Stop-Installer 'Package file is a reparse point; refusing an external source target.'
    }
    $actual = (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = Join-Path $PackageDirectory 'SHA256SUMS'
    $manifestItems = @(Get-ChildItem -LiteralPath $PackageDirectory -Force -ErrorAction Stop |
        Where-Object { $_.Name -ieq 'SHA256SUMS' })
    if ($manifestItems.Count -gt 1) { Stop-Installer 'Multiple SHA256SUMS entries were found.' }
    if ($manifestItems.Count -eq 1) {
        $manifestItem = $manifestItems[0]
        if ($manifestItem.PSIsContainer -or
            (($manifestItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            Stop-Installer 'SHA256SUMS is not a regular sibling file.'
        }
        $manifest = $manifestItem.FullName
        $entries = @()
        foreach ($line in (Get-Content -LiteralPath $manifest -ErrorAction Stop)) {
            if ($line -match '^\s*([0-9A-Fa-f]{64})\s+\*?(.+?)\s*$') {
                $entryName = [IO.Path]::GetFileName($Matches[2].Replace('/', '\'))
                if ($entryName -ceq $SourceName) {
                    $entries += $Matches[1].ToLowerInvariant()
                }
            }
        }
        if ($entries.Count -ne 1) {
            Stop-Installer "SHA256SUMS must contain exactly one entry for $SourceName."
        }
        if ($entries[0] -cne $actual) {
            Stop-Installer "SHA-256 verification failed for $SourceName."
        }
        Write-Host "Source SHA-256 verified through SHA256SUMS: $actual"
    } else {
        Write-Warning 'SHA256SUMS is absent; file integrity cannot be checked against a package manifest.'
        $confirmation = (Read-Host 'Type CONTINUE WITHOUT SHA256SUMS to continue').Trim()
        if ($confirmation -cne 'CONTINUE WITHOUT SHA256SUMS') {
            Stop-Installer 'Installation cancelled because SHA256SUMS is absent.'
        }
        Write-Warning "Continuing with computed source SHA-256: $actual"
    }
    return $actual
}

function Test-X64EfiApplication([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 96) { return $false }
    if (($bytes[0] -ne 0x4D) -or ($bytes[1] -ne 0x5A)) { return $false }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if (($peOffset -lt 0) -or (($peOffset + 94) -gt $bytes.Length)) { return $false }
    if (($bytes[$peOffset] -ne 0x50) -or ($bytes[$peOffset + 1] -ne 0x45) -or
        ($bytes[$peOffset + 2] -ne 0) -or ($bytes[$peOffset + 3] -ne 0)) { return $false }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    $subsystem = [BitConverter]::ToUInt16($bytes, $peOffset + 92)
    return (($machine -eq 0x8664) -and ($optionalMagic -eq 0x020B) -and ($subsystem -eq 10))
}

function Copy-FileDurable([string]$Source, [string]$Destination) {
    $inputStream = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $outputStream = New-Object IO.FileStream(
            $Destination,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None,
            1048576,
            [IO.FileOptions]::WriteThrough
        )
        try {
            $inputStream.CopyTo($outputStream)
            $outputStream.Flush($true)
        } finally {
            $outputStream.Dispose()
        }
    } finally {
        $inputStream.Dispose()
    }
}

function Install-PciDatabase([string]$BootDirectory) {
    $destination = Join-Path $BootDirectory 'pci.ids'
    if (Test-Path -LiteralPath $destination) {
        $item = Get-Item -LiteralPath $destination -Force -ErrorAction Stop
        if ($item.PSIsContainer -or (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            Stop-Installer 'Existing pci.ids is not a regular file.'
        }
    }
    $temp = Join-Path $BootDirectory ('.pci.ids-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Copy-FileDurable $script:DatabasePath $temp
        $hash = (Get-FileHash -LiteralPath $temp -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -cne $script:DatabaseHash) { Stop-Installer 'Temporary pci.ids hash mismatch.' }
        Move-Item -LiteralPath $temp -Destination $destination -Force -ErrorAction Stop
        $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -cne $script:DatabaseHash) { Stop-Installer 'Installed pci.ids hash mismatch.' }
        Write-Host "Installed and verified GPU names: $destination"
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction Stop }
    }
}

function Move-LoaderFile([string]$Source, [string]$Destination) {
    # Same-volume move; deliberately refuse to overwrite an unexpected path.
    [IO.File]::Move($Source, $Destination)
}

function Install-Efi([string]$VolumeRoot, [string]$SourcePath, [string]$SourceHash) {
    $bootDirectory = Join-Path $VolumeRoot 'EFI\BOOT'
    foreach ($component in @($VolumeRoot, (Join-Path $VolumeRoot 'EFI'), $bootDirectory)) {
        if (Test-Path -LiteralPath $component) {
            $item = Get-Item -LiteralPath $component -Force -ErrorAction Stop
            if (-not $item.PSIsContainer -or (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
                Stop-Installer "Loader directory is not a regular directory: $component"
            }
        }
    }
    New-Item -ItemType Directory -Path $bootDirectory -Force -ErrorAction Stop | Out-Null
    $destination = Join-Path $bootDirectory 'BOOTX64.EFI'
    $backup = $null
    $originalHash = $null
    if (Test-Path -LiteralPath $destination) {
        if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
            Stop-Installer 'Existing BOOTX64.EFI is not a regular file.'
        }
        $destinationItem = Get-Item -LiteralPath $destination -Force -ErrorAction Stop
        if (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Stop-Installer 'Existing BOOTX64.EFI is a reparse point; refusing replacement.'
        }
        Write-Warning "An existing loader was found at $destination"
        $choice = (Read-Host 'Type B to back it up and replace it, or press Enter to cancel').Trim()
        if ($choice -cne 'B') { Stop-Installer 'Installation cancelled; existing loader was preserved.' }
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $backup = Join-Path $bootDirectory "BOOTX64-backup-$stamp.efi"
        if (Test-Path -LiteralPath $backup) { Stop-Installer "Backup already exists: $backup" }
        $originalHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
        Copy-FileDurable $destination $backup
        if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
            Stop-Installer 'Existing-loader backup was not created.'
        }
        $backupHash = (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash
        if ($backupHash -cne $originalHash) { Stop-Installer 'Existing-loader backup verification failed.' }
        Write-Host "Existing loader backed up as $([IO.Path]::GetFileName($backup))"
    }
    # Stage completely before touching the active loader. File.Replace is not
    # supported by every FAT driver, so commit uses two same-directory moves.
    # The verified backup survives both interruption and a failed rollback.
    $temp = Join-Path $bootDirectory ('.BOOTX64-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $previous = Join-Path $bootDirectory ('.BOOTX64-' + [Guid]::NewGuid().ToString('N') + '.previous')
    $movedPrevious = $false
    $installedNew = $false
    try {
        Copy-FileDurable $SourcePath $temp
        $stagedHash = (Get-FileHash -LiteralPath $temp -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($stagedHash -cne $SourceHash) { throw 'Staged BOOTX64.EFI hash verification failed; active loader preserved.' }
        if ($backup) {
            $currentHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
            if ($currentHash -cne $originalHash) { throw 'Active loader changed during staging; refusing replacement.' }
            Move-LoaderFile $destination $previous
            $movedPrevious = $true
        }
        Move-LoaderFile $temp $destination
        $installedNew = $true
        $installedHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($installedHash -cne $SourceHash) { throw 'Installed BOOTX64.EFI hash verification failed.' }
    } catch {
        $failure = $_
        try {
            if ($installedNew) { [IO.File]::Delete($destination) }
            if ($movedPrevious) {
                Move-LoaderFile $previous $destination
                if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -cne $originalHash) {
                    throw 'Restored loader hash mismatch.'
                }
            }
        } catch {
            throw "Loader replacement and automatic restoration failed. Recover from $backup or $previous. Original error: $failure; restoration: $_"
        }
        throw $failure
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction Stop }
    }
    if ($movedPrevious) { Remove-Item -LiteralPath $previous -Force -ErrorAction Stop }
    Install-PciDatabase $bootDirectory
    Write-Host "Installed and verified: $destination"
    Write-Host "Installed SHA-256: $installedHash"
}

function Get-EligibleFatPartitions {
    $results = @()
    foreach ($disk in @(Get-EligibleUsbDisks)) {
        foreach ($partition in (Get-DiskPartitions $disk.Number)) {
            if ($partition.IsBoot -or $partition.IsSystem -or
                $partition.IsReadOnly -or $partition.IsOffline) { continue }
            $volumes = @(Get-PartitionVolumes $partition)
            if ($volumes.Count -ne 1) { continue }
            $fileSystem = [string]$volumes[0].FileSystem
            if (($fileSystem -cne 'FAT') -and ($fileSystem -cne 'FAT32')) { continue }
            $results += [pscustomobject]@{
                Disk = $disk
                Partition = $partition
                Volume = $volumes[0]
            }
        }
    }
    return $results
}

function Install-ToExistingFat([string]$SourcePath, [string]$SourceHash) {
    $candidates = @(Get-EligibleFatPartitions)
    if ($candidates.Count -eq 0) { Stop-Installer 'No eligible FAT/FAT32 partition on a USB disk was found.' }
    Write-Host ''
    Write-Host 'Eligible existing FAT/FAT32 USB partitions:'
    for ($i = 0; $i -lt $candidates.Count; $i++) {
        $candidate = $candidates[$i]
        $letter = if (Test-HasDriveLetter $candidate.Partition) { "$($candidate.Partition.DriveLetter):" } else { '<none>' }
        Write-Host ('[{0}] Disk {1}, partition {2}, {3}, {4}, drive={5}' -f
            ($i + 1), $candidate.Disk.Number, $candidate.Partition.PartitionNumber,
            (Format-Capacity $candidate.Partition.Size), $candidate.Volume.FileSystem, $letter)
    }
    Write-Host '[C] Cancel'
    $answer = (Read-Host 'Selection').Trim()
    if ($answer -match '^[Cc]$') { exit 0 }
    $index = 0
    if ((-not [int]::TryParse($answer, [ref]$index)) -or
        ($index -lt 1) -or ($index -gt $candidates.Count)) {
        Stop-Installer 'Invalid FAT-partition selection.'
    }
    $selected = $candidates[$index - 1]
    $snapshot = New-DiskSnapshot $selected.Disk
    $null = Assert-SameSafeUsbDisk $snapshot $false
    $partition = Get-Partition -DiskNumber $selected.Disk.Number -PartitionNumber $selected.Partition.PartitionNumber -ErrorAction Stop
    if (($partition.Offset -ne $selected.Partition.Offset) -or ($partition.Size -ne $selected.Partition.Size) -or
        $partition.IsBoot -or $partition.IsSystem -or $partition.IsReadOnly -or $partition.IsOffline) {
        Stop-Installer 'The selected partition identity or safety state changed.'
    }
    $volumes = @(Get-PartitionVolumes $partition)
    if (($volumes.Count -ne 1) -or
        (([string]$volumes[0].FileSystem -cne 'FAT') -and ([string]$volumes[0].FileSystem -cne 'FAT32'))) {
        Stop-Installer 'The selected partition is no longer an eligible FAT volume.'
    }
    $temporaryAccessPath = $null
    $temporaryAccessPathAssigned = $false
    try {
        if (-not (Test-HasDriveLetter $partition)) {
            $temporaryAccessPath = Get-FreeDriveAccessPath
            Add-PartitionAccessPath -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -AccessPath $temporaryAccessPath -ErrorAction Stop
            $temporaryAccessPathAssigned = $true
            $partition = Get-Partition -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
            if ("$($partition.DriveLetter):\" -cne $temporaryAccessPath) {
                Stop-Installer 'Temporary drive-letter assignment failed.'
            }
        }
        $root = "$($partition.DriveLetter):\"
        Install-Efi $root $SourcePath $SourceHash
    } finally {
        if ($temporaryAccessPathAssigned) {
            $currentPartition = Get-Partition -DiskNumber $selected.Disk.Number -PartitionNumber $selected.Partition.PartitionNumber -ErrorAction Stop
            if (($currentPartition.Offset -ne $selected.Partition.Offset) -or
                ($currentPartition.Size -ne $selected.Partition.Size)) {
                Stop-Installer 'Temporary-access-path partition identity changed during cleanup.'
            }
            Remove-PartitionAccessPath -DiskNumber $currentPartition.DiskNumber -PartitionNumber $currentPartition.PartitionNumber -AccessPath $temporaryAccessPath -ErrorAction Stop
        }
    }
}

function Install-ToErasedUsb([string]$SourcePath, [string]$SourceHash) {
    $disk = Select-UsbDisk 'Eligible USB disks for destructive preparation:'
    if ($null -eq $disk) { exit 0 }
    $snapshot = New-DiskSnapshot $disk
    if ([string]::IsNullOrWhiteSpace($snapshot.UniqueId)) {
        Stop-Installer 'The selected disk has no stable UniqueId; destructive mode is refused.'
    }
    Write-Host ''
    Show-DiskSummary $disk
    foreach ($partition in (Get-DiskPartitions $disk.Number)) {
        $volumes = @(Get-PartitionVolumes $partition)
        $fileSystem = if ($volumes.Count -eq 1) { [string]$volumes[0].FileSystem } else { '<unknown>' }
        $mountPaths = @(Get-PartitionMountPaths $partition)
        $mountedAt = if ($mountPaths.Count -ne 0) { $mountPaths -join ', ' } else { '<none>' }
        Write-Host ('  Partition {0}: {1}; filesystem={2}; mounted at={3}' -f
            $partition.PartitionNumber, (Format-Capacity $partition.Size), $fileSystem, $mountedAt)
    }
    if ([UInt32]$disk.LogicalSectorSize -ne 512) {
        Stop-Installer 'A 256 MiB FAT32 USB requires 512-byte logical sectors.'
    }
    if ([UInt64]$disk.Size -lt 257MB) { Stop-Installer 'Selected USB disk needs at least 257 MiB for this layout.' }
    Write-Warning 'ALL DATA ON THIS USB DISK WILL BE DESTROYED.' 
    Write-Host 'The disk will use MBR with one active 256 MiB FAT32 primary partition labeled NCVSELECTOR.'
    Write-Host 'All remaining capacity will stay unallocated.'
    $expectedConfirmation = "ERASE $($disk.Number) $($disk.UniqueId)"
    $confirmation = Read-Host "Type $expectedConfirmation to erase this USB and install"
    if ($confirmation -cne $expectedConfirmation) { Write-Host 'Cancelled; the USB was not changed.'; exit 0 }
    $disk = Assert-SameSafeUsbDisk $snapshot $true
    if ([UInt32]$disk.LogicalSectorSize -ne 512) { Stop-Installer 'Logical sector size changed.' }
    $partition = $null
    try {
        # Clear-Disk rejects RAW media; an uninitialized disk is ready for Initialize-Disk.
        if ($disk.PartitionStyle.ToString() -ne 'RAW') {
            Clear-Disk -Number $disk.Number -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
        }
        Initialize-Disk -Number $disk.Number -PartitionStyle MBR -ErrorAction Stop | Out-Null
        $partition = New-Partition -DiskNumber $disk.Number -Offset 1MB -Size 256MB -AssignDriveLetter -IsActive -ErrorAction Stop
        Format-Volume -Partition $partition -FileSystem FAT32 -NewFileSystemLabel 'NCVSELECTOR' -Confirm:$false -ErrorAction Stop | Out-Null
        $partition = Get-Partition -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
        if (-not (Test-HasDriveLetter $partition)) { Stop-Installer 'Prepared FAT32 partition has no temporary drive letter.' }
        Install-Efi "$($partition.DriveLetter):\" $SourcePath $SourceHash
    } finally {
        if (($null -ne $partition) -and (Test-HasDriveLetter $partition)) {
            Remove-PartitionAccessPath -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -AccessPath "$([char]$partition.DriveLetter)`:\" -ErrorAction Stop
        }
    }
}

function Show-NextSteps {
    Write-Host ''
    Write-Host 'Next steps:'
    Write-Host '1. Reboot and select the USB UEFI boot option.'
    Write-Host '2. Keep the monitor on the firmware display while selecting the secondary GPU and boot disk.'
    Write-Host '3. Press Enter to review, then Enter to save Config.ini and continue into boot.'
    Write-Host '4. Switch to the selected GPU input during the final five-second switch countdown (after the earlier five-second setup window); F2 reopens setup.'
    Write-Host 'No second launch is needed after a successful save. Failed checks stop boot.'
    Write-Host 'Guides in the extracted package: Docs/INSTALLATION.md and Docs/TROUBLESHOOTING.md'
    Write-Warning 'Secure Boot may reject this unsigned EFI application. This installer never disables Secure Boot.'
}

try {
    if (-not (Test-Administrator)) { Stop-Installer 'Run this installer with administrator elevation.' }
    if (-not [Environment]::Is64BitOperatingSystem) { Stop-Installer 'Only x86-64 Windows is supported.' }
    $selfDirectory = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
    $packageDirectory = [IO.Path]::GetFullPath($InstallerDirectory)
    if ([string]::Compare($selfDirectory.TrimEnd('\'), $packageDirectory.TrimEnd('\'), $true) -ne 0) {
        Stop-Installer 'InstallerDirectory must be the directory containing Installer.ps1.'
    }
    Assert-NoReparseComponents $PSCommandPath
    Assert-NoReparseComponents $packageDirectory
    $packageRoot = [IO.Path]::GetPathRoot($packageDirectory)
    if ($packageRoot -notmatch '^[A-Za-z]:\\$') {
        Stop-Installer 'The installer package must be on a locally mounted drive.'
    }
    if ($env:SystemDrive -notmatch '^[A-Za-z]:$') { Stop-Installer 'The running Windows volume is not identifiable.' }
    $systemDriveLetter = [char]$env:SystemDrive[0]
    $script:OperatingSystemDisk = (Get-SinglePartitionForDrive $systemDriveLetter).DiskNumber
    $script:InstallerSourceDisk = (Get-SinglePartitionForDrive $packageRoot[0]).DiskNumber

    Write-Host 'Native CSM VGA Selector 1.2 USB Installer'
    Write-Host 'Only USB physical disks are supported. Internal disks and system ESPs are excluded.'
    $secureBootCommand = Get-Command Confirm-SecureBootUEFI -ErrorAction SilentlyContinue
    if ($null -ne $secureBootCommand) {
        try {
            if (Confirm-SecureBootUEFI) {
                Write-Warning 'Secure Boot is enabled and may reject the unsigned EFI application.'
            }
        } catch {
            Write-Warning 'Secure Boot state could not be queried; no firmware setting was changed.'
        }
    }

    Write-Host ''
    Write-Host '[1] Install Release Edition (default)'
    Write-Host '[2] Install Debug Edition'
    Write-Host '[3] Exit'
    $edition = (Read-Host 'Selection [1]').Trim()
    switch -CaseSensitive ($edition) {
        ''  { $sourceName = 'NativeCsmVgaSelector-1.2.efi' }
        '1' { $sourceName = 'NativeCsmVgaSelector-1.2.efi' }
        '2' { $sourceName = 'NativeCsmVgaSelector-1.2-Debug.efi' }
        '3' { exit 0 }
        default { Stop-Installer 'Invalid edition selection.' }
    }
    $sourcePath = Join-Path $packageDirectory $sourceName
    $sourceHash = Get-SourceHash $sourcePath $sourceName $packageDirectory
    $script:DatabasePath = Join-Path $packageDirectory 'pci.ids'
    $script:DatabaseHash = Get-SourceHash $script:DatabasePath 'pci.ids' $packageDirectory
    if (-not (Test-X64EfiApplication $sourcePath)) { Stop-Installer 'Selected source is not an x86-64 EFI application.' }

    Write-Host ''
    Write-Host '[1] Install onto an existing FAT/FAT32 partition on a USB disk'
    Write-Host '[2] Erase and prepare a dedicated USB disk'
    Write-Host '[3] Exit'
    $mode = (Read-Host 'Selection').Trim()
    switch -CaseSensitive ($mode) {
        '1' { Install-ToExistingFat $sourcePath $sourceHash }
        '2' { Install-ToErasedUsb $sourcePath $sourceHash }
        '3' { exit 0 }
        default { Stop-Installer 'Invalid installation-mode selection.' }
    }
    Show-NextSteps
    exit 0
} catch {
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 1
} finally {
    if ($WaitOnExit) { $null = Read-Host 'Press Enter to close the installer' }
}

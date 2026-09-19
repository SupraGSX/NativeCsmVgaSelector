# Reproducing installer checks

The automated tests below use temporary files or mocked device identities.
They do not enumerate or write physical disks and require no administrator
privileges. Run from a clean source checkout:

```sh
python3 Tests/LinuxIdentityTests.py
python3 Tests/PackagingTests.py
pwsh -NoLogo -NoProfile -NonInteractive -File Tests/WindowsLoaderTests.ps1
```

On Windows PowerShell, use:

```powershell
powershell.exe -NoLogo -NoProfile -File Tests\WindowsLoaderTests.ps1
```

The PowerShell test parses the actual installer and loads only its file-copy,
move and install functions. It substitutes faulting copy/move operations and
confirmation input, then verifies complete old/new content and retained backup
files for successful replacement, partial staging, bad staging hash, failed
commit, failed final hash and failed restoration. It never invokes Storage
cmdlets or the installer entry point.

The Linux test runs the actual revalidation function with synthetic identities,
including identical model/capacity and reused device numbers, changed serial/WWN,
changed sysfs path, changed or missing connection sequence, and missing serial.
It also verifies that erase mode stops before destructive work without a
connection sequence. This is not a physical hot-plug timing test.

## Disposable platform validation

Use an isolated Linux or Windows VM with its own virtual system disk, a second
temporary virtual disk attached through an emulated USB storage controller, and
no physical disks, host block-device backing or PCI passthrough. Keep the package
on the guest's system disk. Snapshot the VM before each destructive case.

1. Verify the installer lists only the emulated USB. Confirm internal/system
   disks are excluded. Cancel each menu and destructive confirmation and compare
   the USB image with its snapshot.
2. Exercise existing-FAT mode with a synthetic old `BOOTX64.EFI` and retained
   Config.ini. Verify the old backup, final EFI/pci.ids hashes and unchanged INI.
3. Exercise dedicated-USB mode with the exact displayed erase phrase. Inspect
   the resulting MBR, active 256 MiB FAT32 partition at sector 2048, label, files
   and remaining unallocated capacity.
4. Disconnect/reconnect the virtual USB after selection. On Linux, also attach
   another same-model/same-capacity unit with a reused device number. Require a
   revalidation failure before the next destructive phase.
5. Fill the FAT volume before staging a replacement and inject guest-side file
   failures. Require the active old loader to remain complete or be restored;
   on failed restoration, require the named backup/recovery files to survive.
6. Test interrupted FAT commits separately using snapshots. Recovery must be
   possible from the verified backup; do not interpret a successful rename as
   proof of arbitrary power-loss atomicity.

Record guest OS/PowerShell/kernel and utility versions, exact candidate hash,
virtual disk identity, pre/post image hashes, and logs. Shut down the guest
before inspecting its image offline. These platform scenarios remain pending
for this candidate; the temporary-file tests do not claim to execute them.

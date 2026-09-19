# Native CSM VGA Selector 1.2.2-audit-fixes.1

**Prerelease: physical native-CSM boot testing is still pending.**

This release candidate includes the code-audit fixes and selected-disk warning
policy described in [audit fixes](Docs/AUDIT_FIXES.md). A uniquely selected disk
that passes structural preflight may be attempted despite firmware policy
warnings; those warnings remain in the log and final error screen.

Download **NativeCsmVgaSelector-1.2.2-audit-fixes.1.zip** for the compiled
application and USB installers, or the matching **-source.zip** for the complete
source. Executable and installer payload names retain the 1.2 family name.
See [validation](Docs/VALIDATION.md) for completed tests and platform limits.

An x86-64 UEFI application selects a configured legacy VGA adapter and boot disk
through the firmware's native CSM. It does not supply a CSM or a GPU option ROM.
A compatible legacy VGA ROM, usable native-CSM protocols, and a legacy boot
installation are required. Secure Boot must allow this unsigned application.

## Install

**Start with the [installation and first-boot guide](Docs/INSTALLATION.md).**
It covers Windows, Linux, manual FAT32 USB preparation, target selection,
updates, recovery, and removal. For errors, use the
[troubleshooting guide](Docs/TROUBLESHOOTING.md).

Extract the runtime release ZIP (or a locally built `NativeCsmVgaSelector-1.2.zip`) to a local OS-drive
folder. You do not need to build it if you already have that package. If you
only have source, follow [build instructions](Docs/BUILD.md) first.
The included Linux and Windows USB installers verify and install the EFI and
PCI-name database. They preserve existing configuration and retain their
USB-only disk checks. They do not set up menu markers or firmware boot entries.
Do not run the installers from the unpackaged source tree.

For manual installation, the runtime files are:

```
EFI/BOOT/
  BOOTX64.EFI
  pci.ids
  Config.ini           (created during setup)
  Config.ini.previous  (created after a successful save)
```

`BOOTX64.EFI` is the standard x64 fallback name; a firmware entry may also point
to it. Fallback discovery on fixed MBR disks depends on firmware. GPU and disk
targets are configuration data, not compiled into the application. Boot the
USB's **UEFI** entry; the application later hands off to your legacy OS disk.
Version 1.2 requires a different secondary GPU, one active legacy-VGA owner,
and a supported PCI arrangement. Read the guide's requirements before preparing
the USB; AMI branding alone does not guarantee compatibility.

## First start and target editing

Without configuration or recovery files, the application probes hardware and
opens target selection when eligible GPU and disk candidates exist. Up/Down or
Tab selects a field, Left/Right changes the candidate, Enter opens review, and
Enter again saves and boots. Esc cancels. A successful save closes the setup
files, reloads Config.ini, and continues through the setup window, preflight,
monitor-switch countdown and boot without returning to firmware. If no eligible targets exist,
diagnostic output goes to `Probe.ini` instead of creating a boot configuration.

Configured automatic boots first show a **five-second F2 setup window** before
preflight. Press F2 to edit GPU/disk targets or Esc to cancel. After successful
preflight and its scrolling output, a separate **five-second monitor-switch
countdown** names the selected GPU. Switch inputs during this final countdown.
The two automatic waits total ten seconds; normal validation time is additional.
Routine console scrolling stops during the final countdown, while file/serial
logging and errors remain active. F2 and Esc remain available there too.
A successful edit restarts the sequence with fresh preflight. Failed saves,
reloads or cleanup do not proceed. Clean preflight errors also offer F2.
The review shows retained GPU identity restrictions and menu-marker routing.
Missing targets remain visibly flagged. Conflicts require A to acknowledge
before saving; incompatible Expected* restrictions still require manual INI
editing before boot. Target editing changes only `TargetPci`,
`TargetControllerPci`, and `TargetBbsDescription`; other settings stay intact.

The optional adjacent `pci.ids` provides friendly names. Missing, unreadable,
oversized, or unmatched data falls back to numeric identity. It never selects
hardware and the application does not access the network.

## Configuration and recovery

Only `Config.ini` is active. `[Behavior]` must explicitly contain `Probe=true`
for diagnostics or `Probe=false` for boot. Boot configurations must specify a
GPU, disk controller, and all three endpoint policies. `AutoBoot=false` runs
boot validation without GPU handoff or marker writes. Advanced settings remain
editable in the INI; expected PCI IDs use four hex digits without `0x`.
The obsolete reference-snapshot options are not supported.

Firmware disk names with leading or trailing spaces are saved as quoted
`TargetBbsDescription` values, for example `TargetBbsDescription="USB Disk 0 "`.
Spaces inside those quotes are part of the disk identity; preserve them when
editing. Inside quoted names, `\"` represents a quote and `\\` a backslash.
Ordinary unquoted names retain their existing syntax. Detection in the setup
menu does not prove that firmware can boot a disk; see the firmware requirements
in [troubleshooting](Docs/TROUBLESHOOTING.md#usb-as-the-legacy-boot-target).

USB disks can be selected as legacy boot targets when firmware exposes them as
bootable USB hard disks in its BBS list. The boot path accepts PCI USB controllers
and defaults to the firmware's `USB` boot option for those targets, accepting
both `BBS(HD)` and `BBS(USB)` representations of that option; internal
mass-storage targets retain `Hard Drive`. Exact controller and disk-name matching
is required for USB because several disks can share one controller. This does
not add USB support to firmware or make an EFI-only USB disk legacy-bootable.

Saves validate, write, flush, and read back temporary data. The previous valid
settings are retained as `Config.ini.previous`. If the active file is missing
or unusable, R explicitly recovers a validated previous copy, or a validated
`Config.ini.tmp` if no previous copy is usable. Esc returns without changes.
Neither incomplete boot files nor comment-only remnants suppress recovery.

When no usable recovery copy exists, the error screen stays visible. F2 then Y
offers fresh setup: existing configuration and recovery files are copied and
verified as `*.invalid-NN` before removal. Fresh setup resets advanced policies
and disables menu markers. Cancellation before Y changes nothing. Unreadable
or oversized files, failed preservation, or exhausted archive names prevent
reset; inspect the files externally in that case.

These verified copies support recovery from interrupted saves; they do not
make FAT updates power-loss atomic or replace an external backup.

## Optional menu routing

An optional `[Marker]` section in `Config.ini` selects a pre-existing menu-marker
file by exact disk and partition identity. New configurations disable it.
See [marker settings](Docs/MARKER.md). The marker destination stays fixed when
the selected boot disk changes. No separate launcher or secondary INI is used.

## Build, test, and package

```sh
bash fetch-toolchain.sh
bash build.sh Release
bash build.sh Debug
make -C Tests test
make -C Tests firmware
python3 Scripts/package.py
```

See [build instructions](Docs/BUILD.md) for dependencies and artifact paths,
[validation](Docs/VALIDATION.md) for coverage and limitations,
[compatibility audit](Docs/COMPATIBILITY.md) for firmware edge cases, and
[architecture](Docs/ARCHITECTURE.md) for module responsibilities. Release and
Debug share one source tree and currently use the same optimized compiler
profile; Debug is a packaging label, not a distinct diagnostic implementation.
`VerboseLog` controls logging in either build.

## License

GNU General Public License version 3 only (**GPL-3.0-only**, copyleft).
See [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md).
Existing LGPL and upstream data/toolchain notices are preserved.

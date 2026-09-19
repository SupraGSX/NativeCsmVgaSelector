# Troubleshooting

## Legacy disks and option-ROM conflicts

Disk discovery and boot eligibility are separate checks. A detected disk that
fails eligibility reports `NCV_BLOCKED_BBS_ELIGIBILITY`, with a reason in the
log. Zero BBS status can be supported through corroborated resident-ROM or
firmware-AHCI evidence; it is not automatically a media failure.

`NCV_BLOCKED_ROM_OVERLAP` identifies a recognized conflict before the GPU
handoff. Review the conflicting device's storage or network OpROM policy;
UEFI-only or disabling unused network boot may help while retaining CSM.
Reselect the disk if its name changes. `NCV_BLOCKED_ROM_LAYOUT` means the
current memory layout could not be established. Neither error authorizes
bypassing the guard. See [compatibility](COMPATIBILITY.md) and
[overlap diagnostics](ROM_OVERLAP.md).

Start with the [installation guide](INSTALLATION.md). Keep the exact error
text and the last visible line: a generic EFI status alone rarely identifies
the cause. There is no universal INI setting that makes unsupported firmware,
a missing legacy GPU ROM, or an unlisted legacy disk work.

## Linux installer window closes immediately

Use the current runtime package and keep `launch.sh`, `install.sh`, the EFI
files, `pci.ids`, and `SHA256SUMS` together. **Run as a Program** opens an
interactive terminal and waits for Enter after completion or an error. You
can also run `bash ./install.sh` from an existing terminal to read the output.
The source `Installers/Linux` folder has no payload of its own: build and run
`python3 Scripts/package.py` first, or use the extracted runtime ZIP.

If there is no supported terminal emulator or graphical session, open your
own terminal. If the file manager displays the script as text, enable its
execute permission or use the terminal command above. No USB is selected or
formatted merely by launching the installer.

The `.desktop` launcher must stay beside `launch.sh` and `install.sh`.
Older packages worked through desktop-shortcut APIs but failed when a file
manager's **Run as a Program** action executed the file itself. Messages such
as `[Desktop: not found` or `-c: not found` identify that failure. The current
file includes an executable header and routes both launch methods through
`launch.sh`. Re-extract the complete updated package. File managers may still
require **Allow Launching** or a trust setting for ordinary shortcut opening.
If `env` reports an unsupported `-S` option, run `launch.sh` directly; the
executable desktop header needs GNU coreutils 8.30 or newer.

## USB installation and firmware launch

| Message or symptom | Meaning and next step |
| --- | --- |
| `Installer.ps1 was not found beside this launcher` or `Package file is missing` | Extract the entire runtime ZIP to a local OS-drive folder. Keep the launcher, script, EFI build copies, `pci.ids`, and `SHA256SUMS` together. Do not run a source-tree installer directly. |
| `SHA-256 verification failed`, `SHA256SUMS must contain exactly one entry`, or a database hash mismatch | Files are damaged, mixed between packages, or inconsistent with their manifest. Stop and re-extract a complete trusted package. Do not fix this by editing the expected hash. A valid manifest establishes consistency, not publisher identity. |
| `Logical sector size is unavailable` | Re-extract the current package. An earlier Linux script rejected whitespace-padded `lsblk` output such as `    512`; the current script trims it and checks geometry before unmounting. If the error persists, the device query failed or returned invalid data. |
| `A 256 MiB FAT32 USB requires 512-byte logical sectors` or insufficient capacity | Dedicated preparation needs 512-byte logical sectors and at least 257 MiB capacity. Other sector sizes are rejected before erasure because this fixed layout would not produce a suitable FAT32 volume. Existing-partition mode remains available for a suitable FAT/FAT32 USB. |
| `Required command is unavailable` | On Linux, install the named command's distribution package, then retry. See the dependency list in the installation guide. |
| `Run this installer with administrator elevation` / sudo required | Use the supplied Windows launcher and approve its elevation, or run the Linux script from a terminal where sudo is available. |
| `No eligible USB physical disk was found` | The installer excludes internal/system disks, the disk containing its own files, read-only disks, and other unsafe or unidentified targets. Move the extracted package to your normal OS drive, reconnect the intended writable USB, and check its model/capacity. Do not remove the exclusion checks to force an internal disk through. |
| `No eligible FAT/FAT32 partition on a USB disk was found` | Existing-partition mode cannot use exFAT/NTFS or a disk rejected by the checks above. Use a suitable FAT32 USB, or dedicated preparation on a backed-up spare USB. |
| `Selected physical disk identity changed`, changed parent disk, or multiple mounts | Device identity or mount state changed during selection. Cancel, finish other disk operations, reconnect the intended USB, and select it again. |
| `Existing loader found` | Another `BOOTX64.EFI` already occupies the fallback path. Use another USB if you want to keep that USB's normal boot behavior. Replacement requires typing B and verifying its backup. |
| USB drive letter disappears after successful Windows preparation | The installer removes its temporary drive letter. Check the disk in Disk Management or boot it through the firmware menu; this alone is not a failed installation. |
| USB is absent from the UEFI boot menu | Confirm a readable FAT32 partition and the exact path `EFI/BOOT/BOOTX64.EFI`. Avoid an extra enclosing package folder. Check USB/UEFI boot settings and try another firmware-supported USB port or stick. The legacy USB entry is not the selector's launch path. |
| Firmware `Security Violation` before the selector appears | Secure Boot may have rejected the unsigned EFI file. Check the machine owner's firmware policy. This is different from the target-identity check inside the selector; the installers do not change Secure Boot. |
| Immediate return to firmware with no selector screen | Check file integrity, x64 UEFI support and the boot path first. Firmware launch failures may occur before a log can be created. A traditional BIOS-only boot cannot execute this EFI file. |

## Setup, configuration, and display

| Message or symptom | Meaning and next step |
| --- | --- |
| `Suitable GPU and disk targets were not both found` / only `Probe.ini` appears | Discovery did not produce a confirmed boot configuration. Read `NativeCsmVgaProbe.log` to identify the absent capability or target. `Probe.ini` is diagnostic output and is not automatically used as `Config.ini`. |
| `Saved target unavailable` | A saved PCI address or disk description no longer matches discovery, perhaps after moving a card or changing firmware settings. Use F2 to review replacements. The program does not silently choose one. |
| Selected GPU conflicts with retained restrictions | F2 edits the target address but keeps optional `ExpectedVendor`, `ExpectedDevice`, and subsystem restrictions. Confirm the intended hardware before correcting those fields in a text editor. A acknowledges the warning; it does not override the restrictions or make an incompatible selection valid. IDs use four hexadecimal digits without `0x`. |
| Warning that the boot disk changed but the marker destination stays fixed | Optional menu routing still points at its explicitly configured partition. Check that this is intentional. F2 does not relocate a marker file or rewrite an existing boot menu. |
| `Config.ini: empty or invalid INI syntax` / explicit Probe setting required / incomplete boot settings | The file is damaged, contains unsupported/duplicate settings, or lacks required values. Use R if a validated recovery copy is offered. Otherwise use the guarded F2/Y fresh setup, or fix the file externally. Do not concatenate two complete INIs. |
| `No usable Config.ini.previous or Config.ini.tmp was found` | No acceptable recovery copy exists. F2 then Y preserves failed files as `*.invalid-NN` before fresh setup. If preservation fails, inspect the files from your normal OS before retrying. |
| `Saved Config.ini could not be reloaded for boot` | Saving was followed by a read/validation/close problem. Boot is blocked. Keep the INI/recovery files and check write protection, free space, and the USB filesystem from your normal OS. Restore a known-good copy if appropriate. |
| `executable-relative filesystem initialization failed`, cannot open a log, write/flush error, `Volume Full`, or `Write Protected` | The application cannot use the filesystem next to its executable. Use a writable FAT32 USB with space for configuration and logs. Copy important files elsewhere before filesystem repair or replacing the stick. |
| GPU name is numeric rather than friendly | Missing, unreadable, oversized, or unmatched `pci.ids` falls back to numeric identity. Put the supplied file beside `BOOTX64.EFI`; this affects names, not GPU compatibility. |
| Setup succeeds but the screen becomes black during handoff | Switch the monitor to the selected GPU during the final five-second countdown. Use a cable/input that works with that GPU. A late input change or display handshake issue can hide pre-OS output; black video alone does not establish that boot stopped. If the OS never appears, collect the boot log before changing targets or drivers. |
| Saving returns to firmware instead of showing the countdown | Confirm you installed the save-and-boot revision rather than an older copy. Successful interactive saves should continue. Cancellation, incomplete discovery, explicit diagnostics, or a failed check can still stop/return. Read the last message and logs. |
| `AutoBoot=false; validated runtime plan returned read-only...` | This is intentional validation-only mode. It performs no GPU handoff or marker write. Set `AutoBoot=true` in the existing `[Behavior]` section when ready to attempt a real boot. |

Keep the first setup screen on the firmware's original display. The prompt
names the destination GPU before switching. Keyboard target selection edits
GPU and disk targets, not every advanced INI field.

## Hardware and boot-log messages

These names are literal identifiers used by the program. Several checks share
a broad classification: read the nearby detailed lines as well. A setup
candidate or a `COMPATIBLE` probe classification is not proof that the full
hardware handoff will succeed.

| Message in the log | Meaning and next step |
| --- | --- |
| `NCV_BLOCKED_CONFIG` | Required boot configuration was not accepted. Recover or regenerate a complete configuration; use the specific missing-key messages above it. |
| `NCV_PROBE_BLOCKED_NO_NATIVE_CSM_INTERFACE` or `NCV_BLOCKED_CSM_CAPABILITY` | Required native CSM interfaces or Compatibility16 data are missing/unusable. Check whether the motherboard actually provides and enables compatible CSM. UEFI-only firmware cannot acquire CSM through an INI edit. |
| `NCV_PROBE_BLOCKED_NO_LEGACY_REGION_PROTOCOL` | Firmware does not expose the required legacy-memory interface. This is a firmware capability issue, not a USB file-layout problem. |
| `NCV_BLOCKED_ACTIVE_VGA` | Firmware did not report the required active/target arrangement. Check the logged active-owner count, target address and topology. Version 1.2 requires one active legacy-VGA owner and a different target. `Already Started` from this check means the target is already active. |
| `NCV_BLOCKED_TARGET_IDENTITY` | The detected target disagrees with an optional `Expected*` binding. Verify which card is intended before updating the retained restriction; do not blindly disable checks. |
| `NCV_PROBE_BLOCKED_NO_TARGET_VGA`, `NCV_BLOCKED_TARGET_VGA`, or `NCV_BLOCKED_TARGET_PATH` | The selected GPU is missing, not usable as the required legacy VGA endpoint, or its PCI bridge path is invalid. Review the address and the detailed preceding messages. |
| `NCV_PROBE_BLOCKED_BRIDGE_PATH_AMBIGUOUS` or `Target/active topology gate failed` | The PCI arrangement failed validation. The two GPUs need the same root bridge and parent host. Consult your motherboard's slot topology; moving a card may change both its compatibility and saved address. |
| `Configured target segment=... cannot be represented` | The selected GPU is outside supported PCI segment 0. There is no segment override in this version. |
| `NCV_PROBE_BLOCKED_TARGET_ROM_UNSUPPORTED` or `NCV_BLOCKED_TARGET_ROM` | Firmware did not expose an acceptable target legacy VGA ROM, or a probe identity check failed. Check preceding identity/ROM messages. EFI/GOP-only support is not enough. This guide does not require flashing the card. |
| `NCV_BLOCKED_BOOT_CONTROLLER`, `NCV_BLOCKED_BBS_TARGET`, or `NCV_PROBE_BLOCKED_BOOT_TARGET_AMBIGUOUS` | The selected controller/disk is missing, ambiguous, or not represented correctly in firmware's legacy disk list (BBS). Check legacy storage boot support and the selected disk description. The boot path accepts PCI mass-storage controllers and PCI USB controllers (class `0C:03`); USB requires an exact disk description. It does not add missing legacy NVMe or USB support to firmware. See the USB boot requirements below. |
| `NCV_BLOCKED_BBS_ELIGIBILITY` | The selected disk was found, but its boot eligibility was rejected. The preceding `Matching disk rejected:` line records the condition. An all-zero status word is not automatically a failed disk: supported resident-ROM and firmware-AHCI paths can corroborate it. Firmware status/priority and LegacyDevOrder restrictions on the uniquely selected disk are advisory in the audit-fix candidate; insufficient handler evidence, unsafe ranges and ambiguous identity still stop boot. This message does not diagnose filesystem corruption. |
| `NCV_BBS_FIRMWARE_AHCI_VERIFIED` | Informational: PCI identity, the system-BIOS handler prefix and strings support the selected disk's zero-status path. Other boot, uniqueness and overlap checks still apply. This is not proof the OS has booted. |
| `NCV_BLOCKED_LEGACY_OPTION` | A suitable existing firmware legacy boot option could not be identified. Check legacy boot availability and any configured option-description filters. The app does not create that option for you. |
| `NCV_PROBE_COMPATIBLE_WITH_FIRMWARE_QUIRK` | Discovery encountered a BBS-reporting problem despite finding basic capabilities. Preserve the log; this is not an assurance that Boot mode will pass. |
| `Menu marker handoff failed` | Optional marker validation/write failed. Verify its existing file, header, disk signature, partition start/size and profile. Keep `[Marker] Enabled=false` for ordinary USB use unless you intentionally configured a compatible menu. See [marker configuration](MARKER.md). |
| `NCV_BOOT_FAIL_PREFLIGHT` | The boot attempt stopped before its hardware handoff because an earlier check failed. The preceding `NCV_BLOCKED_*` or detailed message carries the cause. |
| `NCV_BOOT_FAIL_ACTIVE_GOP_DISCONNECT`, `...ROUTE_TRANSFER`, `...LEGACY_REGION_UNLOCK`, `...TARGET_LEGACY_VGA_ROM_READBACK`, or `...TARGET_LEGACY_VGA_INT10_OWNERSHIP` | A firmware operation, VGA-routing step, ROM copy, or legacy video check failed. These are beyond INI syntax. Record the exact last line and log. If the system has halted during handoff, cold-start the machine before another attempt rather than trying another target in the same partially changed firmware session. |
| `NCV_BOOT_FAIL_BBS_TRANSACTION` or `NCV_BOOT_FAIL_LEGACY_BOOT_OPTION` | Preparing firmware's final legacy boot target failed. Preserve the detailed log. Do not substitute random BBS indices or bypass validation. |
| `NCV_BOOT_FAIL_LEGACY_BOOT_RETURNED` | Firmware's legacy boot call returned instead of transferring control successfully. The program halts rather than trying another boot path. Collect the log after restarting and check the selected disk's legacy bootability. |

An OS bootloader error, Windows crash, or missing device driver after the OS
starts is a separate stage. The selector does not repair OS filesystems or
install drivers. Record whether the failure occurs before the countdown,
during GPU handoff, in the disk bootloader, or inside the OS.

## Collect useful diagnostics

After returning to a working OS, open the selector USB's `EFI/BOOT` folder and
copy these files if present:

- `NativeCsmVgaProbe.log`: discovery, target eligibility and setup output.
- `NativeCsmVgaBoot.log`: boot validation and handoff progress. With
  `VerboseLog=false`, detail may be reduced.
- `Config.ini` and any relevant `Probe.ini`, `.previous`, `.tmp`, or
  `*.invalid-NN` files.
- The exact EFI version/hash, motherboard model and firmware version, both
  GPU models and slots, target disk/controller, and the exact error text.

Logs are appended and can include earlier attempts. Use the final attempt's
messages when reporting a failure. Back up logs before removing old copies to
capture a shorter run. A log can end abruptly when firmware hangs, so a missing
final message does not prove success.

For a discovery-only run, back up `Config.ini` and change its existing
`[Behavior] Probe=false` line to `Probe=true`. It can write diagnostic files,
but does not perform GPU handoff. Restore `Probe=false` afterward. For boot-plan
validation without handoff, keep `Probe=false` and set `AutoBoot=false` under
`[Behavior]`. Use `VerboseLog=true` under `[Behavior]` for detailed logging. Change
existing keys rather than adding duplicates, and retain a known-good copy.

Before sharing logs or configuration publicly, inspect them for private disk
descriptions, identifiers and paths. Share the relevant messages and hardware
details, not a full disk image or firmware dump.

## Disk appears in the probe log but is missing from target setup

Earlier builds rejected firmware disk descriptions ending or starting with a
space, including some USB storage names. Updated source preserves those names
in quoted `TargetBbsDescription` values through candidate generation, target
editing, saving, and reloading. The original disk description is still used
for matching; it is not trimmed into another disk's identity. A source fix does
not change an already downloaded release ZIP or installed EFI executable.

## USB as the legacy boot target

Current source supports the USB-HDD boot path as well as internal disks. USB
controller validation accepts PCI class `0C:03`, and the live controller type
determines the default generic Boot#### option: `USB`, excluding `Hard Drive`,
for USB; `Hard Drive`, excluding `USB`, for internal mass-storage controllers.
Changing the disk in setup changes those defaults on the next boot attempt.

The firmware must expose an active generic legacy USB boot option, encoded as
either `BBS(HD)` (type 2) or `BBS(USB)` (type 5), and a bootable, enabled
USB-class disk entry of type `BBS_HARDDISK`. These two firmware records need not
use the same device type. The original boot-option path and opaque arguments
are preserved; a type-5 USB group cannot be paired with an internal disk.
The selected USB must
already contain a legacy bootloader. An EFI-only disk, USB optical/floppy
emulation, or firmware without legacy USB support is not covered by this path.
The application itself still starts through its **UEFI** entry.

An earlier local candidate accepted only the type-2 representation of the
generic USB option. On firmware using type 5, this produced
`NCV_BLOCKED_LEGACY_OPTION` before GPU handoff despite the USB disk being
discovered correctly. The updated candidate accepts both representations.

Selection binds the controller's PCI address and the exact BBS disk description,
including spaces. Multiple indistinguishable matches, a missing disk, a failed
or disabled BBS entry, or a missing/ambiguous generic firmware option blocks boot.
There is no automatic fallback to an internal disk. Keep the USB connected to
the selected controller; rerun setup if its firmware identity changes.

If firmware uses a different generic legacy option name, inspect the probe log
and set `[Boot] LegacyOptionDescription` to that exact Boot#### description.
`ExcludeDescription` can also be overridden; it must be a nonempty string not
contained in the intended option name. For example, firmware that groups USB
and internal disks under `Hard Drive` needs that explicit option override and
`ExcludeDescription=USB`. Disk matching still uses the selected USB controller
and its exact description. Do not substitute a UEFI USB boot option.

OVMF tests exercise the real controller validator, BBS target selection, owned
handoff arguments and reversible priority transaction with synthetic firmware.
They cannot establish physical USB/GPU handoff compatibility; test the new EFI
on the target motherboard before relying on it.

## Boot stopped, log failure, or a blank screen after countdown

Returnable boot failures now leave a **BOOT STOPPED** screen with the last
reported stage, EFI status (including its hexadecimal value), and the first
specific failure code when available. Photograph this screen. Enter returns
to firmware; other keys do not dismiss it. If firmware keyboard input is
unavailable or fails, the screen remains and a manual restart is required.
No graphics controller is reconnected to force an error screen to appear.

Log initialization, write, flush and close failures are reported independently
of the failing file logger. A **BOOT LOG ERROR** means the file may be incomplete;
it does not authorize continuing past a failed safety check. The original boot
failure takes precedence over subsequent cleanup errors. Normal transaction
records still require durable file logging before console mirroring.

After irreversible hardware handoff has started, existing fatal decisions
remain fatal: a named error and summary are attempted, then execution stays
halted. There is no new retry, rollback policy or return to firmware. Console
output is best effort once the primary display has been disconnected.

The initial five-second F2 window is before preflight; the final five-second
monitor-switch countdown follows successful preflight. Its completion alone still does
not establish that any GPU switching was attempted: final log and marker
operations must succeed. F2 edits or Esc cancellation finish cleanup first.
Preflight errors offer F2 only when cleanup succeeded. A firmware call that never returns
cannot produce a subsequent error message; inspect the last durable
`...BEGIN` / `...COMPLETE` records in `NativeCsmVgaBoot.log`. In particular,
`NCV_BOOT_ACTIVE_GOP_DISCONNECT_BEGIN` is recorded after the optional menu
marker succeeds and before the disconnect call. The following file barrier
must also succeed before the call is made. A missing completion is evidence
of the last reported boundary, not proof of the exact instruction that hung.

## Windows installer closes or cannot list a blank USB

Keep the CMD launcher and Installer.ps1 from the same current runtime ZIP.
The CMD launcher now leaves the elevated installer's completion/error message
visible until Enter. Use Command Prompt to read failures that happen before
PowerShell starts. Older installer copies could reject blank disks, fail on a
single-partition list, or misread an unassigned drive letter; replace the whole
package rather than suppressing disk-validation errors.

## Compatibility boundaries

See [the compatibility audit](COMPATIBILITY.md) for handled CSM-table and
memory-map variants, VGA decode modes, USB/BBS requirements, retained topology
limits, and failures that cannot return a visible error. A checksummed table's
optional zero fields are not sufficient reason to reject native CSM support.
Ambiguous GPU ownership or disk identity still blocks a handoff.

## Another device's ROM blocks a disk boot

`NCV_BLOCKED_ROM_OVERLAP` protects all recognized resident option ROMs, including
network/PXE ROMs. Check the logged conflict PCI identity and address; do not
assume it belongs to the selected disk. If the identity is a network controller,
review its legacy PXE/boot-ROM policy (UEFI-only or disabled if unused), keeping
CSM enabled for legacy OS boot. Retest and collect the new log: removing the
first conflict does not establish that later checks will pass. The application
does not modify firmware OpROM policies automatically.

## Initial setup-window errors

`NCV_BOOT_FAIL_SETUP_INPUT` means the firmware keyboard read failed;
`NCV_BOOT_FAIL_SETUP_TIMER` means its wait service failed. The application stops
before runtime preflight or GPU switching. Photograph the error because the boot
log has not been opened yet. An absent keyboard still permits the automatic
five-second wait; an explicit protocol error is not treated as a timeout.

Firmware boot warnings are retained in the final error summary. “Native boot was
attempted” means the single native handoff was reached; “native disk boot was
not reached” means an earlier check or operation stopped the attempt. These
messages retain the user's chosen disk and do not diagnose filesystem damage.

# Legacy storage compatibility

Release 1.2.2 includes corroborated disk handling and an early option-ROM
overlap guard. The firmware-AHCI path has passed a physical boot test; see
[validation](VALIDATION.md) for the scope. It does **not** make a GPU ROM and
an overlapping resident storage ROM coexist. An overlapping layout still stops before the GPU disconnect or copy.

## Changes

- ROM layout and provenance are refreshed after the single GetBbsInfo query.
  A firmware that initializes storage ROMs lazily cannot reuse a stale
  before-query snapshot to authorize the GPU copy.

- A uniquely selected legacy disk may use an all-zero BBS status word when
  its live PCI controller, unchanged resident ROM, and valid linked `$PnP`
  header independently corroborate its vendor/device, class, disk-handler
  entry point and description/manufacturer references.
- A separate path supports a uniquely selected AHCI disk whose handler and
  strings reside in the system BIOS area (`E0000`–`FFFFF`). The PCI controller
  must unambiguously match at segment zero with AHCI class `01:06:01`.
  Readable firmware snapshots must contain an unchanged, non-padding 64-byte
  handler prefix and bounded, printable, terminated strings. An absent
  manufacturer pointer is allowed. These BIOS entry points do not need a
  PCI ROM header or a 512-byte-aligned segment. This is bounded corroboration,
  not verification of the full executable code extent or OS bootability.
- Firmware evidence is refreshed after GetBbsInfo, alongside the ROM guard,
  and rechecked when eligibility is used. Missing evidence disables only the
  zero-status exception. Normal-status disks keep their existing path.
- A found disk that fails eligibility now reports
  `NCV_BLOCKED_BBS_ELIGIBILITY` and logs the rejected condition, distinct from
  a missing disk. Duplicate matching disk identities remain ambiguous even
  when one entry is otherwise ineligible.
- A PnP manufacturer offset of zero is treated as absent only with that
  corroboration. Arbitrary `segment:0000` pointers retain their old handling.
- The actual PCI controller and PnP header must agree on class/subclass.
  PCIR vendor/device identifiers must match, but a stale PCIR class does not
  veto that independent agreement. Captured firmware demonstrates this case.
- The exception applies only to the selected disk. Other zero-status entries
  retain their priorities. For the uniquely user-selected disk, firmware
  disabled/failed/no-media/reserved flags, priority sentinels and a validated
  LegacyDevOrder disable bit are advisory. The original values are logged;
  the selected disk is attempted if structural preflight passes. Unselected
  discouraged entries retain their priorities. Ambiguous PCI segments, duplicate matching PnP headers,
  malformed chains, unsafe ranges and changed ROM contents are rejected.
- Resident storage-ROM contents are checked again before priority journaling
  and when validating the boot arguments. A change reports
  `NCV_BOOT_FAIL_STORAGE_ROM_CHANGED`; it is not hidden by string normalization.
- `NCV_POST_DISPATCH_VGA_FOOTPRINT` records the initialized GPU ROM's declared
  runtime size. This distinguishes initialization size from runtime size for
  further investigation. It is not evidence that initialization bytes may be
  skipped, or that an initialized ROM can be relocated.

No storage driver is installed, no firmware boot variable is rewritten, and
no BBS field other than the existing journaled BootPriority is written. The
configuration still selects the actual disk and firmware boot group. If an
option-ROM policy change renames the disk, reselect it in setup; this build
does not silently weaken the configured disk-name match.

## Why the overlap remains a stop

The paired private captures establish a resident storage ROM whose interrupt
handler branches into bytes covered by the selected GPU's initialization
image. The problem is executable code ownership, not just a malformed label
or an overstrict status check.

TianoCore's CSM implementation initializes PCI 2.x ROMs in their final shadow
area. Its separate initialization/runtime placement path requires a PCI 3.x
ROM and compatible CSM support. The captured GPU ROM is older. Calling
InstallPciRom again does not provide relocation: the reference implementation
returns the existing shadow information for a ROM it has already installed.

Saving the overlap and restoring it after dispatch would leave disk code
unavailable during initialization, and might then overwrite the GPU's live
runtime code. Copying the initialized disk ROM elsewhere leaves existing far
pointers, interrupt chains and firmware bookkeeping unresolved. Neither is
implemented as a generic workaround.

Further work needs an initialization strategy that preserves executable
storage code throughout the handoff, or arranges sufficient VGA shadow space
before storage ROM initialization. The post-dispatch footprint record is one
additional measurement toward that work. It is not a claim that all firmware
or all option-ROM policies can be supported by this application.

## Validation scope

The firmware harness runs the actual provenance parser and BBS eligibility /
priority-plan code with synthetic fixtures and optional private capture data.
Its VM has disposable media and no physical disk or PCI passthrough. The
capture cases independently assert disk eligibility and shadow-layout safety:
recognizing a disk does not turn an overlapping layout into a permitted boot.
Existing disk/USB, configuration, diagnostics and compiler-layout checks must
also pass. Physical native CSM, GPU initialization and OS boot remain separate
hardware tests.

## Primary references

- [TianoCore CSM ROM initialization and BBS construction](https://github.com/tianocore/edk2/blob/edk2-stable202308/OvmfPkg/Csm/LegacyBiosDxe/LegacyPci.c):
  `UpdateBevBcvTable`, `LegacyBiosInstallRom`, `LegacyBiosInstallPciRom`.
- [TianoCore BBS query implementation](https://github.com/tianocore/edk2/blob/edk2-stable202308/OvmfPkg/Csm/LegacyBiosDxe/LegacyBbs.c):
  `LegacyBiosGetBbsInfo` can connect PCI roots, build the table and call
  `Legacy16UpdateBbs`. The fresh post-query snapshot is defensive; no shadow
  change across that query was observed in the two captured configurations.
- [Intel Framework CSM specification 0.98](https://www.intel.com/content/dam/www/public/us/en/documents/reference-guides/efi-compatibility-support-module-specification-v098.pdf):
  legacy BIOS protocol, BBS table and Compatibility16DispatchOprom interfaces.

These interfaces and the reference code explain the design constraints; they
do not establish that a particular proprietary firmware uses identical code.

## User-selected disk warnings in the audit-fix candidate

`NCV_DISK_FIRMWARE_WARNING_BBS` retains the firmware BBS status and priority.
`NCV_DISK_FIRMWARE_WARNING_ORDER` records an explicit firmware ordering disable.
Neither is proof of a broken disk, and neither silently changes firmware variables.
The error screen carries these warnings forward and says whether native boot
was attempted or never reached. A native firmware implementation can still reject
an attempted disk; the selector records a returned status. No-media status may
also be stale, so user choice is allowed after all structural checks pass.

Unknown zero-status handlers still need provenance. A unique identity, valid
pointers/strings, readable and writable ranges, unchanged ownership, and clear
ROM layout remain required. This is an explicit attempt policy, not automatic
fallback to another disk. The read-only preflight reserves BBS journal buffers
and checks priority feasibility; live state is refreshed and verified again
before writes after ROM dispatch.

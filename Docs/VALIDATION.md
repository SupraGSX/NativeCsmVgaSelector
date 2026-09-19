# Validation — prerelease 1.2.3

## Current prerelease executable

Release and Debug contain the same 1.2.3 EFI. Its size and SHA256 are recorded
in [VERSION.md](../VERSION.md). Physical native-CSM boot testing of this changed
binary remains pending. The executable banner and installer payload names
retain the 1.2 family name; boot logs record `NCV_BUILD=1.2.3`.

Version 1.2.3 retains the complete code-audit implementation and changes its
build label. Both editions are rebuilt and checked against the frozen layout.
See [the completed audit checks](AUDIT_FIXES.md#validation-and-remaining-platform-checks)
for the applicable host and synthetic-firmware results and their limitations.

Both editions pass the frozen ABI checks: configuration 576 bytes, runtime
plan 24,824 bytes, and legacy target context 3,816 bytes. Disassembly retains
the reviewed Boot prologue (`0x7000 + 0x578`, same saved registers), one GOP
disconnect and one native legacy-boot handoff. This layout check is a
regression precaution, not a claim of a proven firmware size threshold.

## Historical v1.2.2 physical test — 2026-09-19

This result belongs to the older 237,568-byte v1.2.2 EFI, SHA256
`e0f14f7904682e9b86161dac7ac8f04e29f60796c8b1e4db5a83698f004d8e06`,
and does not validate the current 1.2.3 prerelease.
The returned USB executable matches that historical hash. Its newest boot log
records firmware-AHCI corroboration, a unique selected disk, completed GPU
routing/ROM dispatch/INT10 validation, a verified BBS priority transaction,
and the native legacy-boot boundary. No rejection or returned failure follows
in that attempt. The tester separately confirmed that Windows booted.

This establishes successful boot for the tested configuration. The log alone
cannot observe OS completion after control leaves the EFI application.
Private logs and hardware identifiers are not included in release packages.

## Historical v1.2.2 automated checks

| Check | Result |
| --- | --- |
| ROM layout, resident-ROM evidence, firmware-AHCI evidence, paired private captures and selected-only priority policy | 200 assertions passed |
| Disk/USB target selection, including an ineligible duplicate identity | 153 assertions passed |
| Firmware table, memory-map and PCI portability | 102 assertions passed |
| Split setup/countdown, cancellation, editing, cleanup and overlap behavior | 10 OVMF scenarios passed |
| General firmware-AHCI positive cases | 96 variants passed: different PCI IDs, controller locations, disk names and BIOS pointer representations |
| Firmware-AHCI ASan/UBSan and leak checks | 12,000 malformed-input cases, allocation failures, pointer bounds, controller collisions and changed bytes passed |
| Existing host configuration, marker, PCI-name, editing, recovery, placement-parser and Linux-launcher checks | Passed |
| Runtime manifests and source package consistency | Verified during release preparation |

Firmware tests use disposable images without physical disks or PCI passthrough.
The new firmware-handler fixtures contain synthetic bytes. Earlier private
captures exercise ROM ownership and overlap detection, not execution of the
captured ROMs. Sanitizer tests check bounded parsing and memory ownership.

The compatibility path is not keyed to a vendor, model, or fixed controller
address. It nevertheless requires a uniquely corroborated controller, readable
BIOS-resident pointers, stable handler-prefix bytes and bounded strings. It
does not validate the handler's full executable extent. The historical release
used stricter firmware-status policy. In version 1.2.3, selected-disk
firmware status/order restrictions are advisory; ambiguous identities, missing
handler provenance, unsafe ranges and actual ROM overlaps remain stops.
See [storage handling](LEGACY_STORAGE.md) and [compatibility](COMPATIBILITY.md).

## Reproducing checks

Build both editions using the pinned EDK II toolchain. `build.sh` now requires
the reviewed prologue and single-handoff checks before exporting an EFI and
writes private compiler/toolchain/source provenance to `Build/*/BUILD.json`.
`bash Scripts/validate.sh --firmware` builds both editions, runs the host and
synthetic firmware suites, packages the candidate and verifies exact members.
The PowerShell file tests run when `pwsh` is available; native Windows/physical
CSM validation remains a separate gate.
Use the following test targets:

```
make -C Tests test
make -C Tests firmware-disk-host
make -C Tests firmware-rom-guard
make -C Tests firmware-boot-target
make -C Tests firmware-portability
make -C Tests firmware-countdown
```

Set `EDK2_DIR` when the pinned toolchain is outside the checkout. Never run
concurrent EDK builds against the same toolchain workspace. Additional
dispatcher, diagnostics, recovery, and marker harnesses remain in Tests.
Optional private capture inputs are deliberately not distributed.

## Retained installer validation

Earlier Windows/Linux installer versions were exercised with
disposable virtual USBs, including installation, cancellation, existing-loader
backup, checksum rejection and internal-disk exclusion. They prepare one active
256 MiB FAT32 partition at a 1 MiB offset on an MBR USB disk. Windows tests used
an isolated Windows 11 guest; physical backing storage remained read-only.
Linux direct-script and desktop-launcher checks cover paths with spaces and
keeping errors visible. Those full installer runs do not establish the changed installers' native
platform behavior. Current file-operation and identity tests are documented
in [the audit checks](AUDIT_FIXES.md) and [installer testing](INSTALLER_TESTING.md);
native Windows storage and physical hot-plug tests remain pending.

Passing these checks does not guarantee every native-CSM implementation, GPU,
display sink, storage device or firmware setting. In particular, the overlap
assessment does not relocate ROMs or authorize alternative execution.

# Compatibility — release 1.2.2

The disk compatibility checks use firmware data and discovered PCI properties,
not a motherboard, controller-vendor or disk-model allowlist. A zero BBS status
word can be accepted when the uniquely selected disk has independently
corroborated resident-ROM or BIOS-resident AHCI evidence. Exact identity,
pointer bounds, readable/stable bytes, priority and disable checks still apply.
See [legacy storage handling](LEGACY_STORAGE.md) for the supported conditions.

The released EFI passed a physical boot with the firmware-AHCI path. Automated
tests also cover varied controller identities, disk names and BIOS pointers.
This establishes support for that class of representation; it does not promise
that every proprietary firmware implementation will boot successfully.

## Option-ROM memory conflicts

Preflight checks for recognized conflicts before GPU disconnection or ROM
copying, and refreshes that check after BBS discovery. A confirmed overlap
remains a stop even when the selected disk is otherwise eligible. A conflicting
ROM can belong to another connected device.

Where available, changing the conflicting device's storage or network OpROM
policy to UEFI-only may avoid the conflict while keeping CSM enabled for the
OS. Reselect the boot disk if its firmware name changes. The program does not
change firmware settings or relocate initialized ROMs automatically. See
[overlap diagnostics](ROM_OVERLAP.md).

## Corrected assumptions

| Edge case | Current behavior |
| --- | --- |
| CSM table with `PciExpressBase = 0` | Accepted when its checksum and other required checks pass. The CSM specification explicitly allows this value when PCI Express is absent. |
| CSM table with `LastPciBus = 0` | Accepted with a valid checksum; zero is a valid maximum bus index. GPU selection still uses the independently discovered PCI inventory. |
| Shorter known CSM table prefix | Fields needed for dispatch remain mandatory. Absent optional suffix fields do not alone reject a checksummed table. A partially present PCI Express base field is rejected. |
| CSM table with a longer extension | Checksum covers the declared length, while the structure copy is capped at the known structure size. The full table must fit in the scanned legacy region and readable memory. |
| Bad CSM checksum | The existing narrowly corroborated workaround remains limited to the original known table lengths, exact system-table relationship, nonzero aligned PCI Express base and nonzero last bus, plus valid call bytes and PnP signature. The new optional-field allowances cannot enable this workaround. |
| Firmware data crosses adjacent memory descriptors | Consecutive readable descriptors can satisfy one range, including unsorted maps and padded descriptor records. Gaps, read protection, MMIO, overlapping ownership and arithmetic overflow stop or reject access. |
| Malformed successful `GetMemoryMap()` result | Empty/oversized output, undersized strides and partial descriptor records fail before pointer validation. Growth retries remain bounded. |
| Active VGA uses `VGA_IO_16` | Discovery and Boot recognize this standard decode attribute, alongside VGA memory and ordinary VGA I/O. Two apparent owners still cause rejection. |
| Generic USB boot option uses `BBS(USB)` rather than `BBS(HD)` | Both option representations are accepted. The individual target must still be a uniquely matched eligible USB-class hard disk. Its type-2 live BBS record may differ from the type-5 generic option; the firmware's original handoff path and opaque data are preserved. |

The table changes follow the [Intel Framework CSM specification, revision 0.98](https://www.intel.com/content/dam/www/public/us/en/documents/reference-guides/efi-compatibility-support-module-specification-v098.pdf),
section 3.3.2 and its revision history. VGA decode attribute values use the
[pinned EDK II PCI I/O definitions](https://github.com/tianocore/edk2/blob/b03a21a63e3bd001f52c527e5a57feddb53a690b/MdePkg/Include/Protocol/PciIo.h)
(`VGA_IO_16` identifies 16-bit VGA I/O decoding). Supporting a bounded known prefix is
an application policy; it does not promise compatibility with every historic
CSM revision's handoff semantics.

## Limits deliberately retained

| Situation | Current requirement / reason |
| --- | --- |
| CSM absent or disabled; UEFI-only GPU | The required native CSM protocols and a usable legacy GPU option ROM must exist. This application does not supply a CSM or synthesize a legacy ROM. |
| Secure Boot / wrong architecture | The provided executable is unsigned x86-64 EFI. Firmware must permit it to execute; IA32 EFI is not supported by this build. |
| Selected GPU already active | Rejected before the transfer. This is a secondary-GPU switching application, not a replacement for the normal primary-GPU boot entry. |
| Multiple apparent active VGA owners | Rejected rather than guessing which controller can be disconnected. Ordinary PCI I/O enable or palette-only access does not establish ownership. |
| Different PCI roots/hosts or nonzero target segment | Existing topology limits remain. The legacy ROM dispatch ABI represents bus/device/function without a PCI segment. Cross-root routing requires separate design and hardware evidence. |
| Bridges with incomplete/conflicting device paths | Boot requires validated ancestry and matching bus registers. A diagnostic bus-number fallback never authorizes bridge writes. |
| Truncated, mismatched or unsupported option ROM | Existing image bounds, checksum, identity, chain and dispatch checks remain. No blind ROM execution or replacement ROM injection is introduced. |
| Several disks on one USB controller | Controller location alone is insufficient. The exact live BBS disk must be uniquely matched. |
| Localized/grouped firmware boot options | Explicit INI option-description overrides are available. The automatic match must remain unique and preserve the firmware's opaque handoff data. |
| USB visible to UEFI but absent from CSM BBS | UEFI discovery cannot invent legacy USB support. Firmware must expose a suitable legacy boot target; the operating system separately needs its storage driver. |
| Unusual or oversized BBS tables | Existing bounds and pointer checks remain; this release does not widen the native dispatch ABI. |
| Monitor/capture card loses signal | A successful firmware call does not prove a sink has locked onto the new signal. The countdown helps input switching; ROM/INT10 checks and durable checkpoints provide evidence, not a promise of visible output. |
| Firmware call hangs or resets the machine | It cannot return an EFI error for the program to display. Pre-call checkpoints remain important. Automatic GPU retries or console reconnection after irreversible operations are not added. |
| Missing keyboard or failed log writes | Existing diagnostics report the condition where a console remains usable. Do not remove the boot USB during execution. A black screen can still prevent a human from seeing a correctly generated message. |

## Validation scope

`make -C Tests firmware-portability` exercises the actual CSM table scanner, memory-map capture/range code and GPU
endpoint selection with synthetic firmware data under OVMF. It includes
accepted variants and rejected malformed or ambiguous inputs. In the private
test build only, the scanner bounds point at allocated low RAM containing test
tables. Checksum coverage, bounded copying, call/PnP checks and unique selection
run normally. The tests do not execute a native CSM, GPU ROM or physical bridge
transfer. Test-only range changes never enter production source or EFI files.

A hardware log exposed a gap in the earlier USB implementation: its
generic option was type 5 while the individual live disk was type 2. Earlier
fixtures covered only type-2 generic USB options, so the old checks rejected
this valid representation before GPU handoff. The expanded boot-target suite
covers both representations, USB/internal disk matching, priority transactions,
override rules and rejected USB-group/internal-disk pairings. This correction
does not establish that the later GPU or USB handoff succeeds on every system.
The diagnostics suite covers returnable and
fatal error presentation, queued-input handling and log failures. Normal setup,
editing and marker scenarios cover the dispatcher separately.

Release and Debug must pass the unchanged ABI and reviewed Boot prologue checks
before deployment. Earlier isolated OVMF disconnect/stack experiments did not
reproduce the historical size-sensitive physical failure. Its root cause is
still unknown; the layout guard is a regression precaution.

For physical validation, first exercise the existing known-working target from
the test USB. Then test another GPU/disk/firmware combination independently.
After a failure, retain `Config.ini`, both log files and a photo of any final
error screen, together with motherboard/firmware and GPU models. A board that
passes is evidence for that configuration, not all AMI firmware.

See `VALIDATION.md` for recorded tests and the physical result above.
Other hardware combinations still require physical validation.

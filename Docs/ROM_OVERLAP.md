# Option-ROM overlap diagnostics

Release 1.2.2 includes an opt-in, read-only placement assessment after an
overlap rejection. **Alternative GPU ROM execution is not implemented or
enabled.** The assessment does not fix overlapping GPU and storage ROM code.
The usual boot path remains when preflight finds no conflict.

## What happens on a conflict

The existing guard rejects a confirmed overlap before disconnecting the GPU or
copying its ROM. After the boot routine returns and releases its resources, a
menu offers:

- **Enter:** return to firmware. Review the conflicting device’s legacy storage
  or PXE/network OpROM policy. If available, use UEFI-only for that ROM or disable
  unused network boot, keeping CSM enabled. Then reselect the disk if its name changed.
  Returning from an EFI application does not guarantee that firmware opens its
  settings page. This application does not change firmware settings itself.
- **A:** assess whether the selected ROM advertises the prerequisites for a
  standard separate initialization/runtime placement path. This reads captured
  metadata and writes an optional `NativeCsmVgaRecovery.log` beside the EFI.
  It does not execute or relocate anything.
- **Esc:** cancel and return to firmware.
- **F2:** edit targets only after a clean, returnable preflight. Saving starts
  fresh preflight; this does not bypass the overlap guard. After choosing A,
  this menu permits only return/cancel, including when assessment-log cleanup fails.

The assessment always explains why alternative execution remains unavailable.
There is no automatic selection, timeout-driven retry, new configuration setting,
or persisted opt-in. Repeated A presses do not repeat the log write. A missing
keyboard or failed input device leaves the error visible for a manual restart.
An unwritable log does not prevent displaying the assessment or cancelling.

Other errors retain their existing handling. No recovery menu is offered after
an irreversible handoff or when required cleanup reports failure. A failure
after hardware changes still requires a restart; the application does not
pretend it can restore undocumented firmware state.

## Assessment codes

| Code | Meaning |
| --- | --- |
| `NCV_ALT_ROM_NO_SPLIT_CONTRACT` | The older ROM does not advertise the standard separate initialization/runtime contract. This is not proof that every possible custom technique is impossible. |
| `NCV_ALT_ROM_RUNTIME_UNKNOWN` | No usable maximum runtime size is declared. |
| `NCV_ALT_ROM_RUNTIME_OVERLAP` | The declared runtime extent exceeds the conservative space limit. |
| `NCV_ALT_ROM_METADATA_UNAVAILABLE` | ROM placement metadata is missing, malformed, or inconsistent. |
| `NCV_ALT_ROM_EXECUTOR_UNAVAILABLE` | ROM size prerequisites pass, but firmware support, destination ownership and an execution backend are not validated. This is not permission to boot. |

The size limit is bounded by both the existing primary VGA ROM extent and the
first detected conflict. It is a necessary-condition check, **not** proof of free
or writable memory. The assessment does not allocate low memory, call CSM
protocols, modify PCI routing, copy shadow ROMs, change firmware variables, or
change Config.ini. The independent full option-ROM validation remains required.

## Implementation boundaries

`RomRecovery` owns copies of facts, not pointers into the released runtime plan.
Only typed overlap results can populate it; formatted diagnostic strings cannot
enable recovery. A fresh guard check revokes earlier evidence. The outer
dispatcher offers the menu only for the returning overlap status after cleanup.

`RomPlacement` is a bounded, read-only metadata parser and assessment function.
None of its results authorizes execution. The core boot routine and the frozen
configuration/runtime-plan/BBS-context layouts are unchanged. Keep the reviewed
production Boot prologue and single handoff; never relax those checks to fit a
new recovery implementation.

## Requirements before an execution option can be enabled

1. Establish an actual execution contract. TianoCore's reference PCI 3.x path
   requires both a suitable ROM and a CSM16 PCI interface version of at least
   3.0. The Compatibility16 table revision alone does not establish that support.
   Older ROMs need a separately demonstrated technique.
2. Prove ownership, accessibility, sizing and separation of initialization and
   runtime destinations. Reserve the destinations through the supported firmware
   mechanism. Unused-looking bytes or padding are not an allocation API.
3. Preserve live storage code and its interrupt/firmware references throughout
   GPU initialization. Restoring overwritten bytes afterwards is insufficient.
4. Rebuild and revalidate the selected targets, live layout and resources after
   the user opts in. An old menu snapshot must never become a boot authorization.
   Run a fresh display-switch countdown before the irreversible transition.
5. Exercise the new executor on suitable reference firmware, then validate on
   physical hardware. Failures after hardware modification must stop for a
   restart, not return to the menu and attempt another initialization.

The captured older GPU currently receives `NCV_ALT_ROM_NO_SPLIT_CONTRACT`.
Its initialization image cannot simply be assumed relocatable because a
different address has enough bytes. No generic relocation workaround has been
established for that case.

## Tests and references

Run `make -C Tests test`, `make -C Tests firmware-rom-recovery`, and the ROM-guard
suite described in [validation](VALIDATION.md). The recovery suite uses the real
dispatcher/UI with injected failures and disposable OVMF disks. It asserts one
boot invocation, unchanged configuration, fresh user input, typed eligibility,
error persistence, and no alternative executor. It does not execute captured
GPU or storage ROMs or model proprietary firmware internals.

See [legacy-storage constraints and primary references](LEGACY_STORAGE.md)
for the CSM specification and TianoCore implementation. `DispatchOprom`'s
destination field is part of a supported relocation contract; the presence of
that field alone does not make an older option ROM relocatable.

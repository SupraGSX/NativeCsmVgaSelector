# Native CSM VGA Selector 1.2 agent guide

## Architecture and provenance

`NativeCsmVgaSelector.c` performs strict configuration loading and dispatches
to Probe or Boot mode. Successful interactive setup closes its resources and
reloads Config.ini before entering Boot preflight and its final countdown; it does not
return to firmware or recursively invoke the EFI application. Probe owns PCI/BBS discovery, complete display
and storage candidate collections, ranking, and candidate-INI generation.
Boot constructs a compact validated runtime plan, disconnects the active GOP
controller, transfers VGA routing, enables the target endpoint, copies and
dispatches its validated legacy ROM through Compatibility16, verifies INT 10h,
temporarily applies the live BBS priority transaction, and calls native
`LegacyBoot()` exactly once.

Use the package-root build.sh with pinned source-local EDK II. Both editions
use the RELEASE compiler profile. See ../README.md.

## Original Boot baseline invariants

- `PROBE_CONFIG`: 576 bytes.
- `NATIVE_CSM_VGA_RUNTIME_PLAN`: 24,824 bytes.
- `LEGACY_BOOT_BOOT_TARGET_CONTEXT`: 3,816 bytes.
- Historical original Boot frame: 30,208 bytes including saved registers.
- Current reviewed Boot prologue: 0x7000 + 0x578 allocation, unchanged saved-register pushes.
- Exactly one production controller disconnect:

  ```c
  gBS->DisconnectController (Plan.Active.Handle, NULL, NULL);
  ```

- Exactly one production native `LegacyBoot()` call.
- Candidate collections and candidate-INI generation are Probe-only and must
  remain unreachable from Boot mode.
- Boot object ownership, protocol arguments, hardware operations, and native CSM
  boot ordering are frozen. Configuration/UI changes require regression tests.

### Critical stack-frame regression

A historical report associated a small Boot-frame increase with failure at
active-GOP disconnect and recovery after reverting it. The exact failing binary
has not been isolated and reproduced, and later working builds intentionally
reduced the original frame. Independent OVMF disconnect/guard-page experiments
did not reproduce that failure. Do not claim a proven 16-byte threshold or a
specific firmware memory defect from this correlation.

Preserve the current reviewed prologue as a regression precaution. Do not
perform broad Boot refactors, add Boot locals, alter layouts, or change object
lifetimes. Every Boot-reachable change requires disassembly review and a new
physical hardware test. Do not relax the layout check to make a candidate pass.

Recover from the frozen source and known-good EFI, and cold-reset after an
irreversible native-CSM attempt. Never deploy to USB without separate explicit
authorization.

## Generalization

Targets must come from Config.ini including its optional Marker section. Do not add
machine-specific PCI addresses, disk identifiers, profile names or home paths.
Internal names describe legacy boot, validation, and reference snapshots. Reference-snapshot INI keys are not accepted. Reserved ABI fields remain disabled. No hardware
snapshot is compiled into this revision. The normal strict configuration,
ROM, bridge routing, BBS and firmware validation remains in force.

The combined project release uses GPL-3.0-only. Preserve all upstream SPDX and copyright notices; see THIRD_PARTY_NOTICES.md.

See VALIDATION.md for this revision’s disassembly result and required physical testing.

Marker config is separate from PROBE_CONFIG and the runtime plan to preserve their ABI sizes. MarkerApply runs only after automatic Boot preflight; MarkerRestore is dispatched only on a returning core.

BootReport owns fixed diagnostic buffers outside Boot locals. Its Boot-reachable
entry points use EFIAPI and noinline boundaries. Friendly stages are derived
from existing NCV records: adding stage calls throughout RuntimePlan changed
LTO inlining and grew the Boot frame by 64 bytes during development. That
approach was removed, and the reviewed prologue restored. Do not relax the
layout check. Error reporting must not introduce GPU retries or reconnect
consoles after native handoff. Returnable errors wait in the outer dispatcher;
existing irreversible-failure paths still halt.


## Legacy storage compatibility

See LEGACY_STORAGE.md for limits. Zero BBS status is accepted only for a
uniquely selected, corroborated resident-ROM or firmware-AHCI disk. Other zero-status entries
stay untouched. The physical overlap remains blocked; do not describe this
build as a complete fix for overlapping ROMs.

LegacyRomGuard retains a borrowed preflight plan only until target-context
release. Its second snapshot occurs after GetBbsInfo and before BBS selection;
it does not invoke GetBbsInfo again. LegacyStorageRom owns copies of shadow
memory/readability and the PCI inventory, freed by LegacyRomGuardReset.
LegacyFirmwareDisk separately owns a readable E/F-segment snapshot and PCI
inventory copy; the live-memory pointer is borrowed. It checks a bounded,
unchanged handler prefix and strings, not the full executable code extent.
Missing firmware evidence leaves this exception unavailable without blocking
normal-status disks. No
new fields were added to the frozen runtime-plan or BBS-context layouts.

GCC LTO changed several baseline inline decisions during this work. The
explicit inline/noinline boundaries in LegacyBootTarget and Marker retain the
reviewed Boot prologue without padding or relaxing the checker. They do not
establish a hardware defect or a magic image-size threshold. Recheck the
production disassembly when changing these helpers.

The ROM harness redirects physical reads in its private copy of the guard to
allocated fixture RAM; the production implementation has no test hooks. The
ASan/UBSan host runner uses copied private fixture files produced by the
firmware runner; never package those captures as public test data.

## Overlap diagnostics

See ROM_OVERLAP.md. RomRecovery stores only owned metadata from a
typed, returning overlap rejection. The UI runs in the dispatcher after core
cleanup and marker restoration. It never automatically retries the core or changes hardware,
or reuses freed plan pointers. Fresh checks clear stale eligibility; cleanup
errors and irreversible failures suppress the menu.

RomPlacement results are necessary-condition diagnostics, never permission to
execute. There is no alternative executor in this build. Do not turn a passing
size check into a dispatch call or infer PCI 3.x relocation support from the
Compatibility16 table revision. Any future executor requires independent
firmware capability validation, owned destinations, fresh target/layout checks,
and physical validation. Preserve the existing single-handoff path throughout.

## Final countdown

BootCountdown owns display-name and action state outside Boot's locals. Its
EFIAPI/noinline wait runs after preflight, before MarkerApply/DisconnectController.
Routine logger console mirroring ends there; file/serial logging and independent
fatal/error summaries remain. AutoBoot=false never waits or switches.

F2/Esc return EFI_ABORTED and release the plan/log/files before the dispatcher
can act. A typed cleanup result, not diagnostic strings, gates editing. Marker
restore errors also suppress it. Clean preflight failures may offer F2 from the
outer error/recovery screen. A fresh save reloads Config.ini and repeats all
preflight. No discovery/UI call is reachable while the Boot frame is active.
Any cleanup failure or completed countdown disables this retry route. Preserve
the reviewed frame and single hardware handoff; do not add Boot locals.


## Split five-second windows

BootSetupWindow runs in the dispatcher before Boot, with PCI names still available
for setup. Early F2 opens setup before runtime preflight exists; Esc cancels.
Input/timer errors stop. The final BootCountdownWait is five seconds too. Normal
automatic waits total ten seconds plus unchanged preflight work. AutoBoot=false
skips both. Final-window F2/Esc still require the typed cleanup path above. Do
not move early setup inside the owning Boot frame or add a third automatic wait.

## Audit-fix candidate (2026-09-19)

The user explicitly chose advisory firmware disk policy: allow an explicitly
selected, structurally validated disk despite disabled/failed/no-media/priority
or LegacyDevOrder restrictions; retain its raw warning values in logs and the
final error summary. Do not reintroduce a firmware-policy-only veto. Unknown
zero-status handler provenance and all identity/memory/ROM guards still apply.

The dispatch table preparation now has a directly tested production helper.
LegacyDispatch owns one reserved low-memory page outside Boot locals, allocated
before countdown and freed on returning cleanup. BBS journal resources and
feasibility are prepared before switching, then refreshed at the original
mutation boundary. No extra disconnect/LegacyBoot call or retry was introduced.
The unused ROM shadow backup was removed. Policy/order private includes keep
existing compilation boundaries; ABI and exact Boot prologue stay frozen.
The candidate requires a new physical boot test; source changes do not deploy it.

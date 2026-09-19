# Architecture

`NativeCsmVgaSelectorPkg/Application` is the shared implementation.

- `NativeCsmVgaSelector.c`: pre-handoff dispatch and F2 setup after clean core return.
- `BootCountdown.c`: early setup window, final named countdown, input requests
  and typed cleanup status;
  no hardware operations or recursive boot/setup calls.
- `CandidateIni.c`: portable strict parser, boot requirements, candidate ranking
  and INI formatting; used by firmware and host tests.
- `ConfigFile.c`: bounded adjacent-file reads, verified copies, save/restore I/O.
- `ConfigRecovery.c`: usable-config acceptance, explicit recovery and preserved
  fresh setup. Its UI runs before any hardware handoff.
- `ProbeConfig.c`: conversion to the layout-stable boot configuration and
  candidate/edited target saves. `ConfigEdit.c` retains unrelated INI bytes.
- `NativeCsmVgaProbe.c`: PCI/BBS discovery and setup orchestration.
- `TargetSelection.c`: keyboard target selection, retained-policy review and
  warnings. `PciNames.c`/`PciIdLookup.c` provide optional offline display names.
- `NativeCsmVgaBoot.c`, `RuntimePlan.c`, `LegacyBootTarget.c`: validated VGA
  routing, legacy ROM dispatch, BBS transaction and native legacy boot.
- `Marker.c`: bounded, verified optional menu-marker write/restore at handoff.
- `LegacyRomGuard.c`: refreshed shadow-layout checks and provenance lifetime.
  `LegacyStorageRom.c` validates resident PCI ROM/PnP evidence;
  `LegacyFirmwareDisk.c` separately corroborates BIOS-resident AHCI handlers.
  Each owns its snapshots and PCI inventory copies outside the boot frame.
  Neither executes a disk-handler pointer or bypasses the overlap guard.

Configuration recovery state, marker settings and UI data remain outside the
sensitive boot-plan structures. The boot phase retains its single controller
disconnect and native LegacyBoot call. Fatal errors after irreversible hardware
changes do not open a new interactive setup path. See AGENTS.md for constraints.

## Continuing after setup

The dispatcher loops through setup and returning preflight attempts. Automatic
boots first offer a five-second F2/Esc window outside Boot. F2 opens setup before
a runtime plan exists. A separate five-second countdown runs after preflight, before marker application and GOP
disconnect. Its target name is copied before the PCI name database is released.
F2/Esc request a return, never setup inside the owning Boot frame. Setup can run
only after plan, logger, volume and marker cleanup succeed. Cleanup failure or
a decision to proceed to handoff disables editing; error reporting cannot
authorize retry from log strings.
Probe reports boot readiness only after an explicitly confirmed interactive
save and successful file/log closure. Diagnostic-only runs, missing candidates,
cancellation, and failed saves never request handoff. A separate helper reloads
Config.ini, including retained policies and marker settings, before returning
to the dispatcher. It resets edit state and rejects a probe-mode or incomplete
reload. The helper and discovery stack have unwound before Boot is called;
The order of hardware operations is unchanged. AutoBoot=false
continues to mean validation only.

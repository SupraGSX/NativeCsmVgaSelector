# Changes in 1.2.3

This candidate builds on v1.2.2 and implements the September 19 audit and the
selected-disk warning policy. Physical native-CSM boot testing of this changed
binary is still pending. Earlier hardware results apply to the previous binary.

## Disk-selection policy

The user-selected disk may be attempted despite firmware disabled/failed,
no-media/reserved status, discouraged priority, or a validated `LegacyDevOrder`
disable marker. Those values remain explicit **firmware boot warnings**. They
are retained in the log and carried to the final error screen, which distinguishes
an actual native boot attempt from an earlier stop. A firmware warning does not
establish that a disk is damaged. Firmware may still reject the eventual call.

Structural preflight remains mandatory: unique identity, valid controller and
boot arguments, safe memory and strings, stable ownership, zero-status handler
provenance when applicable, and no ROM overlap. Unselected discouraged disks
retain their priorities. No firmware ordering variable is written. There is one
native boot call and one display disconnect, with no automatic retry.

## Audit findings addressed

| Finding | Implemented behavior |
| --- | --- |
| F1: disabled disk fallback | Replaced the accidental loss of policy with an explicit selected-disk override. Keep validated order/disable data, preserve unselected disabled rows, and retain warnings through cleanup and error reporting. |
| F2: missing GPU device/function | Production dispatch preparation supplies `(Device << 3) | Function`; tests check every device/function combination and the other actual dispatch fields. |
| F3: Linux USB identity | Snapshot and recheck serial, WWN, sysfs identity and kernel disk connection sequence along with existing fields. Destructive mode requires the sequence, checks identity between phases, and uses device locking for partition operations. Confirmation includes the chosen device and connection. |
| F4: BBS priority collision | Apply dense priorities only to eligible transaction rows. Preserved rows cannot mask another row's planned priority. Rollback tests retain exact original bytes. |
| F5: unchecked low-memory hashes | Validate all mandatory IVT/BDA/video/firmware ranges before initial physical reads and again around ROM execution. Protected, missing and overlapping descriptors stop the affected operation. |
| F6: inherited watchdog | Establish watchdog policy before configuration recovery or interactive setup. Disable the inherited timer; handle unsupported and error returns explicitly. |
| F7: optional description edit | Insert a missing description into the existing Boot section, including at EOF without a newline. Preserve unrelated settings and distinguish capacity from invalid-edit errors. |
| F8: Windows replacement | Stage, flush and hash the new EFI before touching the active loader. Preserve a verified backup, restore the previous file on returning commit failures, and retain named recovery files if restoration fails. |
| F9: packaging exclusions | Use an explicit reviewed source manifest; reject unexpected files, prohibited types and symlink paths. Stage ZIP/runtime output and verify exact source members and runtime hashes. |

## Other improvements addressed

1. Preflight checks BBS writability and priority feasibility and reserves journal
   buffers before switching. Reserve the low-memory dispatch page before the
   switch countdown; returning cleanup owns its release. Recheck live BBS/order
   data at the original mutation boundary.
2. Readable and writable memory spans use the same bounded descriptor walker,
   including adjacent/unsorted descriptors, gaps, protection and overlaps.
3. Format each digest as one log record: four records replace 136 fragments.
   Durable stage barriers remain. No measured physical USB speedup is claimed.
4. Remove the unused shadow backup allocation/copy and its late failure path.
5. Capture firmware bytes directly into owned state, removing the temporary
   128 KiB buffer/copy. Storage proofs own compact PCI identity records instead
   of full protocol-bearing inventory records; duplicate-address detection and
   all identity fields used by the proofs remain intact.
6. Exclude the known active VGA from switch targets. Reject ambiguous disk
   identities in setup, display firmware status warnings, and retain selection
   of disks whose firmware policy merely discourages booting them.
7. Propagate configuration-read and dispatcher volume-close failures before Boot.
8. Production builds require the exact reviewed prologue and single-handoff
   checks before exporting EFI. Explicit exceptions remain active under Python
   optimization. Record source/compiler/binutils/toolchain state privately.
   `bash Scripts/validate.sh --firmware` provides a combined validation/package run.
9. Test real dispatch preparation, page allocation/cleanup, watchdog arguments,
   warning retention, close errors, parsed disk order, optional-key edits, USB
   identity changes, packaging, and installer file-operation failures. See
   [the installer test recipe](INSTALLER_TESTING.md) for platform validation.
10. Isolate firmware disk policy and ordering-variable parsing in private
    implementation includes without changing their compilation order or the
    frozen runtime/BBS structures. Share memory-range and PCI-identity helpers.
11. Correct the two separate five-second windows, use exact destructive
    confirmation phrases, document advisory disk policy, and record the full
    candidate build identifier in boot logs.

## Validation and remaining platform checks

Host parser tests pass with ASan/UBSan. The firmware disk host suite passes
96 accepted variants and 12,000 malformed cases, including leak detection.
OVMF checks pass: 640 portability/protocol-boundary assertions, 281 disk-target
assertions, 172 public ROM/storage assertions, ten countdown scenarios, and
all 28 dispatcher/setup/recovery scenarios, including editing a valid BDF-only
configuration through the actual F2 save flow.
The six PowerShell staging/commit/rollback scenarios pass against temporary
files under portable PowerShell 7 on Linux. Five packaging tests and four Linux
identity test groups pass. No physical drive is used by these tests.

The production ABI and reviewed `0x7000 + 0x578` Boot prologue remain required;
the build gate is not relaxed. Physical native-CSM boot remains untested for this
candidate. Native Windows storage operations, actual FAT power-loss behavior,
and physical USB hot-plug timing require platform validation. Mocked revalidation
and returning file failures do not establish those hardware properties.

Disk preparation comprises multiple operations, not an atomic transaction.
FAT moves do not guarantee recovery from arbitrary power loss; retained backups
provide a documented recovery path. None of these changes establishes a cause
for the historical physical disk-corruption reports or stack-layout sensitivity.

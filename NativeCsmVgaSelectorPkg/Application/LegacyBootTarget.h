/** @file
  Strict legacy boot-target discovery and reversible BBS-priority
  transaction support.

  This module does not call LegacyBoot, InstallPciRom, Int86, SetVariable, or
  any PCI protocol operation.  It discovers one existing active legacy
  generic hard-drive or USB Boot#### option, separately identifies one live BBS
  target, owns bounded copies of the arguments that a separate caller may
  later pass to LegacyBoot, and can perform only a journaled BootPriority
  transaction on the returned BBS table.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_SELECTOR_LEGACY_BOOT_BOOT_TARGET_H_
#define NATIVE_CSM_VGA_SELECTOR_LEGACY_BOOT_BOOT_TARGET_H_

#include <Uefi.h>

#include <Protocol/DevicePath.h>
#include <Protocol/LegacyBios.h>

#include "Log.h"

#define LEGACY_BOOT_BOOT_TARGET_MAX_BBS_ENTRIES     256U
#define LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES    128U
#define LEGACY_BOOT_BOOT_TARGET_MAX_PRIORITY_WRITES \
  (LEGACY_BOOT_BOOT_TARGET_MAX_BBS_ENTRIES * 2U)


typedef struct {
  UINT16  Index;
  UINT16  OldPriority;
  UINT16  RequestedPriority;
} LEGACY_BOOT_BBS_PRIORITY_WRITE;

typedef struct {
  UINT8  ManufacturerReadResult;
  UINT8  DescriptionReadResult;
  CHAR8  Manufacturer[LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U];
  CHAR8  Description[LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U];
} LEGACY_BOOT_BBS_STRING_SNAPSHOT;

typedef struct {
  UINT32  Signature;

  BOOLEAN     GetBbsInfoCalled;
  EFI_STATUS  GetBbsInfoStatus;
  BOOLEAN     DiscoveryComplete;

  UINT16  HddCount;
  UINT16  BbsCount;
  BBS_TABLE  *FirmwareBbsTable;

  /** Read-only discovery snapshot used to bind the later durable journal. **/
  BBS_TABLE  *BbsDiscoverySnapshot;
  LEGACY_BOOT_BBS_STRING_SNAPSHOT  *BbsDiscoveryStrings;
  UINT32     BbsDiscoveryCrc32;
  UINT32     BbsDiscoveryIdentityCrc32;
  UINT32     BbsDiscoveryStringsCrc32;

  /**
    Read-only, application-owned copy of the pinned LegacyDevOrder variable.
    If the variable is missing, malformed, unsupported, or cannot be safely
    parsed, LegacyDevOrderUsable is FALSE and the priority plan uses the
    explicitly logged live-BBS-index deterministic fallback. A present value
    is usable only after complete strict parsing against the live BBS table.
  **/
  BOOLEAN  LegacyDevOrderPresent;
  BOOLEAN  LegacyDevOrderUsable;
  UINT32   LegacyDevOrderAttributes;
  UINT8    *LegacyDevOrderData;
  UINTN    LegacyDevOrderSize;
  UINT32   LegacyDevOrderCrc32;
  /** Global raw-variable order rank for every covered live BBS index. **/
  UINT16   *LegacyDevOrderRanks;
  /** TRUE when the corresponding Data value has the disabled 0xFF high byte. **/
  BOOLEAN  *LegacyDevOrderDisabled;

  /** Durable pre-mutation copy, populated only by JournalAndValidate. **/
  BBS_TABLE  *BbsJournal;
  LEGACY_BOOT_BBS_STRING_SNAPSHOT  *BbsJournalStrings;
  /** Final priorities planned for the armed transaction. **/
  UINT16     *PlannedPriorities;
  /** Expected live priorities while a multi-write transaction is active. **/
  UINT16     *ExpectedPriorities;
  /** Allocation-free scratch copy used for live verification and rollback. **/
  BBS_TABLE  *VerificationScratch;

  UINTN   BbsTableBytes;
  UINT32  BbsJournalCrc32;
  UINT32  BbsIdentityCrc32;
  UINT32  BbsPriorityCrc32;
  UINT32  BbsJournalStringsCrc32;
  UINT32  PlannedPriorityCrc32;
  BOOLEAN JournalAttempted;
  BOOLEAN JournalComplete;

  UINT16  LiveBbsIndex;
  BBS_TABLE  LiveBbsEntry;
  CHAR8      LiveManufacturer[LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U];
  CHAR8      LiveDescription[LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U];

  UINT16  BootOptionNumber;
  UINT32  BootOptionAttributes;
  CHAR16  *BootOptionDescription;
  UINTN   BootOptionDescriptionBytes;
  UINT32  BootOptionDescriptionCrc32;

  /** Exact validated BBS device path, including its end node. **/
  UINT8   *BbsDevicePath;
  UINTN   BbsDevicePathSize;
  UINT32  BbsDevicePathCrc32;
  UINT16  BbsDeviceType;
  UINT16  BbsStatusFlag;
  CHAR8   BbsDescription[LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U];

  /** Exact opaque Boot#### OptionalData copy. Firmware variables are never changed. **/
  UINT8   *LoadOptions;
  UINTN   LoadOptionsSize;
  UINT32  OriginalLoadOptionsCrc32;
  UINT32  LoadOptionsCrc32;

  BOOLEAN                         PriorityTransactionAttempted;
  BOOLEAN                         PriorityTransactionDirty;
  BOOLEAN                         PrioritiesApplied;
  UINTN                           PriorityWriteCount;
  LEGACY_BOOT_BBS_PRIORITY_WRITE       PriorityWrites[
                                    LEGACY_BOOT_BOOT_TARGET_MAX_PRIORITY_WRITES
                                    ];
} LEGACY_BOOT_BOOT_TARGET_CONTEXT;

/**
  Enumerates BootOrder and referenced Boot#### variables first, selecting
  exactly one valid active generic BBS_HARDDISK option. Malformed, missing,
  truncated, and unsupported referenced entries are logged and skipped. It
  then invokes GetBbsInfo exactly once and separately identifies one live BBS
  entry at TargetBus:TargetDevice.TargetFunction whose bounded live description
  equals TargetDescription (case-insensitive, preserving spaces). A generic
  hard-disk option can represent either internal disks or USB-HDD boot;
  LegacyOptionDescription selects the appropriate firmware option.

  No firmware variable or BBS-table field is written by this operation.
  Context must be zero-initialized by the caller and is one-shot until released.
**/
EFI_STATUS
LegacyBootBootTargetDiscover (
  IN  APP_LOGGER                 *Logger,
  IN  EFI_LEGACY_BIOS_PROTOCOL   *LegacyBios,
  IN  UINT32                     TargetBus,
  IN  UINT32                     TargetDevice,
  IN  UINT32                     TargetFunction,
  IN  CONST CHAR16               *TargetDescription,
  IN  CONST CHAR16               *LegacyOptionDescription,
  IN  CONST CHAR16               *ExcludeDescription,
  OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT *Context
  );

/**
  Revalidates the application-owned Boot#### description, exact BBS path, and
  byte-identical opaque OptionalData copy without reading firmware-owned BBS
  data.
**/
EFI_STATUS
LegacyBootBootTargetValidateOwnedCopies (
  IN APP_LOGGER                         *Logger,
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT   *Context
  );

/**
  Captures, logs, and flushes the definitive BBS mutation journal after the
  caller has durably committed its PCI journal.  It then performs a complete
  live reread without calling GetBbsInfo.  This operation is one-shot.
**/
EFI_STATUS
LegacyBootBootTargetJournalAndValidate (
  IN     APP_LOGGER                    *Logger,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT    *Context
  );

/** Verifies exact original BBS identity and priority state. **/
EFI_STATUS
LegacyBootBootTargetValidateOriginal (
  IN APP_LOGGER                         *Logger,
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT   *Context
  );

/** Verifies exact BBS identity and the complete applied priority plan. **/
EFI_STATUS
LegacyBootBootTargetValidateApplied (
  IN APP_LOGGER                         *Logger,
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT   *Context
  );

/**
  Returns immutable, application-owned arguments for a later LegacyBoot call.
  This module itself never invokes LegacyBoot.
**/
EFI_STATUS
LegacyBootBootTargetGetLegacyBootArguments (
  IN  CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  OUT BBS_BBS_DEVICE_PATH               **BootOption,
  OUT UINT32                            *LoadOptionsSize,
  OUT VOID                              **LoadOptions
  );

/** Returns exact bounds and CRC32 values for the two owned binary copies. **/
EFI_STATUS
LegacyBootBootTargetGetOwnedCopyInfo (
  IN  CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  OUT CONST UINT8                       **BbsDevicePath,
  OUT UINTN                             *BbsDevicePathSize,
  OUT UINT32                            *BbsDevicePathCrc32,
  OUT CONST UINT8                       **LoadOptions,
  OUT UINTN                             *LoadOptionsSize,
  OUT UINT32                            *LoadOptionsCrc32
  );

/**
  Applies the reversible reference-style priority plan using BootPriority-only
  writes. Invalid/reserved, disabled, failed, and no-media entries are
  preserved; eligible entries are first made unprioritized, then the selected
  target is assigned priority zero,
  remaining valid HDD entries follow in original firmware preference order,
  and remaining valid entries follow after them.  Every write is flushed,
  read back, and verified.  Any failure triggers immediate reverse rollback.
**/
EFI_STATUS
LegacyBootBootPriorityApply (
  IN     APP_LOGGER                    *Logger,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT    *Context
  );

/** Restores successful priority writes in exact reverse order and verifies. **/
EFI_STATUS
LegacyBootBootPriorityRollback (
  IN     APP_LOGGER                    *Logger,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT    *Context
  );

/**
  Releases application-owned storage.  Refuses to release a context whose BBS
  priority transaction is still dirty.
**/
EFI_STATUS
LegacyBootBootTargetRelease (
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context
  );

EFI_STATUS EFIAPI LegacyBootBootTargetPrepare (APP_LOGGER *Logger, LEGACY_BOOT_BOOT_TARGET_CONTEXT *Context);

#endif

/** Read-only preflight for the legacy VGA copy destination.
    SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_LEGACY_ROM_GUARD_H_
#define NCV_LEGACY_ROM_GUARD_H_
#include "RuntimePlan.h"

#define NCV_ROM_SHADOW_BASE  0xC0000U
#define NCV_ROM_SHADOW_SIZE 0x20000U
#define NCV_ROM_BLOCK_SIZE  512U
#define NCV_ROM_BLOCK_COUNT (NCV_ROM_SHADOW_SIZE / NCV_ROM_BLOCK_SIZE)

typedef enum {
  RomGuardClear,
  RomGuardOverlap,
  RomGuardDiskHandlerOverlap,
  RomGuardUnreadable,
  RomGuardUnknownLayout
} LEGACY_ROM_GUARD_REASON;

typedef struct {
  LEGACY_ROM_GUARD_REASON Reason;
  UINTN ActiveBytes;
  UINTN Address;
  UINTN Bytes;
  UINT16 Vendor;
  UINT16 Device;
} LEGACY_ROM_GUARD_RESULT;

/* Inspect a copied shadow snapshot; never execute or modify ROM bytes. Clear
   means no recognized conflict, not proof that every firmware reference is safe. */
LEGACY_ROM_GUARD_REASON
LegacyRomGuardInspect (
  CONST UINT8 *Shadow, CONST UINT8 *ValidBlocks, UINTN CopyBytes,
  UINT16 ActiveVendor, UINT16 ActiveDevice, UINTN DiskHandler,
  LEGACY_ROM_GUARD_RESULT *Result
  );

EFI_STATUS EFIAPI
LegacyRomGuardCheck (APP_LOGGER *Logger, CONST NATIVE_CSM_VGA_RUNTIME_PLAN *Plan);
EFI_STATUS EFIAPI LegacyRomGuardRecheckAfterBbs (APP_LOGGER *Logger);
VOID EFIAPI LegacyRomGuardReset (VOID);
#endif

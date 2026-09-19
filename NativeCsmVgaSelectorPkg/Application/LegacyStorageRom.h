/** Verified resident storage-ROM provenance; no hardware or BBS writes.
    SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_LEGACY_STORAGE_ROM_H_
#define NCV_LEGACY_STORAGE_ROM_H_
#include "PciEnumerate.h"
#include <Protocol/LegacyBios.h>

/* Snapshot and live view both describe C0000-DFFFF. The production live view
   is physical shadow memory; tests use ordinary RAM. Inventory is copied. */
EFI_STATUS EFIAPI LegacyStorageRomPrepare (
  CONST UINT8 *Snapshot, CONST UINT8 *ValidBlocks, CONST UINT8 *LiveView,
  CONST PCI_INVENTORY *Inventory);
VOID EFIAPI LegacyStorageRomReset (VOID);

/* Requires one matching physical PCI controller and one valid $PnP header
   whose disk handler and string references equal the supplied BBS entry.
   Priority/status policy belongs to the caller. No ROM checksum is required
   after initialization; the saved resident bytes must remain unchanged. */
BOOLEAN EFIAPI LegacyStorageRomMatch (CONST BBS_TABLE *Entry, BOOLEAN *NoManufacturer OPTIONAL);
EFI_STATUS EFIAPI LegacyStorageRomVerify (APP_LOGGER *Logger);
/* Called only after GPU dispatch; evidence of runtime footprint, not a license
   to overwrite or relocate an existing ROM during initialization. */
EFI_STATUS EFIAPI LegacyStorageRomReportVga (APP_LOGGER *Logger);
#endif

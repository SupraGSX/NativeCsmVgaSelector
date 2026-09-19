/** Firmware-resident AHCI disk corroboration; never executes a handler.
    SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_LEGACY_FIRMWARE_DISK_H_
#define NCV_LEGACY_FIRMWARE_DISK_H_
#include "PciEnumerate.h"
#include <Protocol/LegacyBios.h>

#define NCV_DISK_FIRMWARE_BASE  0xE0000U
#define NCV_DISK_FIRMWARE_BYTES 0x20000U
#define NCV_DISK_FIRMWARE_BLOCK 512U
#define NCV_DISK_FIRMWARE_BLOCKS (NCV_DISK_FIRMWARE_BYTES / NCV_DISK_FIRMWARE_BLOCK)

VOID EFIAPI LegacyFirmwareDiskReset (VOID);
EFI_STATUS EFIAPI LegacyFirmwareDiskCapture (CONST PCI_INVENTORY *Inventory);
/* Snapshot/Valid/Live describe E0000-FFFFF; owns copies of snapshot/inventory.
   A missing snapshot never changes the existing normal-status boot policy. */
EFI_STATUS EFIAPI LegacyFirmwareDiskPrepare (
  CONST UINT8 *Snapshot, CONST UINT8 *Valid, CONST UINT8 *Live,
  CONST PCI_INVENTORY *Inventory);
/* Corroboration only: caller enforces selected-disk uniqueness and the full
   BBS/string journal. Firmware status/order warnings follow user choice. */
BOOLEAN EFIAPI LegacyFirmwareDiskMatch (CONST BBS_TABLE *Entry);
#endif

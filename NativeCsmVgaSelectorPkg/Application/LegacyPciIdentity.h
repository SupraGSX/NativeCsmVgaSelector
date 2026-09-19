/** Owned identity used by legacy storage proofs. No protocol pointers retained.
    SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_LEGACY_PCI_IDENTITY_H_
#define NCV_LEGACY_PCI_IDENTITY_H_
#include "PciEnumerate.h"
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
typedef struct {
  EFI_STATUS LocationStatus, ConfigStatus;
  UINTN Segment, Bus, Device, Function;
  UINT8 Config[12];
} LEGACY_PCI_IDENTITY;

STATIC LEGACY_PCI_IDENTITY *CopyLegacyPciIdentities (CONST PCI_INVENTORY *Inventory)
{
  LEGACY_PCI_IDENTITY *Copy;
  UINTN Index;
  if (Inventory->Count == 0 || Inventory->Count > MAX_UINTN / sizeof (*Copy) ||
      Inventory->Devices == NULL) { return NULL; }
  Copy = AllocateZeroPool (Inventory->Count * sizeof (*Copy));
  if (Copy == NULL) { return NULL; }
  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD *Source = &Inventory->Devices[Index];
    Copy[Index].LocationStatus = Source->LocationStatus;
    Copy[Index].ConfigStatus = Source->ConfigStatus;
    Copy[Index].Segment = Source->Segment;
    Copy[Index].Bus = Source->Bus;
    Copy[Index].Device = Source->Device;
    Copy[Index].Function = Source->Function;
    CopyMem (Copy[Index].Config, Source->Config, sizeof (Copy[Index].Config));
  }
  return Copy;
}
#endif

/** SPDX-License-Identifier: GPL-3.0-only */
#include "LegacyDispatch.h"
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
STATIC EFI_PHYSICAL_ADDRESS Reserved;
STATIC BOOLEAN Owned;

EFI_STATUS EFIAPI __attribute__((noinline)) LegacyDispatchRelease (VOID)
{
  EFI_STATUS Status;
  if (!Owned) { return EFI_SUCCESS; }
  Status = gBS->FreePages (Reserved, 1);
  if (!EFI_ERROR (Status)) { Reserved = 0; Owned = FALSE; }
  return Status;
}

EFI_STATUS EFIAPI __attribute__((noinline)) LegacyDispatchReserve (VOID)
{
  EFI_PHYSICAL_ADDRESS Address = 0x9f000;
  EFI_STATUS Status;
  if (Owned) { return EFI_ALREADY_STARTED; }
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1, &Address);
  if (EFI_ERROR (Status)) { return Status; }
  Reserved = Address;
  Owned = TRUE;
  if (Address < 0x500 || Address >= 0xa0000) {
    Status = LegacyDispatchRelease ();
    return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

EFI_PHYSICAL_ADDRESS EFIAPI __attribute__((noinline)) LegacyDispatchAddress (VOID)
{ return Reserved; }

VOID EFIAPI __attribute__((noinline)) LegacyDispatchPrepare (
  CONST NATIVE_CSM_VGA_RUNTIME_PLAN *Plan, EFI_DISPATCH_OPROM_TABLE *Table)
{
  ZeroMem (Table, sizeof (*Table));
  Table->PnPInstallationCheckSegment = Plan->Compatibility16.PnpSegment;
  Table->PnPInstallationCheckOffset = Plan->Compatibility16.PnpOffset;
  Table->OpromSegment = 0xc000;
  Table->PciBus = Plan->Target.Bus;
  Table->PciDeviceFunction = (UINT8)((Plan->Target.Device << 3) | Plan->Target.Function);
  Table->NumberBbsEntries = (UINT8)Plan->BootTarget.BbsCount;
  Table->BbsTablePointer = (UINT32)(UINTN)Plan->BootTarget.FirmwareBbsTable;
}

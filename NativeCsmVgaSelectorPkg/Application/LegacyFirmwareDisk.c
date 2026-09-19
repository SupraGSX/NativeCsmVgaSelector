/** Validate BIOS-resident AHCI handlers without requiring a PCI ROM header.
    Some CSM disk entries leave all status bits unset. Those bits alone cannot
    describe media health. A separately checked firmware path can corroborate
    such a disk, while explicit failure/disable policy remains with the caller.
    SPDX-License-Identifier: GPL-3.0-only */
#include "LegacyFirmwareDisk.h"
#include "MemoryMap.h"
#include <Library/BaseMemoryLib.h>
#include "LegacyPciIdentity.h"
#include <Library/MemoryAllocationLib.h>

typedef struct {
  UINT8 Snapshot[NCV_DISK_FIRMWARE_BYTES];
  UINT8 Valid[NCV_DISK_FIRMWARE_BLOCKS];
  CONST UINT8 *Live;
  LEGACY_PCI_IDENTITY *Devices;
  UINTN Count;
} FIRMWARE_DISK_STATE;
STATIC FIRMWARE_DISK_STATE *mFirmware;

VOID EFIAPI __attribute__((noinline)) LegacyFirmwareDiskReset (VOID)
{
  if (mFirmware != NULL) {
    if (mFirmware->Devices != NULL) { FreePool (mFirmware->Devices); }
    FreePool (mFirmware);
    mFirmware = NULL;
  }
}

STATIC EFI_STATUS AllocateFirmwareState (CONST PCI_INVENTORY *Inventory, CONST UINT8 *Live)
{
  LegacyFirmwareDiskReset ();
  if (Live == NULL || Inventory == NULL ||
      Inventory->Count > MAX_UINTN / sizeof (PCI_DEVICE_RECORD) ||
      (Inventory->Count != 0 && Inventory->Devices == NULL)) { return EFI_INVALID_PARAMETER; }
  mFirmware = AllocateZeroPool (sizeof (*mFirmware));
  if (mFirmware == NULL) { return EFI_OUT_OF_RESOURCES; }
  if (Inventory->Count != 0) {
    mFirmware->Devices = CopyLegacyPciIdentities (Inventory);
    if (mFirmware->Devices == NULL) { LegacyFirmwareDiskReset (); return EFI_OUT_OF_RESOURCES; }
  }
  mFirmware->Count = Inventory->Count;
  mFirmware->Live = Live;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI __attribute__((noinline)) LegacyFirmwareDiskPrepare (
  CONST UINT8 *Snapshot, CONST UINT8 *Valid, CONST UINT8 *Live,
  CONST PCI_INVENTORY *Inventory)
{
  EFI_STATUS Status;
  if (Snapshot == NULL || Valid == NULL) {
    LegacyFirmwareDiskReset ();
    return EFI_INVALID_PARAMETER;
  }
  Status = AllocateFirmwareState (Inventory, Live);
  if (EFI_ERROR (Status)) { return Status; }
  CopyMem (mFirmware->Snapshot, Snapshot, NCV_DISK_FIRMWARE_BYTES);
  CopyMem (mFirmware->Valid, Valid, NCV_DISK_FIRMWARE_BLOCKS);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI __attribute__((noinline)) LegacyFirmwareDiskCapture (CONST PCI_INVENTORY *Inventory)
{
  MEMORY_MAP_SNAPSHOT Map;
  UINTN Block;
  EFI_STATUS Status = AllocateFirmwareState (Inventory, (CONST UINT8 *)(UINTN)NCV_DISK_FIRMWARE_BASE);
  if (EFI_ERROR (Status)) { return Status; }
  Status = MemoryMapCapture (&Map);
  if (EFI_ERROR (Status)) { LegacyFirmwareDiskReset (); return Status; }
  for (Block = 0; Block < NCV_DISK_FIRMWARE_BLOCKS; ++Block) {
    if (MemoryMapRangeIsReadable (&Map,
        NCV_DISK_FIRMWARE_BASE + Block * NCV_DISK_FIRMWARE_BLOCK,
        NCV_DISK_FIRMWARE_BLOCK, NULL)) {
      CopyMem (mFirmware->Snapshot + Block * NCV_DISK_FIRMWARE_BLOCK,
        (CONST VOID *)(UINTN)(NCV_DISK_FIRMWARE_BASE + Block * NCV_DISK_FIRMWARE_BLOCK),
        NCV_DISK_FIRMWARE_BLOCK);
      mFirmware->Valid[Block] = 1;
    }
  }
  MemoryMapRelease (&Map);
  return EFI_SUCCESS;
}

STATIC BOOLEAN StableRange (UINTN Address, UINTN Bytes)
{
  UINTN At, Block, Last;
  if (mFirmware == NULL || Bytes == 0 || Address < NCV_DISK_FIRMWARE_BASE) { return FALSE; }
  At = Address - NCV_DISK_FIRMWARE_BASE;
  if (At >= NCV_DISK_FIRMWARE_BYTES || Bytes > NCV_DISK_FIRMWARE_BYTES - At) { return FALSE; }
  Last = (At + Bytes - 1) / NCV_DISK_FIRMWARE_BLOCK;
  for (Block = At / NCV_DISK_FIRMWARE_BLOCK; Block <= Last; ++Block) {
    if (mFirmware->Valid[Block] != 1) { return FALSE; }
  }
  return (BOOLEAN)(CompareMem (mFirmware->Snapshot + At, mFirmware->Live + At, Bytes) == 0);
}

STATIC BOOLEAN FirmwareString (UINT16 Segment, UINT16 Offset, BOOLEAN Optional)
{
  UINTN Address, Index;
  UINT8 Value;
  if (Segment == 0 && Offset == 0) { return Optional; }
  Address = (UINTN)Segment * 16 + Offset;
  for (Index = 0; Index < 128; ++Index) {
    if (!StableRange (Address + Index, 1)) { return FALSE; }
    Value = mFirmware->Snapshot[Address + Index - NCV_DISK_FIRMWARE_BASE];
    if (Value == 0) { return (BOOLEAN)(Index != 0); }
    if (Value < 0x20 || Value > 0x7e) { return FALSE; }
  }
  return FALSE;
}

BOOLEAN EFIAPI __attribute__((noinline)) LegacyFirmwareDiskMatch (CONST BBS_TABLE *Entry)
{
  CONST LEGACY_PCI_IDENTITY *Device = NULL;
  UINTN Index, Matches = 0, Handler;
  BOOLEAN NonPadding = FALSE;
  if (mFirmware == NULL || Entry == NULL || Entry->DeviceType != BBS_HARDDISK ||
      Entry->Class != 1 || Entry->SubClass != 6 || Entry->Bus > 255 ||
      Entry->Device > 31 || Entry->Function > 7 || Entry->BootHandlerSegment == 0) { return FALSE; }
  for (Index = 0; Index < mFirmware->Count; ++Index) {
    CONST LEGACY_PCI_IDENTITY *D = &mFirmware->Devices[Index];
    if (!EFI_ERROR (D->LocationStatus) && D->Bus == Entry->Bus &&
        D->Device == Entry->Device && D->Function == Entry->Function) { ++Matches; Device = D; }
  }
  // Segmentless BBS cannot disambiguate colliding controller addresses.
  if (Matches != 1 || Device == NULL || Device->Segment != 0 || EFI_ERROR (Device->ConfigStatus) ||
      (Device->Config[0] == 0 && Device->Config[1] == 0) ||
      (Device->Config[0] == 0xff && Device->Config[1] == 0xff) ||
      (Device->Config[2] == 0xff && Device->Config[3] == 0xff) ||
      Device->Config[0x0b] != 1 || Device->Config[0x0a] != 6 || Device->Config[9] != 1) { return FALSE; }
  Handler = (UINTN)Entry->BootHandlerSegment * 16 + Entry->BootHandlerOffset;
  // A system BIOS handler is not a PCI option-ROM base. Do not require a
  // 512-byte boundary or PCIR/PnP header. This region cannot overlap the
  // existing guarded C/D-segment GPU destination. Never call this pointer.
  if (!StableRange (Handler, 64) ||
      !FirmwareString (Entry->DescStringSegment, Entry->DescStringOffset, FALSE) ||
      !FirmwareString (Entry->MfgStringSegment, Entry->MfgStringOffset, TRUE)) { return FALSE; }
  for (Index = 0; Index < 64; ++Index) {
    UINT8 Value = mFirmware->Snapshot[Handler - NCV_DISK_FIRMWARE_BASE + Index];
    if (Value != 0 && Value != 0xff) { NonPadding = TRUE; }
  }
  return NonPadding;
}

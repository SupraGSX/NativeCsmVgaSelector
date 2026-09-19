/** Corroborate legacy disk entries whose option ROM leaves StatusFlags zero.
    SPDX-License-Identifier: GPL-3.0-only */
#include "LegacyStorageRom.h"
#include <Library/BaseMemoryLib.h>
#include "LegacyPciIdentity.h"
#include <Library/MemoryAllocationLib.h>

#define SHADOW_BASE 0xc0000U
#define SHADOW_BYTES 0x20000U
#define BLOCK_BYTES 512U
#define BLOCKS (SHADOW_BYTES / BLOCK_BYTES)
#define MAX_PNP_HEADERS 32U

typedef struct {
  UINT8 Snapshot[SHADOW_BYTES];
  UINT8 Valid[BLOCKS];
  CONST UINT8 *Live;
  LEGACY_PCI_IDENTITY *Devices;
  UINTN Count;
} STORAGE_ROM_STATE;
STATIC STORAGE_ROM_STATE *mState;

STATIC UINT16 Le16 (CONST UINT8 *P)
{ return (UINT16)(P[0] | ((UINT16)P[1] << 8)); }

STATIC BOOLEAN Readable (UINTN Offset, UINTN Bytes)
{
  UINTN Block, Last;
  if (mState == NULL || Bytes == 0 || Offset >= SHADOW_BYTES || Bytes > SHADOW_BYTES - Offset) { return FALSE; }
  Last = (Offset + Bytes - 1) / BLOCK_BYTES;
  for (Block = Offset / BLOCK_BYTES; Block <= Last; ++Block) {
    if (mState->Valid[Block] != 1) { return FALSE; }
  }
  return TRUE;
}

STATIC BOOLEAN Rom (UINTN At, UINTN *Bytes, UINTN *Pcir)
{
  CONST UINT8 *R;
  UINTN Length;
  if (!Readable (At, 0x1c)) { return FALSE; }
  R = mState->Snapshot + At;
  if (R[0] != 0x55 || R[1] != 0xaa) { return FALSE; }
  *Bytes = (UINTN)R[2] * BLOCK_BYTES;
  if (!Readable (At, *Bytes)) { return FALSE; }
  *Pcir = Le16 (R + 0x18);
  if (*Pcir < 0x1c || *Pcir > *Bytes || 0x18 > *Bytes - *Pcir) { return FALSE; }
  Length = Le16 (R + *Pcir + 0x0a);
  return (BOOLEAN)(CompareMem (R + *Pcir, "PCIR", 4) == 0 && Length >= 0x18 &&
    Length <= *Bytes - *Pcir && R[*Pcir + 0x14] == 0 &&
    Le16 (R + *Pcir + 4) != 0 && Le16 (R + *Pcir + 4) != 0xffff);
}

STATIC BOOLEAN String (CONST UINT8 *R, UINTN Bytes, UINT16 Offset, BOOLEAN Optional)
{
  UINTN Index;
  if (Offset == 0) { return Optional; }
  if (Offset >= Bytes) { return FALSE; }
  for (Index = Offset; Index < Bytes && Index - Offset < 128; ++Index) {
    if (R[Index] == 0) { return (BOOLEAN)(Index > Offset); }
    if (R[Index] < 0x20 || R[Index] > 0x7e) { return FALSE; }
  }
  return FALSE;
}

VOID EFIAPI __attribute__((noinline)) LegacyStorageRomReset (VOID)
{
  if (mState != NULL) {
    if (mState->Devices != NULL) { FreePool (mState->Devices); }
    FreePool (mState);
    mState = NULL;
  }
}

EFI_STATUS EFIAPI __attribute__((noinline)) LegacyStorageRomPrepare (
  CONST UINT8 *Snapshot, CONST UINT8 *ValidBlocks, CONST UINT8 *LiveView,
  CONST PCI_INVENTORY *Inventory)
{
  LegacyStorageRomReset ();
  if (Snapshot == NULL || ValidBlocks == NULL || LiveView == NULL || Inventory == NULL ||
      Inventory->Count > MAX_UINTN / sizeof (PCI_DEVICE_RECORD) || (Inventory->Count != 0 && Inventory->Devices == NULL)) { return EFI_INVALID_PARAMETER; }
  mState = AllocateZeroPool (sizeof (*mState));
  if (mState == NULL) { return EFI_OUT_OF_RESOURCES; }
  if (Inventory->Count != 0) {
    mState->Devices = CopyLegacyPciIdentities (Inventory);
    if (mState->Devices == NULL) { LegacyStorageRomReset (); return EFI_OUT_OF_RESOURCES; }
  }
  mState->Count = Inventory->Count;
  mState->Live = LiveView;
  CopyMem (mState->Snapshot, Snapshot, SHADOW_BYTES);
  CopyMem (mState->Valid, ValidBlocks, BLOCKS);
  return EFI_SUCCESS;
}

BOOLEAN EFIAPI __attribute__((noinline)) LegacyStorageRomMatch (CONST BBS_TABLE *Entry, BOOLEAN *NoManufacturer)
{
  UINTN At, Bytes, Pcir, Index, Controllers, Pnp, Length, Seen[MAX_PNP_HEADERS], Count, Matches;
  CONST UINT8 *R, *P;
  CONST LEGACY_PCI_IDENTITY *Device;
  UINT8 Sum;
  BOOLEAN MissingMfg;
  if (NoManufacturer != NULL) { *NoManufacturer = FALSE; }
  if (mState == NULL || Entry == NULL || Entry->DeviceType != BBS_HARDDISK || Entry->Class != 1 ||
      Entry->Bus > 255 || Entry->Device > 31 || Entry->Function > 7) { return FALSE; }
  At = (UINTN)Entry->BootHandlerSegment * 16;
  if (At < SHADOW_BASE || At >= SHADOW_BASE + SHADOW_BYTES || At % BLOCK_BYTES != 0) { return FALSE; }
  At -= SHADOW_BASE;
  if (!Rom (At, &Bytes, &Pcir)) { return FALSE; }
  R = mState->Snapshot + At;
  if (CompareMem (R, mState->Live + At, Bytes) != 0) { return FALSE; }
  if (Entry->DescStringSegment != Entry->BootHandlerSegment ||
      (Entry->MfgStringSegment != Entry->BootHandlerSegment &&
       !(Entry->MfgStringSegment == 0 && Entry->MfgStringOffset == 0)) ||
      Entry->BootHandlerOffset == 0 || Entry->BootHandlerOffset >= Bytes) { return FALSE; }
  /* Some initialized storage ROMs advertise a stale/non-storage PCIR class.
     Use PCIR for vendor/device identity, and require agreement between the
     actual PCI controller, PnP header and BBS entry for class/subclass. */
  Controllers = 0;
  Device = NULL;
  for (Index = 0; Index < mState->Count; ++Index) {
    CONST LEGACY_PCI_IDENTITY *D = &mState->Devices[Index];
    if (!EFI_ERROR (D->LocationStatus) && D->Bus == Entry->Bus && D->Device == Entry->Device && D->Function == Entry->Function) {
      ++Controllers; Device = D;
    }
  }
  /* BBS lacks a PCI segment: never infer ownership across colliding BDFs. */
  if (Controllers != 1 || Device == NULL || Device->Segment != 0 || EFI_ERROR (Device->ConfigStatus) ||
      Le16 (Device->Config) != Le16 (R + Pcir + 4) || Le16 (Device->Config + 2) != Le16 (R + Pcir + 6) ||
      Device->Config[0x0b] != Entry->Class || Device->Config[0x0a] != Entry->SubClass) { return FALSE; }
  Pnp = Le16 (R + 0x1a);
  Count = Matches = 0; MissingMfg = FALSE;
  while (Pnp != 0) {
    if (Count == MAX_PNP_HEADERS || Pnp < 0x1c || Pnp > Bytes || 32 > Bytes - Pnp) { return FALSE; }
    for (Index = 0; Index < Count; ++Index) { if (Seen[Index] == Pnp) { return FALSE; } }
    Seen[Count++] = Pnp;
    P = R + Pnp; Length = (UINTN)P[5] * 16;
    if (CompareMem (P, "$PnP", 4) != 0 || P[4] != 1 || Length < 32 || Length > Bytes - Pnp) { return FALSE; }
    Sum = 0;
    for (Index = 0; Index < Length; ++Index) { Sum = (UINT8)(Sum + P[Index]); }
    if (Sum != 0) { return FALSE; }
    if (Entry->Class == P[18] && Entry->SubClass == P[19] &&
        Entry->DescStringOffset == Le16 (P + 16) && Entry->MfgStringOffset == Le16 (P + 14) &&
        (Entry->BootHandlerOffset == Le16 (P + 22) || Entry->BootHandlerOffset == Le16 (P + 26)) &&
        String (R, Bytes, Le16 (P + 16), FALSE) && String (R, Bytes, Le16 (P + 14), TRUE)) {
      ++Matches; MissingMfg = (BOOLEAN)(Le16 (P + 14) == 0);
    }
    Pnp = Le16 (P + 6);
  }
  if (Matches != 1) { return FALSE; }
  if (NoManufacturer != NULL) { *NoManufacturer = MissingMfg; }
  return TRUE;
}

EFI_STATUS EFIAPI __attribute__((noinline)) LegacyStorageRomVerify (APP_LOGGER *Logger)
{
  UINTN At, Bytes, Pcir, Index;
  BOOLEAN Storage;
  if (mState == NULL) { return EFI_SUCCESS; }
  for (At = 0; At < SHADOW_BYTES; At += BLOCK_BYTES) {
    if (!Rom (At, &Bytes, &Pcir)) { continue; }
    Storage = FALSE;
    for (Index = 0; Index < mState->Count; ++Index) {
      CONST LEGACY_PCI_IDENTITY *D = &mState->Devices[Index];
      if (!EFI_ERROR (D->ConfigStatus) && D->Config[0x0b] == 1 &&
          Le16 (D->Config) == Le16 (mState->Snapshot + At + Pcir + 4) &&
          Le16 (D->Config + 2) == Le16 (mState->Snapshot + At + Pcir + 6)) { Storage = TRUE; }
    }
    if (!Storage) { continue; }
    if (CompareMem (mState->Snapshot + At, mState->Live + At, Bytes) != 0) {
      LogPrint (Logger, L"NCV_BOOT_FAIL_STORAGE_ROM_CHANGED\r\n"
        L"Resident storage ROM at 0x%lx changed since preflight; no legacy boot will be attempted.\r\n", SHADOW_BASE + At);
      return EFI_CRC_ERROR;
    }
    At += Bytes - BLOCK_BYTES;
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI __attribute__((noinline)) LegacyStorageRomReportVga (APP_LOGGER *Logger)
{
  UINTN Bytes;
  if (mState == NULL) { return EFI_SUCCESS; }
  if (!Readable (0, 3)) { return EFI_NOT_READY; }
  Bytes = (UINTN)mState->Live[2] * BLOCK_BYTES;
  if (mState->Live[0] != 0x55 || mState->Live[1] != 0xaa || Bytes == 0) {
    return LogPrint (Logger, L"NCV_POST_DISPATCH_VGA_FOOTPRINT_UNAVAILABLE initialized ROM has no usable runtime-size header\r\n");
  }
  return LogPrint (Logger, L"NCV_POST_DISPATCH_VGA_FOOTPRINT runtime-header-bytes=0x%lx end=0x%lx\r\n"
    L"This is the ROM's post-initialization size declaration, not proof that its initialization tail can be omitted or another ROM relocated.\r\n",
    Bytes, SHADOW_BASE + Bytes);
}

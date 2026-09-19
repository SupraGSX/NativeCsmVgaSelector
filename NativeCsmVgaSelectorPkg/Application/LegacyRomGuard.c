/** Detect occupied option-ROM space before the irreversible VGA handoff.
    SPDX-License-Identifier: GPL-3.0-only */
#include "LegacyRomGuard.h"
#include "MemoryMap.h"
#include "LegacyStorageRom.h"
#include "LegacyFirmwareDisk.h"
#include "RomRecovery.h"
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

/* Borrowed only while the validated Boot plan is alive. Never retained after
   target-context release, and never consulted after the GPU dispatch. */
STATIC CONST NATIVE_CSM_VGA_RUNTIME_PLAN *mPreflightPlan;

VOID EFIAPI __attribute__((noinline)) LegacyRomGuardReset (VOID)
{
  mPreflightPlan = NULL;
  LegacyStorageRomReset ();
  LegacyFirmwareDiskReset ();
}

EFI_STATUS EFIAPI __attribute__((noinline)) LegacyRomGuardRecheckAfterBbs (APP_LOGGER *Logger)
{
  if (mPreflightPlan == NULL) { return EFI_SUCCESS; } // Standalone BBS clients.
  LogPrint (Logger, L"NCV_ROM_LAYOUT_RECHECK_AFTER_BBS\r\n");
  return LegacyRomGuardCheck (Logger, mPreflightPlan);
}

STATIC UINT16 Le16 (CONST UINT8 *P)
{
  return (UINT16)(P[0] | ((UINT16)P[1] << 8));
}

STATIC BOOLEAN Readable (CONST UINT8 *Valid, UINTN Offset, UINTN Bytes)
{
  UINTN Block, Last;
  if (Valid == NULL || Bytes == 0 || Offset >= NCV_ROM_SHADOW_SIZE ||
      Bytes > NCV_ROM_SHADOW_SIZE - Offset) { return FALSE; }
  Last = (Offset + Bytes - 1) / NCV_ROM_BLOCK_SIZE;
  for (Block = Offset / NCV_ROM_BLOCK_SIZE; Block <= Last; ++Block) {
    if (Valid[Block] != 1) { return FALSE; }
  }
  return TRUE;
}

STATIC BOOLEAN Extent (CONST UINT8 *Shadow, CONST UINT8 *Valid, UINTN Offset, UINTN *Bytes)
{
  if (!Readable (Valid, Offset, 3) || Shadow[Offset] != 0x55 || Shadow[Offset + 1] != 0xaa) {
    return FALSE;
  }
  *Bytes = (UINTN)Shadow[Offset + 2] * NCV_ROM_BLOCK_SIZE;
  return (BOOLEAN)(*Bytes != 0 && *Bytes <= NCV_ROM_SHADOW_SIZE - Offset);
}

STATIC BOOLEAN Identity (
  CONST UINT8 *Shadow, CONST UINT8 *Valid, UINTN Offset, UINTN Bytes,
  UINT16 *Vendor, UINT16 *Device
  )
{
  UINTN Pcir, Length;
  if (Bytes < 0x1c || !Readable (Valid, Offset, 0x1c)) { return FALSE; }
  Pcir = Le16 (Shadow + Offset + 0x18);
  if (Pcir < 0x1c || Pcir > Bytes || 0x18 > Bytes - Pcir ||
      !Readable (Valid, Offset + Pcir, 0x18)) { return FALSE; }
  if (CompareMem (Shadow + Offset + Pcir, "PCIR", 4) != 0) { return FALSE; }
  Length = Le16 (Shadow + Offset + Pcir + 0x0a);
  if (Length < 0x18 || Length > Bytes - Pcir ||
      !Readable (Valid, Offset + Pcir, Length) ||
      Shadow[Offset + Pcir + 0x14] != 0) { return FALSE; }
  *Vendor = Le16 (Shadow + Offset + Pcir + 4);
  *Device = Le16 (Shadow + Offset + Pcir + 6);
  return (BOOLEAN)(*Vendor != 0 && *Vendor != 0xffff);
}

LEGACY_ROM_GUARD_REASON
LegacyRomGuardInspect (
  CONST UINT8 *Shadow, CONST UINT8 *ValidBlocks, UINTN CopyBytes,
  UINT16 ActiveVendor, UINT16 ActiveDevice, UINTN DiskHandler,
  LEGACY_ROM_GUARD_RESULT *Result
  )
{
  UINTN Offset, Bytes;
  UINT16 Vendor, Device;
  BOOLEAN Identified;
  if (Result == NULL) { return RomGuardUnknownLayout; }
  ZeroMem (Result, sizeof (*Result));
  Result->Reason = RomGuardUnknownLayout;
  Result->Address = NCV_ROM_SHADOW_BASE;
  if (Shadow == NULL || ValidBlocks == NULL || CopyBytes == 0 ||
      CopyBytes > NCV_ROM_SHADOW_SIZE || CopyBytes % NCV_ROM_BLOCK_SIZE != 0 ||
      ActiveVendor == 0 || ActiveVendor == 0xffff) { return Result->Reason; }
  if (!Readable (ValidBlocks, 0, CopyBytes)) {
    Result->Reason = RomGuardUnreadable;
    return Result->Reason;
  }
  if (!Extent (Shadow, ValidBlocks, 0, &Result->ActiveBytes) ||
      !Identity (Shadow, ValidBlocks, 0, Result->ActiveBytes, &Vendor, &Device) ||
      Vendor != ActiveVendor || Device != ActiveDevice) { return Result->Reason; }

  /* The video ROM can contain embedded signatures. Within its declared runtime
     extent require corroborating PCI identity belonging to another device.
     Beyond it, any plausible ROM header is occupied space, including ISA ROMs.
     Initialized ROM checksums and original PCIR image sizes need not describe
     the modified resident image, so neither licenses overwriting that region. */
  for (Offset = NCV_ROM_BLOCK_SIZE; Offset < CopyBytes; Offset += NCV_ROM_BLOCK_SIZE) {
    if (Shadow[Offset] != 0x55 || Shadow[Offset + 1] != 0xaa) { continue; }
    if (!Extent (Shadow, ValidBlocks, Offset, &Bytes)) {
      if (Offset < Result->ActiveBytes) { continue; }
      Result->Address = NCV_ROM_SHADOW_BASE + Offset;
      return Result->Reason;
    }
    Vendor = Device = 0;
    Identified = Identity (Shadow, ValidBlocks, Offset, Bytes, &Vendor, &Device);
    if (Offset < Result->ActiveBytes &&
        (!Identified || (Vendor == ActiveVendor && Device == ActiveDevice))) { continue; }
    Result->Reason = RomGuardOverlap;
    Result->Address = NCV_ROM_SHADOW_BASE + Offset;
    Result->Bytes = Bytes;
    Result->Vendor = Vendor;
    Result->Device = Device;
    return Result->Reason;
  }
  /* A disk interrupt directly inside the planned copy is another independent
     conflict even when a resident-ROM header is absent. Indirect code/data
     references outside recognized ROMs cannot all be established by this scan. */
  if (DiskHandler >= NCV_ROM_SHADOW_BASE && DiskHandler < NCV_ROM_SHADOW_BASE + CopyBytes) {
    Result->Reason = RomGuardDiskHandlerOverlap;
    Result->Address = DiskHandler;
    Result->Bytes = 1;
    return Result->Reason;
  }
  Result->Reason = RomGuardClear;
  Result->Address = Result->Bytes = 0;
  return Result->Reason;
}

/* Keep snapshot buffers and helper locals outside the frozen Boot frame. */
EFI_STATUS EFIAPI __attribute__((noinline))
LegacyRomGuardCheck (APP_LOGGER *Logger, CONST NATIVE_CSM_VGA_RUNTIME_PLAN *Plan)
{
  MEMORY_MAP_SNAPSHOT Map;
  LEGACY_ROM_GUARD_RESULT Result;
  UINT8 Valid[NCV_ROM_BLOCK_COUNT], Vector[4];
  UINT8 *Shadow;
  UINTN Block, DiskHandler;
  EFI_STATUS Status;
  LEGACY_ROM_GUARD_REASON Reason;
  if (Logger == NULL || Plan == NULL) { return EFI_INVALID_PARAMETER; }
  RomRecoveryReset ();
  LegacyRomGuardReset ();
  Shadow = AllocateZeroPool (NCV_ROM_SHADOW_SIZE);
  if (Shadow == NULL) {
    LogPrint (Logger, L"NCV_BLOCKED_ROM_LAYOUT\r\nUnable to allocate a read-only ROM snapshot.\r\n");
    return EFI_OUT_OF_RESOURCES;
  }
  ZeroMem (Valid, sizeof (Valid));
  ZeroMem (&Map, sizeof (Map));
  Status = MemoryMapCapture (&Map);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_ROM_LAYOUT\r\nCannot validate readable ROM memory: %r\r\n", Status);
    FreePool (Shadow);
    return Status;
  }
  for (Block = 0; Block < NCV_ROM_BLOCK_COUNT; ++Block) {
    if (MemoryMapRangeIsReadable (&Map,
        NCV_ROM_SHADOW_BASE + Block * NCV_ROM_BLOCK_SIZE, NCV_ROM_BLOCK_SIZE, NULL)) {
      CopyMem (Shadow + Block * NCV_ROM_BLOCK_SIZE,
        (CONST VOID *)(UINTN)(NCV_ROM_SHADOW_BASE + Block * NCV_ROM_BLOCK_SIZE), NCV_ROM_BLOCK_SIZE);
      Valid[Block] = 1;
    }
  }
  if (!MemoryMapRangeIsReadable (&Map, 0x13U * 4U, sizeof (Vector), NULL)) {
    MemoryMapRelease (&Map);
    FreePool (Shadow);
    LogPrint (Logger, L"NCV_BLOCKED_ROM_LAYOUT\r\nCannot read the legacy disk interrupt vector.\r\n");
    return EFI_UNSUPPORTED;
  }
  CopyMem (Vector, (CONST VOID *)(UINTN)(0x13U * 4U), sizeof (Vector));
  MemoryMapRelease (&Map);
  DiskHandler = (UINTN)Le16 (Vector + 2) * 16 + Le16 (Vector);
  Reason = LegacyRomGuardInspect (Shadow, Valid, Plan->SelectedRomSize,
    Plan->Active.Vendor, Plan->Active.DeviceId, DiskHandler, &Result);
  Status = EFI_SUCCESS;
  if (Reason == RomGuardClear) {
    Status = LegacyStorageRomPrepare (Shadow, Valid, (CONST UINT8 *)(UINTN)NCV_ROM_SHADOW_BASE, &Plan->Inventory);
    if (!EFI_ERROR (Status)) {
      // Failure to snapshot optional firmware provenance must not block an
      // otherwise supported normal-status disk. Zero-status AHCI remains
      // ineligible without a complete corroborating snapshot.
      if (EFI_ERROR (LegacyFirmwareDiskCapture (&Plan->Inventory))) {
        LogPrint (Logger, L"NCV_BBS_FIRMWARE_SNAPSHOT_UNAVAILABLE zero-status firmware disk support unavailable\r\n");
      }
    }
  }
  FreePool (Shadow);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_ROM_LAYOUT\r\nCannot retain the storage-ROM provenance snapshot: %r\r\n", Status);
    return Status;
  }
  LogPrint (Logger,
    L"Legacy ROM preflight: active-runtime=0x%lx planned-copy=[0x%x,0x%lx) INT13=0x%lx\r\n",
    Result.ActiveBytes, NCV_ROM_SHADOW_BASE, NCV_ROM_SHADOW_BASE + Plan->SelectedRomSize, DiskHandler);
  if (Reason == RomGuardClear) {
    mPreflightPlan = Plan;
    LogPrint (Logger, L"NCV_ROM_LAYOUT_CHECKED no recognized option-ROM overlap\r\n");
    return EFI_SUCCESS;
  }
  LogPrint (Logger, L"ROM conflict/detail: reason=%u address=0x%lx bytes=0x%lx PCI=%04x:%04x\r\n",
    (UINT32)Reason, Result.Address, Result.Bytes, Result.Vendor, Result.Device);
  if (Reason == RomGuardOverlap || Reason == RomGuardDiskHandlerOverlap) {
    RomRecoveryRecord (Plan, &Result);
    LogPrint (Logger, L"NCV_BLOCKED_ROM_OVERLAP\r\n"
      L"The selected GPU ROM would overwrite another legacy ROM or disk handler.\r\n"
      L"No GPU switch or ROM write was attempted.\r\n"
      L"Review legacy storage and PXE/network OpROM settings; keep CSM enabled.\r\n"
      L"Use UEFI-only for the conflicting ROM, or disable unused network boot.\r\n"
      L"Then reselect the boot disk in setup because its firmware name may change.\r\n");
    return EFI_UNSUPPORTED;
  }
  LogPrint (Logger, L"NCV_BLOCKED_ROM_LAYOUT\r\n"
    L"Cannot establish the existing legacy ROM layout. No GPU switch was attempted.\r\n");
  return EFI_COMPROMISED_DATA;
}

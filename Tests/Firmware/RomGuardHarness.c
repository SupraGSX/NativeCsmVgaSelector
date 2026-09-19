/* Exercise production BBS predicates directly, without exporting test hooks. */
#define LegacyBootBootTargetDiscover FixtureLegacyBootBootTargetDiscover
#define LegacyBootBootTargetValidateOwnedCopies FixtureLegacyBootBootTargetValidateOwnedCopies
#define LegacyBootBootTargetJournalAndValidate FixtureLegacyBootBootTargetJournalAndValidate
#define LegacyBootBootTargetValidateOriginal FixtureLegacyBootBootTargetValidateOriginal
#define LegacyBootBootTargetValidateApplied FixtureLegacyBootBootTargetValidateApplied
#define LegacyBootBootTargetGetLegacyBootArguments FixtureLegacyBootBootTargetGetLegacyBootArguments
#define LegacyBootBootTargetGetOwnedCopyInfo FixtureLegacyBootBootTargetGetOwnedCopyInfo
#define LegacyBootBootPriorityApply FixtureLegacyBootBootPriorityApply
#define LegacyBootBootPriorityRollback FixtureLegacyBootBootPriorityRollback
#define LegacyBootBootTargetRelease FixtureLegacyBootBootTargetRelease
#define LegacyBootBootTargetPrepare FixtureLegacyBootBootTargetPrepare
#include "LegacyBootTarget.c"
#undef LegacyBootBootTargetPrepare
#undef LegacyBootBootTargetDiscover
#undef LegacyBootBootTargetValidateOwnedCopies
#undef LegacyBootBootTargetJournalAndValidate
#undef LegacyBootBootTargetValidateOriginal
#undef LegacyBootBootTargetValidateApplied
#undef LegacyBootBootTargetGetLegacyBootArguments
#undef LegacyBootBootTargetGetOwnedCopyInfo
#undef LegacyBootBootPriorityApply
#undef LegacyBootBootPriorityRollback
#undef LegacyBootBootTargetRelease

/* Offline ROM-layout fixtures in an isolated OVMF guest; no CSM/GPU dispatch.
   SPDX-License-Identifier: GPL-3.0-only */
#include <Uefi.h>
#include "LegacyRomGuard.h"
#include "BootReport.h"
#include "RomRecovery.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "LegacyFirmwareDisk.h"

STATIC APP_LOGGER Logger;
STATIC UINTN Checks, FailedLine;
STATIC UINT8 Shadow[NCV_ROM_SHADOW_SIZE], Original[NCV_ROM_SHADOW_SIZE];
STATIC UINT8 Valid[NCV_ROM_BLOCK_COUNT];
UINT8 *RomGuardTestPhysicalShadow = Shadow;
UINT8 RomGuardTestVector[4] = {0, 0, 0, 0xf0};
STATIC LEGACY_ROM_GUARD_RESULT Detail;
#define REQUIRE(E) do { ++Checks; if (!(E)) { FailedLine = __LINE__; \
  LogPrint (&Logger, L"FAIL %u: %a\r\n", (UINT32)__LINE__, #E); return EFI_ABORTED; } } while (0)

STATIC VOID W16 (UINTN At, UINT16 Value)
{ Shadow[At] = (UINT8)Value; Shadow[At + 1] = (UINT8)(Value >> 8); }

STATIC VOID Rom (UINTN At, UINTN Size, UINT16 Vendor, UINT16 Device)
{
  ZeroMem (Shadow + At, 0x40);
  Shadow[At] = 0x55; Shadow[At + 1] = 0xaa; Shadow[At + 2] = (UINT8)(Size / 512);
  W16 (At + 0x18, 0x1c);
  CopyMem (Shadow + At + 0x1c, "PCIR", 4);
  W16 (At + 0x20, Vendor); W16 (At + 0x22, Device); W16 (At + 0x26, 0x18);
}

STATIC VOID Reset (VOID)
{
  SetMem (Shadow, sizeof (Shadow), 0xff);
  SetMem (Valid, sizeof (Valid), 1);
  Rom (0, 0xe800, 0x1234, 0x5678);
}

STATIC LEGACY_ROM_GUARD_REASON Inspect (UINTN Bytes, UINTN Vector)
{ return LegacyRomGuardInspect (Shadow, Valid, Bytes, 0x1234, 0x5678, Vector, &Detail); }

STATIC EFI_STATUS Synthetic (VOID)
{
  Reset ();
  REQUIRE (Inspect (0xec00, 0xf0010) == RomGuardClear);
  REQUIRE (Detail.ActiveBytes == 0xe800);
  REQUIRE (Inspect (0xe800, 0xf0010) == RomGuardClear);
  REQUIRE (Inspect (0, 0) == RomGuardUnknownLayout);
  REQUIRE (Inspect (MAX_UINTN, 0) == RomGuardUnknownLayout);
  REQUIRE (Inspect (0x20001, 0) == RomGuardUnknownLayout);
  REQUIRE (Inspect (0xec01, 0) == RomGuardUnknownLayout);
  REQUIRE (LegacyRomGuardInspect (NULL, Valid, 0xec00, 1, 2, 0, &Detail) == RomGuardUnknownLayout);
  REQUIRE (LegacyRomGuardInspect (Shadow, NULL, 0xec00, 1, 2, 0, &Detail) == RomGuardUnknownLayout);
  REQUIRE (LegacyRomGuardInspect (Shadow, Valid, 0xec00, 1, 2, 0, NULL) == RomGuardUnknownLayout);
  REQUIRE (LegacyRomGuardInspect (Shadow, Valid, 0xec00, 0xffff, 2, 0, &Detail) == RomGuardUnknownLayout);
  REQUIRE (LegacyRomGuardInspect (Shadow, Valid, 0xec00, 0x9999, 2, 0, &Detail) == RomGuardUnknownLayout);
  Shadow[0] = 0;
  REQUIRE (Inspect (0xec00, 0) == RomGuardUnknownLayout);
  Reset (); Shadow[2] = 0;
  REQUIRE (Inspect (0xec00, 0) == RomGuardUnknownLayout);
  Reset (); W16 (0x18, 0xffff);
  REQUIRE (Inspect (0xec00, 0) == RomGuardUnknownLayout);
  Reset (); W16 (0x26, 0xffff);
  REQUIRE (Inspect (0xec00, 0) == RomGuardUnknownLayout);
  Reset (); Shadow[0x30] = 3;
  REQUIRE (Inspect (0xec00, 0) == RomGuardUnknownLayout);
  Reset (); Valid[0] = 0;
  REQUIRE (Inspect (0xec00, 0) == RomGuardUnreadable);
  Reset (); Valid[0xea00 / 512] = 0;
  REQUIRE (Inspect (0xec00, 0) == RomGuardUnreadable);
  Reset (); Valid[0xe800 / 512] = 2;
  REQUIRE (Inspect (0xec00, 0) == RomGuardUnreadable);
  Reset (); Valid[0xf000 / 512] = 0;
  REQUIRE (Inspect (0xec00, 0) == RomGuardClear);
  // Synthetic network ROM: the guard protects non-storage residents too.
  Reset (); Rom (0xe800, 0x2000, 0x1d6a, 0x0001);
  CopyMem (Original, Shadow, sizeof (Shadow));
  REQUIRE (Inspect (0xec00, 0xfec59) == RomGuardOverlap);
  REQUIRE (Detail.Address == 0xce800 && Detail.Bytes == 0x2000);
  REQUIRE (Detail.Vendor == 0x1d6a && Detail.Device == 1);
  REQUIRE (CompareMem (Shadow, Original, sizeof (Shadow)) == 0);
  Reset (); Rom (0xe800, 0x3e00, 0xabcd, 0x1001);
  CopyMem (Original, Shadow, sizeof (Shadow));
  REQUIRE (Inspect (0xec00, 0xd0010) == RomGuardOverlap);
  REQUIRE (Detail.Address == 0xce800 && Detail.Bytes == 0x3e00);
  REQUIRE (Detail.Vendor == 0xabcd && Detail.Device == 0x1001);
  REQUIRE (CompareMem (Original, Shadow, sizeof (Shadow)) == 0);
  REQUIRE (Inspect (0xe800, 0xd0010) == RomGuardClear); // Exact adjacent boundary.
  Reset (); Rom (0xec00, 0x200, 0xabcd, 0x1001);
  REQUIRE (Inspect (0xec00, 0xf0010) == RomGuardClear);
  Reset (); Rom (0xe800, 0x3e00, 0xabcd, 0x1001); Shadow[0xe81c] = 0;
  REQUIRE (Inspect (0xec00, 0xf0010) == RomGuardOverlap); // ISA/missing PCI identity.
  REQUIRE (Detail.Vendor == 0 && Detail.Device == 0);
  Reset (); Rom (0xe800, 0, 0xabcd, 0x1001);
  REQUIRE (Inspect (0xec00, 0xf0010) == RomGuardUnknownLayout);
  Reset (); Rom (0xe800, 0x1fe00, 0xabcd, 0x1001);
  REQUIRE (Inspect (0xec00, 0xf0010) == RomGuardUnknownLayout);
  Reset (); Rom (0x400, 0x400, 0xabcd, 0x1001);
  REQUIRE (Inspect (0xec00, 0xf0010) == RomGuardOverlap); // Foreign identity inside active extent.
  Reset (); Rom (0x400, 0x400, 0x1234, 0x5678);
  REQUIRE (Inspect (0xec00, 0xf0010) == RomGuardClear); // Embedded same-device image.
  Reset (); Shadow[0x400] = 0x55; Shadow[0x401] = 0xaa;
  REQUIRE (Inspect (0xec00, 0xf0010) == RomGuardClear); // Uncorroborated embedded data.
  Reset ();
  REQUIRE (Inspect (0xec00, 0xcec00) == RomGuardClear);
  REQUIRE (Inspect (0xec00, 0xcebff) == RomGuardDiskHandlerOverlap);
  REQUIRE (Inspect (0xec00, 0xc0000) == RomGuardDiskHandlerOverlap);
  REQUIRE (Detail.Address == 0xc0000);
  {
    STATIC NATIVE_CSM_VGA_RUNTIME_PLAN Plan;
    ZeroMem (&Plan, sizeof (Plan));
    RomRecoveryReset ();
    REQUIRE (!RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
    RomRecoveryRecord (&Plan, &Detail);
    REQUIRE (RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
    REQUIRE (!RomRecoveryAvailable (EFI_CRC_ERROR, EFI_SUCCESS));
    REQUIRE (!RomRecoveryAvailable (EFI_SUCCESS, EFI_SUCCESS));
    REQUIRE (!RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_ACCESS_DENIED));
    LegacyRomGuardReset ();
    REQUIRE (RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
    Plan.RouteDirty = TRUE; RomRecoveryRecord (&Plan, &Detail);
    REQUIRE (!RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
    Plan.RouteDirty = FALSE; Plan.EndpointWriteAttempted = TRUE; RomRecoveryRecord (&Plan, &Detail);
    REQUIRE (!RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
    Plan.EndpointWriteAttempted = FALSE; Plan.EndpointWriteApplied = TRUE; RomRecoveryRecord (&Plan, &Detail);
    REQUIRE (!RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
    Plan.EndpointWriteApplied = FALSE; Detail.Reason = RomGuardUnreadable; RomRecoveryRecord (&Plan, &Detail);
    REQUIRE (!RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
    RomRecoveryRecord (NULL, &Detail);
    REQUIRE (!RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
  }
  LogPrint (&Logger, L"PASS synthetic ROM extents, identities, boundaries and read-only behavior\r\n");
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ReadExact (APP_FILE_CONTEXT *Files, CONST CHAR16 *Name, VOID *Buffer, UINTN Bytes)
{
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  UINTN Size = Bytes;
  Status = AppFileOpenAdjacent (Files, Name, EFI_FILE_MODE_READ, 0, &File);
  if (EFI_ERROR (Status)) { return Status; }
  Status = File->Read (File, &Size, Buffer);
  File->Close (File);
  return EFI_ERROR (Status) ? Status : (Size == Bytes ? EFI_SUCCESS : EFI_BAD_BUFFER_SIZE);
}

STATIC EFI_STATUS Captured (APP_FILE_CONTEXT *Files)
{
  UINT32 Meta[7]; // active vendor/device, copy length, INT13, expected reason/address/length
  UINTN Index;
  EFI_STATUS Status;
  CONST CHAR16 *Names[] = {L"capture-a.bin", L"capture-b.bin"};
  CONST CHAR16 *Metadata[] = {L"capture-a.meta", L"capture-b.meta"};
  for (Index = 0; Index < 2; ++Index) {
    Status = ReadExact (Files, Metadata[Index], Meta, sizeof (Meta));
    if (Status == EFI_NOT_FOUND && Index == 0) {
      LogPrint (&Logger, L"Private paired captures not supplied; synthetic cases only.\r\n");
      return EFI_SUCCESS;
    }
    REQUIRE (!EFI_ERROR (Status));
    REQUIRE (!EFI_ERROR (ReadExact (Files, Names[Index], Shadow, sizeof (Shadow))));
    SetMem (Valid, sizeof (Valid), 1);
    CopyMem (Original, Shadow, sizeof (Shadow));
    REQUIRE (LegacyRomGuardInspect (Shadow, Valid, Meta[2], (UINT16)Meta[0], (UINT16)Meta[1], Meta[3], &Detail) == Meta[4]);
    REQUIRE (Detail.Address == Meta[5] && Detail.Bytes == Meta[6]);
    REQUIRE (CompareMem (Shadow, Original, sizeof (Shadow)) == 0);
    LogPrint (&Logger, L"PASS captured layout %u: result=%u address=%lx bytes=%lx\r\n",
      (UINT32)Index, (UINT32)Detail.Reason, Detail.Address, Detail.Bytes);
  }
  return EFI_SUCCESS;
}

#include "StorageRomTests.inc"

STATIC CHAR16 Screen[4096];
STATIC UINTN ScreenUsed, KeyCalls;
STATIC EFI_STATUS EFIAPI Output (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Text)
{
  (VOID)This;
  while (*Text != 0 && ScreenUsed + 1 < ARRAY_SIZE (Screen)) { Screen[ScreenUsed++] = *Text++; }
  Screen[ScreenUsed] = 0;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Key (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Value)
{
  (VOID)This;
  if (KeyCalls++ == 0) { return EFI_NOT_READY; }
  Value->ScanCode = 0; Value->UnicodeChar = L'\r'; return EFI_SUCCESS;
}
STATIC EFI_STATUS SummaryTest (VOID)
{
  EFI_SYSTEM_TABLE System, *Saved = gST;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input;
  CopyMem (&System, gST, sizeof (System));
  ZeroMem (&Console, sizeof (Console)); ZeroMem (&Input, sizeof (Input));
  Console.OutputString = Output; Input.ReadKeyStroke = Key;
  System.ConOut = &Console; System.StdErr = &Console; System.ConIn = &Input;
  gST = &System; // Application-local pointer only, not a firmware table write.
  BootReportReset ();
  BootReportObserve (L"NCV_BLOCKED_ROM_OVERLAP\r\n");
  BootReportObserve (L"NCV_BLOCKED_TARGET_ROM\r\nNCV_BOOT_FAIL_PREFLIGHT\r\n");
  BootReportHold (EFI_UNSUPPORTED);
  gST = Saved;
  REQUIRE (StrStr (Screen, L"Code: NCV_BLOCKED_ROM_OVERLAP") != NULL);
  REQUIRE (StrStr (Screen, L"No GPU switch attempted") != NULL);
  REQUIRE (StrStr (Screen, L"storage and PXE/network OpROM settings; keep CSM enabled") != NULL);
  REQUIRE (StrStr (Screen, L"disable unused network boot") != NULL);
  REQUIRE (StrStr (Screen, L"reselect the disk") != NULL);
  LogPrint (&Logger, L"PASS visible actionable error and first-failure preservation\r\n%s", Screen);
  return EFI_SUCCESS;
}

#include "FirmwareDiskTests.inc"

EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  APP_FILE_CONTEXT Files;
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  CHAR8 Text[128]; UINTN Size;
  (VOID)SystemTable;
  Status = AppFileInitialize (ImageHandle, &Files);
  if (EFI_ERROR (Status)) { return Status; }
  Status = LogInitialize (&Logger, &Files, TRUE, L"RomGuardTest.log");
  if (EFI_ERROR (Status)) { AppFileClose (&Files); return Status; }
  Status = Synthetic ();
  if (!EFI_ERROR (Status)) { Status = Captured (&Files); }
  if (!EFI_ERROR (Status)) { Status = StorageTests (); }
  if (!EFI_ERROR (Status)) { Status = FirmwareDiskTests (); }
  if (!EFI_ERROR (Status)) { Status = GuardRefreshTests (); }
  if (!EFI_ERROR (Status)) { Status = CapturedStorage (&Files); }
  LegacyStorageRomReset ();
  if (!EFI_ERROR (Status)) { Status = SummaryTest (); }
  AsciiSPrint (Text, sizeof (Text), "%a checks=%u failed-line=%u status=%r\n",
    EFI_ERROR (Status) ? "FAIL" : "PASS", (UINT32)Checks, (UINT32)FailedLine, Status);
  LogPrint (&Logger, L"%a", Text); LogClose (&Logger);
  if (!EFI_ERROR (AppFileOpenAdjacent (&Files, L"RomGuardTest.result",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &File))) {
    Size = AsciiStrLen (Text); File->Write (File, &Size, Text); File->Flush (File); File->Close (File);
  }
  AppFileClose (&Files);
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  return Status;
}

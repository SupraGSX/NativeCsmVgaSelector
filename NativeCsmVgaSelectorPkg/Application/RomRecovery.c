/** Recovery UI is outside the frozen native boot routine.
    SPDX-License-Identifier: GPL-3.0-only */
#include "RomRecovery.h"
#include "RomPlacement.h"
#include "BootReport.h"
#include "BootCountdown.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC struct {
  BOOLEAN Pending;
  UINTN ConflictAddress;
  UINTN ConflictBytes;
  UINT32 RuntimeLimit;
  NCV_ROM_PLACEMENT_FACTS Facts;
} Recovery;

VOID EFIAPI __attribute__((noinline)) RomRecoveryReset (VOID)
{ ZeroMem (&Recovery, sizeof (Recovery)); }

VOID EFIAPI __attribute__((noinline)) RomRecoveryRecord (
  CONST NATIVE_CSM_VGA_RUNTIME_PLAN *Plan, CONST LEGACY_ROM_GUARD_RESULT *Conflict)
{
  RomRecoveryReset ();
  if (Plan == NULL || Conflict == NULL || Plan->RouteDirty || Plan->EndpointWriteAttempted ||
      Plan->EndpointWriteApplied ||
      (Conflict->Reason != RomGuardOverlap && Conflict->Reason != RomGuardDiskHandlerOverlap) ||
      Conflict->Address < NCV_ROM_SHADOW_BASE || Conflict->Address >= NCV_ROM_SHADOW_BASE + 0x10000 ||
      Conflict->ActiveBytes == 0 || Conflict->ActiveBytes > NCV_ROM_SHADOW_SIZE) { return; }
  Recovery.Pending = TRUE;
  Recovery.ConflictAddress = Conflict->Address;
  Recovery.ConflictBytes = Conflict->Bytes;
  Recovery.RuntimeLimit = (UINT32)MIN (Conflict->ActiveBytes, Conflict->Address - NCV_ROM_SHADOW_BASE);
  if (Plan->RomValidation.MatchingLegacyImageFound &&
      Plan->RomValidation.MatchingLegacyCandidateCount == 1 &&
      Plan->SelectedRomSize == Plan->RomValidation.MatchingLegacyImageSize) {
    NcvRomPlacementReadFacts (Plan->SelectedRom, Plan->SelectedRomSize, &Recovery.Facts);
  }
}

BOOLEAN EFIAPI __attribute__((noinline)) RomRecoveryAvailable (EFI_STATUS BootStatus, EFI_STATUS CleanupStatus)
{ return (BOOLEAN)(Recovery.Pending && BootStatus == EFI_UNSUPPORTED && !EFI_ERROR (CleanupStatus)); }

STATIC VOID ReportAssessment (EFI_HANDLE ImageHandle)
{
  CONST CHAR16 *Code, *Reason;
  NCV_ROM_PLACEMENT_RESULT Result;
  APP_FILE_CONTEXT Files;
  APP_LOGGER Logger;
  EFI_STATUS Status, CloseStatus;
  Result = NcvRomPlacementAssess (&Recovery.Facts, Recovery.RuntimeLimit);
  switch (Result) {
    case NcvPlacementInPlaceRom:
      Code = L"NCV_ALT_ROM_NO_SPLIT_CONTRACT";
      Reason = L"This older GPU ROM does not advertise separate initialization/runtime placement."; break;
    case NcvPlacementUnknownRuntime:
      Code = L"NCV_ALT_ROM_RUNTIME_UNKNOWN";
      Reason = L"The GPU ROM declares no usable maximum runtime size."; break;
    case NcvPlacementRuntimeOverlap:
      Code = L"NCV_ALT_ROM_RUNTIME_OVERLAP";
      Reason = L"The declared GPU runtime size exceeds the conservative space limit."; break;
    case NcvPlacementNeedsFirmwareAndBackend:
      Code = L"NCV_ALT_ROM_EXECUTOR_UNAVAILABLE";
      Reason = L"ROM size prerequisites pass; firmware relocation and execution are not validated."; break;
    default:
      Code = L"NCV_ALT_ROM_METADATA_UNAVAILABLE";
      Reason = L"Validated GPU placement metadata is unavailable or inconsistent."; break;
  }
  Print (L"\r\nRead-only placement assessment: %s\r\n%s\r\n", Code, Reason);
  Print (L"Image=0x%x runtime-max=0x%x limit=0x%x PCIR-revision=%u\r\n",
    Recovery.Facts.ImageBytes, Recovery.Facts.MaxRuntimeBytes, Recovery.RuntimeLimit, Recovery.Facts.Revision);
  Print (L"Alternative boot is unavailable in this build. No ROM code was executed.\r\n");
  ZeroMem (&Files, sizeof (Files)); ZeroMem (&Logger, sizeof (Logger));
  Status = AppFileInitialize (ImageHandle, &Files);
  if (!EFI_ERROR (Status)) {
    Status = LogInitialize (&Logger, &Files, TRUE, L"NativeCsmVgaRecovery.log");
    if (!EFI_ERROR (Status)) {
      Logger.Console = NULL; Logger.Serial = NULL;
      Status = LogPrint (&Logger,
        L"NCV_ROM_RECOVERY_READ_ONLY\r\n%s\r\n%s\r\n"
        L"conflict=0x%lx bytes=0x%lx image=0x%x runtime-max=0x%x limit=0x%x revision=%u\r\n"
        L"No GPU/CSM calls, ROM writes or retry. Snapshot is evidence, not permission to execute.\r\n",
        Code, Reason, Recovery.ConflictAddress, Recovery.ConflictBytes, Recovery.Facts.ImageBytes,
        Recovery.Facts.MaxRuntimeBytes, Recovery.RuntimeLimit, Recovery.Facts.Revision);
    }
    CloseStatus = LogClose (&Logger);
    if (!EFI_ERROR (Status)) { Status = CloseStatus; }
    CloseStatus = AppFileClose (&Files);
    if (!EFI_ERROR (Status)) { Status = CloseStatus; }
  }
  if (EFI_ERROR (Status)) { Print (L"Assessment log could not be saved: %r. Photograph this screen.\r\n", Status); }
  else { Print (L"Assessment saved beside the EFI file: NativeCsmVgaRecovery.log\r\n"); }
}

BOOLEAN EFIAPI __attribute__((noinline)) RomRecoveryOffer (
  EFI_HANDLE ImageHandle, EFI_STATUS BootStatus, EFI_STATUS CleanupStatus)
{
  EFI_INPUT_KEY Key;
  EFI_STATUS InputStatus;
  UINTN Count;
  BOOLEAN Assessed = FALSE;
  if (!RomRecoveryAvailable (BootStatus, CleanupStatus)) { RomRecoveryReset (); return FALSE; }
  /* A fresh key is mandatory: held/queued input must not choose an action. */
  if (gST != NULL && gST->ConIn != NULL && gST->ConIn->ReadKeyStroke != NULL) {
    for (Count = 0; Count < 32; ++Count) {
      if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) { break; }
    }
  }
  BootReportShow (BootStatus);
  Print (L"\r\nNCV_ROM_RECOVERY_MENU - overlap detected before hardware handoff\r\n"
         L"Enter: return to firmware to review storage/PXE OpROM settings\r\n"
         L"A: assess alternative GPU placement (read-only; does not boot)\r\n"
         L"Esc: cancel boot and return to firmware\r\n");
  if (BootCountdownCanEdit ()) { Print (L"F2: edit GPU and boot-disk settings.\r\n"); }
  for (;;) {
    if (gST == NULL || gST->ConIn == NULL || gST->ConIn->ReadKeyStroke == NULL) {
      Print (L"Firmware keyboard unavailable. Restart manually; no boot attempted.\r\n"); CpuDeadLoop ();
    }
    InputStatus = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (InputStatus == EFI_SUCCESS) {
      if (!Assessed && Key.ScanCode == SCAN_F2 && BootCountdownRequestEdit ()) { RomRecoveryReset (); return TRUE; }
      if (Key.UnicodeChar == L'\r' || Key.ScanCode == SCAN_ESC) { RomRecoveryReset (); return TRUE; }
      if ((Key.UnicodeChar == L'a' || Key.UnicodeChar == L'A') && !Assessed) {
        Assessed = TRUE;
        ReportAssessment (ImageHandle);
        Print (L"Enter: return to firmware. Esc: cancel. No automatic retry.\r\n");
      }
    } else if (InputStatus != EFI_NOT_READY) {
      Print (L"Firmware keyboard failed. Restart manually; no boot attempted.\r\n"); CpuDeadLoop ();
    }
    gBS->Stall (100000);
  }
}

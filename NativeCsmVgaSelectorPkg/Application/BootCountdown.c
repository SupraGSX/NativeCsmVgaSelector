/** Count down only after successful preflight. No hardware operations or retry.
    SPDX-License-Identifier: GPL-3.0-only */
#include "BootCountdown.h"
#include "BootReport.h"
#include "PciNames.h"
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC CHAR8 TargetName[128];
STATIC BOOLEAN Prepared, ContinueSelected, CleanReturn;
STATIC enum { NoAction, EditAction, CancelAction } Action;

EFI_STATUS EFIAPI __attribute__((noinline)) BootSetupWindow (BOOLEAN *EditRequested)
{
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  UINTN Seconds, Tick;
  if (EditRequested == NULL) { return EFI_INVALID_PARAMETER; }
  *EditRequested = FALSE;
  BootReportSetStage (L"Waiting for setup selection");
  Print (L"\r\nF2: edit GPU and boot disk. Esc: cancel.\r\n");
  for (Seconds = 5; Seconds > 0; --Seconds) {
    Print (L"Preflight starts in %u second%s.\r\n", (UINT32)Seconds, Seconds == 1 ? L"" : L"s");
    for (Tick = 0; Tick < 20; ++Tick) {
      if (gST->ConIn != NULL && gST->ConIn->ReadKeyStroke != NULL) {
        Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
        if (Status == EFI_SUCCESS && Key.ScanCode == SCAN_F2) { *EditRequested = TRUE; return EFI_SUCCESS; }
        if (Status == EFI_SUCCESS && Key.ScanCode == SCAN_ESC) { return EFI_ABORTED; }
        if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
          BootReportObserve (L"NCV_BOOT_FAIL_SETUP_INPUT"); return Status;
        }
      }
      Status = gBS->Stall (50000);
      if (EFI_ERROR (Status)) {
        BootReportObserve (L"NCV_BOOT_FAIL_SETUP_TIMER"); return Status;
      }
    }
  }
  return EFI_SUCCESS;
}

VOID EFIAPI __attribute__((noinline)) BootCountdownPrepare (CONST PROBE_PCI_ADDRESS *Target)
{
  Prepared = FALSE; ContinueSelected = CleanReturn = FALSE; Action = NoAction;
  TargetName[0] = 0;
  if (Target == NULL) { return; }
  PciNamesForTarget (Target, TargetName, sizeof (TargetName));
  Prepared = TRUE;
}

EFI_STATUS EFIAPI __attribute__((noinline)) BootCountdownWait (APP_LOGGER *Logger)
{
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  UINTN Seconds, Tick;
  if (!Prepared || Logger == NULL || ContinueSelected || Action != NoAction) { return EFI_NOT_READY; }
  Status = LogPrint (Logger, L"NCV_BOOT_INPUT_COUNTDOWN_BEGIN\r\n");
  if (!EFI_ERROR (Status)) { Status = LogFileBarrier (Logger); }
  if (EFI_ERROR (Status)) { return Status; }
  /* Preserve full file/serial diagnostics. Only routine console mirroring stops;
     BootReport and best-effort fatal reporting still address surviving consoles. */
  Logger->Console = NULL;
  BootReportSetStage (L"Waiting for display input switch");
  Print (L"\r\nPreflight passed. Switch your monitor to the %a input now.\r\n", TargetName);
  Print (L"F2: edit GPU and boot disk. Esc: cancel.\r\n");
  for (Seconds = 5; Seconds > 0; --Seconds) {
    Print (L"GPU switch in %u second%s.\r\n", (UINT32)Seconds, Seconds == 1 ? L"" : L"s");
    for (Tick = 0; Tick < 20; ++Tick) {
      if (gST->ConIn != NULL && gST->ConIn->ReadKeyStroke != NULL) {
        Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
        if (Status == EFI_SUCCESS && (Key.ScanCode == SCAN_F2 || Key.ScanCode == SCAN_ESC)) {
          Action = Key.ScanCode == SCAN_F2 ? EditAction : CancelAction;
          LogPrint (Logger, Action == EditAction ? L"NCV_BOOT_INPUT_EDIT_REQUESTED\r\n" : L"NCV_BOOT_INPUT_CANCELLED\r\n");
          return EFI_ABORTED;
        }
        if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
          LogPrint (Logger, L"NCV_BOOT_FAIL_COUNTDOWN_INPUT\r\n"); return Status;
        }
      }
      Status = gBS->Stall (50000);
      if (EFI_ERROR (Status)) {
        LogPrint (Logger, L"NCV_BOOT_FAIL_COUNTDOWN_TIMER\r\n"); return Status;
      }
    }
  }
  ContinueSelected = TRUE; // No returning setup request after proceeding toward handoff.
  Status = LogPrint (Logger, L"NCV_BOOT_INPUT_COUNTDOWN_COMPLETE\r\n");
  if (!EFI_ERROR (Status)) { Status = LogFileBarrier (Logger); }
  return Status;
}

EFI_STATUS EFIAPI __attribute__((noinline)) BootCountdownFinish (
  APP_LOGGER *Logger, APP_FILE_CONTEXT *Files, EFI_STATUS Status, EFI_STATUS PlanCleanupStatus)
{
  BOOLEAN Closed, UserAction;
  EFI_STATUS Result;
  UserAction = (BOOLEAN)(!ContinueSelected && Action != NoAction && Status == EFI_ABORTED);
  /* Cancellation is not a primary failure: report a failed release/close instead.
     Other paths retain the existing cleanup/primary-error precedence. */
  Result = EFI_ERROR (PlanCleanupStatus) ? PlanCleanupStatus : (UserAction ? EFI_SUCCESS : Status);
  Result = BootReportFinishDetailed (Logger, Files, Result, &Closed);
  CleanReturn = (BOOLEAN)(Prepared && !ContinueSelected && !EFI_ERROR (PlanCleanupStatus) && Closed);
  if (UserAction && !EFI_ERROR (Result) && CleanReturn) { return EFI_ABORTED; }
  Action = NoAction;
  return Result;
}

BOOLEAN EFIAPI __attribute__((noinline)) BootCountdownCanEdit (VOID)
{ return CleanReturn; }

BOOLEAN EFIAPI __attribute__((noinline)) BootCountdownRequestEdit (VOID)
{
  if (!CleanReturn) { return FALSE; }
  Action = EditAction; return TRUE;
}

BOOLEAN EFIAPI __attribute__((noinline)) BootCountdownTakeEdit (EFI_STATUS Status, EFI_STATUS MarkerStatus)
{
  BOOLEAN Result = (BOOLEAN)(CleanReturn && Action == EditAction && EFI_ERROR (Status) && !EFI_ERROR (MarkerStatus));
  Action = NoAction;
  return Result;
}

BOOLEAN EFIAPI __attribute__((noinline)) BootCountdownTakeCancel (EFI_STATUS Status, EFI_STATUS MarkerStatus)
{
  BOOLEAN Result;
  if (EFI_ERROR (MarkerStatus)) { CleanReturn = FALSE; }
  Result = (BOOLEAN)(CleanReturn && Action == CancelAction && Status == EFI_ABORTED && !EFI_ERROR (MarkerStatus));
  if (Action == CancelAction) { Action = NoAction; }
  return Result;
}

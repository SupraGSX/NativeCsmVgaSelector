/** Persistent boot error summaries. No file I/O, allocation or new boot policy
    in the reporting path; no console reconnection or firmware variable writes.
    SPDX-License-Identifier: GPL-3.0-only */
#include "BootReport.h"
#include "BootCountdown.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

/* Keep diagnostic state out of PROBE_CONFIG, the runtime plan and Boot locals.
   The noinline boundaries also keep report buffers out of the sensitive frame. */
STATIC BOOLEAN Active;
STATIC CHAR16 Stage[96];
STATIC CHAR16 Checkpoint[96];
STATIC CHAR16 FailureCode[96];
STATIC EFI_STATUS LogFailure;
STATIC CHAR16 Output[256];
STATIC BOOLEAN DiskBbsWarning, DiskOrderWarning, DiskBootAttempted;

/* Translate existing checkpoints rather than adding calls/locals throughout
   the compiler-inlined runtime plan inside the frozen Boot frame. */
STATIC CONST struct { CONST CHAR16 *Token; CONST CHAR16 *Description; } Stages[] = {
  { L"NCV_BOOT_START", L"Validating boot configuration" },
  { L"NCV_CONFIG_PARSED", L"Checking native CSM capabilities" },
  { L"NCV_BLOCKED_CONFIG", L"Validating boot configuration" },
  { L"NCV_BLOCKED_CSM_CAPABILITY", L"Checking native CSM capabilities" },
  { L"NCV_BLOCKED_TARGET_IDENTITY", L"Matching the selected GPU" },
  { L"NCV_BLOCKED_ACTIVE_VGA", L"Matching active and selected GPUs" },
  { L"NCV_BLOCKED_TARGET_PATH", L"Validating GPU bridge routes" },
  { L"NCV_BLOCKED_TARGET_VGA", L"Reading target GPU state" },
  { L"NCV_BLOCKED_TARGET_ROM", L"Validating GPU option ROM" },
  { L"NCV_BLOCKED_ROM_OVERLAP", L"Protecting existing legacy option ROMs" },
  { L"NCV_BOOT_FAIL_STORAGE_ROM_CHANGED", L"Verifying preserved storage option ROMs" },
  { L"NCV_BLOCKED_ROM_LAYOUT", L"Checking legacy ROM memory layout" },
  { L"NCV_BLOCKED_BOOT_CONTROLLER", L"Matching boot disk controller" },
  { L"NCV_BLOCKED_BBS_TARGET", L"Matching the live legacy disk" },
  { L"NCV_BLOCKED_BBS_ELIGIBILITY", L"Validating the matched legacy disk" },
  { L"NCV_BLOCKED_LEGACY_OPTION", L"Matching firmware boot option" },
  { L"NCV_BOOT_PREFLIGHT_COMPLETE", L"Committing pre-switch diagnostics" },
  { L"NCV_BOOT_ACTIVE_GOP_DISCONNECT_BEGIN", L"Disconnecting primary firmware display" },
  { L"NCV_BOOT_VGA_ROUTE_TRANSFER_BEGIN", L"Routing the selected GPU" },
  { L"NCV_BOOT_LEGACY_REGION_UNLOCK_BEGIN", L"Unlocking legacy video memory" },
  { L"NCV_BOOT_FAIL_ROM_BACKUP_ALLOCATION", L"Allocating GPU ROM backup" },
  { L"NCV_BOOT_TARGET_LEGACY_VGA_ROM_COPY_BEGIN", L"Copying and verifying GPU option ROM" },
  { L"NCV_BOOT_FAIL_DISPATCH_ALLOCATION", L"Allocating low-memory dispatch table" },
  { L"NCV_BOOT_COMPATIBILITY16_FARCALL_BEGIN", L"Executing GPU option ROM through firmware" },
  { L"NCV_BOOT_COMPATIBILITY16_FARCALL_RETURNED", L"Verifying GPU ROM initialization and INT10" },
  { L"NCV_BOOT_TARGET_BBS_TRANSACTION_BEGIN", L"Applying legacy disk boot priorities" },
  { L"NCV_BOOT_LEGACY_BOOT_OPTION_READY", L"Preparing native legacy handoff" },
  { L"NCV_BOOT_NATIVE_LEGACY_BOOT_BEGIN", L"Calling native legacy boot" }
};

STATIC VOID CopyBounded (CHAR16 *To, UINTN Capacity, CONST CHAR16 *From)
{
  UINTN Index;
  for (Index = 0; Index + 1 < Capacity && From[Index] != 0; ++Index) {
    To[Index] = From[Index];
  }
  To[Index] = 0;
}

STATIC VOID Emit (CONST CHAR16 *Text)
{
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Out;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Err;
  if (gST == NULL) { return; }
  Out = gST->ConOut;
  Err = gST->StdErr;
  if (Out != NULL && Out->OutputString != NULL) {
    Out->OutputString (Out, (CHAR16 *)Text);
  }
  if (Err != NULL && Err != Out && Err->OutputString != NULL) {
    Err->OutputString (Err, (CHAR16 *)Text);
  }
}

STATIC VOID Field (CONST CHAR16 *Name, CONST CHAR16 *Value)
{
  UnicodeSPrint (Output, sizeof (Output), L"%s: %s\r\n", Name, Value);
  Emit (Output);
}

EFI_STATUS EFIAPI BootReportDisableWatchdog (VOID)
{
  EFI_STATUS Status = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  return Status == EFI_UNSUPPORTED ? EFI_SUCCESS : Status;
}

VOID EFIAPI __attribute__((noinline)) BootReportReset (VOID)
{
  Active = TRUE;
  LogFailure = EFI_SUCCESS;
  DiskBbsWarning = DiskOrderWarning = DiskBootAttempted = FALSE;
  Stage[0] = Checkpoint[0] = FailureCode[0] = 0;
}

VOID EFIAPI __attribute__((noinline)) BootReportSetStage (CONST CHAR16 *Text)
{
  if (Active && Text != NULL) { CopyBounded (Stage, ARRAY_SIZE (Stage), Text); }
}

VOID EFIAPI __attribute__((noinline)) BootReportDiskBootAttempt (VOID)
{
  if (Active) { DiskBootAttempted = TRUE; }
}

VOID EFIAPI __attribute__((noinline)) BootReportObserve (CONST CHAR16 *Text)
{
  UINTN Length, Index;
  if (!Active || Text == NULL) { return; }
  /* Match only record beginnings, not disk names or arbitrary log text. */
  while (*Text != 0) {
    if (StrnCmp (Text, L"NCV_DISK_FIRMWARE_WARNING_BBS\r", ARRAY_SIZE (L"NCV_DISK_FIRMWARE_WARNING_BBS") ) == 0) { DiskBbsWarning = TRUE; }
    if (StrnCmp (Text, L"NCV_DISK_FIRMWARE_WARNING_ORDER\r", ARRAY_SIZE (L"NCV_DISK_FIRMWARE_WARNING_ORDER") ) == 0) { DiskOrderWarning = TRUE; }
    if (StrnCmp (Text, L"NCV_", 4) == 0) {
      Length = 0;
      while ((Text[Length] >= L'A' && Text[Length] <= L'Z') ||
             (Text[Length] >= L'0' && Text[Length] <= L'9') || Text[Length] == L'_') {
        ++Length;
      }
      if (Length > 4 && Length < ARRAY_SIZE (Checkpoint)) {
        /* Preserve the first specific failure rather than the later generic
           preflight failure or cleanup checkpoints. */
        if (FailureCode[0] == 0) {
          CopyMem (Checkpoint, Text, Length * sizeof (CHAR16));
          Checkpoint[Length] = 0;
          for (Index = 0; Index < ARRAY_SIZE (Stages); ++Index) {
            if (StrCmp (Checkpoint, Stages[Index].Token) == 0) {
              CopyBounded (Stage, ARRAY_SIZE (Stage), Stages[Index].Description);
              break;
            }
          }
          if (StrStr (Checkpoint, L"_FAIL_") != NULL ||
              StrStr (Checkpoint, L"_BLOCKED_") != NULL) {
            CopyBounded (FailureCode, ARRAY_SIZE (FailureCode), Checkpoint);
          }
        }
      }
    }
    while (*Text != 0 && *Text != L'\n') { ++Text; }
    if (*Text == L'\n') { ++Text; }
  }
}

VOID EFIAPI __attribute__((noinline)) BootReportLogFailure (EFI_STATUS Status)
{
  if (!Active || !EFI_ERROR (Status) || EFI_ERROR (LogFailure)) { return; }
  LogFailure = Status;
  UnicodeSPrint (Output, sizeof (Output),
    L"\r\nBOOT LOG ERROR: %r (0x%016lx)\r\n", Status, (UINT64)Status);
  Emit (Output);
  Emit (L"The log may be incomplete. Photograph any error summary.\r\n");
}

STATIC VOID Summary (EFI_STATUS Status, BOOLEAN Halted)
{
  Emit (L"\r\n=== Native CSM VGA Selector: BOOT STOPPED ===\r\n");
  Field (L"Stage", Stage[0] ? Stage : L"Boot preparation");
  UnicodeSPrint (Output, sizeof (Output), L"Status: %r (0x%016lx)\r\n",
    Status, (UINT64)Status);
  Emit (Output);
  Field (L"Code", FailureCode[0] ? FailureCode : L"NCV_BOOT_FAIL_RETURNED");
  if (StrCmp (FailureCode, L"NCV_BLOCKED_ROM_OVERLAP") == 0) {
    Emit (L"GPU ROM conflicts with existing legacy boot code. No GPU switch attempted.\r\n"
          L"Review legacy storage and PXE/network OpROM settings; keep CSM enabled.\r\n"
          L"Use UEFI-only for the conflicting ROM, or disable unused network boot.\r\n"
          L"Then reselect the disk in setup; its firmware name may change.\r\n");
  }
  if (DiskBbsWarning || DiskOrderWarning) {
    Emit (L"Firmware boot warning for the user-selected disk:\r\n");
    if (DiskBbsWarning) { Emit (L"  Firmware BBS status/priority discouraged booting this disk.\r\n"); }
    if (DiskOrderWarning) { Emit (L"  Firmware LegacyDevOrder marked this disk disabled.\r\n"); }
    Emit (DiskBootAttempted ?
      L"  Native boot was attempted at the user's request despite that warning.\r\n" :
      L"  User override was selected; native disk boot was not reached.\r\n");
    Emit (L"  Original firmware values and disk identity are recorded in the log.\r\n");
  }
  if (Checkpoint[0]) { Field (L"Last record", Checkpoint); }
  if (EFI_ERROR (LogFailure)) {
    UnicodeSPrint (Output, sizeof (Output), L"Log error: %r (0x%016lx)\r\n",
      LogFailure, (UINT64)LogFailure);
    Emit (Output);
  }
  Emit (L"Photograph this screen. NativeCsmVgaBoot.log is beside the EFI file.\r\n");
  if (Halted) {
    Emit (L"Hardware handoff has started. Stopped; no retry will be attempted.\r\n"
          L"Restart the PC before trying again.\r\n");
  }
}

VOID EFIAPI __attribute__((noinline)) BootReportShow (EFI_STATUS Status)
{ if (EFI_ERROR (Status)) { Summary (Status, FALSE); } }

VOID EFIAPI __attribute__((noinline)) BootReportHold (EFI_STATUS Status)
{
  EFI_INPUT_KEY Key;
  EFI_STATUS InputStatus;
  UINTN Count;
  if (!EFI_ERROR (Status)) { return; }
  /* Drain only a bounded amount of type-ahead before asking for a NEW Enter. */
  if (gST != NULL && gST->ConIn != NULL && gST->ConIn->ReadKeyStroke != NULL) {
    for (Count = 0; Count < 32; ++Count) {
      if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) { break; }
    }
  }
  Summary (Status, FALSE);
  Emit (L"Press Enter to return to firmware.\r\n");
  if (BootCountdownCanEdit ()) { Emit (L"F2: edit GPU and boot-disk settings.\r\n"); }
  for (;;) {
    if (gST == NULL || gST->ConIn == NULL || gST->ConIn->ReadKeyStroke == NULL) {
      Emit (L"Firmware keyboard unavailable. Restart manually; the error remains displayed.\r\n");
      CpuDeadLoop ();
    }
    InputStatus = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (InputStatus == EFI_SUCCESS && Key.UnicodeChar == L'\r') { return; }
    if (InputStatus == EFI_SUCCESS && Key.ScanCode == SCAN_F2 && BootCountdownRequestEdit ()) { return; }
    if (EFI_ERROR (InputStatus) && InputStatus != EFI_NOT_READY) {
      Emit (L"Firmware keyboard failed. Restart manually; the error remains displayed.\r\n");
      CpuDeadLoop ();
    }
    gBS->Stall (100000);
  }
}

EFI_STATUS EFIAPI __attribute__((noinline)) BootReportFinish (
  APP_LOGGER *Logger, APP_FILE_CONTEXT *Files, EFI_STATUS Status
  )
{ return BootReportFinishDetailed (Logger, Files, Status, NULL); }

EFI_STATUS EFIAPI __attribute__((noinline)) BootReportFinishDetailed (
  APP_LOGGER *Logger, APP_FILE_CONTEXT *Files, EFI_STATUS Status, BOOLEAN *Closed)
{
  EFI_STATUS CloseStatus;
  if (Closed != NULL) { *Closed = TRUE; }
  if (!EFI_ERROR (Status)) { BootReportSetStage (L"Closing boot diagnostics"); }
  CloseStatus = LogFlush (Logger);
  if (Closed != NULL && EFI_ERROR (CloseStatus)) { *Closed = FALSE; }
  BootReportLogFailure (CloseStatus);
  if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) { Status = CloseStatus; }
  CloseStatus = LogClose (Logger);
  if (Closed != NULL && EFI_ERROR (CloseStatus)) { *Closed = FALSE; }
  BootReportLogFailure (CloseStatus);
  if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) { Status = CloseStatus; }
  CloseStatus = AppFileClose (Files);
  if (Closed != NULL && EFI_ERROR (CloseStatus)) { *Closed = FALSE; }
  if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) { Status = CloseStatus; }
  return Status;
}

VOID EFIAPI __attribute__((noinline)) BootReportHalt (
  APP_LOGGER *Logger, CONST CHAR16 *Code, EFI_STATUS Status
  )
{
  BootReportObserve (Code);
  if (!EFI_ERROR (Status)) { Status = EFI_ABORTED; }
  /* Try durable diagnostics first, then an allocation-free console summary.
     Console visibility after GOP disconnect is necessarily best effort. */
  LogPrintBestEffort (Logger, L"%s status=0x%016lx (%r) stage=%s\r\n",
    Code, (UINT64)Status, Status, Stage);
  BootReportLogFailure (LogCommitAndClose (Logger));
  Summary (Status, TRUE);
  CpuDeadLoop ();
}

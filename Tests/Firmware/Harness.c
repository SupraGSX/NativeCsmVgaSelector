/* Synthetic firmware UI harness; never use as a production loader.
   SPDX-License-Identifier: GPL-3.0-only */
#define NativeCsmVgaProbeRun TestProbe
#define NativeCsmVgaBootRun TestBoot
#define MarkerRestore TestMarkerRestore
#define UefiMain DispatcherMain
#include "DispatcherUnderTest.c"
#undef UefiMain
#undef MarkerRestore
#include "TargetSelection.h"
#include "Log.h"
#include "ConfigFile.h"
#include "BootReport.h"
#include <Library/PrintLib.h>

STATIC CHAR8 Mode[40];
STATIC EFI_HANDLE TestImage;
STATIC EFI_TEXT_STRING OriginalOutput;
STATIC CHAR8 ErrorScreen[32768];
STATIC UINTN ErrorScreenBytes;
STATIC EFI_FILE_PROTOCOL *CaptureFile;
STATIC EFI_INPUT_READ_KEY OriginalReadKey;
STATIC BOOLEAN QueuedEnter;
STATIC UINTN RecoveryQueuedKeys, BootCalls;
STATIC BOOLEAN BootFrameActive;
EFI_STATUS MarkerRestore (VOID);

EFI_STATUS TestMarkerRestore (VOID)
{
  if (AsciiStrCmp (Mode, "diag-overlap-cleanup") == 0) { return EFI_ACCESS_DENIED; }
  return MarkerRestore ();
}

STATIC EFI_STATUS EFIAPI RecoveryTypeAhead (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
  if (RecoveryQueuedKeys < 2) {
    Key->ScanCode = 0; Key->UnicodeChar = RecoveryQueuedKeys++ == 0 ? L'a' : L'\r';
    return EFI_SUCCESS;
  }
  return OriginalReadKey (This, Key);
}

/* Let failure scenarios call the production boot routine, which cannot get
   past native-CSM preflight under OVMF. Ordinary UI cases keep the stub. */
#undef NativeCsmVgaBootRun
EFI_STATUS EFIAPI NativeCsmVgaBootRun (EFI_HANDLE ImageHandle, CONST PROBE_CONFIG *Config);

STATIC VOID Report (EFI_HANDLE ImageHandle, CONST CHAR16 *Name, CONST CHAR8 *Text)
{
  APP_FILE_CONTEXT Files;
  EFI_FILE_PROTOCOL *File;
  UINTN Size = AsciiStrLen (Text);
  if (EFI_ERROR (AppFileInitialize (ImageHandle, &Files))) { return; }
  if (!EFI_ERROR (AppFileOpenAdjacent (&Files, Name,
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &File))) {
    File->Write (File, &Size, (VOID *)Text);
    File->Flush (File);
    File->Close (File);
  }
  AppFileClose (&Files);
}

#include "CountdownTests.inc"

STATIC EFI_STATUS EFIAPI CaptureOutput (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Text)
{
  UINTN Index;
  if (AsciiStrnCmp (Mode, "countdown-", 10) == 0 || ErrorScreenBytes || StrStr (Text, L"BOOT LOG ERROR") != NULL ||
      StrStr (Text, L"BOOT STOPPED") != NULL) {
    for (Index = 0; Text[Index] && ErrorScreenBytes + 1 < sizeof (ErrorScreen); ++Index) {
      ErrorScreen[ErrorScreenBytes++] = Text[Index] < 128 ? (CHAR8)Text[Index] : '?';
    }
    ErrorScreen[ErrorScreenBytes] = 0;
    if (CaptureFile != NULL) {
      UINTN Size = ErrorScreenBytes;
      CaptureFile->SetPosition (CaptureFile, 0);
      CaptureFile->Write (CaptureFile, &Size, ErrorScreen);
      CaptureFile->Flush (CaptureFile);
    }
  }
  return OriginalOutput (This, Text);
}

STATIC EFI_STATUS EFIAPI FaultWrite (EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buffer)
{
  (VOID)This; (VOID)Buffer;
  if (AsciiStrCmp (Mode, "diag-log-write") == 0 ||
      AsciiStrCmp (Mode, "diag-primary-and-log") == 0) { *Size = 0; return EFI_VOLUME_FULL; }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI FaultFlush (EFI_FILE_PROTOCOL *This)
{
  (VOID)This;
  return AsciiStrCmp (Mode, "diag-log-flush") == 0 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI FaultClose (EFI_FILE_PROTOCOL *This)
{
  (VOID)This;
  return AsciiStrCmp (Mode, "diag-log-close") == 0 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI TypeAhead (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
  if (QueuedEnter) {
    QueuedEnter = FALSE;
    Key->ScanCode = 0;
    Key->UnicodeChar = L'\r';
    return EFI_SUCCESS;
  }
  return OriginalReadKey (This, Key);
}

STATIC EFI_STATUS DiagnosticBoot (EFI_HANDLE ImageHandle, CONST PROBE_CONFIG *Config)
{
  APP_LOGGER Logger;
  APP_FILE_CONTEXT Files;
  EFI_FILE_PROTOCOL File;
  EFI_STATUS Status;
  if (AsciiStrnCmp (Mode, "diag-overlap", 12) == 0 || AsciiStrnCmp (Mode, "countdown-overlap", 17) == 0) {
    STATIC NATIVE_CSM_VGA_RUNTIME_PLAN Plan;
    STATIC UINT8 Rom[1024];
    LEGACY_ROM_GUARD_RESULT Conflict;
    UINTN I; UINT8 Sum = 0;
    ZeroMem (&Plan, sizeof (Plan)); ZeroMem (&Conflict, sizeof (Conflict)); ZeroMem (Rom, sizeof (Rom));
    Rom[0] = 0x55; Rom[1] = 0xaa; Rom[2] = 2; Rom[0x18] = 0x1c;
    CopyMem (Rom + 0x1c, "PCIR", 4); Rom[0x26] = 0x1c; Rom[0x2c] = 2; Rom[0x32] = 1;
    if (AsciiStrCmp (Mode, "diag-overlap-new") == 0) { Rom[0x28] = 3; }
    for (I = 0; I < sizeof (Rom); ++I) { Sum = (UINT8)(Sum + Rom[I]); }
    Rom[sizeof (Rom) - 1] = (UINT8)(0 - Sum);
    Plan.SelectedRom = Rom; Plan.SelectedRomSize = sizeof (Rom);
    Plan.RomValidation.MatchingLegacyImageFound = TRUE;
    Plan.RomValidation.MatchingLegacyCandidateCount = 1;
    Plan.RomValidation.MatchingLegacyImageSize = sizeof (Rom);
    Conflict.Reason = RomGuardOverlap; Conflict.ActiveBytes = 512;
    Conflict.Address = 0xc0200; Conflict.Bytes = 512;
    if (AsciiStrCmp (Mode, "diag-overlap-dirty") == 0) { Plan.RouteDirty = TRUE; }
    RomRecoveryRecord (&Plan, &Conflict);
    // Recovery must own its evidence after normal preflight cleanup.
    ZeroMem (&Plan, sizeof (Plan)); ZeroMem (Rom, sizeof (Rom)); LegacyRomGuardReset ();
    BootReportObserve (L"NCV_BLOCKED_ROM_OVERLAP");
    if (AsciiStrCmp (Mode, "diag-overlap-no-evidence") == 0) { RomRecoveryReset (); }
    if (AsciiStrCmp (Mode, "diag-overlap-typeahead") == 0) {
      OriginalReadKey = gST->ConIn->ReadKeyStroke;
      gST->ConIn->ReadKeyStroke = RecoveryTypeAhead;
    }
    if (AsciiStrCmp (Mode, "diag-overlap-keyboard") == 0) { gST->ConIn = NULL; }
    if (AsciiStrCmp (Mode, "diag-overlap-fatal") == 0) {
      ZeroMem (&Logger, sizeof (Logger));
      BootReportHalt (&Logger, L"NCV_BOOT_FAIL_TEST_IRREVERSIBLE", EFI_DEVICE_ERROR);
    }
    return AsciiStrCmp (Mode, "diag-overlap-status") == 0 ? EFI_CRC_ERROR : EFI_UNSUPPORTED;
  }
  if (AsciiStrCmp (Mode, "diag-boot-open") == 0) {
    return NativeCsmVgaBootRun (NULL, Config);
  }
  if (AsciiStrCmp (Mode, "diag-log-open") == 0 ||
      AsciiStrCmp (Mode, "diag-preflight") == 0) {
    return NativeCsmVgaBootRun (ImageHandle, Config);
  }
  if (AsciiStrCmp (Mode, "diag-keyboard") == 0 ||
      AsciiStrCmp (Mode, "diag-typeahead") == 0) {
    BootReportSetStage (L"Simulated preflight rejection");
    BootReportObserve (L"NCV_BLOCKED_BOOT_CONTROLLER");
    BootReportObserve (L"NCV_BOOT_FAIL_PREFLIGHT");
    if (AsciiStrCmp (Mode, "diag-keyboard") == 0) {
      gST->ConIn = NULL;
    } else {
      OriginalReadKey = gST->ConIn->ReadKeyStroke;
      QueuedEnter = TRUE;
      gST->ConIn->ReadKeyStroke = TypeAhead;
    }
    return EFI_NO_MAPPING;
  }
  if (AsciiStrCmp (Mode, "diag-fatal") == 0) {
    Status = AppFileInitialize (ImageHandle, &Files);
    if (EFI_ERROR (Status)) { return Status; }
    Status = LogInitialize (&Logger, &Files, TRUE, L"FatalTest.log");
    if (EFI_ERROR (Status)) { return Status; }
    BootReportSetStage (L"Simulated irreversible handoff failure");
    BootReportHalt (&Logger, L"NCV_BOOT_FAIL_ROM_BACKUP_ALLOCATION", EFI_OUT_OF_RESOURCES);
  }
  ZeroMem (&Files, sizeof (Files));
  ZeroMem (&Logger, sizeof (Logger));
  ZeroMem (&File, sizeof (File));
  File.Write = FaultWrite;
  File.Flush = FaultFlush;
  File.Close = FaultClose;
  Logger.File = &File;
  Logger.Console = gST->ConOut;
  Logger.DurableFileFirst = TRUE;
  BootReportSetStage (L"Writing boot diagnostics");
  Status = LogPrint (&Logger, L"NCV_BLOCKED_BOOT_CONTROLLER\r\n");
  LogPrint (&Logger, L"NCV_BOOT_FAIL_PREFLIGHT\r\n");
  if (AsciiStrCmp (Mode, "diag-primary-and-log") == 0) {
    return BootReportFinish (&Logger, &Files, EFI_NO_MAPPING);
  }
  if (AsciiStrCmp (Mode, "diag-log-close") == 0) {
    return BootReportFinish (&Logger, &Files, EFI_SUCCESS);
  }
  return Status;
}

STATIC EFI_STATUS SyntheticBoot (EFI_HANDLE ImageHandle, CONST PROBE_CONFIG *Config)
{
  CHAR8 Text[160];
  APP_FILE_CONTEXT Files;
  APP_LOGGER Logger;
  EFI_STATUS Status;
  AsciiSPrint (Text, sizeof (Text), "%u", (UINT32)++BootCalls);
  Report (ImageHandle, L"BootCallCount.txt", Text);
  if (AsciiStrnCmp (Mode, "diag-", 5) == 0) {
    Report (ImageHandle, L"DiagnosticBoot.called", Mode);
    return DiagnosticBoot (ImageHandle, Config);
  }
  ZeroMem (&Files, sizeof (Files)); ZeroMem (&Logger, sizeof (Logger));
  Status = AppFileInitialize (ImageHandle, &Files);
  if (EFI_ERROR (Status)) { return Status; }
  Status = LogInitialize (&Logger, &Files, TRUE, L"CountdownTest.log");
  if (!EFI_ERROR (Status)) { Status = LogBeginDurableFileFirst (&Logger); }
  if (EFI_ERROR (Status)) { return BootCountdownFinish (&Logger, &Files, Status, EFI_SUCCESS); }
  Print (L"TEST PREFLIGHT COMPLETE\r\n");
  LogPrint (&Logger, L"NCV_BOOT_PREFLIGHT_COMPLETE\r\n");
  if (AsciiStrnCmp (Mode, "countdown-overlap", 17) == 0 && BootCalls == 1) {
    Status = DiagnosticBoot (ImageHandle, Config);
    return BootCountdownFinish (&Logger, &Files, Status, EFI_SUCCESS);
  }
  if (AsciiStrCmp (Mode, "countdown-preflight-edit") == 0 && BootCalls == 1) {
    BootReportObserve (L"NCV_BLOCKED_BOOT_CONTROLLER");
    return BootCountdownFinish (&Logger, &Files, EFI_NOT_FOUND, EFI_SUCCESS);
  }
  if (Config->AutoBoot) {
    Status = BootCountdownWait (&Logger);
    if (EFI_ERROR (Status)) {
      return BootCountdownFinish (&Logger, &Files, Status,
        AsciiStrCmp (Mode, "countdown-edit-cleanup") == 0 ? EFI_DEVICE_ERROR : EFI_SUCCESS);
    }
  }
  LogPrint (&Logger, L"TEST_HANDOFF_REACHED\r\n");
  Status = BootCountdownFinish (&Logger, &Files, EFI_SUCCESS, EFI_SUCCESS);
  if (EFI_ERROR (Status)) { return Status; }
  AsciiSPrint (Text, sizeof (Text), "GPU=%u disk=%u probe=%u first=%u editing=%u auto=%u",
    Config->TargetPci.Bus, Config->TargetControllerPci.Bus, Config->Probe,
    Config->FirstRun, ProbeConfigIsEditing (), Config->AutoBoot);
  Print (L"TEST BOOT: %a\r\n", Text);
  Report (ImageHandle, L"BootTest.result", Text);
  if (AsciiStrStr (Mode, "-padded") != NULL) {
    AsciiSPrint (Text, sizeof (Text), "%s", Config->TargetBbsDescription);
    Report (ImageHandle, L"DiskName.result", Text);
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI TestBoot (EFI_HANDLE ImageHandle, CONST PROBE_CONFIG *Config)
{
  EFI_STATUS Status;
  BootFrameActive = TRUE;
  Status = SyntheticBoot (ImageHandle, Config);
  BootFrameActive = FALSE;
  return Status;
}

EFI_STATUS EFIAPI TestProbe (
  EFI_HANDLE ImageHandle, CONST PROBE_CONFIG *Config, BOOLEAN *BootReady
  )
{
  NCV_DISPLAY_CANDIDATE Gpu[2];
  NCV_STORAGE_CANDIDATE Disk[2];
  UINTN G = 0, D = 0, I;
  EFI_STATUS Status, CloseStatus;
  APP_FILE_CONTEXT Files;
  APP_LOGGER Log;

  *BootReady = FALSE;
  Report (ImageHandle, L"SetupEntered.result", "yes");
  if (BootCalls != 0) {
    // Setup may only be re-entered after the previous owning frame has closed.
    if (BootFrameActive) { Report (ImageHandle, L"SetupOwnership.error", "unclean"); return EFI_DEVICE_ERROR; }
    Report (ImageHandle, L"SetupAfterCleanup.result", "clean");
  }
  if (AsciiStrCmp (Mode, "diagnostic") == 0 || AsciiStrCmp (Mode, "incomplete") == 0) {
    return EFI_SUCCESS;
  }
  ZeroMem (Gpu, sizeof (Gpu));
  ZeroMem (Disk, sizeof (Disk));
  for (I = 0; I < 2; I++) {
    Gpu[I].Address.Bus = (NCV_U8)(3 + I);
    Gpu[I].AddressValid = 1; Gpu[I].Vendor = 0x1234; Gpu[I].DeviceId = 0x1111;
    Gpu[I].OptionRomExposed = 1; Gpu[I].AcceptedLegacyRom = 1;
    Gpu[I].PathDiscovered = 1; Gpu[I].PathValidated = 1; Gpu[I].Eligible = 1;
    Disk[I].Address.Bus = (NCV_U8)(5 + I);
    Disk[I].AddressValid = 1; Disk[I].Eligible = 1; Disk[I].BbsIndex = I;
  }
  CopyMem (Disk[0].Description, "Example disk A", 15);
  CopyMem (Disk[1].Description, "Example disk B", 15);
  if (AsciiStrStr (Mode, "-padded") != NULL) {
    CopyMem (Disk[1].Description, "SanDisk Extreme Pro 0 ", sizeof ("SanDisk Extreme Pro 0 "));
    Disk[1].Usb = 1;
  }
  for (I = 0; I < 2; I++) {
    Disk[I].Eligible = (NCV_BOOL)NcvIniValueIsExactlyRepresentable (
      Disk[I].Description, sizeof (Disk[I].Description));
  }
  if (ProbeConfigIsEditing ()) {
    G = Config->TargetPci.Bus == 4 ? 1 : 0;
    D = Config->TargetControllerPci.Bus == 6 ? 1 : 0;
  }
  Status = SelectBootTargets (Gpu, 2, &G, Disk, 2, &D,
    ProbeConfigIsEditing () ? ProbeConfigReviewSettings () : NULL,
    ProbeConfigIsEditing () && Config->TargetPci.Bus != 3 && Config->TargetPci.Bus != 4);
  if (EFI_ERROR (Status)) { return Status; }
  Status = AppFileInitialize (ImageHandle, &Files);
  if (EFI_ERROR (Status)) { return Status; }
  Status = LogInitialize (&Log, &Files, TRUE, L"UiTest.log");
  if (EFI_ERROR (Status)) { AppFileClose (&Files); return Status; }
  Status = ProbeConfigWriteCandidateIni (&Files, &Log, Gpu, 2, G, Disk, 2, D, TRUE);
  CloseStatus = LogClose (&Log);
  if (!EFI_ERROR (Status)) { Status = CloseStatus; }
  CloseStatus = AppFileClose (&Files);
  if (!EFI_ERROR (Status)) { Status = CloseStatus; }
  *BootReady = !EFI_ERROR (Status);
  Print (L"SAVE STATUS: %r\r\n", Status);
  if (AsciiStrCmp (Mode, "save-failure") == 0) {
    /* Even a stale true ready flag must not override a failed save/close. */
    return EFI_DEVICE_ERROR;
  }
  if (*BootReady && AsciiStrCmp (Mode, "reload-failure") == 0) {
    /* Break both copies after save to prove dispatch reopens the actual file. */
    Report (ImageHandle, L"Config.ini", "[invalid]\n");
    Report (ImageHandle, L"Config.ini.previous", "[invalid]\n");
  }
  return Status;
}

EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  APP_FILE_CONTEXT Files;
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  UINTN Size = sizeof (Mode) - 1;
  CHAR8 Text[64];
  TestImage = ImageHandle;
  if (!EFI_ERROR (AppFileInitialize (ImageHandle, &Files))) {
    if (!EFI_ERROR (AppFileOpenAdjacent (&Files, L"UiTest.mode", EFI_FILE_MODE_READ, 0, &File))) {
      File->Read (File, &Size, Mode);
      File->Close (File);
    }
    AppFileClose (&Files);
  }
  if (AsciiStrnCmp (Mode, "diag-", 5) == 0 || (AsciiStrnCmp (Mode, "countdown-", 10) == 0 && AsciiStrCmp (Mode, "countdown-unit") != 0)) {
    // Do not open/close the same image protocols from a logger callback: a
    // nested AppFileInitialize/Close would invalidate the boot owner's record.
    if (!EFI_ERROR (AppFileInitialize (ImageHandle, &Files))) {
      AppFileOpenAdjacent (&Files, L"ErrorScreen.txt",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &CaptureFile);
      AppFileClose (&Files);
    }
    OriginalOutput = gST->ConOut->OutputString;
    gST->ConOut->OutputString = CaptureOutput;
  }
  if (AsciiStrCmp (Mode, "countdown-unit") == 0) {
    CountdownUnitTests (ImageHandle);
    for (;;) { gBS->Stall (100000); }
  }
  Status = DispatcherMain (ImageHandle, SystemTable);
  AsciiSPrint (Text, sizeof (Text), "returned=%r", Status);
  Report (ImageHandle, L"DispatcherTest.result", Text);
  for (;;) { gBS->Stall (100000); }
  return EFI_SUCCESS;
}

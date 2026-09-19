/* Production countdown/cleanup helpers with synthetic input, timer and file
   protocols. No physical disk or controller operations. GPL-3.0-only. */
STATIC UINTN CountdownCase, CountdownTicks, CountdownMicros, CountdownChecks;
STATIC BOOLEAN CountdownCloseFailure, CountdownWriteFailure, CountdownFlushFailure;
STATIC EFI_STATUS EFIAPI CountdownWrite (EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buffer)
{ (VOID)This; (VOID)Size; (VOID)Buffer; return CountdownWriteFailure ? EFI_VOLUME_FULL : EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI CountdownFlush (EFI_FILE_PROTOCOL *This)
{ (VOID)This; return CountdownFlushFailure ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI CountdownClose (EFI_FILE_PROTOCOL *This)
{ (VOID)This; return CountdownCloseFailure ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI CountdownKey (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
  (VOID)This; Key->ScanCode = 0; Key->UnicodeChar = 0;
  if (CountdownCase == 1) { Key->ScanCode = SCAN_F2; return EFI_SUCCESS; }
  if (CountdownCase == 2) { Key->ScanCode = SCAN_ESC; return EFI_SUCCESS; }
  if (CountdownCase == 3) { Key->UnicodeChar = L'\r'; return EFI_SUCCESS; }
  if (CountdownCase == 4) { return EFI_DEVICE_ERROR; }
  return EFI_NOT_READY;
}
STATIC EFI_STATUS EFIAPI CountdownStall (UINTN Micros)
{
  ++CountdownTicks; CountdownMicros += Micros;
  return CountdownCase == 5 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
#define COUNT_CHECK(E) do { ++CountdownChecks; if (!(E)) { \
  AsciiSPrint (Result, sizeof (Result), "FAIL line=%u case=%u: %a", (UINT32)__LINE__, (UINT32)CountdownCase, #E); \
  goto Done; } } while (0)
STATIC VOID CountdownUnitTests (EFI_HANDLE ImageHandle)
{
  EFI_STALL SavedStall = gBS->Stall;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input, *SavedInput = gST->ConIn;
  EFI_FILE_PROTOCOL File;
  APP_LOGGER Logger;
  APP_FILE_CONTEXT Files;
  PROBE_PCI_ADDRESS Target;
  EFI_STATUS Status;
  UINTN Trial;
  BOOLEAN EditRequested;
  CHAR8 Result[240] = "PASS";
  ZeroMem (&Input, sizeof (Input)); Input.ReadKeyStroke = CountdownKey;
  ZeroMem (&Target, sizeof (Target));
  gBS->Stall = CountdownStall; gST->ConIn = &Input;
  for (Trial = 0; Trial < 14; ++Trial) {
    BootReportReset (); BootCountdownPrepare (&Target);
    ZeroMem (&Logger, sizeof (Logger)); ZeroMem (&Files, sizeof (Files)); ZeroMem (&File, sizeof (File));
    File.Write = CountdownWrite; File.Flush = CountdownFlush; File.Close = CountdownClose;
    Logger.File = &File; Logger.Console = gST->ConOut; Logger.DurableFileFirst = TRUE;
    CountdownTicks = CountdownMicros = 0; CountdownCase = Trial <= 5 ? Trial : 0;
    CountdownCloseFailure = CountdownWriteFailure = CountdownFlushFailure = FALSE;
    gST->ConIn = Trial == 6 ? NULL : &Input;
    if (Trial == 7) { CountdownWriteFailure = TRUE; }
    if (Trial >= 8) { CountdownCase = 1; }
    if (Trial == 13) { CountdownCase = 2; }
    Status = BootCountdownWait (&Logger);
    if (Trial == 0 || Trial == 3 || Trial == 6) {
      COUNT_CHECK (Status == EFI_SUCCESS && CountdownTicks == 100 && CountdownMicros == 5000000);
      COUNT_CHECK (Logger.Console == NULL);
    } else if (Trial == 4 || Trial == 5 || Trial == 7) {
      COUNT_CHECK (EFI_ERROR (Status) && Status != EFI_ABORTED);
      COUNT_CHECK (CountdownTicks == (Trial == 5 ? 1 : 0));
    } else {
      COUNT_CHECK (Status == EFI_ABORTED && CountdownTicks == 0);
      COUNT_CHECK (!BootCountdownCanEdit ()); // Request alone is insufficient.
    }
    if (Trial == 8) { CountdownCloseFailure = TRUE; }
    if (Trial == 12) { CountdownFlushFailure = TRUE; }
    Status = BootCountdownFinish (&Logger, &Files, Status, Trial == 9 ? EFI_DEVICE_ERROR : EFI_SUCCESS);
    if (Trial == 1) {
      COUNT_CHECK (BootCountdownCanEdit ());
      COUNT_CHECK (!BootCountdownTakeCancel (Status, EFI_SUCCESS));
      COUNT_CHECK (BootCountdownTakeEdit (Status, EFI_SUCCESS));
      COUNT_CHECK (!BootCountdownTakeEdit (Status, EFI_SUCCESS));
    } else if (Trial == 2 || Trial == 13) {
      COUNT_CHECK (BootCountdownTakeCancel (Status, EFI_SUCCESS));
      COUNT_CHECK (!BootCountdownTakeCancel (Status, EFI_SUCCESS));
      COUNT_CHECK (!BootCountdownTakeEdit (Status, EFI_SUCCESS));
    } else if (Trial == 8 || Trial == 9 || Trial == 12) {
      COUNT_CHECK (Status == EFI_DEVICE_ERROR && !BootCountdownCanEdit ());
      COUNT_CHECK (!BootCountdownRequestEdit () && !BootCountdownTakeEdit (Status, EFI_SUCCESS));
    } else if (Trial == 10) {
      COUNT_CHECK (!BootCountdownTakeCancel (Status, EFI_ACCESS_DENIED));
      COUNT_CHECK (!BootCountdownCanEdit () && !BootCountdownTakeEdit (Status, EFI_ACCESS_DENIED));
    } else if (Trial == 11) {
      BootCountdownPrepare (&Target); // Fresh attempt must discard a stale F2.
      COUNT_CHECK (!BootCountdownCanEdit () && !BootCountdownTakeEdit (Status, EFI_SUCCESS));
    } else if (Trial == 0 || Trial == 3 || Trial == 6) {
      COUNT_CHECK (!BootCountdownCanEdit () && !BootCountdownRequestEdit ());
      COUNT_CHECK (!BootCountdownTakeEdit (EFI_DEVICE_ERROR, EFI_SUCCESS));
    }
  }
  for (Trial = 0; Trial < 7; ++Trial) {
    CountdownCase = Trial; CountdownTicks = CountdownMicros = 0;
    gST->ConIn = Trial == 6 ? NULL : &Input;
    EditRequested = TRUE;
    Status = BootSetupWindow (&EditRequested);
    if (Trial == 0 || Trial == 3 || Trial == 6) {
      COUNT_CHECK (Status == EFI_SUCCESS && !EditRequested);
      COUNT_CHECK (CountdownTicks == 100 && CountdownMicros == 5000000);
    } else if (Trial == 1) {
      COUNT_CHECK (Status == EFI_SUCCESS && EditRequested && CountdownTicks == 0);
    } else if (Trial == 2) {
      COUNT_CHECK (Status == EFI_ABORTED && !EditRequested && CountdownTicks == 0);
    } else {
      COUNT_CHECK (Status == EFI_DEVICE_ERROR && !EditRequested);
      COUNT_CHECK (CountdownTicks == (Trial == 5 ? 1 : 0));
    }
  }
  COUNT_CHECK (BootSetupWindow (NULL) == EFI_INVALID_PARAMETER);
  BootCountdownPrepare (NULL);
  COUNT_CHECK (BootCountdownWait (&Logger) == EFI_NOT_READY);
  AsciiSPrint (Result, sizeof (Result), "PASS %u checks", (UINT32)CountdownChecks);
Done:
  gBS->Stall = SavedStall; gST->ConIn = SavedInput;
  Report (ImageHandle, L"CountdownUnit.result", Result);
}
#undef COUNT_CHECK

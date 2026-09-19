/** @file Pre-handoff configuration recovery and explicit reset.
  SPDX-License-Identifier: GPL-3.0-only
**/
#include "ConfigRecovery.h"
#include "ConfigFile.h"
#include "CandidateIni.h"
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC CONST CHAR16 *ConfigNames[] = {
  L"Config.ini", L"Config.ini.previous", L"Config.ini.tmp"
};

STATIC EFI_STATUS ReadKey (EFI_INPUT_KEY *Key)
{
  UINTN Event;
  EFI_STATUS Status;
  if (gST->ConIn == NULL || gST->ConIn->WaitForKey == NULL) {
    return EFI_UNSUPPORTED;
  }
  do {
    Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Event);
    if (EFI_ERROR (Status)) { return Status; }
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, Key);
  } while (Status == EFI_NOT_READY);
  return Status;
}

EFI_STATUS ConfigReadUsable (
  APP_FILE_CONTEXT *Files, CONST CHAR16 *Name, UINT8 **Bytes, UINTN *Size
  )
{
  NCV_CONFIG_CORE Core;
  NCV_CONFIG_RESULT Result;
  NCV_U8 Missing;
  EFI_STATUS Status = ReadConfigBytesNamed (Files, Name, Bytes, Size);
  if (EFI_ERROR (Status)) {
    if (Status != EFI_NOT_FOUND) { Print (L"%s: read failed (%r).\r\n", Name, Status); }
    return Status;
  }
  NcvConfigDefaults (&Core);
  Result = NcvParseConfigStrict (*Bytes, *Size, &Core);
  if (*Size == 0 || Result != NcvConfigSuccess) {
    Print (L"%s: empty or invalid INI syntax (parser status %u).\r\n", Name, (UINT32)Result);
  } else if (!Core.HasProbe) {
    Print (L"%s: explicit [Behavior] Probe=true or Probe=false is required.\r\n", Name);
  } else if (NcvValidateConfigMode (&Core, &Missing) != NcvConfigSuccess) {
    Print (L"%s: incomplete boot settings (missing-field flags 0x%02x).\r\n", Name, Missing);
    Print (L"Required: GPU, disk controller, and all three endpoint policies.\r\n");
  } else {
    return EFI_SUCCESS;
  }
  FreePool (*Bytes);
  *Bytes = NULL;
  *Size = 0;
  return EFI_COMPROMISED_DATA;
}

EFI_STATUS ConfigLoadRecoverable (APP_FILE_CONTEXT *Files, UINT8 **Bytes, UINTN *Size)
{
  EFI_STATUS Status, BackupStatus;
  UINTN Index;
  EFI_INPUT_KEY Key;
  Status = ConfigReadUsable (Files, ConfigNames[0], Bytes, Size);
  if (!EFI_ERROR (Status)) { return Status; }
  for (Index = 1; Index < ARRAY_SIZE (ConfigNames); ++Index) {
    BackupStatus = ConfigReadUsable (Files, ConfigNames[Index], Bytes, Size);
    if (!EFI_ERROR (BackupStatus)) {
      Print (L"Config.ini could not be loaded (%r).\r\n", Status);
      Print (L"Press R to recover validated %s, or Esc to return.\r\n", ConfigNames[Index]);
      FreePool (*Bytes);
      *Bytes = NULL;
      *Size = 0;
      for (;;) {
        BackupStatus = ReadKey (&Key);
        if (EFI_ERROR (BackupStatus)) { return BackupStatus; }
        if (Key.ScanCode == SCAN_ESC || Key.UnicodeChar == 0x1b) { return EFI_ABORTED; }
        if (Key.UnicodeChar == L'r' || Key.UnicodeChar == L'R') { break; }
      }
      BackupStatus = RestoreAdjacentRollback (Files, ConfigNames[0], ConfigNames[Index]);
      if (EFI_ERROR (BackupStatus)) { return BackupStatus; }
      return ConfigReadUsable (Files, ConfigNames[0], Bytes, Size);
    }
    /* An invalid recovery file must not silently become first-run setup. */
    if (BackupStatus != EFI_NOT_FOUND && Status == EFI_NOT_FOUND) {
      Status = EFI_COMPROMISED_DATA;
    }
  }
  if (Status != EFI_NOT_FOUND) {
    Print (L"No usable Config.ini.previous or Config.ini.tmp was found.\r\n");
  }
  return Status;
}

/* Complete every verified copy before deleting any source. Never overwrite
   earlier evidence. Bounded reads deliberately refuse oversized/unreadable files. */
STATIC EFI_STATUS PreserveFailedConfigs (APP_FILE_CONTEXT *Files)
{
  BOOLEAN Present[3], Exists;
  CHAR16 SavedName[80];
  UINTN Index, Number;
  EFI_STATUS Status;
  for (Index = 0; Index < ARRAY_SIZE (ConfigNames); ++Index) {
    Status = AdjacentFileExists (Files, ConfigNames[Index], &Present[Index]);
    if (EFI_ERROR (Status)) { return Status; }
    if (!Present[Index]) { continue; }
    for (Number = 1; Number <= 99; ++Number) {
      UnicodeSPrint (SavedName, sizeof (SavedName), L"%s.invalid-%02u", ConfigNames[Index], (UINT32)Number);
      Status = AdjacentFileExists (Files, SavedName, &Exists);
      if (EFI_ERROR (Status)) { return Status; }
      if (!Exists) { break; }
    }
    if (Number > 99) { return EFI_VOLUME_FULL; }
    Status = CreateVerifiedRollbackCopy (Files, ConfigNames[Index], SavedName);
    if (EFI_ERROR (Status)) { return Status; }
    Print (L"Preserved and verified: %s\r\n", SavedName);
  }
  for (Index = 0; Index < ARRAY_SIZE (ConfigNames); ++Index) {
    if (Present[Index]) {
      Status = DeleteAdjacentIfPresent (Files, ConfigNames[Index]);
      if (EFI_ERROR (Status)) { return Status; }
    }
  }
  return EFI_SUCCESS;
}

VOID ConfigWaitForReturn (EFI_STATUS Failure)
{
  EFI_INPUT_KEY Key;
  Print (L"\r\nConfiguration/setup stopped: %r\r\nPress a key to return to firmware.\r\n", Failure);
  ReadKey (&Key);
}

EFI_STATUS ConfigOfferFreshSetup (APP_FILE_CONTEXT *Files, EFI_STATUS Failure)
{
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  Print (L"\r\nConfig.ini could not be used: %r\r\n", Failure);
  Print (L"F2: preserve failed files and start fresh target setup\r\nEsc: return without changes\r\n");
  for (;;) {
    Status = ReadKey (&Key);
    if (EFI_ERROR (Status)) { return Status; }
    if (Key.ScanCode == SCAN_ESC || Key.UnicodeChar == 0x1b) { return EFI_ABORTED; }
    if (Key.ScanCode == SCAN_F2) { break; }
  }
  Print (L"Fresh setup resets advanced policies and disables menu markers.\r\n");
  Print (L"Files will be retained as *.invalid-NN. Press Y to confirm, any other key to cancel.\r\n");
  Status = ReadKey (&Key);
  if (EFI_ERROR (Status)) { return Status; }
  if (Key.UnicodeChar != L'y' && Key.UnicodeChar != L'Y') { return EFI_ABORTED; }
  Status = PreserveFailedConfigs (Files);
  if (EFI_ERROR (Status)) { ConfigWaitForReturn (Status); }
  return Status;
}

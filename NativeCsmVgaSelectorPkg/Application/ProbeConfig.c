/** @file
  Strict shared-parser configuration and candidate INI publication.

  Candidate ranking, formatting, parsing, and Boot-required validation live in
  CandidateIni.c so the EFI path and native host tests execute the same rules.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include "ProbeConfig.h"
#include "Marker.h"
#include "ConfigEdit.h"
#include "ConfigFile.h"
#include "ConfigRecovery.h"
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>

#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#include "StatusPrint.h"

#define PROBE_CONFIG_FILE_NAME        L"Config.ini"
#define PROBE_DISCOVERED_FILE_NAME    L"Probe.ini"
#define PROBE_CONFIG_TEMP_FILE_NAME   L"Config.ini.tmp"
#define PROBE_PROBE_TEMP_FILE_NAME    L"Probe.ini.tmp"
#define PROBE_PROBE_ROLLBACK_FILE_NAME L"Probe.ini.previous"
#define PROBE_CONFIG_MAX_BYTES        NCV_CANDIDATE_INI_BYTES

STATIC_ASSERT (
  (INTN)NativeCsmVgaEndpointIgnore == (INTN)NcvEndpointIgnore,
  "Endpoint Ignore policy mismatch"
  );
STATIC_ASSERT (
  (INTN)NativeCsmVgaEndpointOn == (INTN)NcvEndpointOn,
  "Endpoint On policy mismatch"
  );
STATIC_ASSERT (
  (INTN)NativeCsmVgaEndpointOff == (INTN)NcvEndpointOff,
  "Endpoint Off policy mismatch"
  );

STATIC
EFI_STATUS
CoreStatusToEfi (
  IN NCV_CONFIG_RESULT Result
  )
{
  switch (Result) {
    case NcvConfigSuccess:
      return EFI_SUCCESS;
    case NcvConfigInvalidParameter:
      return EFI_INVALID_PARAMETER;
    case NcvConfigUnsupported:
      return EFI_UNSUPPORTED;
    case NcvConfigCompromisedData:
    default:
      return EFI_COMPROMISED_DATA;
  }
}

STATIC
EFI_STATUS
CopyAsciiToUnicode (
  OUT CHAR16       *Destination,
  IN  UINTN        DestinationChars,
  IN  CONST CHAR8  *Source
  )
{
  UINTN Index;

  if ((Destination == NULL) || (DestinationChars == 0) || (Source == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Source[Index] != '\0'; ++Index) {
    if ((Index + 1U >= DestinationChars) ||
        ((UINT8)Source[Index] < 0x20U) ||
        ((UINT8)Source[Index] > 0x7eU))
    {
      return EFI_COMPROMISED_DATA;
    }
    Destination[Index] = (CHAR16)(UINT8)Source[Index];
  }
  Destination[Index] = L'\0';
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ProbeConfigImportCore (
  IN  CONST NCV_CONFIG_CORE  *Core,
  OUT PROBE_CONFIG           *Config
  )
{
  EFI_STATUS Status;

  if ((Core == NULL) || (Config == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Config, sizeof (*Config));
  Config->Present = (BOOLEAN)Core->HasProbe;
  Config->Probe = (BOOLEAN)Core->Probe;
  Config->DiscoveryMode = (BOOLEAN)Core->Probe;
  Config->HasTargetPci = (BOOLEAN)Core->HasTargetPci;
  Config->HasTargetControllerPci = (BOOLEAN)Core->HasTargetControllerPci;
  Config->HasExpectedVendor = (BOOLEAN)Core->HasExpectedVendor;
  Config->HasExpectedDevice = (BOOLEAN)Core->HasExpectedDevice;
  Config->HasExpectedSubsystemVendor =
    (BOOLEAN)Core->HasExpectedSubsystemVendor;
  Config->HasExpectedSubsystemDevice =
    (BOOLEAN)Core->HasExpectedSubsystemDevice;
  Config->HasLegacyOptionDescription =
    (BOOLEAN)Core->HasLegacyOptionDescription;
  Config->HasExcludeDescription = (BOOLEAN)Core->HasExcludeDescription;
  Config->HasTargetBbsDescription =
    (BOOLEAN)Core->HasTargetBbsDescription;
  Config->HasVerboseLog = (BOOLEAN)Core->HasVerboseLog;
  Config->HasAutoBoot = (BOOLEAN)Core->HasAutoBoot;
  Config->HasRequireReferenceSnapshotMatch =
    (BOOLEAN)Core->HasRequireReferenceSnapshotMatch;
  Config->HasIoDecoding = (BOOLEAN)Core->HasIoDecoding;
  Config->HasMemoryDecoding = (BOOLEAN)Core->HasMemoryDecoding;
  Config->HasBusMastering = (BOOLEAN)Core->HasBusMastering;
  Config->VerboseLog = (BOOLEAN)Core->VerboseLog;
  Config->AutoBoot = (BOOLEAN)Core->AutoBoot;
  Config->RequireReferenceSnapshotMatch =
    (BOOLEAN)Core->RequireReferenceSnapshotMatch;
  Config->IoDecoding =
    (NATIVE_CSM_VGA_ENDPOINT_POLICY)Core->IoDecoding;
  Config->MemoryDecoding =
    (NATIVE_CSM_VGA_ENDPOINT_POLICY)Core->MemoryDecoding;
  Config->BusMastering =
    (NATIVE_CSM_VGA_ENDPOINT_POLICY)Core->BusMastering;
  Config->TargetPci.Segment = Core->TargetPci.Segment;
  Config->TargetPci.Bus = Core->TargetPci.Bus;
  Config->TargetPci.Device = Core->TargetPci.Device;
  Config->TargetPci.Function = Core->TargetPci.Function;
  Config->TargetControllerPci.Segment =
    Core->TargetControllerPci.Segment;
  Config->TargetControllerPci.Bus = Core->TargetControllerPci.Bus;
  Config->TargetControllerPci.Device =
    Core->TargetControllerPci.Device;
  Config->TargetControllerPci.Function =
    Core->TargetControllerPci.Function;
  Config->ExpectedVendor = Core->ExpectedVendor;
  Config->ExpectedDevice = Core->ExpectedDevice;
  Config->ExpectedSubsystemVendor = Core->ExpectedSubsystemVendor;
  Config->ExpectedSubsystemDevice = Core->ExpectedSubsystemDevice;

  if (Core->Name[0] != '\0') {
    Status = CopyAsciiToUnicode (
               Config->Name,
               ARRAY_SIZE (Config->Name),
               Core->Name
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  Status = CopyAsciiToUnicode (
             Config->LegacyOptionDescription,
             ARRAY_SIZE (Config->LegacyOptionDescription),
             Core->LegacyOptionDescription
             );
  if (!EFI_ERROR (Status)) {
    Status = CopyAsciiToUnicode (
               Config->ExcludeDescription,
               ARRAY_SIZE (Config->ExcludeDescription),
               Core->ExcludeDescription
               );
  }
  if (!EFI_ERROR (Status) && (Core->TargetBbsDescription[0] != '\0')) {
    Status = CopyAsciiToUnicode (
               Config->TargetBbsDescription,
               ARRAY_SIZE (Config->TargetBbsDescription),
               Core->TargetBbsDescription
               );
  }
  return Status;
}

VOID
ProbeConfigPrintAddress (
  IN APP_LOGGER               *Logger,
  IN CONST CHAR16             *Label,
  IN CONST PROBE_PCI_ADDRESS  *Address
  )
{
  if ((Logger != NULL) && (Label != NULL) && (Address != NULL)) {
    LogPrint (
      Logger,
      L"%s%04x:%02x:%02x.%x\r\n",
      Label,
      Address->Segment,
      Address->Bus,
      Address->Device,
      Address->Function
      );
  }
}

BOOLEAN
ProbeConfigAddressEqual (
  IN CONST PROBE_PCI_ADDRESS  *Left,
  IN CONST PROBE_PCI_ADDRESS  *Right
  )
{
  return (BOOLEAN)((Left != NULL) && (Right != NULL) &&
                   (Left->Segment == Right->Segment) &&
                   (Left->Bus == Right->Bus) &&
                   (Left->Device == Right->Device) &&
                   (Left->Function == Right->Function));
}

VOID
ProbeConfigGetBootRequirements (
  IN  CONST PROBE_CONFIG     *Config,
  OUT NCV_BOOT_REQUIREMENTS  *Requirements
  )
{
  if (Requirements == NULL) {
    return;
  }
  ZeroMem (Requirements, sizeof (*Requirements));
  if (Config == NULL) {
    return;
  }

  Requirements->Probe = (NCV_BOOL)Config->Probe;
  Requirements->HasTargetPci = (NCV_BOOL)Config->HasTargetPci;
  Requirements->HasTargetControllerPci =
    (NCV_BOOL)Config->HasTargetControllerPci;
  Requirements->HasIoDecoding = (NCV_BOOL)Config->HasIoDecoding;
  Requirements->HasMemoryDecoding = (NCV_BOOL)Config->HasMemoryDecoding;
  Requirements->HasBusMastering = (NCV_BOOL)Config->HasBusMastering;
}

STATIC BOOLEAN Editing;
STATIC NCV_CONFIG_CORE ReviewSettings;

VOID ProbeConfigBeginEdit (VOID) { Editing = TRUE; }
VOID ProbeConfigEndEdit (VOID) { Editing = FALSE; }
BOOLEAN ProbeConfigIsEditing (VOID) { return Editing; }
CONST NCV_CONFIG_CORE *ProbeConfigReviewSettings (VOID) { return &ReviewSettings; }

EFI_STATUS
ProbeConfigLoad (
  IN  APP_FILE_CONTEXT  *Files,
  IN  APP_LOGGER        *Logger,
  OUT PROBE_CONFIG      *Config
  )
{
  EFI_STATUS         Status;
  UINT8              *Bytes;
  UINTN              ByteCount;
  NCV_CONFIG_CORE    Core;
  NCV_CONFIG_RESULT  ParseResult;

  if ((Files == NULL) || (Config == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  NcvConfigDefaults (&Core);
  MarkerConfigure (&Core.Marker);
  Bytes = NULL;
  ByteCount = 0;
  Status = ConfigLoadRecoverable (Files, &Bytes, &ByteCount);
  if (Status == EFI_NOT_FOUND) {
    Status = ProbeConfigImportCore (&Core, Config);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Config->FirstRun = TRUE;
    if (Logger != NULL) {
      LogPrint (
        Logger,
        L"Config.ini absent; defaulting to Probe=true\r\n"
        );
    }
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    if (Logger != NULL) {
      LogPrint (
        Logger,
        L"Config.ini read failed: 0x%016lx (%s)\r\n",
        (UINT64)Status,
        EfiStatusName (Status)
        );
    }
    return Status;
  }

  ParseResult = NcvParseConfigStrict (Bytes, ByteCount, &Core);
  FreePool (Bytes);
  Status = CoreStatusToEfi (ParseResult);
  if (!EFI_ERROR (Status)) {
    Status = ProbeConfigImportCore (&Core, Config);
    if (!EFI_ERROR (Status)) {
      MarkerConfigure (&Core.Marker);
      CopyMem (&ReviewSettings, &Core, sizeof (Core));
    }
  }
  if (EFI_ERROR (Status)) {
    if (Logger != NULL) {
      LogPrint (
        Logger,
        L"Config.ini rejected: 0x%016lx (%s)\r\n",
        (UINT64)Status,
        EfiStatusName (Status)
        );
    }
    return Status;
  }

  if (Logger == NULL) {
    return EFI_SUCCESS;
  }
  LogPrint (
    Logger,
    L"Config.ini parsed strictly Probe=%s\r\n",
    Config->Probe ? L"true" : L"false"
    );
  if (Config->HasTargetPci) {
    ProbeConfigPrintAddress (Logger, L"  TargetPci=", &Config->TargetPci);
  }
  if (Config->HasTargetControllerPci) {
    ProbeConfigPrintAddress (
      Logger,
      L"  TargetControllerPci=",
      &Config->TargetControllerPci
      );
  }
  if (Config->HasExpectedVendor) {
    LogPrint (Logger, L"  ExpectedVendor=%04x\r\n", Config->ExpectedVendor);
  }
  if (Config->HasExpectedDevice) {
    LogPrint (Logger, L"  ExpectedDevice=%04x\r\n", Config->ExpectedDevice);
  }
  if (Config->HasExpectedSubsystemVendor) {
    LogPrint (
      Logger,
      L"  ExpectedSubsystemVendor=%04x\r\n",
      Config->ExpectedSubsystemVendor
      );
  }
  if (Config->HasExpectedSubsystemDevice) {
    LogPrint (
      Logger,
      L"  ExpectedSubsystemDevice=%04x\r\n",
      Config->ExpectedSubsystemDevice
      );
  }
  LogPrint (
    Logger,
    L"  LegacyOptionDescription=%s (%s)\r\n",
    Config->LegacyOptionDescription,
    Config->HasLegacyOptionDescription ? L"configured" : L"default"
    );
  LogPrint (
    Logger,
    L"  ExcludeDescription=%s (%s)\r\n",
    Config->ExcludeDescription,
    Config->HasExcludeDescription ? L"configured" : L"default"
    );
  if (Config->HasTargetBbsDescription) {
    LogPrint (
      Logger,
      L"  TargetBbsDescription=%s\r\n",
      Config->TargetBbsDescription
      );
  }
  LogPrint (
    Logger,
    L"  AutoBoot=%s\r\n",
    Config->AutoBoot ? L"true" : L"false"
    );
  LogPrint (
    Logger,
    L"  VerboseLog=%s\r\n",
    Config->VerboseLog ? L"true" : L"false"
    );
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
CoreAddressMatchesCandidate (
  IN CONST NCV_PCI_ADDRESS  *Address,
  IN CONST NCV_PCI_ADDRESS  *Candidate
  )
{
  return (BOOLEAN)(
    (Address != NULL) && (Candidate != NULL) &&
    (Address->Segment == Candidate->Segment) &&
    (Address->Bus == Candidate->Bus) &&
    (Address->Device == Candidate->Device) &&
    (Address->Function == Candidate->Function)
    );
}

STATIC
EFI_STATUS
VerifyGeneratedBytes (
  IN CONST UINT8                  *Bytes,
  IN UINTN                        ByteCount,
  IN BOOLEAN                      Complete,
  IN CONST NCV_DISPLAY_CANDIDATE  *SelectedDisplay OPTIONAL,
  IN CONST NCV_STORAGE_CANDIDATE  *SelectedStorage OPTIONAL
  )
{
  NCV_CONFIG_CORE       Core;
  NCV_CONFIG_RESULT     ParseResult;
  NCV_BOOT_REQUIREMENTS Requirements;

  if ((Bytes == NULL) || (ByteCount == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  NcvConfigDefaults (&Core);
  ParseResult = NcvParseConfigStrict (Bytes, ByteCount, &Core);
  if (ParseResult != NcvConfigSuccess) {
    return CoreStatusToEfi (ParseResult);
  }

  NcvConfigCoreGetBootRequirements (&Core, &Requirements);
  if (Complete) {
    if ((SelectedDisplay == NULL) || (SelectedStorage == NULL) ||
        (NcvBootRequirementsValidate (&Requirements) != 0) ||
        !Core.HasTargetBbsDescription ||
        !CoreAddressMatchesCandidate (
           &Core.TargetPci,
           &SelectedDisplay->Address
           ) ||
        !CoreAddressMatchesCandidate (
           &Core.TargetControllerPci,
           &SelectedStorage->Address
           ) ||
        (AsciiStrCmp (
           Core.TargetBbsDescription,
           SelectedStorage->Description
           ) != 0))
    {
      return EFI_COMPROMISED_DATA;
    }
  } else if (Core.HasTargetPci || Core.HasTargetControllerPci ||
             Core.HasTargetBbsDescription)
  {
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS SaveEditedTargets (
  APP_FILE_CONTEXT *Files, CONST NCV_DISPLAY_CANDIDATE *Display,
  CONST NCV_STORAGE_CANDIDATE *Storage
  )
{
  UINT8 *Original = NULL, *Readback = NULL;
  UINTN Size = 0, ReadSize = 0, NewSize = 0;
  CHAR8 *Text, Gpu[32], Controller[32];
  EFI_STATUS Status;
  BOOLEAN Rollback = FALSE;
  int EditResult;
  Status = ConfigReadUsable (Files, PROBE_CONFIG_FILE_NAME, &Original, &Size);
  if (EFI_ERROR (Status)) { return Status; }
  Text = AllocateZeroPool (PROBE_CONFIG_MAX_BYTES);
  if (Text == NULL) { FreePool (Original); return EFI_OUT_OF_RESOURCES; }
  AsciiSPrint (Gpu, sizeof (Gpu), "%04x:%02x:%02x.%x", Display->Address.Segment,
              Display->Address.Bus, Display->Address.Device, Display->Address.Function);
  AsciiSPrint (Controller, sizeof (Controller), "%04x:%02x:%02x.%x", Storage->Address.Segment,
              Storage->Address.Bus, Storage->Address.Device, Storage->Address.Function);
  EditResult = NcvEditTargets ((CHAR8 *)Original, Size, Gpu, Controller, Storage->Description,
                               Text, PROBE_CONFIG_MAX_BYTES, &NewSize);
  if (EditResult != 0) {
    Status = EditResult == NCV_EDIT_CAPACITY ? EFI_BAD_BUFFER_SIZE : EFI_COMPROMISED_DATA;
    goto Done;
  }
  Status = VerifyGeneratedBytes ((UINT8 *)Text, NewSize, TRUE, Display, Storage);
  if (EFI_ERROR (Status)) { goto Done; }
  /* The current source remains valid while its older recovery copy is rotated. */
  Status = DeleteAdjacentIfPresent (Files, L"Config.ini.previous");
  if (EFI_ERROR (Status)) { goto Done; }
  Status = WriteAdjacentViaTemp (Files, L"Config.ini", L"Config.ini.tmp",
                                L"Config.ini.previous", Text, NewSize, FALSE, &Rollback);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = ConfigReadUsable (Files, L"Config.ini", &Readback, &ReadSize);
  if (!EFI_ERROR (Status) && (ReadSize != NewSize || CompareMem (Readback, Text, NewSize) != 0)) {
    Status = EFI_COMPROMISED_DATA;
  }
  if (!EFI_ERROR (Status)) {
    Print (L"Config.ini saved. Previous valid settings retained in Config.ini.previous.\r\n");
  }
Done:
  if (EFI_ERROR (Status)) {
    Print (L"Settings save failed (%r). Recovery files retained; check configuration before boot.\r\n", Status);
  }
  if (Readback != NULL) { FreePool (Readback); }
  FreePool (Original);
  FreePool (Text);
  return Status;
}

EFI_STATUS
ProbeConfigWriteCandidateIni (
  IN APP_FILE_CONTEXT                 *Files,
  IN APP_LOGGER                       *Logger,
  IN CONST NCV_DISPLAY_CANDIDATE      *DisplayCandidates,
  IN UINTN                            DisplayCandidateCount,
  IN UINTN                            SelectedDisplayIndex,
  IN CONST NCV_STORAGE_CANDIDATE      *StorageCandidates,
  IN UINTN                            StorageCandidateCount,
  IN UINTN                            SelectedStorageIndex,
  IN BOOLEAN                          FirstRun
  )
{
  CHAR8                        *Text;
  UINTN                        TextSize;
  BOOLEAN                      Complete;
  NCV_OUTPUT_KIND              OutputKind;
  CONST CHAR16                 *FileName;
  CONST CHAR16                 *TempName;
  CONST CHAR16                 *RollbackName;
  CONST NCV_DISPLAY_CANDIDATE  *SelectedDisplay;
  CONST NCV_STORAGE_CANDIDATE  *SelectedStorage;
  EFI_STATUS                   Status;
  UINT8                        *Readback;
  UINTN                        ReadbackSize;
  PROBE_CONFIG                 Parsed;
  NCV_BOOT_REQUIREMENTS        Requirements;
  BOOLEAN                      RollbackPreserved;

  if ((Files == NULL) || (Logger == NULL) ||
      (DisplayCandidates == NULL) || (StorageCandidates == NULL) ||
      (DisplayCandidateCount > NCV_MAX_DISPLAY_CANDIDATES) ||
      (StorageCandidateCount > NCV_MAX_STORAGE_CANDIDATES) ||
      ((SelectedDisplayIndex != NCV_NO_SELECTION) &&
       (SelectedDisplayIndex >= DisplayCandidateCount)) ||
      ((SelectedStorageIndex != NCV_NO_SELECTION) &&
       (SelectedStorageIndex >= StorageCandidateCount)))
  {
    return EFI_INVALID_PARAMETER;
  }

  Complete = (BOOLEAN)(
    (SelectedDisplayIndex != NCV_NO_SELECTION) &&
    (SelectedStorageIndex != NCV_NO_SELECTION)
    );
  OutputKind = NcvChooseOutputKind (
                 (NCV_BOOL)FirstRun,
                 SelectedDisplayIndex,
                 SelectedStorageIndex
                 );
  FileName = (OutputKind == NcvOutputActiveIni) ?
             PROBE_CONFIG_FILE_NAME : PROBE_DISCOVERED_FILE_NAME;
  TempName = (OutputKind == NcvOutputActiveIni) ?
             PROBE_CONFIG_TEMP_FILE_NAME : PROBE_PROBE_TEMP_FILE_NAME;
  RollbackName = (OutputKind == NcvOutputActiveIni) ?
                 NULL : PROBE_PROBE_ROLLBACK_FILE_NAME;
  SelectedDisplay = Complete ?
                    &DisplayCandidates[SelectedDisplayIndex] : NULL;
  SelectedStorage = Complete ?
                    &StorageCandidates[SelectedStorageIndex] : NULL;

  if (SelectedDisplayIndex == NCV_NO_SELECTION) {
    LogPrint (
      Logger,
      L"No eligible display candidate; boot-ready Config.ini will not be created\r\n"
      );
  }
  if (SelectedStorageIndex == NCV_NO_SELECTION) {
    LogPrint (
      Logger,
      L"No eligible storage candidate; boot-ready Config.ini will not be created\r\n"
      );
  }

  if (Editing) {
    if (!Complete) return EFI_NOT_FOUND;
    return SaveEditedTargets(Files, SelectedDisplay, SelectedStorage);
  }

  Text = AllocateZeroPool (NCV_CANDIDATE_INI_BYTES);
  if (Text == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  TextSize = 0;
  if (NcvFormatCandidateIni (
        DisplayCandidates,
        DisplayCandidateCount,
        SelectedDisplayIndex,
        StorageCandidates,
        StorageCandidateCount,
        SelectedStorageIndex,
        Text,
        NCV_CANDIDATE_INI_BYTES,
        &TextSize
        ) != 0)
  {
    FreePool (Text);
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = VerifyGeneratedBytes (
             (CONST UINT8 *)Text,
             TextSize,
             Complete,
             SelectedDisplay,
             SelectedStorage
             );
  if (EFI_ERROR (Status)) {
    FreePool (Text);
    return Status;
  }

  Status = WriteAdjacentViaTemp (
             Files,
             FileName,
             TempName,
             RollbackName,
             Text,
             TextSize,
             (BOOLEAN)(OutputKind == NcvOutputActiveIni),
             &RollbackPreserved
             );
  if (EFI_ERROR (Status)) {
    EFI_STATUS  OriginalStatus;
    EFI_STATUS  RecoveryStatus;

    OriginalStatus = Status;
    if (RollbackPreserved) {
      RecoveryStatus = RestoreAdjacentRollback (
                         Files,
                         FileName,
                         RollbackName
                         );
      if (!EFI_ERROR (RecoveryStatus)) {
        RecoveryStatus = DeleteAdjacentIfPresent (Files, RollbackName);
      }
      if (EFI_ERROR (RecoveryStatus)) {
        FreePool (Text);
        return RecoveryStatus;
      }
    }
    FreePool (Text);
    return OriginalStatus;
  }

  Readback = NULL;
  ReadbackSize = 0;
  Status = ReadConfigBytesNamed (
             Files,
             FileName,
             &Readback,
             &ReadbackSize
             );
  if (!EFI_ERROR (Status) &&
      ((ReadbackSize != TextSize) ||
       (CompareMem (Readback, Text, TextSize) != 0)))
  {
    Status = EFI_COMPROMISED_DATA;
  }
  if (!EFI_ERROR (Status)) {
    Status = VerifyGeneratedBytes (
               Readback,
               ReadbackSize,
               Complete,
               SelectedDisplay,
               SelectedStorage
               );
  }
  FreePool (Text);
  if (Readback != NULL) {
    FreePool (Readback);
  }
  if (EFI_ERROR (Status)) {
    EFI_STATUS CleanupStatus;

    if (RollbackPreserved) {
      CleanupStatus = RestoreAdjacentRollback (
                        Files,
                        FileName,
                        RollbackName
                        );
      if (!EFI_ERROR (CleanupStatus)) {
        CleanupStatus = DeleteAdjacentIfPresent (Files, RollbackName);
      }
    } else {
      CleanupStatus = DeleteAdjacentIfPresent (Files, FileName);
    }
    LogPrint (
      Logger,
      L"NCV_GENERATED_INI_REPARSE_FAILED\r\n"
      );
    return EFI_ERROR (CleanupStatus) ? CleanupStatus : Status;
  }

  if (RollbackPreserved) {
    Status = DeleteAdjacentIfPresent (Files, RollbackName);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (OutputKind == NcvOutputActiveIni) {
    ZeroMem (&Parsed, sizeof (Parsed));
    Status = ProbeConfigLoad (Files, Logger, &Parsed);
    ProbeConfigGetBootRequirements (&Parsed, &Requirements);
    if (EFI_ERROR (Status) ||
        (NcvBootRequirementsValidate (&Requirements) != 0) ||
        !Parsed.HasTargetBbsDescription)
    {
      EFI_STATUS CleanupStatus;

      CleanupStatus = DeleteAdjacentIfPresent (Files, FileName);
      LogPrint (
        Logger,
        L"NCV_FIRST_RUN_INI_REPARSE_FAILED\r\n"
        );
      if (EFI_ERROR (CleanupStatus)) {
        return CleanupStatus;
      }
      return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
    }
    LogPrint (
      Logger,
      L"NCV_FIRST_RUN_ACTIVE_INI_GENERATED\r\n"
      );
    LogPrint (
      Logger,
      L"NCV_GENERATED_INI_REPARSE_PASS\r\n"
      );
    Status = CreateVerifiedRollbackCopy(Files, PROBE_CONFIG_FILE_NAME, L"Config.ini.previous");
    if (EFI_ERROR(Status)) return Status;
    LogPrint (Logger, L"NCV_SAVED_CONFIG_MODE=BOOT\r\n");
  } else {
    LogPrint (
      Logger,
      L"NCV_GENERATED_INI_REPARSE_PASS\r\n"
      );
    if (FirstRun) {
      LogPrint (
        Logger,
        L"NCV_FIRST_RUN_INI_REQUIRES_USER_SELECTION\r\n"
        );
    } else {
      LogPrint (
        Logger,
        L"NCV_EXISTING_ACTIVE_INI_PRESERVED\r\n"
        );
      LogPrint (Logger, L"NCV_PROBE_INI_GENERATED\r\n");
    }
  }
  return EFI_SUCCESS;
}

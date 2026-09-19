/** @file Verified adjacent-file operations.
  SPDX-License-Identifier: LGPL-2.1-or-later
**/
#include "ConfigFile.h"
#include "CandidateIni.h"
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#define PROBE_CONFIG_MAX_BYTES NCV_CANDIDATE_INI_BYTES

EFI_STATUS
ReadConfigBytesNamed (
  IN  APP_FILE_CONTEXT  *Files,
  IN  CONST CHAR16      *FileName,
  OUT UINT8             **Buffer,
  OUT UINTN             *BufferSize
  )
{
  EFI_STATUS         Status, CloseStatus;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              Size;
  UINTN              ReadSize;
  UINT8              *Result;

  if ((Files == NULL) || (FileName == NULL) ||
      (Buffer == NULL) || (BufferSize == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Buffer = NULL;
  *BufferSize = 0;
  File = NULL;
  Info = NULL;
  InfoSize = 0;
  Status = AppFileOpenAdjacent (
             Files,
             FileName,
             EFI_FILE_MODE_READ,
             0,
             &File
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if ((Status != EFI_BUFFER_TOO_SMALL) ||
      (InfoSize < SIZE_OF_EFI_FILE_INFO))
  {
    File->Close (File);
    return EFI_COMPROMISED_DATA;
  }

  Info = AllocateZeroPool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status) || (Info->FileSize > PROBE_CONFIG_MAX_BYTES)) {
    FreePool (Info);
    File->Close (File);
    return EFI_ERROR (Status) ? Status : EFI_BAD_BUFFER_SIZE;
  }

  Size = (UINTN)Info->FileSize;
  FreePool (Info);
  Result = AllocateZeroPool (Size + 1U);
  if (Result == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  ReadSize = Size;
  Status = (Size == 0) ? EFI_SUCCESS :
           File->Read (File, &ReadSize, Result);
  CloseStatus = File->Close (File);
  if (!EFI_ERROR (Status)) { Status = CloseStatus; }
  if (EFI_ERROR (Status) || (ReadSize != Size)) {
    FreePool (Result);
    return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
  }

  *Buffer = Result;
  *BufferSize = Size;
  return EFI_SUCCESS;
}

EFI_STATUS
AdjacentFileExists (
  IN  APP_FILE_CONTEXT  *Files,
  IN  CONST CHAR16      *FileName,
  OUT BOOLEAN           *Exists
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;

  if ((Files == NULL) || (FileName == NULL) || (Exists == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Exists = FALSE;
  File = NULL;
  Status = AppFileOpenAdjacent (
             Files,
             FileName,
             EFI_FILE_MODE_READ,
             0,
             &File
             );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  *Exists = TRUE;
  return File->Close (File);
}

EFI_STATUS
DeleteAdjacentIfPresent (
  IN APP_FILE_CONTEXT  *Files,
  IN CONST CHAR16      *FileName
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  BOOLEAN            Exists;

  File = NULL;
  Status = AppFileOpenAdjacent (
             Files,
             FileName,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
             0,
             &File
             );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = File->Delete (File);
  File = NULL;
  if (Status != EFI_SUCCESS) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Status = AdjacentFileExists (Files, FileName, &Exists);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Exists ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
EFI_STATUS
WriteAll (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST CHAR8        *Text,
  IN UINTN              TextSize
  )
{
  EFI_STATUS Status;
  UINTN      Size;

  if ((File == NULL) || (Text == NULL) || (TextSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Size = TextSize;
  Status = File->Write (File, &Size, (VOID *)Text);
  if (EFI_ERROR (Status) || (Size != TextSize)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }
  return File->Flush (File);
}

STATIC
EFI_STATUS
VerifyOpenFileBytes (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST CHAR8        *Expected,
  IN UINTN              ExpectedSize
  )
{
  EFI_STATUS  Status;
  UINT8       *Readback;
  UINTN       ReadSize;

  if ((File == NULL) || (Expected == NULL) || (ExpectedSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Readback = AllocateZeroPool (ExpectedSize);
  if (Readback == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = File->SetPosition (File, 0);
  if (!EFI_ERROR (Status)) {
    ReadSize = ExpectedSize;
    Status = File->Read (File, &ReadSize, Readback);
    if (!EFI_ERROR (Status) &&
        ((ReadSize != ExpectedSize) ||
         (CompareMem (Readback, Expected, ExpectedSize) != 0)))
    {
      Status = EFI_COMPROMISED_DATA;
    }
  }
  FreePool (Readback);
  return Status;
}

STATIC
EFI_STATUS
RenameOpenAdjacent (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST CHAR16       *NewName
  )
{
  EFI_STATUS      Status;
  CONST CHAR16    *NormalizedName;
  EFI_FILE_INFO   *OldInfo;
  EFI_FILE_INFO   *NewInfo;
  UINTN           OldInfoSize;
  UINTN           NewInfoSize;
  UINTN           NameChars;

  if ((File == NULL) || (NewName == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = AppFileValidateBaseName (NewName, &NormalizedName);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  OldInfo = NULL;
  OldInfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &OldInfoSize, NULL);
  if ((Status != EFI_BUFFER_TOO_SMALL) ||
      (OldInfoSize < SIZE_OF_EFI_FILE_INFO))
  {
    return EFI_COMPROMISED_DATA;
  }

  OldInfo = AllocateZeroPool (OldInfoSize);
  if (OldInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = File->GetInfo (
                   File,
                   &gEfiFileInfoGuid,
                   &OldInfoSize,
                   OldInfo
                   );
  if (EFI_ERROR (Status)) {
    FreePool (OldInfo);
    return Status;
  }

  NameChars = StrLen (NormalizedName) + 1U;
  NewInfoSize = SIZE_OF_EFI_FILE_INFO + (NameChars * sizeof (CHAR16));
  NewInfo = AllocateZeroPool (NewInfoSize);
  if (NewInfo == NULL) {
    FreePool (OldInfo);
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (NewInfo, OldInfo, SIZE_OF_EFI_FILE_INFO);
  NewInfo->Size = NewInfoSize;
  Status = StrCpyS (
             NewInfo->FileName,
             NameChars,
             NormalizedName
             );
  if (!EFI_ERROR (Status)) {
    Status = File->SetInfo (
                     File,
                     &gEfiFileInfoGuid,
                     NewInfoSize,
                     NewInfo
                     );
  }
  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }
  FreePool (NewInfo);
  FreePool (OldInfo);
  return Status;
}

EFI_STATUS
CreateVerifiedRollbackCopy (
  IN APP_FILE_CONTEXT  *Files,
  IN CONST CHAR16      *TargetName,
  IN CONST CHAR16      *RollbackName
  )
{
  EFI_STATUS         Status;
  EFI_STATUS         CleanupStatus;
  EFI_FILE_PROTOCOL  *Rollback;
  UINT8              *Original;
  UINTN              OriginalSize;
  UINT8              *Readback;
  UINTN              ReadbackSize;
  BOOLEAN            Exists;

  if ((Files == NULL) || (TargetName == NULL) || (RollbackName == NULL) ||
      (StrCmp (TargetName, RollbackName) == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = AdjacentFileExists (Files, RollbackName, &Exists);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Exists) {
    return EFI_ACCESS_DENIED;
  }

  Original = NULL;
  OriginalSize = 0;
  Status = ReadConfigBytesNamed (
             Files,
             TargetName,
             &Original,
             &OriginalSize
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Rollback = NULL;
  Status = AppFileOpenAdjacent (
             Files,
             RollbackName,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
             0,
             &Rollback
             );
  if (EFI_ERROR (Status)) {
    FreePool (Original);
    return Status;
  }
  Status = (OriginalSize == 0) ?
           Rollback->Flush (Rollback) :
           WriteAll (
             Rollback,
             (CONST CHAR8 *)Original,
             OriginalSize
             );
  if (!EFI_ERROR (Status) && (OriginalSize != 0)) {
    Status = VerifyOpenFileBytes (
               Rollback,
               (CONST CHAR8 *)Original,
               OriginalSize
               );
  }
  if (EFI_ERROR (Status)) {
    CleanupStatus = Rollback->Delete (Rollback);
    Rollback = NULL;
    FreePool (Original);
    return EFI_ERROR (CleanupStatus) ? CleanupStatus : Status;
  }
  Status = Rollback->Close (Rollback);
  Rollback = NULL;
  if (EFI_ERROR (Status)) {
    FreePool (Original);
    return Status;
  }

  Readback = NULL;
  ReadbackSize = 0;
  Status = ReadConfigBytesNamed (
             Files,
             RollbackName,
             &Readback,
             &ReadbackSize
             );
  if (!EFI_ERROR (Status) &&
      ((ReadbackSize != OriginalSize) ||
       (CompareMem (Readback, Original, OriginalSize) != 0)))
  {
    Status = EFI_COMPROMISED_DATA;
  }
  if (Readback != NULL) {
    FreePool (Readback);
  }
  FreePool (Original);
  if (EFI_ERROR (Status)) {
    CleanupStatus = DeleteAdjacentIfPresent (Files, RollbackName);
    return EFI_ERROR (CleanupStatus) ? CleanupStatus : Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
RestoreAdjacentRollback (
  IN APP_FILE_CONTEXT  *Files,
  IN CONST CHAR16      *TargetName,
  IN CONST CHAR16      *RollbackName
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Target;
  UINT8              *Original;
  UINTN              OriginalSize;
  UINT8              *Readback;
  UINTN              ReadbackSize;

  if ((Files == NULL) || (TargetName == NULL) || (RollbackName == NULL) ||
      (StrCmp (TargetName, RollbackName) == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Original = NULL;
  OriginalSize = 0;
  Status = ReadConfigBytesNamed (
             Files,
             RollbackName,
             &Original,
             &OriginalSize
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = DeleteAdjacentIfPresent (Files, TargetName);
  if (EFI_ERROR (Status)) {
    FreePool (Original);
    return Status;
  }

  Target = NULL;
  Status = AppFileOpenAdjacent (
             Files,
             TargetName,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
             0,
             &Target
             );
  if (EFI_ERROR (Status)) {
    FreePool (Original);
    return Status;
  }
  Status = (OriginalSize == 0) ?
           Target->Flush (Target) :
           WriteAll (Target, (CONST CHAR8 *)Original, OriginalSize);
  if (!EFI_ERROR (Status) && (OriginalSize != 0)) {
    Status = VerifyOpenFileBytes (
               Target,
               (CONST CHAR8 *)Original,
               OriginalSize
               );
  }
  if (EFI_ERROR (Status)) {
    Target->Close (Target);
    FreePool (Original);
    return Status;
  }
  Status = Target->Close (Target);
  Target = NULL;
  if (EFI_ERROR (Status)) {
    FreePool (Original);
    return Status;
  }

  Readback = NULL;
  ReadbackSize = 0;
  Status = ReadConfigBytesNamed (
             Files,
             TargetName,
             &Readback,
             &ReadbackSize
             );
  if (!EFI_ERROR (Status) &&
      ((ReadbackSize != OriginalSize) ||
       (CompareMem (Readback, Original, OriginalSize) != 0)))
  {
    Status = EFI_COMPROMISED_DATA;
  }
  if (Readback != NULL) {
    FreePool (Readback);
  }
  FreePool (Original);
  return Status;
}

EFI_STATUS
WriteAdjacentViaTemp (
  IN APP_FILE_CONTEXT  *Files,
  IN CONST CHAR16      *TargetName,
  IN CONST CHAR16      *TempName,
  IN CONST CHAR16      *RollbackName OPTIONAL,
  IN CONST CHAR8       *Text,
  IN UINTN             TextSize,
  IN BOOLEAN           TargetMustNotExist,
  OUT BOOLEAN          *RollbackPreserved
  )
{
  EFI_STATUS         Status;
  EFI_STATUS         CleanupStatus;
  EFI_FILE_PROTOCOL  *Temp;
  BOOLEAN            Exists;
  BOOLEAN            RollbackExists;
  BOOLEAN            RollbackCreated;

  if ((Files == NULL) || (TargetName == NULL) || (TempName == NULL) ||
      (Text == NULL) || (TextSize == 0) || (RollbackPreserved == NULL) ||
      (StrCmp (TargetName, TempName) == 0) ||
      (!TargetMustNotExist &&
       ((RollbackName == NULL) ||
        (StrCmp (TargetName, RollbackName) == 0) ||
        (StrCmp (TempName, RollbackName) == 0))))
  {
    return EFI_INVALID_PARAMETER;
  }
  *RollbackPreserved = FALSE;

  Status = AdjacentFileExists (Files, TargetName, &Exists);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (TargetMustNotExist && Exists) {
    return EFI_ACCESS_DENIED;
  }
  if (!TargetMustNotExist) {
    Status = AdjacentFileExists (Files, RollbackName, &RollbackExists);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (RollbackExists) {
      return EFI_ACCESS_DENIED;
    }
  }

  Status = DeleteAdjacentIfPresent (Files, TempName);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Temp = NULL;
  Status = AppFileOpenAdjacent (
             Files,
             TempName,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
             0,
             &Temp
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = WriteAll (Temp, Text, TextSize);
  if (EFI_ERROR (Status)) {
    CleanupStatus = Temp->Delete (Temp);
    Temp = NULL;
    return EFI_ERROR (CleanupStatus) ? CleanupStatus : Status;
  }
  Status = VerifyOpenFileBytes (Temp, Text, TextSize);
  if (EFI_ERROR (Status)) {
    CleanupStatus = Temp->Delete (Temp);
    Temp = NULL;
    return EFI_ERROR (CleanupStatus) ? CleanupStatus : Status;
  }

  RollbackCreated = FALSE;
  if (TargetMustNotExist) {
    Status = AdjacentFileExists (Files, TargetName, &Exists);
    if (EFI_ERROR (Status) || Exists) {
      CleanupStatus = Temp->Delete (Temp);
      Temp = NULL;
      if (EFI_ERROR (Status)) {
        return Status;
      }
      if (CleanupStatus != EFI_SUCCESS) {
        return EFI_ERROR (CleanupStatus) ?
               CleanupStatus : EFI_DEVICE_ERROR;
      }
      return EFI_ACCESS_DENIED;
    }
  } else if (Exists) {
    Status = CreateVerifiedRollbackCopy (
               Files,
               TargetName,
               RollbackName
               );
    if (EFI_ERROR (Status)) {
      CleanupStatus = Temp->Delete (Temp);
      Temp = NULL;
      return EFI_ERROR (CleanupStatus) ? CleanupStatus : Status;
    }
    RollbackCreated = TRUE;
    *RollbackPreserved = TRUE;
    Status = DeleteAdjacentIfPresent (Files, TargetName);
    if (EFI_ERROR (Status)) {
      Temp->Delete (Temp);
      Temp = NULL;
      return Status;
    }
  }

  Status = RenameOpenAdjacent (Temp, TargetName);
  if (EFI_ERROR (Status)) {
    CleanupStatus = Temp->Delete (Temp);
    Temp = NULL;
    return EFI_ERROR (CleanupStatus) ? CleanupStatus : Status;
  }
  Status = Temp->Close (Temp);
  Temp = NULL;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AdjacentFileExists (Files, TargetName, &Exists);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (!Exists) {
    return EFI_DEVICE_ERROR;
  }
  *RollbackPreserved = RollbackCreated;
  return EFI_SUCCESS;
}


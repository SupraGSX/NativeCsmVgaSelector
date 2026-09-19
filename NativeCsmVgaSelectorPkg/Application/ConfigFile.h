/** SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef NCV_CONFIG_FILE_H
#define NCV_CONFIG_FILE_H
#include "AppFile.h"

EFI_STATUS
ReadConfigBytesNamed (
  IN  APP_FILE_CONTEXT  *Files,
  IN  CONST CHAR16      *FileName,
  OUT UINT8             **Buffer,
  OUT UINTN             *BufferSize
  );

EFI_STATUS
AdjacentFileExists (
  IN  APP_FILE_CONTEXT  *Files,
  IN  CONST CHAR16      *FileName,
  OUT BOOLEAN           *Exists
  );

EFI_STATUS
DeleteAdjacentIfPresent (
  IN APP_FILE_CONTEXT  *Files,
  IN CONST CHAR16      *FileName
  );

EFI_STATUS
CreateVerifiedRollbackCopy (
  IN APP_FILE_CONTEXT  *Files,
  IN CONST CHAR16      *TargetName,
  IN CONST CHAR16      *RollbackName
  );

EFI_STATUS
RestoreAdjacentRollback (
  IN APP_FILE_CONTEXT  *Files,
  IN CONST CHAR16      *TargetName,
  IN CONST CHAR16      *RollbackName
  );

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
  );
#endif

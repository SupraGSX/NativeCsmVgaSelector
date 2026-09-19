/** @file
  Console, serial, and executable-relative UTF-8 file logging.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include "Log.h"
#include "BootReport.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC
EFI_STATUS
RecordFirstError (
  IN EFI_STATUS  Current,
  IN EFI_STATUS  Candidate
  )
{
  if (!EFI_ERROR (Current) && EFI_ERROR (Candidate)) {
    return Candidate;
  }

  return Current;
}

STATIC
EFI_STATUS
WriteConsoleTargets (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Text,
  IN BOOLEAN       IncludeCurrentSystemConsoles
  )
{
  EFI_STATUS                       Status;
  EFI_STATUS                       SinkStatus;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *CurrentConOut;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *CurrentStdErr;

  if ((Logger == NULL) || (Text == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status        = EFI_SUCCESS;
  CurrentConOut = (gST != NULL) ? gST->ConOut : NULL;
  CurrentStdErr = (gST != NULL) ? gST->StdErr : NULL;
  if (Logger->Console != NULL) {
    SinkStatus = Logger->Console->OutputString (Logger->Console, (CHAR16 *)Text);
    Status     = RecordFirstError (Status, SinkStatus);
  }

  if (IncludeCurrentSystemConsoles && (CurrentConOut != NULL) &&
      (CurrentConOut != Logger->Console))
  {
    SinkStatus = CurrentConOut->OutputString (CurrentConOut, (CHAR16 *)Text);
    Status     = RecordFirstError (Status, SinkStatus);
  }

  if (IncludeCurrentSystemConsoles && (CurrentStdErr != NULL) &&
      (CurrentStdErr != Logger->Console) &&
      (CurrentStdErr != CurrentConOut))
  {
    SinkStatus = CurrentStdErr->OutputString (CurrentStdErr, (CHAR16 *)Text);
    Status     = RecordFirstError (Status, SinkStatus);
  }

  return Status;
}

STATIC
EFI_STATUS
UnicodeToUtf8 (
  IN  CONST CHAR16  *Text,
  OUT UINT8         **Utf8,
  OUT UINTN         *Utf8Size
  )
{
  UINTN   Length;
  UINTN   Input;
  UINTN   Output;
  UINT32  CodePoint;
  CHAR16  Character;
  CHAR16  LowSurrogate;
  UINT8   *Result;

  if ((Text == NULL) || (Utf8 == NULL) || (Utf8Size == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Utf8     = NULL;
  *Utf8Size = 0;
  Length    = StrLen (Text);
  if (Length > (MAX_UINTN - 1U) / 3U) {
    BootReportLogFailure (EFI_OUT_OF_RESOURCES);
    return EFI_OUT_OF_RESOURCES;
  }

  Result = AllocatePool (Length * 3U + 1U);
  if (Result == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Output = 0;
  for (Input = 0; Input < Length; ++Input) {
    Character = Text[Input];
    if ((Character >= 0xD800U) && (Character <= 0xDBFFU) &&
        (Input + 1U < Length))
    {
      LowSurrogate = Text[Input + 1U];
      if ((LowSurrogate >= 0xDC00U) && (LowSurrogate <= 0xDFFFU)) {
        CodePoint = 0x10000U +
                    (((UINT32)Character - 0xD800U) << 10) +
                    ((UINT32)LowSurrogate - 0xDC00U);
        ++Input;
      } else {
        CodePoint = 0xFFFDU;
      }
    } else if ((Character >= 0xD800U) && (Character <= 0xDFFFU)) {
      CodePoint = 0xFFFDU;
    } else {
      CodePoint = Character;
    }

    if (CodePoint < 0x80U) {
      Result[Output++] = (UINT8)CodePoint;
    } else if (CodePoint < 0x800U) {
      Result[Output++] = (UINT8)(0xC0U | (CodePoint >> 6));
      Result[Output++] = (UINT8)(0x80U | (CodePoint & 0x3FU));
    } else if (CodePoint < 0x10000U) {
      Result[Output++] = (UINT8)(0xE0U | (CodePoint >> 12));
      Result[Output++] = (UINT8)(0x80U | ((CodePoint >> 6) & 0x3FU));
      Result[Output++] = (UINT8)(0x80U | (CodePoint & 0x3FU));
    } else {
      Result[Output++] = (UINT8)(0xF0U | (CodePoint >> 18));
      Result[Output++] = (UINT8)(0x80U | ((CodePoint >> 12) & 0x3FU));
      Result[Output++] = (UINT8)(0x80U | ((CodePoint >> 6) & 0x3FU));
      Result[Output++] = (UINT8)(0x80U | (CodePoint & 0x3FU));
    }
  }

  Result[Output] = 0;
  *Utf8          = Result;
  *Utf8Size      = Output;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
WriteSerialAll (
  IN EFI_SERIAL_IO_PROTOCOL  *Serial,
  IN CONST UINT8             *Buffer,
  IN UINTN                   BufferSize
  )
{
  EFI_STATUS  Status;
  UINTN       Written;
  UINTN       Offset;

  if ((Serial == NULL) || ((Buffer == NULL) && (BufferSize != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  Offset = 0;
  while (Offset < BufferSize) {
    Written = BufferSize - Offset;
    Status  = Serial->Write (Serial, &Written, (VOID *)(Buffer + Offset));
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Written == 0) {
      return EFI_DEVICE_ERROR;
    }

    Offset += Written;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
WriteFileAll (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST UINT8        *Buffer,
  IN UINTN              BufferSize
  )
{
  EFI_STATUS  Status;
  UINTN       Written;
  UINTN       Offset;

  if ((File == NULL) || ((Buffer == NULL) && (BufferSize != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  Offset = 0;
  while (Offset < BufferSize) {
    Written = BufferSize - Offset;
    Status  = File->Write (File, &Written, (VOID *)(Buffer + Offset));
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Written == 0) {
      return EFI_DEVICE_ERROR;
    }

    Offset += Written;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
LogVPrintInternal (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Format,
  IN VA_LIST       Marker,
  IN BOOLEAN       MirrorDespiteFileFailure
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  SinkStatus;
  EFI_STATUS  MirrorStatus;
  CHAR16      *Formatted;
  UINT8       *Utf8;
  UINTN       Utf8Size;

  if ((Logger == NULL) || (Format == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Formatted = CatVSPrint (NULL, Format, Marker);
  if (Formatted == NULL) {
    Logger->FirstWriteError = RecordFirstError (
                                Logger->FirstWriteError,
                                EFI_OUT_OF_RESOURCES
                                );
    if (Logger->DurableFileFirst || (Logger->File != NULL)) {
      Logger->FirstFileError = RecordFirstError (
                                 Logger->FirstFileError,
                                 EFI_OUT_OF_RESOURCES
                                 );
    }

    BootReportLogFailure (EFI_OUT_OF_RESOURCES);
    return EFI_OUT_OF_RESOURCES;
  }

  BootReportObserve (Formatted);
  Status = EFI_SUCCESS;
  Utf8     = NULL;
  Utf8Size = 0;
  if ((Logger->Serial != NULL) || (Logger->File != NULL)) {
    SinkStatus = UnicodeToUtf8 (Formatted, &Utf8, &Utf8Size);
    Status     = RecordFirstError (Status, SinkStatus);
    if (EFI_ERROR (SinkStatus) && (Logger->File != NULL)) {
      Logger->FirstFileError = RecordFirstError (
                                 Logger->FirstFileError,
                                 SinkStatus
                                 );
    }

    if (EFI_ERROR (SinkStatus) && MirrorDespiteFileFailure &&
        ((Logger->Console != NULL) ||
         ((gST != NULL) &&
          ((gST->ConOut != NULL) || (gST->StdErr != NULL)))))
    {
      MirrorStatus = WriteConsoleTargets (Logger, Formatted, TRUE);
      Status       = RecordFirstError (Status, MirrorStatus);
    }

    if (!EFI_ERROR (SinkStatus)) {
      if (Logger->DurableFileFirst) {
        SinkStatus = WriteFileAll (Logger->File, Utf8, Utf8Size);
        Status     = RecordFirstError (Status, SinkStatus);
        Logger->FirstFileError = RecordFirstError (
                                   Logger->FirstFileError,
                                   SinkStatus
                                   );
        if (!EFI_ERROR (SinkStatus)) {
          SinkStatus = Logger->File->Flush (Logger->File);
          Status     = RecordFirstError (Status, SinkStatus);
          Logger->FirstFileError = RecordFirstError (
                                     Logger->FirstFileError,
                                     SinkStatus
                                     );
        }

        // Before the irreversible boundary, do not display a transaction
        // record unless that record is durable.  After the boundary there is
        // no safe rollback/return decision left to protect, so the dedicated
        // best-effort API still reports to every surviving output sink.
        if (MirrorDespiteFileFailure ||
            !EFI_ERROR (Logger->FirstFileError))
        {
          SinkStatus = WriteConsoleTargets (
                         Logger,
                         Formatted,
                         MirrorDespiteFileFailure
                         );
          Status = RecordFirstError (Status, SinkStatus);

          if (Logger->Serial != NULL) {
            SinkStatus = WriteSerialAll (Logger->Serial, Utf8, Utf8Size);
            Status     = RecordFirstError (Status, SinkStatus);
          }
        }
      } else {
        SinkStatus = WriteConsoleTargets (
                       Logger,
                       Formatted,
                       MirrorDespiteFileFailure
                       );
        Status = RecordFirstError (Status, SinkStatus);

        if (Logger->Serial != NULL) {
          SinkStatus = WriteSerialAll (Logger->Serial, Utf8, Utf8Size);
          Status     = RecordFirstError (Status, SinkStatus);
        }

        if (Logger->File != NULL) {
          SinkStatus = WriteFileAll (Logger->File, Utf8, Utf8Size);
          Status     = RecordFirstError (Status, SinkStatus);
          Logger->FirstFileError = RecordFirstError (
                                     Logger->FirstFileError,
                                     SinkStatus
                                     );
        }
      }
    }
  } else if ((Logger->Console != NULL) ||
             (MirrorDespiteFileFailure && (gST != NULL) &&
              ((gST->ConOut != NULL) || (gST->StdErr != NULL))))
  {
    SinkStatus = WriteConsoleTargets (
                   Logger,
                   Formatted,
                   MirrorDespiteFileFailure
                   );
    Status     = RecordFirstError (Status, SinkStatus);
  }

  if (Utf8 != NULL) {
    FreePool (Utf8);
  }

  FreePool (Formatted);
  if ((Logger->Console == NULL) && (Logger->Serial == NULL) &&
      (Logger->File == NULL) &&
      !(MirrorDespiteFileFailure && (gST != NULL) &&
        ((gST->ConOut != NULL) || (gST->StdErr != NULL))))
  {
    Status = EFI_UNSUPPORTED;
  }

  Logger->FirstWriteError = RecordFirstError (Logger->FirstWriteError, Status);
  BootReportLogFailure (Logger->FirstFileError);
  return Status;
}

STATIC
EFI_STATUS
LogRotateOversized (
  IN OUT APP_LOGGER *Logger,
  IN OUT UINT64 *EndPosition
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *OldFile;
  if (*EndPosition < 1024U * 1024U) { return EFI_SUCCESS; }
  OldFile = Logger->File;
  Logger->File = NULL;
  Status = OldFile->Delete (OldFile); /* Delete closes the handle even on failure. */
  if (Status != EFI_SUCCESS) { return EFI_DEVICE_ERROR; }
  Status = AppFileOpenAdjacent (
             Logger->Files, Logger->FileName,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
             0, &Logger->File);
  if (EFI_ERROR (Status)) { Logger->File = NULL; return Status; }
  *EndPosition = 0;
  return EFI_SUCCESS;
}

EFI_STATUS
LogInitialize (
  OUT APP_LOGGER        *Logger,
  IN  APP_FILE_CONTEXT  *Files OPTIONAL,
  IN  BOOLEAN           Verbose,
  IN  CONST CHAR16      *LogFileName OPTIONAL
  )
{
  EFI_STATUS         Status;
  EFI_STATUS         CloseStatus;
  UINT64             EndPosition;
  STATIC CONST UINT8 Utf8Bom[] = { 0xEF, 0xBB, 0xBF };

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (LogFileName != NULL && StrCmp (LogFileName, L"NativeCsmVgaBoot.log") == 0) {
    BootReportSetStage (L"Opening boot log");
  }
  ZeroMem (Logger, sizeof (*Logger));
  Logger->Verbose = Verbose;
  if (gST != NULL) {
    Logger->Console = gST->ConOut;
  }

  Logger->SerialLocateStatus = gBS->LocateProtocol (
                                      &gEfiSerialIoProtocolGuid,
                                      NULL,
                                      (VOID **)&Logger->Serial
                                      );
  if (EFI_ERROR (Logger->SerialLocateStatus)) {
    Logger->Serial = NULL;
  }

  Logger->FileOpenStatus = EFI_SUCCESS;
  Logger->FileCloseStatus = EFI_SUCCESS;
  Logger->FirstWriteError = EFI_SUCCESS;
  Logger->FirstFileError  = EFI_SUCCESS;
  Logger->DurableFileFirst = FALSE;
  if ((LogFileName == NULL) || (LogFileName[0] == L'\0')) {
    return ((Logger->Console == NULL) && (Logger->Serial == NULL)) ?
           EFI_UNSUPPORTED : EFI_SUCCESS;
  }

  if (Files == NULL) {
    Logger->FileOpenStatus = EFI_INVALID_PARAMETER;
    return Logger->FileOpenStatus;
  }

  if (StrnLenS (LogFileName, ARRAY_SIZE (Logger->FileName)) >=
      ARRAY_SIZE (Logger->FileName))
  {
    Logger->FileOpenStatus = EFI_BAD_BUFFER_SIZE;
    return Logger->FileOpenStatus;
  }

  Logger->Files = Files;
  StrCpyS (
    Logger->FileName,
    ARRAY_SIZE (Logger->FileName),
    LogFileName
    );

  Logger->FileOpenStatus = AppFileOpenAdjacent (
                             Files,
                             LogFileName,
                             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                             0,
                             &Logger->File
                             );
  if (EFI_ERROR (Logger->FileOpenStatus)) {
    Logger->File = NULL;
    return Logger->FileOpenStatus;
  }

  Status = Logger->File->SetPosition (Logger->File, MAX_UINT64);
  if (EFI_ERROR (Status)) {
    CloseStatus             = Logger->File->Close (Logger->File);
    Logger->FileCloseStatus = CloseStatus;
    Logger->File           = NULL;
    Logger->FileOpenStatus = Status;
    return Status;
  }

  EndPosition = 0;
  Status      = Logger->File->GetPosition (Logger->File, &EndPosition);
  if (EFI_ERROR (Status)) {
    CloseStatus             = Logger->File->Close (Logger->File);
    Logger->FileCloseStatus = CloseStatus;
    Logger->File           = NULL;
    Logger->FileOpenStatus = Status;
    return Status;
  }

  if (EndPosition >= 1024U * 1024U) {
    Status = LogRotateOversized (Logger, &EndPosition);
    if (EFI_ERROR (Status)) {
      Logger->FileOpenStatus = Status;
      return Status;
    }
  }

  if (EndPosition == 0) {
    Status = WriteFileAll (Logger->File, Utf8Bom, sizeof (Utf8Bom));
    if (EFI_ERROR (Status)) {
      CloseStatus             = Logger->File->Close (Logger->File);
      Logger->FileCloseStatus = CloseStatus;
      Logger->File           = NULL;
      Logger->FileOpenStatus = Status;
      return Status;
    }
  }

  return EFI_SUCCESS;
}

VOID
LogSetVerbose (
  IN OUT APP_LOGGER  *Logger,
  IN     BOOLEAN     Verbose
  )
{
  if (Logger != NULL) {
    Logger->Verbose = Verbose;
  }
}

EFI_STATUS
EFIAPI
LogPrint (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Format,
  ...
  )
{
  EFI_STATUS  Status;
  VA_LIST     Marker;

  if ((Logger == NULL) || (Format == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  VA_START (Marker, Format);
  Status = LogVPrintInternal (Logger, Format, Marker, FALSE);
  VA_END (Marker);
  return Status;
}

EFI_STATUS
EFIAPI
LogVerbose (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Format,
  ...
  )
{
  EFI_STATUS  Status;
  VA_LIST     Marker;

  if ((Logger == NULL) || (Format == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Logger->Verbose) {
    return EFI_SUCCESS;
  }

  VA_START (Marker, Format);
  Status = LogVPrintInternal (Logger, Format, Marker, FALSE);
  VA_END (Marker);
  return Status;
}

EFI_STATUS
EFIAPI
LogPrintBestEffort (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Format,
  ...
  )
{
  EFI_STATUS  Status;
  VA_LIST     Marker;

  if ((Logger == NULL) || (Format == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  VA_START (Marker, Format);
  Status = LogVPrintInternal (Logger, Format, Marker, TRUE);
  VA_END (Marker);
  return Status;
}

EFI_STATUS
LogFlush (
  IN APP_LOGGER  *Logger
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  FlushStatus;

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Logger->FirstWriteError;
  if (Logger->File == NULL) {
    return Status;
  }

  FlushStatus = Logger->File->Flush (Logger->File);
  Logger->FirstWriteError = RecordFirstError (
                              Logger->FirstWriteError,
                              FlushStatus
                              );
  Logger->FirstFileError = RecordFirstError (
                             Logger->FirstFileError,
                             FlushStatus
                             );
  return RecordFirstError (Status, FlushStatus);
}

EFI_STATUS
LogFileBarrier (
  IN OUT APP_LOGGER  *Logger
  )
{
  EFI_STATUS  Status;

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Logger->File == NULL) {
    Logger->FirstFileError = RecordFirstError (
                               Logger->FirstFileError,
                               EFI_NOT_READY
                               );
    return Logger->FirstFileError;
  }

  Status = Logger->File->Flush (Logger->File);
  Logger->FirstFileError = RecordFirstError (Logger->FirstFileError, Status);
  Logger->FirstWriteError = RecordFirstError (Logger->FirstWriteError, Status);
  return Logger->FirstFileError;
}

EFI_STATUS
LogBeginDurableFileFirst (
  IN OUT APP_LOGGER  *Logger
  )
{
  EFI_STATUS  Status;

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (EFI_ERROR (Logger->FileOpenStatus)) {
    return Logger->FileOpenStatus;
  }

  Status = LogFileBarrier (Logger);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Logger->DurableFileFirst = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
LogCommitAndReopen (
  IN OUT APP_LOGGER  *Logger
  )
{
  EFI_STATUS         Status;
  EFI_STATUS         CloseStatus;
  EFI_STATUS         OpenStatus;
  EFI_STATUS         PositionStatus;
  EFI_FILE_PROTOCOL  *OldFile;

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Logger->File == NULL) || (Logger->Files == NULL) ||
      (Logger->FileName[0] == L'\0'))
  {
    Logger->FirstFileError = RecordFirstError (
                               Logger->FirstFileError,
                               EFI_NOT_READY
                               );
    return Logger->FirstFileError;
  }

  Status  = LogFileBarrier (Logger);
  OldFile = Logger->File;
  Logger->File = NULL;
  CloseStatus = OldFile->Close (OldFile);
  Logger->FileCloseStatus = CloseStatus;
  Logger->FirstFileError = RecordFirstError (
                             Logger->FirstFileError,
                             CloseStatus
                             );
  Logger->FirstWriteError = RecordFirstError (
                              Logger->FirstWriteError,
                              CloseStatus
                              );
  Status = RecordFirstError (Status, CloseStatus);

  OpenStatus = AppFileOpenAdjacent (
                 Logger->Files,
                 Logger->FileName,
                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                 EFI_FILE_MODE_CREATE,
                 0,
                 &Logger->File
                 );
  Logger->FileOpenStatus = OpenStatus;
  Logger->FirstFileError = RecordFirstError (
                             Logger->FirstFileError,
                             OpenStatus
                             );
  Logger->FirstWriteError = RecordFirstError (
                              Logger->FirstWriteError,
                              OpenStatus
                              );
  Status = RecordFirstError (Status, OpenStatus);
  if (EFI_ERROR (OpenStatus)) {
    Logger->File = NULL;
    return Status;
  }

  PositionStatus = Logger->File->SetPosition (Logger->File, MAX_UINT64);
  Logger->FirstFileError = RecordFirstError (
                             Logger->FirstFileError,
                             PositionStatus
                             );
  Logger->FirstWriteError = RecordFirstError (
                              Logger->FirstWriteError,
                              PositionStatus
                              );
  Status = RecordFirstError (Status, PositionStatus);
  if (EFI_ERROR (PositionStatus)) {
    CloseStatus = Logger->File->Close (Logger->File);
    Logger->File = NULL;
    Logger->FileCloseStatus = CloseStatus;
    Logger->FirstFileError = RecordFirstError (
                               Logger->FirstFileError,
                               CloseStatus
                               );
    Logger->FirstWriteError = RecordFirstError (
                                Logger->FirstWriteError,
                                CloseStatus
                                );
    return RecordFirstError (Status, CloseStatus);
  }

  return Status;
}

EFI_STATUS
LogCommitAndClose (
  IN OUT APP_LOGGER  *Logger
  )
{
  EFI_STATUS         Status;
  EFI_STATUS         CloseStatus;
  EFI_FILE_PROTOCOL  *OldFile;

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Logger->File == NULL) || (Logger->Files == NULL) ||
      (Logger->FileName[0] == L'\0'))
  {
    Logger->FirstFileError = RecordFirstError (
                               Logger->FirstFileError,
                               EFI_NOT_READY
                               );
    return Logger->FirstFileError;
  }

  Status       = LogFileBarrier (Logger);
  OldFile      = Logger->File;
  Logger->File = NULL;
  CloseStatus  = OldFile->Close (OldFile);
  Logger->FileCloseStatus = CloseStatus;
  Logger->FirstFileError = RecordFirstError (
                             Logger->FirstFileError,
                             CloseStatus
                             );
  Logger->FirstWriteError = RecordFirstError (
                              Logger->FirstWriteError,
                              CloseStatus
                              );
  return RecordFirstError (Status, CloseStatus);
}

EFI_STATUS
LogReopenAppend (
  IN OUT APP_LOGGER  *Logger
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  CloseStatus;

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Logger->File != NULL) || (Logger->Files == NULL) ||
      (Logger->FileName[0] == L'\0'))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = AppFileOpenAdjacent (
             Logger->Files,
             Logger->FileName,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
             0,
             &Logger->File
             );
  Logger->FileOpenStatus = Status;
  Logger->FirstFileError = RecordFirstError (
                             Logger->FirstFileError,
                             Status
                             );
  Logger->FirstWriteError = RecordFirstError (
                              Logger->FirstWriteError,
                              Status
                              );
  if (EFI_ERROR (Status)) {
    Logger->File = NULL;
    return Status;
  }

  Status = Logger->File->SetPosition (Logger->File, MAX_UINT64);
  Logger->FirstFileError = RecordFirstError (
                             Logger->FirstFileError,
                             Status
                             );
  Logger->FirstWriteError = RecordFirstError (
                              Logger->FirstWriteError,
                              Status
                              );
  if (EFI_ERROR (Status)) {
    CloseStatus = Logger->File->Close (Logger->File);
    Logger->File = NULL;
    Logger->FileCloseStatus = CloseStatus;
    Logger->FirstFileError = RecordFirstError (
                               Logger->FirstFileError,
                               CloseStatus
                               );
    Logger->FirstWriteError = RecordFirstError (
                                Logger->FirstWriteError,
                                CloseStatus
                                );
    return RecordFirstError (Status, CloseStatus);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
LogClose (
  IN OUT APP_LOGGER  *Logger
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  CloseStatus;

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = LogFlush (Logger);
  if (Logger->File != NULL) {
    CloseStatus = Logger->File->Close (Logger->File);
    Status      = RecordFirstError (Status, CloseStatus);
  }

  ZeroMem (Logger, sizeof (*Logger));
  return Status;
}

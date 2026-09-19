/** @file
  Console, serial, and executable-relative UTF-8 file logging.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_SELECTOR_LOG_H_
#define NATIVE_CSM_VGA_SELECTOR_LOG_H_

#include <Uefi.h>

#include <Protocol/SerialIo.h>
#include <Protocol/SimpleTextOut.h>

#include "AppFile.h"

typedef struct {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *Console;
  EFI_SERIAL_IO_PROTOCOL           *Serial;
  EFI_FILE_PROTOCOL                *File;
  BOOLEAN                          Verbose;
  EFI_STATUS                       SerialLocateStatus;
  EFI_STATUS                       FileOpenStatus;
  EFI_STATUS                       FileCloseStatus;
  EFI_STATUS                       FirstWriteError;
  EFI_STATUS                       FirstFileError;
  BOOLEAN                          DurableFileFirst;
  APP_FILE_CONTEXT                 *Files;
  CHAR16                           FileName[256];
} APP_LOGGER;

/**
  Initializes console logging, discovers an already-configured Serial I/O
  protocol, and optionally opens LogFileName for append beside the executable.

  Serial I/O is never reset or reconfigured. An empty/NULL LogFileName disables
  file logging. If a configured file cannot be opened, the logger remains valid
  for console/serial output and the file error is returned.
**/
EFI_STATUS
LogInitialize (
  OUT APP_LOGGER        *Logger,
  IN  APP_FILE_CONTEXT  *Files OPTIONAL,
  IN  BOOLEAN           Verbose,
  IN  CONST CHAR16      *LogFileName OPTIONAL
  );

VOID
LogSetVerbose (
  IN OUT APP_LOGGER  *Logger,
  IN     BOOLEAN     Verbose
  );

EFI_STATUS
EFIAPI
LogPrint (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Format,
  ...
  );

EFI_STATUS
EFIAPI
LogVerbose (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Format,
  ...
  );

/**
  Writes and flushes the file first when present, then always attempts every
  surviving console and serial sink even if the durable file has failed.

  This is reserved for reporting after an irreversible native-firmware
  boundary, where suppressing the remaining consoles would lose useful status
  but a file failure can no longer authorize a rollback or normal return.
**/
EFI_STATUS
EFIAPI
LogPrintBestEffort (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Format,
  ...
  );

EFI_STATUS
LogFlush (
  IN APP_LOGGER  *Logger
  );

/**
  Requires an open, healthy file sink, flushes it, and switches subsequent
  messages to file-write-plus-flush before console or serial mirroring.
**/
EFI_STATUS
LogBeginDurableFileFirst (
  IN OUT APP_LOGGER  *Logger
  );

/** Flushes only the file sink and returns its sticky first error. **/
EFI_STATUS
LogFileBarrier (
  IN OUT APP_LOGGER  *Logger
  );

/**
  Flushes and closes the current FAT log, then reopens the same adjacent file
  for append.  This provides an intermediate close/reopen commit point while
  keeping the pre-boundary logger active.
**/
EFI_STATUS
LogCommitAndReopen (
  IN OUT APP_LOGGER  *Logger
  );

/**
  Flushes and closes the current FAT log without discarding the executable-
  relative reopen metadata.  Subsequent log writes use only the remaining
  console/serial sinks until LogReopenAppend() is called.
**/
EFI_STATUS
LogCommitAndClose (
  IN OUT APP_LOGGER  *Logger
  );

/** Reopens a log previously closed by LogCommitAndClose() for append. **/
EFI_STATUS
LogReopenAppend (
  IN OUT APP_LOGGER  *Logger
  );

EFI_STATUS
LogClose (
  IN OUT APP_LOGGER  *Logger
  );

#endif

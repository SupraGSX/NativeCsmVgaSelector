/** @file
  Executable-relative file access for the native CSM VGA selector.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_SELECTOR_APP_FILE_H_
#define NATIVE_CSM_VGA_SELECTOR_APP_FILE_H_

#include <Uefi.h>

#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

typedef struct {
  EFI_HANDLE                       ImageHandle;
  EFI_HANDLE                       FileSystemDeviceHandle;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *ImageDirectory;
  CHAR16                           *ImageDirectoryPath;
  BOOLEAN                          ImageDirectoryAliasesRoot;
  BOOLEAN                          LoadedImageOpened;
  BOOLEAN                          FileSystemOpened;
} APP_FILE_CONTEXT;

/**
  Opens the filesystem and directory containing ImageHandle's executable.

  Every MEDIA_FILEPATH_DP fragment in the loaded-image device path is used.
**/
EFI_STATUS
AppFileInitialize (
  IN  EFI_HANDLE        ImageHandle,
  OUT APP_FILE_CONTEXT  *Context
  );

/**
  Validates a single filename. One leading slash or backslash is accepted for
  compatibility and omitted from NormalizedName.
**/
EFI_STATUS
AppFileValidateBaseName (
  IN  CONST CHAR16  *Name,
  OUT CONST CHAR16  **NormalizedName OPTIONAL
  );

/** Opens a safe basename relative to the executable directory. **/
EFI_STATUS
AppFileOpenAdjacent (
  IN  APP_FILE_CONTEXT  *Context,
  IN  CONST CHAR16      *BaseName,
  IN  UINT64            OpenMode,
  IN  UINT64            Attributes,
  OUT EFI_FILE_PROTOCOL **File
  );

/** Closes all handles and releases storage owned by Context. **/
EFI_STATUS
AppFileClose (
  IN OUT APP_FILE_CONTEXT  *Context
  );

#endif

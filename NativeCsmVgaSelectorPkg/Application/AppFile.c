/** @file
  Executable-relative file access for the native CSM VGA selector.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include "AppFile.h"

#include <Protocol/DevicePath.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define APP_FILE_MAX_DEVICE_PATH_BYTES  (64U * 1024U)
#define APP_FILE_MAX_BASE_NAME_CHARS    255U

STATIC
EFI_STATUS
AppendFilePathFragments (
  IN  CONST EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  OUT CHAR16                          **CombinedPath
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Node;
  CONST FILEPATH_DEVICE_PATH      *FileNode;
  UINTN                           DevicePathSize;
  UINTN                           Offset;
  UINTN                           NodeSize;
  UINTN                           FragmentCapacity;
  UINTN                           FragmentChars;
  UINTN                           FragmentCount;
  UINTN                           TotalChars;
  UINTN                           Destination;
  UINTN                           Index;
  CHAR16                          *Result;

  if ((FilePath == NULL) || (CombinedPath == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *CombinedPath  = NULL;
  if (!IsDevicePathValid (FilePath, APP_FILE_MAX_DEVICE_PATH_BYTES)) {
    return EFI_COMPROMISED_DATA;
  }

  DevicePathSize = GetDevicePathSize (FilePath);
  if ((DevicePathSize < sizeof (EFI_DEVICE_PATH_PROTOCOL)) ||
      (DevicePathSize > APP_FILE_MAX_DEVICE_PATH_BYTES))
  {
    return EFI_COMPROMISED_DATA;
  }

  TotalChars    = 0;
  FragmentCount = 0;
  Offset        = 0;
  while (Offset + sizeof (EFI_DEVICE_PATH_PROTOCOL) <= DevicePathSize) {
    Node     = (CONST EFI_DEVICE_PATH_PROTOCOL *)((CONST UINT8 *)FilePath + Offset);
    NodeSize = DevicePathNodeLength (Node);
    if ((NodeSize < sizeof (EFI_DEVICE_PATH_PROTOCOL)) ||
        (NodeSize > DevicePathSize - Offset))
    {
      return EFI_COMPROMISED_DATA;
    }

    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MEDIA_FILEPATH_DP))
    {
      if (((NodeSize - sizeof (EFI_DEVICE_PATH_PROTOCOL)) < sizeof (CHAR16)) ||
          (((NodeSize - sizeof (EFI_DEVICE_PATH_PROTOCOL)) & 1U) != 0))
      {
        return EFI_COMPROMISED_DATA;
      }

      FileNode         = (CONST FILEPATH_DEVICE_PATH *)Node;
      FragmentCapacity = (NodeSize - sizeof (EFI_DEVICE_PATH_PROTOCOL)) / sizeof (CHAR16);
      FragmentChars    = 0;
      while ((FragmentChars < FragmentCapacity) &&
             (FileNode->PathName[FragmentChars] != L'\0'))
      {
        ++FragmentChars;
      }

      if (FragmentChars == FragmentCapacity) {
        return EFI_COMPROMISED_DATA;
      }

      if (FragmentChars > (MAX_UINTN - TotalChars)) {
        return EFI_OUT_OF_RESOURCES;
      }

      TotalChars += FragmentChars;
      ++FragmentCount;
    }

    Offset += NodeSize;
    if (IsDevicePathEnd (Node)) {
      break;
    }
  }

  if ((Offset > DevicePathSize) || (TotalChars == 0) ||
      (FragmentCount > MAX_UINTN - TotalChars) ||
      (TotalChars + FragmentCount >=
       APP_FILE_MAX_DEVICE_PATH_BYTES / sizeof (CHAR16)))
  {
    return EFI_NOT_FOUND;
  }

  Result = AllocateZeroPool ((TotalChars + FragmentCount + 1U) * sizeof (CHAR16));
  if (Result == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Destination = 0;
  Offset      = 0;
  while (Offset + sizeof (EFI_DEVICE_PATH_PROTOCOL) <= DevicePathSize) {
    Node     = (CONST EFI_DEVICE_PATH_PROTOCOL *)((CONST UINT8 *)FilePath + Offset);
    NodeSize = DevicePathNodeLength (Node);
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MEDIA_FILEPATH_DP))
    {
      FileNode         = (CONST FILEPATH_DEVICE_PATH *)Node;
      FragmentCapacity = (NodeSize - sizeof (EFI_DEVICE_PATH_PROTOCOL)) / sizeof (CHAR16);
      if ((Destination != 0) && (FileNode->PathName[0] != L'\0') &&
          (Result[Destination - 1U] != L'\\') &&
          (Result[Destination - 1U] != L'/') &&
          (FileNode->PathName[0] != L'\\') &&
          (FileNode->PathName[0] != L'/'))
      {
        Result[Destination++] = L'\\';
      }

      for (Index = 0;
           (Index < FragmentCapacity) && (FileNode->PathName[Index] != L'\0');
           ++Index)
      {
        Result[Destination++] = (FileNode->PathName[Index] == L'/') ?
                                L'\\' : FileNode->PathName[Index];
      }
    }

    Offset += NodeSize;
    if (IsDevicePathEnd (Node)) {
      break;
    }
  }

  Result[Destination] = L'\0';
  *CombinedPath       = Result;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
GetImageDirectoryPath (
  IN  CONST EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  OUT CHAR16                          **DirectoryPath
  )
{
  EFI_STATUS  Status;
  CHAR16      *CombinedPath;
  CHAR16      *Result;
  UINTN       Length;
  UINTN       LastSeparator;
  UINTN       Start;
  UINTN       Index;
  UINTN       ResultChars;

  if (DirectoryPath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *DirectoryPath = NULL;
  CombinedPath   = NULL;
  Status         = AppendFilePathFragments (FilePath, &CombinedPath);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Length        = StrLen (CombinedPath);
  LastSeparator = MAX_UINTN;
  for (Index = 0; Index < Length; ++Index) {
    if (CombinedPath[Index] == L'\\') {
      LastSeparator = Index;
    }
  }

  if ((Length == 0) || (LastSeparator == Length - 1U)) {
    FreePool (CombinedPath);
    return EFI_COMPROMISED_DATA;
  }

  Start = 0;
  while ((Start < Length) && (CombinedPath[Start] == L'\\')) {
    ++Start;
  }

  if ((LastSeparator == MAX_UINTN) || (LastSeparator <= Start)) {
    ResultChars = 0;
  } else {
    ResultChars = LastSeparator - Start;
  }

  Result = AllocateZeroPool ((ResultChars + 1U) * sizeof (CHAR16));
  if (Result == NULL) {
    FreePool (CombinedPath);
    return EFI_OUT_OF_RESOURCES;
  }

  if (ResultChars != 0) {
    CopyMem (Result, CombinedPath + Start, ResultChars * sizeof (CHAR16));
  }

  Result[ResultChars] = L'\0';
  FreePool (CombinedPath);
  *DirectoryPath = Result;
  return EFI_SUCCESS;
}

EFI_STATUS
AppFileInitialize (
  IN  EFI_HANDLE        ImageHandle,
  OUT APP_FILE_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  CloseStatus;

  if ((ImageHandle == NULL) || (Context == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Context, sizeof (*Context));
  Context->ImageHandle = ImageHandle;

  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Context->LoadedImage,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  Context->LoadedImageOpened = TRUE;

  if ((Context->LoadedImage->DeviceHandle == NULL) ||
      (Context->LoadedImage->FilePath == NULL))
  {
    Status = EFI_NOT_FOUND;
    goto Error;
  }
  Context->FileSystemDeviceHandle = Context->LoadedImage->DeviceHandle;

  Status = gBS->OpenProtocol (
                  Context->LoadedImage->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Context->FileSystem,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  Context->FileSystemOpened = TRUE;

  Status = Context->FileSystem->OpenVolume (Context->FileSystem, &Context->Root);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = GetImageDirectoryPath (
             Context->LoadedImage->FilePath,
             &Context->ImageDirectoryPath
             );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  if (Context->ImageDirectoryPath[0] == L'\0') {
    Context->ImageDirectory            = Context->Root;
    Context->ImageDirectoryAliasesRoot = TRUE;
  } else {
    Status = Context->Root->Open (
                              Context->Root,
                              &Context->ImageDirectory,
                              Context->ImageDirectoryPath,
                              EFI_FILE_MODE_READ,
                              0
                              );
    if (EFI_ERROR (Status)) {
      goto Error;
    }
  }

  return EFI_SUCCESS;

Error:
  CloseStatus = AppFileClose (Context);
  if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
    Status = CloseStatus;
  }

  return Status;
}

EFI_STATUS
AppFileValidateBaseName (
  IN  CONST CHAR16  *Name,
  OUT CONST CHAR16  **NormalizedName OPTIONAL
  )
{
  CONST CHAR16  *BaseName;
  UINTN         Length;
  UINTN         Index;
  CHAR16        Character;

  if (NormalizedName != NULL) {
    *NormalizedName = NULL;
  }

  if (Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  BaseName = Name;
  if ((BaseName[0] == L'\\') || (BaseName[0] == L'/')) {
    ++BaseName;
  }

  Length = StrLen (BaseName);
  if ((Length == 0) || (Length > APP_FILE_MAX_BASE_NAME_CHARS)) {
    return EFI_INVALID_PARAMETER;
  }

  if (((Length == 1) && (BaseName[0] == L'.')) ||
      ((Length == 2) && (BaseName[0] == L'.') && (BaseName[1] == L'.')) ||
      (BaseName[Length - 1U] == L'.') || (BaseName[Length - 1U] == L' '))
  {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Length; ++Index) {
    Character = BaseName[Index];
    if ((Character < 0x20) || (Character == 0x7F) ||
        (Character == L'\\') || (Character == L'/') ||
        (Character == L':') || (Character == L'"') ||
        (Character == L'*') || (Character == L'?') ||
        (Character == L'<') || (Character == L'>') ||
        (Character == L'|'))
    {
      return EFI_INVALID_PARAMETER;
    }
  }

  if (NormalizedName != NULL) {
    *NormalizedName = BaseName;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
AppFileOpenAdjacent (
  IN  APP_FILE_CONTEXT   *Context,
  IN  CONST CHAR16       *BaseName,
  IN  UINT64             OpenMode,
  IN  UINT64             Attributes,
  OUT EFI_FILE_PROTOCOL  **File
  )
{
  EFI_STATUS    Status;
  CONST CHAR16  *NormalizedName;

  if ((Context == NULL) || (Context->ImageDirectory == NULL) || (File == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *File = NULL;
  Status = AppFileValidateBaseName (BaseName, &NormalizedName);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Context->ImageDirectory->Open (
                                    Context->ImageDirectory,
                                    File,
                                    (CHAR16 *)NormalizedName,
                                    OpenMode,
                                    Attributes
                                    );
}

EFI_STATUS
AppFileClose (
  IN OUT APP_FILE_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  CloseStatus;

  if (Context == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EFI_SUCCESS;
  if ((Context->ImageDirectory != NULL) && !Context->ImageDirectoryAliasesRoot) {
    CloseStatus = Context->ImageDirectory->Close (Context->ImageDirectory);
    if (EFI_ERROR (CloseStatus)) {
      Status = CloseStatus;
    }
  }

  if (Context->Root != NULL) {
    CloseStatus = Context->Root->Close (Context->Root);
    if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
      Status = CloseStatus;
    }
  }

  if (Context->ImageDirectoryPath != NULL) {
    FreePool (Context->ImageDirectoryPath);
  }

  if (Context->FileSystemOpened) {
    CloseStatus = gBS->CloseProtocol (
                         Context->FileSystemDeviceHandle,
                         &gEfiSimpleFileSystemProtocolGuid,
                         Context->ImageHandle,
                         NULL
                         );
    if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
      Status = CloseStatus;
    }
  }

  if (Context->LoadedImageOpened) {
    CloseStatus = gBS->CloseProtocol (
                         Context->ImageHandle,
                         &gEfiLoadedImageProtocolGuid,
                         Context->ImageHandle,
                         NULL
                         );
    if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
      Status = CloseStatus;
    }
  }

  ZeroMem (Context, sizeof (*Context));
  return Status;
}

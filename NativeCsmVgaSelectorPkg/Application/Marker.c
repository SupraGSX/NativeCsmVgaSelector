/** Optional exact-partition menu handoff. No creation, formatting or label matching. */
#include "Marker.h"
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC NCV_MARKER_CONFIG Configuration;
STATIC UINT8 Original[512], Written[512];
STATIC BOOLEAN Attempted;

VOID MarkerConfigure (CONST NCV_MARKER_CONFIG *Config) {
  CopyMem (&Configuration, Config, sizeof (Configuration));
  Attempted = FALSE;
}

STATIC BOOLEAN Matches (EFI_DEVICE_PATH_PROTOCOL *Path) {
  UINTN Count, Size;
  HARDDRIVE_DEVICE_PATH *Hd;
  UINT32 Signature;
  for (Count = 0; Path != NULL && !IsDevicePathEnd (Path) && Count < 128; ++Count) {
    Size = DevicePathNodeLength (Path);
    if (Size < 4 || Size > 1024) { return FALSE; }
    if (DevicePathType (Path) == MEDIA_DEVICE_PATH && DevicePathSubType (Path) == MEDIA_HARDDRIVE_DP) {
      if (Size != sizeof (*Hd)) { return FALSE; }
      Hd = (HARDDRIVE_DEVICE_PATH *)Path;
      CopyMem (&Signature, Hd->Signature, sizeof (Signature));
      return Hd->MBRType == 1 && Hd->SignatureType == 1 &&
        Hd->PartitionNumber == Configuration.Partition &&
        Hd->PartitionStart == Configuration.Start &&
        Hd->PartitionSize == Configuration.Sectors &&
        Signature == Configuration.Signature;
    }
    Path = NextDevicePathNode (Path);
  }
  return FALSE;
}

STATIC EFI_STATUS OpenMarker (EFI_FILE_PROTOCOL **Root, EFI_FILE_PROTOCOL **File) {
  EFI_HANDLE *Handles, Selected;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_STATUS Status;
  UINTN Count, Index, Found;
  CHAR16 Name[128];
  *Root = NULL; *File = NULL; Handles = NULL; Selected = NULL; Found = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  for (Index = 0; Index < Count; ++Index) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path);
    if (!EFI_ERROR (Status) && Matches (Path)) { Selected = Handles[Index]; ++Found; }
  }
  FreePool (Handles);
  if (Found != 1) { return EFI_NO_MAPPING; }
  Status = gBS->HandleProtocol (Selected, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Fs->OpenVolume (Fs, Root);
  if (EFI_ERROR (Status)) { return Status; }
  for (Index = 0; Configuration.Path[Index] != 0; ++Index) { Name[Index] = Configuration.Path[Index]; }
  Name[Index] = 0;
  return (*Root)->Open (*Root, File, Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
}

STATIC EFI_STATUS ReadExact (EFI_FILE_PROTOCOL *File, UINT8 *Bytes) {
  EFI_STATUS Status; UINTN Size; UINT8 Extra;
  Status = File->SetPosition (File, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Size = 512; Status = File->Read (File, &Size, Bytes);
  if (EFI_ERROR (Status)) { return Status; }
  if (Size != 512) { return EFI_BAD_BUFFER_SIZE; }
  Size = 1; Status = File->Read (File, &Size, &Extra);
  if (!EFI_ERROR (Status) && Size != 0) { return EFI_BAD_BUFFER_SIZE; }
  return Status;
}

STATIC EFI_STATUS WriteExact (EFI_FILE_PROTOCOL *File, CONST UINT8 *Bytes) {
  EFI_STATUS Status; UINTN Size; UINT8 Verify[512];
  Status = File->SetPosition (File, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Size = 512; Status = File->Write (File, &Size, (VOID *)Bytes);
  if (EFI_ERROR (Status)) { return Status; }
  if (Size != 512) { return EFI_DEVICE_ERROR; }
  Status = File->Flush (File);
  if (EFI_ERROR (Status)) { return Status; }
  Status = ReadExact (File, Verify);
  if (!EFI_ERROR (Status) && CompareMem (Verify, Bytes, 512) != 0) { return EFI_DEVICE_ERROR; }
  return Status;
}

STATIC EFI_STATUS CloseMarker (EFI_FILE_PROTOCOL *Root, EFI_FILE_PROTOCOL *File, EFI_STATUS Status) {
  EFI_STATUS Closed;
  if (File != NULL) { Closed = File->Close (File); if (!EFI_ERROR (Status)) { Status = Closed; } }
  if (Root != NULL) { Closed = Root->Close (Root); if (!EFI_ERROR (Status)) { Status = Closed; } }
  return Status;
}

// Keep the reviewed out-of-line marker write path and Boot stack footprint.
EFI_STATUS __attribute__((noinline)) MarkerApply (VOID) {
  EFI_FILE_PROTOCOL *Root, *File;
  EFI_STATUS Status;
  UINTN Length;
  if (!Configuration.Enabled) { return EFI_SUCCESS; }
  if (!MarkerConfigIsValid (&Configuration)) { return EFI_INVALID_PARAMETER; }
  Status = OpenMarker (&Root, &File);
  if (!EFI_ERROR (Status)) { Status = ReadExact (File, Original); }
  Length = MarkerStringLength (Configuration.Header);
  if (!EFI_ERROR (Status) &&
      (CompareMem (Original, Configuration.Header, Length) != 0 || Original[Length] != '\n' ||
       CompareMem (Original + Length + 1, "PROFILE=", 8) != 0)) { Status = EFI_COMPROMISED_DATA; }
  if (!EFI_ERROR (Status)) {
    SetMem (Written, sizeof (Written), ' ');
    CopyMem (Written, Configuration.Header, Length); Written[Length] = '\n';
    CopyMem (Written + Length + 1, "PROFILE=", 8);
    CopyMem (Written + Length + 9, Configuration.Profile, MarkerStringLength (Configuration.Profile));
    Written[511] = '\n'; Attempted = TRUE;
    Status = WriteExact (File, Written);
  }
  return CloseMarker (Root, File, Status);
}

/* Called only if the core returns before its irreversible CSM boundary. */
EFI_STATUS MarkerRestore (VOID) {
  EFI_FILE_PROTOCOL *Root, *File;
  EFI_STATUS Status;
  if (!Attempted) { return EFI_SUCCESS; }
  Status = OpenMarker (&Root, &File);
  if (!EFI_ERROR (Status)) { Status = WriteExact (File, Original); }
  Status = CloseMarker (Root, File, Status);
  if (!EFI_ERROR (Status)) { Attempted = FALSE; }
  return Status;
}

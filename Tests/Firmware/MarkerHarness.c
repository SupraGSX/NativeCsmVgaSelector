/* Real marker I/O on a synthetic FAT partition. SPDX-License-Identifier: GPL-3.0-only */
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Marker.h"
#include "CandidateIni.h"
#include "AppFile.h"
#define CHECK(Expression) do { if (!(Expression)) { Print (L"MARKER TEST FAILED line %u\r\n", __LINE__); goto Done; } } while (0)

STATIC EFI_STATUS ReadMarker (APP_FILE_CONTEXT *Files, UINT8 *Buffer)
{
  EFI_FILE_PROTOCOL *File;
  UINTN Size = 512;
  EFI_STATUS Status = Files->Root->Open (Files->Root, &File, L"BOOTSEL.DAT", EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Status = File->Read (File, &Size, Buffer);
  File->Close (File);
  return Size == 512 ? Status : EFI_BAD_BUFFER_SIZE;
}

EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  NCV_CONFIG_CORE Core;
  APP_FILE_CONTEXT Files;
  EFI_FILE_PROTOCOL *Report;
  UINT8 Original[512], Current[512];
  UINTN Size;
  CONST CHAR8 Text[] = "[Marker]\nEnabled=true\nPartitionNumber=1\nDiskSignature=0x12345678\nPartitionStart=2048\nPartitionSectors=131072\nProfile=SECONDARY\nPath=\\BOOTSEL.DAT\n";
  (VOID)SystemTable;
  CHECK (AppFileInitialize (ImageHandle, &Files) == EFI_SUCCESS);
  CHECK (ReadMarker (&Files, Original) == EFI_SUCCESS);
  NcvConfigDefaults (&Core);
  CHECK (NcvParseConfigStrict ((CONST NCV_U8 *)Text, sizeof (Text)-1, &Core) == NcvConfigSuccess);
  Core.Marker.Enabled = 0;
  MarkerConfigure (&Core.Marker);
  CHECK (MarkerApply () == EFI_SUCCESS);
  Core.Marker.Enabled = 1; Core.Marker.Signature++;
  MarkerConfigure (&Core.Marker);
  CHECK (MarkerApply () == EFI_NO_MAPPING);
  Core.Marker.Signature--; Core.Marker.Header[0] = 'X';
  MarkerConfigure (&Core.Marker);
  CHECK (MarkerApply () == EFI_COMPROMISED_DATA);
  CHECK (ReadMarker (&Files, Current) == EFI_SUCCESS && CompareMem (Current, Original, 512) == 0);
  Core.Marker.Header[0] = 'N';
  MarkerConfigure (&Core.Marker);
  CHECK (MarkerApply () == EFI_SUCCESS);
  CHECK (ReadMarker (&Files, Current) == EFI_SUCCESS && CompareMem (Current, Original, 512) != 0);
  CHECK (MarkerRestore () == EFI_SUCCESS);
  CHECK (ReadMarker (&Files, Current) == EFI_SUCCESS && CompareMem (Current, Original, 512) == 0);
  CHECK (MarkerApply () == EFI_SUCCESS);
  CHECK (AppFileOpenAdjacent (&Files, L"MarkerTest.result", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &Report) == EFI_SUCCESS);
  Size = 4;
  CHECK (Report->Write (Report, &Size, "PASS") == EFI_SUCCESS && Size == 4);
  CHECK (Report->Flush (Report) == EFI_SUCCESS);
  Report->Close (Report);
  AppFileClose (&Files);
  Print (L"MARKER TEST PASS: disabled, wrong disk/header, write, exact restore, reapply.\r\n");
Done:
  for (;;) { gBS->Stall (100000); }
  return EFI_SUCCESS;
}

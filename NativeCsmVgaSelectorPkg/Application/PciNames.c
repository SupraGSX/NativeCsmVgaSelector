/** Optional, read-only adjacent pci.ids lookup. Never used to choose hardware.
    SPDX-License-Identifier: GPL-3.0-only */
#include "PciNames.h"
#include "PciIdLookup.h"
#include "AppFile.h"
#include <Protocol/PciIo.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define PCI_IDS_LIMIT (4U * 1024U * 1024U)
STATIC CHAR8 *Database;
STATIC UINTN DatabaseSize;

VOID PciNamesRelease (VOID)
{
  if (Database != NULL) { FreePool (Database); }
  Database = NULL;
  DatabaseSize = 0;
}

VOID PciNamesLoad (EFI_HANDLE ImageHandle)
{
  APP_FILE_CONTEXT Files;
  EFI_FILE_PROTOCOL *File = NULL;
  EFI_STATUS Status, CloseStatus;
  UINT64 Size;
  PciNamesRelease ();
  Status = AppFileInitialize (ImageHandle, &Files);
  if (EFI_ERROR (Status)) { return; }
  Status = AppFileOpenAdjacent (&Files, L"pci.ids", EFI_FILE_MODE_READ, 0, &File);
  if (!EFI_ERROR (Status)) { Status = File->SetPosition (File, MAX_UINT64); }
  if (!EFI_ERROR (Status)) { Status = File->GetPosition (File, &Size); }
  if (!EFI_ERROR (Status) && (Size == 0 || Size > PCI_IDS_LIMIT)) { Status = EFI_BAD_BUFFER_SIZE; }
  if (!EFI_ERROR (Status)) {
    Database = AllocatePool ((UINTN)Size);
    if (Database == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
    } else {
      DatabaseSize = (UINTN)Size;
      Status = File->SetPosition (File, 0);
      if (!EFI_ERROR (Status)) { Status = File->Read (File, &DatabaseSize, Database); }
      if (!EFI_ERROR (Status) && DatabaseSize != (UINTN)Size) { Status = EFI_DEVICE_ERROR; }
    }
  }
  if (File != NULL) {
    CloseStatus = File->Close (File);
    if (!EFI_ERROR (Status)) { Status = CloseStatus; }
  }
  AppFileClose (&Files);
  if (EFI_ERROR (Status)) { PciNamesRelease (); }
}

VOID PciNamesLookup (UINT16 Vendor, UINT16 Device, CHAR8 *Name, UINTN Capacity)
{
  if (Capacity == 0) { return; }
  if (!PciIdLookup (Database, DatabaseSize, Vendor, Device, Name, (unsigned)Capacity)) {
    AsciiSPrint (Name, Capacity, "PCI %04x:%04x", Vendor, Device);
  }
}

VOID PciNamesForTarget (CONST PROBE_PCI_ADDRESS *Target, CHAR8 *Name, UINTN Capacity)
{
  EFI_HANDLE *Handles = NULL;
  EFI_PCI_IO_PROTOCOL *Pci;
  EFI_STATUS Status;
  UINTN Count, Index, Segment, Bus, Device, Function, Matches = 0;
  UINT16 Ids[2], Vendor = 0, Product = 0;
  AsciiSPrint (Name, Capacity, "GPU at %04x:%02x:%02x.%x",
              Target->Segment, Target->Bus, Target->Device, Target->Function);
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return; }
  for (Index = 0; Index < Count; ++Index) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid, (VOID **)&Pci);
    if (EFI_ERROR (Status)) { continue; }
    Status = Pci->GetLocation (Pci, &Segment, &Bus, &Device, &Function);
    if (EFI_ERROR (Status) || Segment != Target->Segment || Bus != Target->Bus ||
        Device != Target->Device || Function != Target->Function) { continue; }
    ++Matches;
    Status = Pci->Pci.Read (Pci, EfiPciIoWidthUint16, 0, 2, Ids);
    if (!EFI_ERROR (Status)) {
      Vendor = Ids[0]; Product = Ids[1];
    } else {
      Matches += 2;
    }
  }
  FreePool (Handles);
  if (Matches == 1) { PciNamesLookup (Vendor, Product, Name, Capacity); }
}

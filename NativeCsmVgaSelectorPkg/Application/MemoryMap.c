/** @file
  Bounded UEFI memory-map helpers used to validate firmware-owned pointers.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "MemoryMap.h"

#define MEMORY_MAP_CAPTURE_ATTEMPTS  4
#define MEMORY_MAP_SLACK_ENTRIES     8

STATIC
BOOLEAN
MemoryTypeMayBeRead (
  IN EFI_MEMORY_TYPE  Type
  )
{
  switch (Type) {
    case EfiReservedMemoryType:
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
    case EfiConventionalMemory:
    case EfiACPIReclaimMemory:
    case EfiACPIMemoryNVS:
    case EfiPersistentMemory:
      return TRUE;

    default:
      return FALSE;
  }
}

EFI_STATUS
MemoryMapCapture (
  OUT MEMORY_MAP_SNAPSHOT  *Snapshot
  )
{
  EFI_STATUS  Status;
  UINTN       MapKey;
  UINTN       RequiredSize;
  UINTN       DescriptorSize;
  UINT32      DescriptorVersion;
  UINTN       Attempt;

  if (Snapshot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Snapshot, sizeof (*Snapshot));
  RequiredSize      = 0;
  MapKey            = 0;
  DescriptorSize    = 0;
  DescriptorVersion = 0;
  Status = gBS->GetMemoryMap (
                  &RequiredSize,
                  NULL,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return EFI_ERROR (Status) ? Status : EFI_PROTOCOL_ERROR;
  }

  if ((DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
      (DescriptorSize > (MAX_UINTN / MEMORY_MAP_SLACK_ENTRIES)) ||
      (RequiredSize > (MAX_UINTN - (DescriptorSize * MEMORY_MAP_SLACK_ENTRIES))))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  RequiredSize += DescriptorSize * MEMORY_MAP_SLACK_ENTRIES;
  for (Attempt = 0; Attempt < MEMORY_MAP_CAPTURE_ATTEMPTS; ++Attempt) {
    Snapshot->Map = AllocateZeroPool (RequiredSize);
    if (Snapshot->Map == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Snapshot->MapSize           = RequiredSize;
    Snapshot->DescriptorSize    = DescriptorSize;
    Snapshot->DescriptorVersion = DescriptorVersion;
    Status = gBS->GetMemoryMap (
                    &Snapshot->MapSize,
                    Snapshot->Map,
                    &MapKey,
                    &Snapshot->DescriptorSize,
                    &Snapshot->DescriptorVersion
                    );
    if (Status == EFI_SUCCESS) {
      if ((Snapshot->MapSize == 0) || (Snapshot->MapSize > RequiredSize) ||
          (Snapshot->DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
          ((Snapshot->MapSize % Snapshot->DescriptorSize) != 0))
      {
        MemoryMapRelease (Snapshot);
        return EFI_BAD_BUFFER_SIZE;
      }

      return EFI_SUCCESS;
    }

    FreePool (Snapshot->Map);
    Snapshot->Map = NULL;
    if (!EFI_ERROR (Status)) {
      ZeroMem (Snapshot, sizeof (*Snapshot));
      return EFI_PROTOCOL_ERROR;
    }

    if (Status != EFI_BUFFER_TOO_SMALL) {
      ZeroMem (Snapshot, sizeof (*Snapshot));
      return Status;
    }

    if ((Snapshot->DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
        (Snapshot->DescriptorSize > (MAX_UINTN / MEMORY_MAP_SLACK_ENTRIES)) ||
        (Snapshot->MapSize >
         (MAX_UINTN - (Snapshot->DescriptorSize * MEMORY_MAP_SLACK_ENTRIES))))
    {
      ZeroMem (Snapshot, sizeof (*Snapshot));
      return EFI_BAD_BUFFER_SIZE;
    }

    RequiredSize = Snapshot->MapSize +
                   (Snapshot->DescriptorSize * MEMORY_MAP_SLACK_ENTRIES);
    DescriptorSize    = Snapshot->DescriptorSize;
    DescriptorVersion = Snapshot->DescriptorVersion;
  }

  ZeroMem (Snapshot, sizeof (*Snapshot));
  return EFI_BUFFER_TOO_SMALL;
}

VOID
MemoryMapRelease (
  IN OUT MEMORY_MAP_SNAPSHOT  *Snapshot
  )
{
  if (Snapshot == NULL) {
    return;
  }

  if (Snapshot->Map != NULL) {
    FreePool (Snapshot->Map);
  }

  ZeroMem (Snapshot, sizeof (*Snapshot));
}

STATIC BOOLEAN
MemoryMapRangeAccessible (
  IN  CONST MEMORY_MAP_SNAPSHOT  *Snapshot,
  IN  UINTN                      Address,
  IN  UINTN                      Length,
  OUT UINTN                      *ReadableBytes OPTIONAL,
  IN  BOOLEAN                    Writable
  )
{
  CONST UINT8                  *Walker;
  CONST EFI_MEMORY_DESCRIPTOR  *Descriptor;
  CONST EFI_MEMORY_DESCRIPTOR  *Covering;
  EFI_PHYSICAL_ADDRESS         Cursor;
  EFI_PHYSICAL_ADDRESS         DescriptorLast;
  EFI_PHYSICAL_ADDRESS         CoveredLast;
  EFI_PHYSICAL_ADDRESS         NextStart;
  UINT64                       DescriptorBytes;
  UINT64                       Available;
  UINTN                        Remaining;
  BOOLEAN                      Overlap;

  if (ReadableBytes != NULL) {
    *ReadableBytes = 0;
  }

  if ((Snapshot == NULL) || (Snapshot->Map == NULL) ||
      (Snapshot->DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
      (Snapshot->MapSize == 0) ||
      ((Snapshot->MapSize % Snapshot->DescriptorSize) != 0) ||
      (Length == 0) || (Address > (MAX_UINTN - (Length - 1))))
  {
    return FALSE;
  }

  Cursor = Address;
  Available = 0;
  // Firmware need not order descriptors. Follow consecutive readable ranges,
  // stopping at holes, protected memory or overlapping ownership. In particular
  // never let one readable descriptor hide an overlapping MMIO/RP descriptor.
  for (;;) {
    Covering = NULL;
    Overlap = FALSE;
    CoveredLast = 0;
    NextStart = MAX_UINT64;
    Walker = (CONST UINT8 *)Snapshot->Map;
    Remaining = Snapshot->MapSize;
    while (Remaining >= Snapshot->DescriptorSize) {
      Descriptor = (CONST EFI_MEMORY_DESCRIPTOR *)Walker;
      if (Descriptor->NumberOfPages > (MAX_UINT64 / EFI_PAGE_SIZE)) {
        return FALSE;
      }

      DescriptorBytes = Descriptor->NumberOfPages * EFI_PAGE_SIZE;
      if (DescriptorBytes != 0) {
        if (Descriptor->PhysicalStart > (MAX_UINT64 - (DescriptorBytes - 1))) {
          return FALSE;
        }

        DescriptorLast = Descriptor->PhysicalStart + DescriptorBytes - 1;
        if ((Cursor >= Descriptor->PhysicalStart) && (Cursor <= DescriptorLast)) {
          if (Covering != NULL) {
            Overlap = TRUE;
          }

          Covering = Descriptor;
          CoveredLast = DescriptorLast;
        } else if ((Descriptor->PhysicalStart > Cursor) &&
                   (Descriptor->PhysicalStart < NextStart))
        {
          NextStart = Descriptor->PhysicalStart;
        }
      }

      Walker += Snapshot->DescriptorSize;
      Remaining -= Snapshot->DescriptorSize;
    }

    if (Overlap || (Covering == NULL) ||
        !MemoryTypeMayBeRead (Covering->Type) ||
        ((Covering->Attribute & EFI_MEMORY_RP) != 0) ||
        (Writable && ((Covering->Attribute & (EFI_MEMORY_RO | EFI_MEMORY_WP)) != 0 ||
          Covering->Type == EfiLoaderCode || Covering->Type == EfiBootServicesCode ||
          Covering->Type == EfiRuntimeServicesCode || Covering->Type == EfiPersistentMemory)))
    {
      break;
    }

    if ((NextStart != MAX_UINT64) && (NextStart <= CoveredLast)) {
      CoveredLast = NextStart - 1;
    }

    if (CoveredLast > MAX_UINTN) {
      CoveredLast = MAX_UINTN;
    }

    Available = CoveredLast - Address;
    Available = (Available >= MAX_UINTN) ? MAX_UINTN : Available + 1;
    if (((ReadableBytes == NULL) && (Available >= Length)) ||
        (CoveredLast == MAX_UINTN))
    {
      break;
    }

    Cursor = CoveredLast + 1;
  }

  if (ReadableBytes != NULL) {
    *ReadableBytes = (UINTN)Available;
  }

  return Available >= Length;
}

BOOLEAN MemoryMapRangeIsReadable (CONST MEMORY_MAP_SNAPSHOT *Snapshot,
  UINTN Address, UINTN Length, UINTN *ReadableBytes)
{
  return MemoryMapRangeAccessible (Snapshot, Address, Length, ReadableBytes, FALSE);
}

BOOLEAN MemoryMapRangeIsWritable (CONST MEMORY_MAP_SNAPSHOT *Snapshot,
  UINTN Address, UINTN Length)
{
  return MemoryMapRangeAccessible (Snapshot, Address, Length, NULL, TRUE);
}

EFI_STATUS EFIAPI __attribute__((noinline))
MemoryMapValidateBootRanges (VOID)
{
  MEMORY_MAP_SNAPSHOT Map;
  EFI_STATUS Status = MemoryMapCapture (&Map);
  if (EFI_ERROR (Status)) { return Status; }
  if (!MemoryMapRangeIsReadable (&Map, 0, 0x500, NULL) ||
      !MemoryMapRangeIsReadable (&Map, 0xc0000, 0x10000, NULL) ||
      !MemoryMapRangeIsReadable (&Map, 0xe0000, 0x20000, NULL)) {
    Status = EFI_SECURITY_VIOLATION;
  }
  MemoryMapRelease (&Map);
  return Status;
}

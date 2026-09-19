/** @file
  Bounded UEFI memory-map helpers used to validate firmware-owned pointers.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_SELECTOR_MEMORY_MAP_H_
#define NATIVE_CSM_VGA_SELECTOR_MEMORY_MAP_H_

#include <Uefi.h>

typedef struct {
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  MapSize;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
} MEMORY_MAP_SNAPSHOT;

EFI_STATUS
MemoryMapCapture (
  OUT MEMORY_MAP_SNAPSHOT  *Snapshot
  );

VOID
MemoryMapRelease (
  IN OUT MEMORY_MAP_SNAPSHOT  *Snapshot
  );

// ReadableBytes is the contiguous safe extent, possibly spanning descriptors.
// It can be nonzero on FALSE when the requested range crosses a gap/protection.
BOOLEAN
MemoryMapRangeIsReadable (
  IN  CONST MEMORY_MAP_SNAPSHOT  *Snapshot,
  IN  UINTN                      Address,
  IN  UINTN                      Length,
  OUT UINTN                      *ReadableBytes OPTIONAL
  );

BOOLEAN MemoryMapRangeIsWritable (CONST MEMORY_MAP_SNAPSHOT *Snapshot, UINTN Address, UINTN Length);
EFI_STATUS EFIAPI MemoryMapValidateBootRanges (VOID);

#endif

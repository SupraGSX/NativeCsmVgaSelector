/* Production parser: boundary, allocation and mutation tests under sanitizers.
   SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "LegacyFirmwareDisk.h"
#include "MemoryMap.h"
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

static unsigned Allocation, FailAllocation;
VOID *EFIAPI AllocateZeroPool (UINTN Size) { return ++Allocation == FailAllocation ? NULL : calloc(1, Size); }
VOID *EFIAPI AllocateCopyPool (UINTN Size, CONST VOID *Buffer) {
  void *p;
  if (++Allocation == FailAllocation) { return NULL; }
  p = malloc(Size); if (p) { memcpy(p, Buffer, Size); } return p;
}
VOID EFIAPI FreePool (VOID *p) { free(p); }
VOID *EFIAPI CopyMem (VOID *a, CONST VOID *b, UINTN n) { return memmove(a, b, n); }
VOID *EFIAPI ZeroMem (VOID *a, UINTN n) { return memset(a, 0, n); }
INTN EFIAPI CompareMem (CONST VOID *a, CONST VOID *b, UINTN n) { return memcmp(a, b, n); }
EFI_STATUS MemoryMapCapture (MEMORY_MAP_SNAPSHOT *m) { (void)m; return EFI_UNSUPPORTED; }
VOID MemoryMapRelease (MEMORY_MAP_SNAPSHOT *m) { (void)m; }
BOOLEAN MemoryMapRangeIsReadable (CONST MEMORY_MAP_SNAPSHOT *m, UINTN a, UINTN n, UINTN *r) {
  (void)m; (void)a; (void)n; (void)r; return FALSE;
}
static unsigned Random = 0x715c8123;
static unsigned Next(void) { Random ^= Random << 13; Random ^= Random >> 17; Random ^= Random << 5; return Random; }

static void GeneralFirmwareCases(void) {
  static const unsigned Addresses[] = {0xe0040, 0xe8253, 0xf6000, 0xfff80};
  static const unsigned Buses[] = {0, 7, 31, 255};
  UINT8 *Bytes = malloc(NCV_DISK_FIRMWARE_BYTES);
  UINT8 Valid[NCV_DISK_FIRMWARE_BLOCKS];
  PCI_DEVICE_RECORD Device; PCI_INVENTORY Inventory; BBS_TABLE Entry;
  unsigned Vendor, Bus, Address, Form, Count = 0;
  assert(Bytes); FailAllocation = 0; memset(Valid, 1, sizeof Valid);
  for (Vendor = 0; Vendor < 3; ++Vendor) for (Bus = 0; Bus < 4; ++Bus)
  for (Address = 0; Address < 4; ++Address) for (Form = 0; Form < 2; ++Form) {
    memset(Bytes, 0xff, NCV_DISK_FIRMWARE_BYTES);
    memset(&Device, 0, sizeof Device); memset(&Inventory, 0, sizeof Inventory);
    memset(&Entry, 0, sizeof Entry);
    Inventory.Count = 1; Inventory.Devices = &Device;
    Device.Bus = Entry.Bus = Buses[Bus]; Device.Device = Entry.Device = Bus * 10;
    Device.Function = Entry.Function = Bus * 2;
    Device.Config[0] = (UINT8)(0x21 + Vendor); Device.Config[1] = (UINT8)(0x43 + Vendor);
    Device.Config[2] = (UINT8)(0x65 + Vendor); Device.Config[3] = (UINT8)(0x87 + Vendor);
    Device.Config[11] = 1; Device.Config[10] = 6; Device.Config[9] = 1;
    Entry.DeviceType = BBS_HARDDISK; Entry.Class = 1; Entry.SubClass = 6;
    Entry.BootPriority = BBS_UNPRIORITIZED_ENTRY;
    Entry.BootHandlerSegment = (UINT16)((Addresses[Address] >> 4) - Form);
    Entry.BootHandlerOffset = (UINT16)((Addresses[Address] & 15) + 16 * Form);
    Entry.DescStringSegment = 0xe000; Entry.DescStringOffset = 0x7000;
    if (Form) { Entry.MfgStringSegment = 0xe000; Entry.MfgStringOffset = 0x7000; }
    memset(Bytes + Addresses[Address] - NCV_DISK_FIRMWARE_BASE, 0x40 + Vendor, 64);
    snprintf((char *)Bytes + 0x7000, 128, "Generic AHCI disk %u at bus %u", Vendor, Buses[Bus]);
    assert(LegacyFirmwareDiskPrepare(Bytes, Valid, Bytes, &Inventory) == EFI_SUCCESS);
    assert(LegacyFirmwareDiskMatch(&Entry)); ++Count;
  }
  assert(Count == 96); LegacyFirmwareDiskReset(); free(Bytes);
  puts("PASS 96 accepted firmware variants: different PCI IDs, controller locations, disk names and BIOS pointer representations");
}

int main(void) {
  UINT8 *Snapshot = malloc(NCV_DISK_FIRMWARE_BYTES), *Live = malloc(NCV_DISK_FIRMWARE_BYTES);
  UINT8 Valid[NCV_DISK_FIRMWARE_BLOCKS];
  PCI_DEVICE_RECORD Devices[2]; PCI_INVENTORY Inventory;
  BBS_TABLE Entry, Saved;
  unsigned I, Iteration;
  assert(Snapshot && Live);
  GeneralFirmwareCases();
  memset(Snapshot, 0xff, NCV_DISK_FIRMWARE_BYTES); memset(Valid, 1, sizeof Valid);
  // Exercise a non-page/ROM-aligned handler and same-segment firmware string.
  for (I = 0; I < 64; ++I) { Snapshot[0xab03 + I] = (UINT8)(I + 1); }
  memcpy(Snapshot + 0x88a8, "Example AHCI disk", 18);
  memcpy(Live, Snapshot, NCV_DISK_FIRMWARE_BYTES);
  memset(Devices, 0, sizeof Devices); memset(&Inventory, 0, sizeof Inventory);
  Inventory.Count = 1; Inventory.Devices = Devices;
  Devices[0].Bus = 7; Devices[0].Config[0] = 0x34; Devices[0].Config[1] = 0x12;
  Devices[0].Config[2] = 0x78; Devices[0].Config[3] = 0x56;
  Devices[0].Config[11] = 1; Devices[0].Config[10] = 6; Devices[0].Config[9] = 1;
  memset(&Entry, 0, sizeof Entry); Entry.Bus = 7; Entry.DeviceType = BBS_HARDDISK;
  Entry.Class = 1; Entry.SubClass = 6; Entry.BootPriority = BBS_UNPRIORITIZED_ENTRY;
  Entry.BootHandlerSegment = Entry.DescStringSegment = 0xe7fa;
  Entry.BootHandlerOffset = 0x2b63; Entry.DescStringOffset = 0x0908; Saved = Entry;
  for (I = 1; I <= 2; ++I) {
    Allocation = 0; FailAllocation = I;
    assert(LegacyFirmwareDiskPrepare(Snapshot, Valid, Live, &Inventory) == EFI_OUT_OF_RESOURCES);
    assert(!LegacyFirmwareDiskMatch(&Entry));
  }
  FailAllocation = 0;
  assert(LegacyFirmwareDiskPrepare(Snapshot, Valid, Live, &Inventory) == EFI_SUCCESS);
  assert(LegacyFirmwareDiskMatch(&Entry));
  Live[0xab03] ^= 1; assert(!LegacyFirmwareDiskMatch(&Entry)); Live[0xab03] ^= 1;
  Live[0x88a8] ^= 1; assert(!LegacyFirmwareDiskMatch(&Entry)); Live[0x88a8] ^= 1;
  Entry.MfgStringSegment = 0xe7fa; Entry.MfgStringOffset = 0x0908;
  assert(LegacyFirmwareDiskMatch(&Entry)); Entry = Saved;
  Entry.BootHandlerSegment = 0xffff; Entry.BootHandlerOffset = 0xffff;
  assert(!LegacyFirmwareDiskMatch(&Entry)); Entry = Saved;
  Entry.DescStringSegment = 0xc000; assert(!LegacyFirmwareDiskMatch(&Entry)); Entry = Saved;
  Devices[1] = Devices[0]; Devices[1].Segment = 1; Inventory.Count = 2;
  assert(LegacyFirmwareDiskPrepare(Snapshot, Valid, Live, &Inventory) == EFI_SUCCESS);
  assert(!LegacyFirmwareDiskMatch(&Entry)); Inventory.Count = 1;
  Devices[0].ConfigStatus = EFI_DEVICE_ERROR;
  assert(LegacyFirmwareDiskPrepare(Snapshot, Valid, Live, &Inventory) == EFI_SUCCESS);
  assert(!LegacyFirmwareDiskMatch(&Entry)); Devices[0].ConfigStatus = EFI_SUCCESS;
  Devices[0].Config[9] = 0;
  assert(LegacyFirmwareDiskPrepare(Snapshot, Valid, Live, &Inventory) == EFI_SUCCESS);
  assert(!LegacyFirmwareDiskMatch(&Entry)); Devices[0].Config[9] = 1;
  memset(Valid, 0, sizeof Valid);
  assert(LegacyFirmwareDiskPrepare(Snapshot, Valid, Live, &Inventory) == EFI_SUCCESS);
  assert(!LegacyFirmwareDiskMatch(&Entry)); memset(Valid, 1, sizeof Valid);
  for (Iteration = 0; Iteration < 12000; ++Iteration) {
    Entry = Saved;
    for (I = 0; I < 8; ++I) { ((UINT8 *)&Entry)[Next() % sizeof Entry] = (UINT8)Next(); }
    memset(Valid, 1, sizeof Valid); Valid[Next() % sizeof Valid] = (UINT8)Next();
    assert(LegacyFirmwareDiskPrepare(Snapshot, Valid, Live, &Inventory) == EFI_SUCCESS);
    (void)LegacyFirmwareDiskMatch(&Entry);
    assert(memcmp(Snapshot, Live, NCV_DISK_FIRMWARE_BYTES) == 0);
  }
  Inventory.Count = MAX_UINTN;
  assert(LegacyFirmwareDiskPrepare(Snapshot, Valid, Live, &Inventory) == EFI_INVALID_PARAMETER);
  assert(!LegacyFirmwareDiskMatch(&Saved)); Inventory.Count = 1;
  assert(LegacyFirmwareDiskCapture(&Inventory) == EFI_UNSUPPORTED);
  assert(!LegacyFirmwareDiskMatch(&Saved));
  LegacyFirmwareDiskReset(); free(Snapshot); free(Live);
  puts("PASS firmware AHCI provenance: 12000 malformed-input cases, allocation faults, pointer bounds, controller collisions, mutation and read-only checks (ASan/UBSan)");
  return 0;
}

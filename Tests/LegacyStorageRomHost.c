/* Sanitizer/fault-injection test of the production parser on copied bytes.
   SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#undef NULL /* EDK II supplies its own equivalent definition. */
#include "LegacyStorageRom.h"
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

static int AllocationFailure, AllocationCount;
VOID *EFIAPI AllocateZeroPool (UINTN Size) {
  if (++AllocationCount == AllocationFailure) { return NULL; }
  return calloc(1, (size_t)Size);
}
VOID *EFIAPI AllocateCopyPool (UINTN Size, CONST VOID *Buffer) {
  void *Result;
  if (++AllocationCount == AllocationFailure) { return NULL; }
  Result = malloc((size_t)Size);
  if (Result != NULL) { memcpy(Result, Buffer, (size_t)Size); }
  return Result;
}
VOID EFIAPI FreePool (VOID *Buffer) { free(Buffer); }
VOID *EFIAPI CopyMem (VOID *Destination, CONST VOID *Source, UINTN Size) { return memmove(Destination, Source, (size_t)Size); }
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Size) { return memset(Buffer, 0, (size_t)Size); }
INTN EFIAPI CompareMem (CONST VOID *A, CONST VOID *B, UINTN Size) { return memcmp(A, B, (size_t)Size); }
EFI_STATUS EFIAPI LogPrint (APP_LOGGER *Logger, CONST CHAR16 *Format, ...) { (void)Logger; (void)Format; return EFI_SUCCESS; }

static unsigned Random = 0x12345678;
static unsigned Next(void) { Random ^= Random << 13; Random ^= Random >> 17; Random ^= Random << 5; return Random; }
static void Read(const char *Name, void *Buffer, size_t Bytes) {
  FILE *File = fopen(Name, "rb"); assert(File != NULL);
  assert(fread(Buffer, 1, Bytes, File) == Bytes); assert(fgetc(File) == EOF); fclose(File);
}
static void W16(UINT8 *Buffer, unsigned Value) { Buffer[0] = (UINT8)Value; Buffer[1] = (UINT8)(Value >> 8); }

int main(int argc, char **argv) {
  UINT8 *Original = malloc(0x20000), *Shadow = malloc(0x20000), *Saved = malloc(0x20000);
  UINT8 Valid[256];
  PCI_DEVICE_RECORD Device;
  PCI_INVENTORY Inventory;
  BBS_TABLE Entry, OriginalEntry;
  UINT32 Meta[5];
  BOOLEAN Missing;
  unsigned Iteration, At, I, Mode, RuntimeBytes;
  EFI_STATUS Status;
  assert(argc == 5 && Original && Shadow && Saved);
  Read(argv[1], Original, 0x20000); Read(argv[2], &OriginalEntry, sizeof(OriginalEntry));
  memset(&Device, 0, sizeof(Device)); Read(argv[3], Device.Config, 64); Read(argv[4], Meta, sizeof(Meta));
  Device.Segment = Meta[0]; Device.Bus = Meta[1]; Device.Device = Meta[2]; Device.Function = Meta[3];
  memset(&Inventory, 0, sizeof(Inventory)); Inventory.Count = 1; Inventory.Devices = &Device;
  At = (unsigned)OriginalEntry.BootHandlerSegment * 16 - 0xc0000;
  assert(At < 0x20000);
  RuntimeBytes = (unsigned)Original[At + 2] * 512;
  assert(RuntimeBytes > 0x228 && RuntimeBytes <= 0x20000 - At);
  memcpy(Shadow, Original, 0x20000); memset(Valid, 1, sizeof(Valid));
  for (I = 1; I <= 2; ++I) {
    AllocationFailure = (int)I; AllocationCount = 0;
    assert(LegacyStorageRomPrepare(Shadow, Valid, Shadow, &Inventory) == EFI_OUT_OF_RESOURCES);
    assert(!LegacyStorageRomMatch(&OriginalEntry, &Missing));
  }
  AllocationFailure = 0;
  assert(LegacyStorageRomPrepare(Shadow, Valid, Shadow, &Inventory) == EFI_SUCCESS);
  assert(LegacyStorageRomMatch(&OriginalEntry, &Missing));
  assert(Missing);
  for (Iteration = 0; Iteration < 12000; ++Iteration) {
    memcpy(Shadow, Original, 0x20000); memcpy(Saved, Original, 0x20000);
    memset(Valid, 1, sizeof(Valid)); Entry = OriginalEntry;
    Mode = Iteration % 8;
    if (Mode == 0) { W16(Shadow + At + 0x18, Next()); }
    if (Mode == 1) { W16(Shadow + At + 0x1a, Next()); }
    if (Mode == 2) { Shadow[At + 2] = (UINT8)Next(); }
    if (Mode == 3) { for (I = 0; I < 4; ++I) { ((UINT8 *)&Entry)[Next() % sizeof(Entry)] = (UINT8)Next(); } }
    if (Mode == 4) { Valid[Next() % 256] = (UINT8)Next(); }
    if (Mode == 5) { for (I = 0; I < 8; ++I) { Shadow[Next() % 0x20000] = (UINT8)Next(); } }
    if (Mode == 6) { for (I = 0; I < 8; ++I) { Shadow[At + (Next() % RuntimeBytes)] = (UINT8)Next(); } }
    memcpy(Saved, Shadow, 0x20000);
    Status = LegacyStorageRomPrepare(Shadow, Valid, Shadow, &Inventory); assert(Status == EFI_SUCCESS);
    (void)LegacyStorageRomMatch(&Entry, &Missing);
    (void)LegacyStorageRomVerify(NULL);
    if (Mode == 7) {
      Shadow[At + 0x228] ^= 1;
      assert(!LegacyStorageRomMatch(&Entry, &Missing));
      assert(LegacyStorageRomVerify(NULL) == EFI_CRC_ERROR);
      Shadow[At + 0x228] ^= 1;
    }
    assert(memcmp(Shadow, Saved, 0x20000) == 0);
    LegacyStorageRomReset();
  }
  Inventory.Count = MAX_UINTN;
  assert(LegacyStorageRomPrepare(Shadow, Valid, Shadow, &Inventory) == EFI_INVALID_PARAMETER);
  LegacyStorageRomReset(); free(Original); free(Shadow); free(Saved);
  puts("PASS: 12000 deterministic malformed-input/mutation cases, allocation failures, overflow and read-only checks (ASan/UBSan)");
  return 0;
}

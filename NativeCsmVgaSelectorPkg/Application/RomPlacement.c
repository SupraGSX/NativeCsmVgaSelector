/** Examine a copied ROM contract, not a free-memory allocator or dispatcher.
    SPDX-License-Identifier: GPL-3.0-only */
#include "RomPlacement.h"

static unsigned int Le16 (const unsigned char *P)
{ return (unsigned int)P[0] | ((unsigned int)P[1] << 8); }

void NcvRomPlacementReadFacts (const unsigned char *Rom, unsigned long long Bytes,
                              NCV_ROM_PLACEMENT_FACTS *Facts)
{
  unsigned int Pcir, Length, InitBytes, Index, Sum = 0;
  if (!Facts) { return; }
  Facts->Valid = Facts->Revision = Facts->ImageBytes = Facts->MaxRuntimeBytes = 0;
  if (!Rom || Bytes < 512 || Bytes > 65536 || Bytes % 512 != 0 ||
      Rom[0] != 0x55 || Rom[1] != 0xaa) { return; }
  InitBytes = (unsigned int)Rom[2] * 512;
  Pcir = Le16 (Rom + 0x18);
  if (InitBytes == 0 || InitBytes > Bytes || Pcir < 0x1c || Pcir > InitBytes ||
      0x18 > InitBytes - Pcir) { return; }
  if (Rom[Pcir] != 'P' || Rom[Pcir + 1] != 'C' || Rom[Pcir + 2] != 'I' || Rom[Pcir + 3] != 'R' ||
      Rom[Pcir + 0x14] != 0 || (unsigned int)Le16 (Rom + Pcir + 0x10) * 512 != Bytes) { return; }
  Length = Le16 (Rom + Pcir + 0x0a);
  if (Length < 0x18 || Length > InitBytes - Pcir ||
      (Rom[Pcir + 0x0c] >= 3 && Length < 0x1c)) { return; }
  for (Index = 0; Index < Bytes; ++Index) { Sum = (Sum + Rom[Index]) & 255; }
  if (Sum) { return; }
  Sum = 0;
  for (Index = 0; Index < InitBytes; ++Index) { Sum = (Sum + Rom[Index]) & 255; }
  if (Sum) { return; }
  Facts->Revision = Rom[Pcir + 0x0c];
  Facts->ImageBytes = (unsigned int)Bytes;
  if (Facts->Revision >= 3) {
    Facts->MaxRuntimeBytes = Le16 (Rom + Pcir + 0x16) * 512;
    if (Facts->MaxRuntimeBytes > InitBytes ||
        (Facts->MaxRuntimeBytes && Facts->MaxRuntimeBytes < Pcir + Length)) {
      Facts->Valid = 0; return;
    }
  }
  Facts->Valid = 1;
}

NCV_ROM_PLACEMENT_RESULT NcvRomPlacementAssess (
  const NCV_ROM_PLACEMENT_FACTS *Facts, unsigned int RuntimeLimit)
{
  if (!Facts || Facts->Valid != 1 || Facts->ImageBytes == 0 || Facts->ImageBytes > 65536 ||
      RuntimeLimit == 0 || RuntimeLimit > 65536) { return NcvPlacementInvalidRom; }
  if (Facts->Revision < 3) { return NcvPlacementInPlaceRom; }
  if (Facts->MaxRuntimeBytes == 0) { return NcvPlacementUnknownRuntime; }
  if (Facts->MaxRuntimeBytes > Facts->ImageBytes) { return NcvPlacementInvalidRom; }
  if (Facts->MaxRuntimeBytes > RuntimeLimit) { return NcvPlacementRuntimeOverlap; }
  /* This is deliberately not a success/permission result. Firmware support,
     an owned initialization destination and a tested executor are still absent. */
  return NcvPlacementNeedsFirmwareAndBackend;
}

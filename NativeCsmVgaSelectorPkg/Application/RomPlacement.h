/** Read-only necessary-condition checks; never authorizes ROM execution.
    SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_ROM_PLACEMENT_H_
#define NCV_ROM_PLACEMENT_H_

typedef struct {
  unsigned int Valid;
  unsigned int Revision;
  unsigned int ImageBytes;
  unsigned int MaxRuntimeBytes;
} NCV_ROM_PLACEMENT_FACTS;

typedef enum {
  NcvPlacementInvalidRom,
  NcvPlacementInPlaceRom,
  NcvPlacementUnknownRuntime,
  NcvPlacementRuntimeOverlap,
  NcvPlacementNeedsFirmwareAndBackend
} NCV_ROM_PLACEMENT_RESULT;

/* Bytes is the actual accessible, selected x86 image length, not an untrusted
   header length. The independent full OptionRom validator is still required. */
void NcvRomPlacementReadFacts (const unsigned char *Rom, unsigned long long Bytes,
                              NCV_ROM_PLACEMENT_FACTS *Facts);
NCV_ROM_PLACEMENT_RESULT NcvRomPlacementAssess (
  const NCV_ROM_PLACEMENT_FACTS *Facts, unsigned int RuntimeLimit);
#endif

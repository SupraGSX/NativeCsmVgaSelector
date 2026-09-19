/** @file
  Bounded, read-only PCI option-ROM parsing and target validation.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_SELECTOR_OPTION_ROM_H_
#define NATIVE_CSM_VGA_SELECTOR_OPTION_ROM_H_

#include <Uefi.h>

#include "Log.h"

/**
  Results from a successful, complete option-ROM walk.

  MatchingLegacyImageOffset and MatchingLegacyImageSize identify the first
  checksum-valid PCAT image matching the requested vendor/device pair.  They
  are offsets and lengths inside the caller-owned exposed ROM buffer; this
  module never copies, dispatches, shadows, or modifies that buffer.
**/
typedef struct {
  UINTN    ImageCount;
  UINTN    MatchingLegacyCandidateCount;
  UINT64   MatchingLegacyImageOffset;
  UINT64   MatchingLegacyImageSize;
  UINT64   MatchingLegacyPcirOffset;
  UINT16   MatchingLegacyCodeRevision;
  UINT8    MatchingLegacyPcirRevision;
  BOOLEAN  FinalIndicatorSeen;
  BOOLEAN  MatchingLegacyImageFound;
  BOOLEAN  MatchingUefiImageSeen;
} OPTION_ROM_VALIDATION;

/**
  Enumerates and validates every image in an exposed PCI option ROM.

  RomSize must exactly equal ExpectedRomSize.  Every byte access is bounded by
  RomSize, every image must make forward progress, and the final PCIR image
  must end exactly at RomSize.  PCI 3.0 device-ID lists and optional metadata
  are walked only inside the containing image.  For PCAT images, the complete
  declared PCIR extent must remain inside the legacy initialization region and,
  when nonzero, the PCI 3.x maximum run-time region.  Optional Configuration
  Utility headers and their declared code extents are validated but not run.

  Success requires at least one PCAT image for ExpectedVendorId and
  ExpectedDeviceId whose PCIR-sized image checksum and legacy Size512 checksum
  both equal zero.  EFI/UEFI images are reported but are never selected.

  This routine is read-only.  It does not call CheckPciRom(), InstallPciRom(),
  or any PCI/CSM service.
**/
EFI_STATUS
OptionRomParseAndValidate (
  IN  APP_LOGGER             *Logger,
  IN  CONST VOID             *RomImage,
  IN  UINT64                 RomSize,
  IN  UINT64                 ExpectedRomSize,
  IN  UINT16                 ExpectedVendorId,
  IN  UINT16                 ExpectedDeviceId,
  OUT OPTION_ROM_VALIDATION  *Validation
  );

#endif

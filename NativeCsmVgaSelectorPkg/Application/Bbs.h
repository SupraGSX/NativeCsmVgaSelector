/** @file
  Read-only reporting for firmware-owned Legacy BIOS BBS data.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_SELECTOR_BBS_H_
#define NATIVE_CSM_VGA_SELECTOR_BBS_H_

#include <Uefi.h>

#include <Protocol/LegacyBios.h>

#include "Log.h"
#include "CandidateIni.h"

#define BBS_PROBE_DESCRIPTION_CHARS  129U

typedef struct {
  UINT16  Segment;
  UINT8   Bus;
  UINT8   Device;
  UINT8   Function;
} BBS_CONTROLLER_TARGET;

typedef struct {
  BOOLEAN     Called;
  EFI_STATUS  Result;
  BOOLEAN     CountsValid;
  UINT16      HddCount;
  UINT16      BbsCount;
  BOOLEAN     TargetRequested;
  BOOLEAN     TargetSegmentRepresentable;
  UINTN       TargetBbsMatches;
  UINTN       TargetHardDiskMatches;
  UINTN       TargetBbsIndex;
  CHAR16      TargetDescription[BBS_PROBE_DESCRIPTION_CHARS];
  NCV_STORAGE_COLLECTION Storage;
} BBS_PROBE_SUMMARY;

/**
  Performs the existing bounded BBS probe and optionally returns a summary.

  Result is the exact status returned by this function. Called is set only
  immediately before invoking GetBbsInfo. CountsValid is set only after a
  successful query has passed the existing count, NULL-table, and safety-cap
  checks; HddCount and BbsCount remain zero otherwise.
**/
EFI_STATUS
BbsProbeAndPrintEx (
  IN  APP_LOGGER                *Logger,
  IN  EFI_LEGACY_BIOS_PROTOCOL  *LegacyBios,
  IN  CONST BBS_CONTROLLER_TARGET *Target OPTIONAL,
  OUT BBS_PROBE_SUMMARY         *Summary OPTIONAL
  );

/** Preserves the original BBS probe API without requesting a summary. **/
EFI_STATUS
BbsProbeAndPrint (
  IN APP_LOGGER                *Logger,
  IN EFI_LEGACY_BIOS_PROTOCOL  *LegacyBios
  );

#endif

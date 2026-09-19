/** @file
  Bounded, read-only BootOrder and legacy BBS boot-option reporting.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_PROBE_BOOT_OPTIONS_H_
#define NATIVE_CSM_VGA_PROBE_BOOT_OPTIONS_H_

#include <Uefi.h>

#include "Log.h"
#include "ProbeConfig.h"

typedef struct {
  EFI_STATUS  BootOrderStatus;
  UINTN       BootOrderEntries;
  UINTN       GenericBbsOptions;
  UINTN       ActiveGenericHardDiskOptions;
  UINTN       ConfiguredDescriptionMatches;
  UINTN       ExcludedDescriptionMatches;
} BOOT_OPTION_PROBE_SUMMARY;

/**
  Reads only BootOrder and referenced Boot#### variables, validates each
  EFI_LOAD_OPTION in bounds, and logs active generic BBS legacy choices.
  A malformed or unrelated option is skipped rather than dereferenced.
**/
EFI_STATUS
BootOptionsProbe (
  IN  APP_LOGGER                  *Logger,
  IN  CONST PROBE_CONFIG          *Config OPTIONAL,
  OUT BOOT_OPTION_PROBE_SUMMARY   *Summary OPTIONAL
  );

#endif

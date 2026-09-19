/** @file
  Strict, minimal configuration parser for the read-only Native CSM VGA probe.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_PROBE_CONFIG_H_
#define NATIVE_CSM_VGA_PROBE_CONFIG_H_

#include <Uefi.h>

#include "AppFile.h"
#include "Log.h"
#include "CandidateIni.h"

#define PROBE_CONFIG_TEXT_CHARS  65U

typedef struct {
  UINT16  Segment;
  UINT8   Bus;
  UINT8   Device;
  UINT8   Function;
} PROBE_PCI_ADDRESS;


typedef enum {
  NativeCsmVgaEndpointIgnore = 0,
  NativeCsmVgaEndpointOn,
  NativeCsmVgaEndpointOff
} NATIVE_CSM_VGA_ENDPOINT_POLICY;

typedef struct {
  BOOLEAN  Present;
  BOOLEAN  FirstRun;
  BOOLEAN  Probe;
  BOOLEAN  DiscoveryMode;
  BOOLEAN  HasTargetPci;
  BOOLEAN  HasTargetControllerPci;
  BOOLEAN  HasExpectedVendor;
  BOOLEAN  HasExpectedDevice;
  BOOLEAN  HasExpectedSubsystemVendor;
  BOOLEAN  HasExpectedSubsystemDevice;
  BOOLEAN  HasLegacyOptionDescription;
  BOOLEAN  HasExcludeDescription;
  BOOLEAN  HasTargetBbsDescription;
  BOOLEAN  HasVerboseLog;
  BOOLEAN  HasAutoBoot;
  /* Reserved ABI fields; no public INI key enables reference snapshots. */
  BOOLEAN  HasRequireReferenceSnapshotMatch;
  BOOLEAN  HasIoDecoding;
  BOOLEAN  HasMemoryDecoding;
  BOOLEAN  HasBusMastering;
  BOOLEAN  VerboseLog;
  BOOLEAN  AutoBoot;
  BOOLEAN  RequireReferenceSnapshotMatch;
  NATIVE_CSM_VGA_ENDPOINT_POLICY  IoDecoding;
  NATIVE_CSM_VGA_ENDPOINT_POLICY  MemoryDecoding;
  NATIVE_CSM_VGA_ENDPOINT_POLICY  BusMastering;
  PROBE_PCI_ADDRESS TargetPci;
  PROBE_PCI_ADDRESS TargetControllerPci;
  UINT16   ExpectedVendor;
  UINT16   ExpectedDevice;
  UINT16   ExpectedSubsystemVendor;
  UINT16   ExpectedSubsystemDevice;
  CHAR16   Name[PROBE_CONFIG_TEXT_CHARS];
  CHAR16   LegacyOptionDescription[PROBE_CONFIG_TEXT_CHARS];
  CHAR16   ExcludeDescription[PROBE_CONFIG_TEXT_CHARS];
  CHAR16   TargetBbsDescription[PROBE_CONFIG_TEXT_CHARS];
} PROBE_CONFIG;

/**
  Loads the strict sibling Config.ini configuration.  EFI_NOT_FOUND is
  deliberately converted to discovery mode.  Every other parse error is
  returned after it has been logged; callers must not substitute a target.
**/
EFI_STATUS
ProbeConfigLoad (
  IN  APP_FILE_CONTEXT  *Files,
  IN  APP_LOGGER        *Logger,
  OUT PROBE_CONFIG      *Config
  );

EFI_STATUS
ProbeConfigWriteCandidateIni (
  IN APP_FILE_CONTEXT                 *Files,
  IN APP_LOGGER                       *Logger,
  IN CONST NCV_DISPLAY_CANDIDATE      *DisplayCandidates,
  IN UINTN                            DisplayCandidateCount,
  IN UINTN                            SelectedDisplayIndex,
  IN CONST NCV_STORAGE_CANDIDATE      *StorageCandidates,
  IN UINTN                            StorageCandidateCount,
  IN UINTN                            SelectedStorageIndex,
  IN BOOLEAN                          FirstRun
  );

VOID
ProbeConfigGetBootRequirements (
  IN  CONST PROBE_CONFIG    *Config,
  OUT NCV_BOOT_REQUIREMENTS *Requirements
  );

VOID
ProbeConfigPrintAddress (
  IN APP_LOGGER               *Logger,
  IN CONST CHAR16             *Label,
  IN CONST PROBE_PCI_ADDRESS  *Address
  );

BOOLEAN
ProbeConfigAddressEqual (
  IN CONST PROBE_PCI_ADDRESS  *Left,
  IN CONST PROBE_PCI_ADDRESS  *Right
  );

VOID ProbeConfigBeginEdit(VOID);
VOID ProbeConfigEndEdit (VOID);
BOOLEAN ProbeConfigIsEditing(VOID);
CONST NCV_CONFIG_CORE *ProbeConfigReviewSettings (VOID);

#endif

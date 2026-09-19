/** @file
  Probe-to-native-CSM-boot validated runtime plan.

  The plan owns the PCI inventory and legacy boot-target context so every
  pointer consumed by the one native LegacyBoot() call remains valid.
**/

#ifndef NATIVE_CSM_VGA_BOOT_RUNTIME_PLAN_H_
#define NATIVE_CSM_VGA_BOOT_RUNTIME_PLAN_H_

#include <Uefi.h>

#include <Protocol/LegacyBios.h>
#include <Protocol/LegacyRegion2.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>

#include "AppFile.h"
#include "Log.h"
#include "OptionRom.h"
#include "PciEnumerate.h"
#include "ProbeConfig.h"
#include "LegacyBootTarget.h"

#define NATIVE_CSM_VGA_BOOT_MAX_BRIDGES  16U

typedef struct {
  CONST PCI_DEVICE_RECORD             *InventoryRecord;
  EFI_HANDLE                          Handle;
  EFI_PCI_IO_PROTOCOL                 *PciIo;
  UINT16                              Segment;
  UINT8                               Bus;
  UINT8                               Device;
  UINT8                               Function;
  UINT16                              Vendor;
  UINT16                              DeviceId;
  UINT16                              SubsystemVendor;
  UINT16                              SubsystemDevice;
  EFI_HANDLE                          RootBridgeHandle;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL     *RootBridgeIo;
  EFI_HANDLE                          ParentHostHandle;
  UINT16                              OriginalCommand;
  UINT64                              OriginalAttributes;
  EFI_STATUS                          OriginalAttributesStatus;
} NATIVE_CSM_VGA_PLAN_ENDPOINT;

typedef struct {
  PCI_DEVICE_RECORD  Record;
  UINT16             OriginalBridgeControl;
  BOOLEAN            WriteAttempted;
  BOOLEAN            WriteApplied;
} NATIVE_CSM_VGA_PLAN_BRIDGE;

typedef struct {
  EFI_COMPATIBILITY16_TABLE  *Table;
  EFI_PHYSICAL_ADDRESS       Address;
  UINT8                      TableLength;
  UINT8                      StoredChecksum;
  UINT8                      CalculatedChecksum;
  UINT16                     CallSegment;
  UINT16                     CallOffset;
  EFI_PHYSICAL_ADDRESS       CallAddress;
  UINT16                     PnpSegment;
  UINT16                     PnpOffset;
  BOOLEAN                    ChecksumValid;
  BOOLEAN                    StructuralCorroboration;
  BOOLEAN                    StaleChecksumAccepted;
} NATIVE_CSM_VGA_PLAN_COMPATIBILITY16;

typedef struct {
  PROBE_CONFIG                         Config;
  PCI_INVENTORY                        Inventory;
  EFI_LEGACY_BIOS_PROTOCOL             *LegacyBios;
  EFI_LEGACY_REGION2_PROTOCOL          *LegacyRegion2;

  NATIVE_CSM_VGA_PLAN_ENDPOINT         Target;
  NATIVE_CSM_VGA_PLAN_ENDPOINT         Active;
  NATIVE_CSM_VGA_PLAN_ENDPOINT         StorageController;

  CONST UINT8                          *ExposedRom;
  UINT64                               ExposedRomSize;
  CONST UINT8                          *SelectedRom;
  UINT64                               SelectedRomOffset;
  UINTN                                SelectedRomSize;
  UINT16                               SelectedRomVendor;
  UINT16                               SelectedRomDevice;
  OPTION_ROM_VALIDATION                RomValidation;

  NATIVE_CSM_VGA_PLAN_BRIDGE           ActivePath[
                                          NATIVE_CSM_VGA_BOOT_MAX_BRIDGES
                                          ];
  NATIVE_CSM_VGA_PLAN_BRIDGE           TargetPath[
                                          NATIVE_CSM_VGA_BOOT_MAX_BRIDGES
                                          ];
  NATIVE_CSM_VGA_PLAN_BRIDGE           SharedPath[
                                          NATIVE_CSM_VGA_BOOT_MAX_BRIDGES
                                          ];
  NATIVE_CSM_VGA_PLAN_BRIDGE           ActiveExclusivePath[
                                          NATIVE_CSM_VGA_BOOT_MAX_BRIDGES
                                          ];
  NATIVE_CSM_VGA_PLAN_BRIDGE           TargetExclusivePath[
                                          NATIVE_CSM_VGA_BOOT_MAX_BRIDGES
                                          ];
  UINTN                                 ActivePathCount;
  UINTN                                 TargetPathCount;
  UINTN                                 SharedPathCount;
  UINTN                                 ActiveExclusiveCount;
  UINTN                                 TargetExclusiveCount;

  NATIVE_CSM_VGA_PLAN_COMPATIBILITY16  Compatibility16;

  PROBE_PCI_ADDRESS                    StorageControllerAddress;
  LEGACY_BOOT_BOOT_TARGET_CONTEXT           BootTarget;
  UINT16                               UniqueBbsIndex;
  BBS_TABLE                            UniqueBbsEntry;
  BBS_BBS_DEVICE_PATH                  *BootOption;
  VOID                                 *OpaqueLoadOptions;
  UINT32                               OpaqueLoadOptionsSize;

  UINT16                               InitialInt10Offset;
  UINT16                               InitialInt10Segment;

  BOOLEAN                              EndpointWriteAttempted;
  BOOLEAN                              EndpointWriteApplied;
  BOOLEAN                              RouteDirty;
  BOOLEAN                              ValidationCompleted;
  BOOLEAN                              ValidationPassed;
  BOOLEAN                              Complete;
} NATIVE_CSM_VGA_RUNTIME_PLAN;

EFI_STATUS
NativeCsmVgaRuntimePlanBuild (
  IN  APP_FILE_CONTEXT                 *Files,
  IN  APP_LOGGER                       *Logger,
  IN  CONST PROBE_CONFIG               *Config,
  OUT NATIVE_CSM_VGA_RUNTIME_PLAN      *Plan
  );

EFI_STATUS
NativeCsmVgaRuntimePlanFinalizeValidation (
  IN     APP_LOGGER                    *Logger,
  IN OUT NATIVE_CSM_VGA_RUNTIME_PLAN   *Plan
  );

EFI_STATUS
NativeCsmVgaRuntimePlanRelease (
  IN OUT NATIVE_CSM_VGA_RUNTIME_PLAN  *Plan
  );

#endif

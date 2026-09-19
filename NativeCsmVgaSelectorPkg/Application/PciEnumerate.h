/** @file
  Read-only PCI enumeration and selected-GPU topology reporting.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_SELECTOR_PCI_ENUMERATE_H_
#define NATIVE_CSM_VGA_SELECTOR_PCI_ENUMERATE_H_

#include <Uefi.h>

#include <Protocol/DevicePath.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>

#include "Log.h"

#define PCI_PROBE_CONFIG_BYTES  64

typedef struct {
  EFI_HANDLE                Handle;
  EFI_PCI_IO_PROTOCOL       *PciIo;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  UINTN                     DevicePathSize;
  EFI_STATUS                DevicePathStatus;
  UINTN                     Segment;
  UINTN                     Bus;
  UINTN                     Device;
  UINTN                     Function;
  EFI_STATUS                LocationStatus;
  EFI_STATUS                ConfigStatus;
  UINT8                     Config[PCI_PROBE_CONFIG_BYTES];
  UINT64                    SupportedAttributes;
  UINT64                    CurrentAttributes;
  EFI_STATUS                SupportedAttributesStatus;
  EFI_STATUS                CurrentAttributesStatus;
  EFI_HANDLE                RootBridgeHandle;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL  *RootBridgeIo;
  UINTN                     RootBridgePrefixSize;
  EFI_STATUS                RootBridgeStatus;
  UINT64                    RootSupportedAttributes;
  UINT64                    RootCurrentAttributes;
  EFI_STATUS                RootAttributesStatus;
} PCI_DEVICE_RECORD;

typedef struct {
  PCI_DEVICE_RECORD  *Devices;
  UINTN              Count;
  UINTN              HandleCount;
} PCI_INVENTORY;

EFI_STATUS
PciInventoryBuild (
  IN  APP_LOGGER     *Logger,
  OUT PCI_INVENTORY  *Inventory
  );

VOID
PciInventoryRelease (
  IN OUT PCI_INVENTORY  *Inventory
  );

EFI_STATUS
PciPrintDisplayDevices (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory
  );

EFI_STATUS
PciPrintSelectedBridgePath (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory,
  IN UINTN                Segment,
  IN UINTN                Bus,
  IN UINTN                Device,
  IN UINTN                Function
  );

EFI_STATUS
PciPrintAllDisplayBridgePaths (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory
  );

/**
  Builds the operating bridge path from strict PCI device-path ancestry.

  This is the mutation-gate form of the read-only Probe path logic: no
  secondary-bus fallback is permitted.  The returned records are ordered from
  the root-most PCI-to-PCI bridge to the bridge directly above Selected, and
  every primary/secondary/subordinate bus relationship is cross-checked.
**/
EFI_STATUS
PciBuildValidatedDevicePathBridgePath (
  IN  APP_LOGGER                    *Logger,
  IN  CONST PCI_INVENTORY           *Inventory,
  IN  CONST PCI_DEVICE_RECORD       *Selected,
  OUT PCI_DEVICE_RECORD             *Bridges OPTIONAL,
  IN  UINTN                         BridgeCapacity,
  OUT UINTN                         *BridgeCount
  );

/**
  Reports strict device-path ancestry discovery and bus-register validation as
  separate read-only facts.  This does not alter the existing operating-path
  builder used by Boot.
**/
EFI_STATUS
PciProbeDevicePathBridgePathStatus (
  IN  APP_LOGGER                    *Logger,
  IN  CONST PCI_INVENTORY           *Inventory,
  IN  CONST PCI_DEVICE_RECORD       *Selected,
  OUT BOOLEAN                       *PathDiscovered,
  OUT BOOLEAN                       *PathValidated
  );

EFI_STATUS
PciVerifyReadOnlySnapshot (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory
  );

#endif

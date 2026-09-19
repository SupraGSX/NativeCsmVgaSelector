/** @file
  NativeCsmVgaProbe: a completely read-only configuration and discovery tool.

  This application intentionally contains no transaction, routing, ROM
  dispatch, controller-disconnect, BBS write, UEFI-variable write, or boot
  operation.  It reports the native CSM and PCI topology required by later,
  separately reviewed work without modifying that topology.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include <Uefi.h>

#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LegacyRegion2.h>
#include <Protocol/PciIo.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "AppFile.h"
#include "Bbs.h"
#include "BootOptions.h"
#include "Log.h"
#include "MemoryMap.h"
#include "OptionRom.h"
#include "PciEnumerate.h"
#include "ProbeConfig.h"
#include "StatusPrint.h"
#include "NativeCsmVgaProbe.h"
#include "CandidateIni.h"
#include "TargetSelection.h"
#include <Protocol/LegacyBios.h>

#define PROBE_VERSION                 L"Native CSM VGA Selector 1.2"
#define PROBE_LOG_FILE                L"NativeCsmVgaProbe.log"
#define PCI_VENDOR_ID_OFFSET          0x00U
#define PCI_DEVICE_ID_OFFSET          0x02U
#define PCI_SUBSYSTEM_VENDOR_OFFSET   0x2CU
#define PCI_SUBSYSTEM_DEVICE_OFFSET   0x2EU
#define PCI_CLASS_OFFSET              0x0BU
#define PCI_SUBCLASS_OFFSET           0x0AU
#define PCI_PROGIF_OFFSET             0x09U
#define PCI_HEADER_TYPE_OFFSET        0x0EU
#define PCI_PRIMARY_BUS_OFFSET        0x18U
#define PCI_SECONDARY_BUS_OFFSET      0x19U
#define PCI_SUBORDINATE_BUS_OFFSET    0x1AU
#define PCI_BRIDGE_CONTROL_OFFSET     0x3EU
#define PCI_HEADER_TYPE_MASK          0x7FU
#define PCI_HEADER_TYPE_BRIDGE        0x01U
#define PCI_BASE_CLASS_DISPLAY        0x03U
#define PCI_BASE_CLASS_STORAGE        0x01U
#define PCI_BASE_CLASS_BRIDGE         0x06U
#define PCI_SUBCLASS_P2P              0x04U
#define PCI_BRIDGE_CONTROL_VGA        BIT3
#define PCI_BRIDGE_CONTROL_VGA_16     BIT4
#define PROBE_COMPAT_SCAN_START       0xE0000U
#define PROBE_COMPAT_SCAN_END         0x100000U
#define PROBE_COMPAT_ALIGNMENT        16U
#define PROBE_COMPAT_MAX_TABLE_LENGTH 255U

typedef struct {
  BOOLEAN              TargetFound;
  BOOLEAN              TargetIsDisplay;
  BOOLEAN              TargetIdentityMatches;
  BOOLEAN              TargetRomLegacyValid;
  EFI_STATUS           TargetRomStatus;
  EFI_STATUS           TargetPathStatus;
  UINTN                LegacyCandidateCount;
  NCV_DISPLAY_COLLECTION Displays;
  UINTN                 SelectedCandidate;
} VIDEO_DISCOVERY;

STATIC
UINT16
Read16 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8));
}

STATIC
BOOLEAN
RecordHasValidLocation (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (BOOLEAN)((Record != NULL) && !EFI_ERROR (Record->LocationStatus) &&
                   (Record->Segment <= MAX_UINT16) && (Record->Bus <= MAX_UINT8) &&
                   (Record->Device <= 31) && (Record->Function <= 7));
}

STATIC
BOOLEAN
RecordHasDisplayClass (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (BOOLEAN)((Record != NULL) && !EFI_ERROR (Record->ConfigStatus) &&
                   (Record->Config[PCI_CLASS_OFFSET] == PCI_BASE_CLASS_DISPLAY));
}

STATIC
BOOLEAN
RecordIsDisplay (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (BOOLEAN)(RecordHasValidLocation (Record) &&
                   RecordHasDisplayClass (Record));
}

STATIC
BOOLEAN
RecordIsP2pBridge (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (BOOLEAN)(RecordHasValidLocation (Record) &&
                   !EFI_ERROR (Record->ConfigStatus) &&
                   ((Record->Config[PCI_HEADER_TYPE_OFFSET] & PCI_HEADER_TYPE_MASK) ==
                    PCI_HEADER_TYPE_BRIDGE) &&
                   (Record->Config[PCI_CLASS_OFFSET] == PCI_BASE_CLASS_BRIDGE) &&
                   (Record->Config[PCI_SUBCLASS_OFFSET] == PCI_SUBCLASS_P2P));
}

STATIC
CONST PCI_DEVICE_RECORD *
FindRecord (
  IN CONST PCI_INVENTORY      *Inventory,
  IN CONST PROBE_PCI_ADDRESS  *Address
  )
{
  UINTN  Index;

  if ((Inventory == NULL) || (Address == NULL)) {
    return NULL;
  }

  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Record;

    Record = &Inventory->Devices[Index];
    if (RecordHasValidLocation (Record) &&
        (Record->Segment == Address->Segment) &&
        (Record->Bus == Address->Bus) &&
        (Record->Device == Address->Device) &&
        (Record->Function == Address->Function))
    {
      return Record;
    }
  }

  return NULL;
}

STATIC
BOOLEAN
RecordIdentityMatchesConfig (
  IN CONST PCI_DEVICE_RECORD  *Record,
  IN CONST PROBE_CONFIG       *Config
  )
{
  UINT16  Vendor;
  UINT16  Device;
  UINT16  SubVendor;
  UINT16  SubDevice;

  if ((Record == NULL) || (Config == NULL) || EFI_ERROR (Record->ConfigStatus)) {
    return FALSE;
  }

  Vendor = Read16 (&Record->Config[PCI_VENDOR_ID_OFFSET]);
  Device = Read16 (&Record->Config[PCI_DEVICE_ID_OFFSET]);
  SubVendor = Read16 (&Record->Config[PCI_SUBSYSTEM_VENDOR_OFFSET]);
  SubDevice = Read16 (&Record->Config[PCI_SUBSYSTEM_DEVICE_OFFSET]);
  return (BOOLEAN)((!Config->HasExpectedVendor || (Vendor == Config->ExpectedVendor)) &&
                   (!Config->HasExpectedDevice || (Device == Config->ExpectedDevice)) &&
                   (!Config->HasExpectedSubsystemVendor ||
                    (SubVendor == Config->ExpectedSubsystemVendor)) &&
                   (!Config->HasExpectedSubsystemDevice ||
                    (SubDevice == Config->ExpectedSubsystemDevice)));
}

STATIC
BOOLEAN
DevicePathPrefix (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *Prefix,
  IN UINTN                           PrefixSize,
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *Path,
  IN UINTN                           PathSize
  )
{
  UINTN  CompareSize;

  if ((Prefix == NULL) || (Path == NULL) ||
      (PrefixSize <= END_DEVICE_PATH_LENGTH) || (PathSize < PrefixSize))
  {
    return FALSE;
  }

  CompareSize = PrefixSize - END_DEVICE_PATH_LENGTH;
  return (BOOLEAN)(CompareMem (Prefix, Path, CompareSize) == 0);
}

STATIC
BOOLEAN
StrictRecordPathPrefix (
  IN CONST PCI_DEVICE_RECORD  *Ancestor,
  IN CONST PCI_DEVICE_RECORD  *Descendant
  )
{
  if ((Ancestor == NULL) || (Descendant == NULL) ||
      (Ancestor->DevicePathSize >= Descendant->DevicePathSize))
  {
    return FALSE;
  }

  return DevicePathPrefix (
           Ancestor->DevicePath,
           Ancestor->DevicePathSize,
           Descendant->DevicePath,
           Descendant->DevicePathSize
           );
}

STATIC
EFI_STATUS
BuildDynamicBridgePath (
  IN  CONST PCI_INVENTORY      *Inventory,
  IN  CONST PCI_DEVICE_RECORD  *Selected,
  OUT UINTN                    *Path,
  OUT UINTN                    *PathCount
  )
{
  UINTN                            Index;
  UINTN                            Inner;
  UINTN                            Value;
  UINTN                            PciNodeCount;
  UINTN                            Offset;
  CONST EFI_DEVICE_PATH_PROTOCOL   *Node;
  UINTN                            NodeSize;
  UINTN                            ChildBus;

  if ((Inventory == NULL) || (Selected == NULL) || (Path == NULL) ||
      (PathCount == NULL) || (Selected->DevicePath == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *PathCount = 0;
  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Candidate;

    Candidate = &Inventory->Devices[Index];
    if (RecordIsP2pBridge (Candidate) && (Candidate->Segment == Selected->Segment) &&
        StrictRecordPathPrefix (Candidate, Selected))
    {
      Path[(*PathCount)++] = Index;
    }
  }

  for (Index = 1; Index < *PathCount; ++Index) {
    Value = Path[Index];
    Inner = Index;
    while ((Inner != 0) &&
           (Inventory->Devices[Path[Inner - 1U]].DevicePathSize >
            Inventory->Devices[Value].DevicePathSize))
    {
      Path[Inner] = Path[Inner - 1U];
      --Inner;
    }
    Path[Inner] = Value;
  }

  PciNodeCount = 0;
  Offset = 0;
  while (Offset + sizeof (EFI_DEVICE_PATH_PROTOCOL) <= Selected->DevicePathSize) {
    Node = (CONST EFI_DEVICE_PATH_PROTOCOL *)((CONST UINT8 *)Selected->DevicePath + Offset);
    NodeSize = DevicePathNodeLength (Node);
    if ((NodeSize < sizeof (EFI_DEVICE_PATH_PROTOCOL)) ||
        (NodeSize > Selected->DevicePathSize - Offset))
    {
      return EFI_COMPROMISED_DATA;
    }

    if ((DevicePathType (Node) == HARDWARE_DEVICE_PATH) &&
        (DevicePathSubType (Node) == HW_PCI_DP))
    {
      ++PciNodeCount;
    }

    Offset += NodeSize;
    if (IsDevicePathEnd (Node)) {
      break;
    }
  }

  if ((PciNodeCount == 0) || ((*PathCount + 1U) != PciNodeCount)) {
    return EFI_NO_MAPPING;
  }

  for (Index = 0; Index < *PathCount; ++Index) {
    CONST PCI_DEVICE_RECORD  *Bridge;

    Bridge = &Inventory->Devices[Path[Index]];
    ChildBus = (Index + 1U < *PathCount) ?
               Inventory->Devices[Path[Index + 1U]].Bus : Selected->Bus;
    if ((Bridge->Config[PCI_PRIMARY_BUS_OFFSET] != Bridge->Bus) ||
        (Bridge->Config[PCI_SECONDARY_BUS_OFFSET] != ChildBus) ||
        (ChildBus > Bridge->Config[PCI_SUBORDINATE_BUS_OFFSET]))
    {
      return EFI_NO_MAPPING;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ReportTargetBridgePath (
  IN  APP_LOGGER               *Logger,
  IN  CONST PCI_INVENTORY      *Inventory,
  IN  CONST PCI_DEVICE_RECORD  *Target
  )
{
  EFI_STATUS  Status;
  UINTN       *Path;
  UINTN       PathCount;
  UINTN       Index;

  if ((Logger == NULL) || (Inventory == NULL) || (Target == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Path = AllocateZeroPool (Inventory->Count * sizeof (*Path));
  if (Path == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  LogPrint (Logger, L"\r\n=== Configured target VGA dynamic bridge path ===\r\n");
  Status = BuildDynamicBridgePath (Inventory, Target, Path, &PathCount);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"Target bridge chain is not unique/valid: 0x%016lx (%s)\r\n", (UINT64)Status, EfiStatusName (Status));
    FreePool (Path);
    return Status;
  }

  LogPrint (Logger, L"Path source=device-path ancestry; bus-register cross-check=passed; bridge count=%u\r\n", (UINT32)PathCount);
  for (Index = 0; Index < PathCount; ++Index) {
    CONST PCI_DEVICE_RECORD  *Bridge;
    UINT16                   Control;

    Bridge = &Inventory->Devices[Path[Index]];
    Control = Read16 (&Bridge->Config[PCI_BRIDGE_CONTROL_OFFSET]);
    LogPrint (
      Logger,
      L"  [%u] %04x:%02x:%02x.%x buses=%02x/%02x/%02x bridge-control=0x%04x VGA-forward=%s VGA-16=%s expected-VGA-bit=supported-by-P2P\r\n",
      (UINT32)Index,
      (UINT32)Bridge->Segment,
      (UINT32)Bridge->Bus,
      (UINT32)Bridge->Device,
      (UINT32)Bridge->Function,
      Bridge->Config[PCI_PRIMARY_BUS_OFFSET],
      Bridge->Config[PCI_SECONDARY_BUS_OFFSET],
      Bridge->Config[PCI_SUBORDINATE_BUS_OFFSET],
      Control,
      (Control & PCI_BRIDGE_CONTROL_VGA) != 0 ? L"on" : L"off",
      (Control & PCI_BRIDGE_CONTROL_VGA_16) != 0 ? L"on" : L"off"
      );
  }

  FreePool (Path);
  return EFI_SUCCESS;
}

STATIC
VOID
ReportRomCandidates (
  IN  APP_LOGGER           *Logger,
  IN  CONST PCI_INVENTORY  *Inventory,
  IN  CONST PROBE_CONFIG   *Config,
  OUT VIDEO_DISCOVERY      *Discovery
  )
{
  UINTN  Index;

  if ((Logger == NULL) || (Inventory == NULL) || (Discovery == NULL)) {
    return;
  }

  ZeroMem (Discovery, sizeof (*Discovery));
  Discovery->TargetRomStatus = EFI_NOT_FOUND;
  LogPrint (Logger, L"\r\n=== Bounded legacy-ROM candidates ===\r\n");
  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Record;
    OPTION_ROM_VALIDATION    Validation;
    EFI_STATUS               Status;
    UINT16                   Vendor;
    UINT16                   Device;
    BOOLEAN                  Target;
    NCV_DISPLAY_CANDIDATE    Collected;
    EFI_STATUS               PathStatus;
    BOOLEAN                  PathDiscovered;
    BOOLEAN                  PathValidated;

    Record = &Inventory->Devices[Index];
    if (!RecordHasDisplayClass (Record)) {
      continue;
    }

    Vendor = Read16 (&Record->Config[PCI_VENDOR_ID_OFFSET]);
    Device = Read16 (&Record->Config[PCI_DEVICE_ID_OFFSET]);
    ZeroMem (&Collected, sizeof (Collected));
    Collected.RawSegment = (NCV_SIZE)Record->Segment;
    Collected.RawBus = (NCV_SIZE)Record->Bus;
    Collected.RawDevice = (NCV_SIZE)Record->Device;
    Collected.RawFunction = (NCV_SIZE)Record->Function;
    Collected.AddressValid = (NCV_BOOL)RecordHasValidLocation (Record);
    if (Collected.AddressValid) {
      Collected.Address.Segment = (NCV_U16)Record->Segment;
      Collected.Address.Bus = (NCV_U8)Record->Bus;
      Collected.Address.Device = (NCV_U8)Record->Device;
      Collected.Address.Function = (NCV_U8)Record->Function;
    }
    Collected.Vendor = Vendor;
    Collected.DeviceId = Device;
    Collected.SubsystemVendor = Read16 (
                                    &Record->Config[
                                      PCI_SUBSYSTEM_VENDOR_OFFSET
                                      ]
                                    );
    Collected.SubsystemDevice = Read16 (
                                    &Record->Config[
                                      PCI_SUBSYSTEM_DEVICE_OFFSET
                                      ]
                                    );
    Collected.ActiveLegacyVgaOwnerKnown =
      (NCV_BOOL)!EFI_ERROR (Record->CurrentAttributesStatus);
    Collected.ActiveLegacyVgaOwner = (NCV_BOOL)(
      !Collected.ActiveLegacyVgaOwnerKnown ||
      ((Record->CurrentAttributes &
       (EFI_PCI_IO_ATTRIBUTE_VGA_IO | EFI_PCI_IO_ATTRIBUTE_VGA_IO_16 | EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY)) != 0)
      );
    if (EFI_ERROR (Record->CurrentAttributesStatus)) {
      LogPrint (
        Logger,
        L"  display inventory index=%u: active legacy-VGA ownership unknown; "
        L"ranked conservatively as active\r\n",
        (UINT32)Index
        );
    }
    PathDiscovered = FALSE;
    PathValidated = FALSE;
    if (Collected.AddressValid) {
      PathStatus = PciProbeDevicePathBridgePathStatus (
                     Logger,
                     Inventory,
                     Record,
                     &PathDiscovered,
                     &PathValidated
                     );
      Collected.PathDiscovered = (NCV_BOOL)PathDiscovered;
      Collected.PathValidated = (NCV_BOOL)PathValidated;
      if (EFI_ERROR (PathStatus)) {
        LogPrint (
          Logger,
          L"  %04x:%02x:%02x.%x: read-only path status=0x%016lx (%s)\r\n",
          (UINT32)Record->Segment,
          (UINT32)Record->Bus,
          (UINT32)Record->Device,
          (UINT32)Record->Function,
          (UINT64)PathStatus,
          EfiStatusName (PathStatus)
          );
      }
    } else {
      LogPrint (
        Logger,
        L"  display inventory index=%u has invalid/unavailable PCI location: "
        L"segment=0x%016lx bus=0x%016lx device=0x%016lx function=0x%016lx; "
        L"retained but ineligible\r\n",
        (UINT32)Index,
        (UINT64)Record->Segment,
        (UINT64)Record->Bus,
        (UINT64)Record->Device,
        (UINT64)Record->Function
        );
    }

    Target = (BOOLEAN)(Collected.AddressValid && (Config != NULL) &&
                       Config->HasTargetPci &&
                       (Record->Segment == Config->TargetPci.Segment) &&
                       (Record->Bus == Config->TargetPci.Bus) &&
                       (Record->Device == Config->TargetPci.Device) &&
                       (Record->Function == Config->TargetPci.Function));
    if (Target) {
      Discovery->TargetFound = TRUE;
      Discovery->TargetIsDisplay = TRUE;
      Discovery->TargetIdentityMatches = RecordIdentityMatchesConfig (Record, Config);
    }

    if ((Record->PciIo == NULL) || (Record->PciIo->RomImage == NULL) ||
        (Record->PciIo->RomSize == 0))
    {
      LogPrint (Logger, L"  %04x:%02x:%02x.%x: no exposed ROM\r\n", (UINT32)Record->Segment, (UINT32)Record->Bus, (UINT32)Record->Device, (UINT32)Record->Function);
      if (Target) {
        Discovery->TargetRomStatus = EFI_NOT_FOUND;
      }
      NcvDisplayCollectionAppend (&Discovery->Displays, &Collected);
      continue;
    }
    Collected.OptionRomExposed = 1;

    ZeroMem (&Validation, sizeof (Validation));
    Status = OptionRomParseAndValidate (
               Logger,
               Record->PciIo->RomImage,
               Record->PciIo->RomSize,
               Record->PciIo->RomSize,
               Vendor,
               Device,
               &Validation
               );
    LogPrint (
      Logger,
      L"  ROM result %04x:%02x:%02x.%x: 0x%016lx (%s), matching-x86=%s\r\n",
      (UINT32)Record->Segment,
      (UINT32)Record->Bus,
      (UINT32)Record->Device,
      (UINT32)Record->Function,
      (UINT64)Status,
      EfiStatusName (Status),
      (!EFI_ERROR (Status) && Validation.MatchingLegacyImageFound) ? L"yes" : L"no"
      );
    if (Target) {
      Discovery->TargetRomStatus = Status;
      Discovery->TargetRomLegacyValid = (BOOLEAN)(!EFI_ERROR (Status) && Validation.MatchingLegacyImageFound);
    }

    if (!EFI_ERROR (Status) && Validation.MatchingLegacyImageFound) {
      Collected.AcceptedLegacyRom = 1;
      ++Discovery->LegacyCandidateCount;
    }
    NcvDisplayCollectionAppend (&Discovery->Displays, &Collected);
  }

  if (Discovery->Displays.Overflow) {
    LogPrint (
      Logger,
      L"Display candidate collection overflow: maximum=%u observed=%u\r\n",
      NCV_MAX_DISPLAY_CANDIDATES,
      (UINT32)Discovery->Displays.ObservedCount
      );
  }

  if (Config != NULL && Config->HasTargetPci && !Discovery->TargetFound) {
    LogPrint (Logger, L"Configured target VGA was not found; no replacement is selected\r\n");
  }
}

STATIC
VOID
ReportStorageControllers (
  IN  APP_LOGGER          *Logger,
  IN  CONST PCI_INVENTORY *Inventory,
  IN  CONST PROBE_CONFIG  *Config,
  OUT BOOLEAN             *TargetFound
  )
{
  UINTN  Index;

  if ((Logger == NULL) || (Inventory == NULL) || (TargetFound == NULL)) {
    return;
  }

  *TargetFound = FALSE;
  LogPrint (Logger, L"\r\n=== PCI storage-controller candidates ===\r\n");
  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Record;
    BOOLEAN                  Target;

    Record = &Inventory->Devices[Index];
    if (!RecordHasValidLocation (Record) || EFI_ERROR (Record->ConfigStatus) ||
        (Record->Config[PCI_CLASS_OFFSET] != PCI_BASE_CLASS_STORAGE))
    {
      continue;
    }

    Target = (BOOLEAN)((Config != NULL) && Config->HasTargetControllerPci &&
                       (Record->Segment == Config->TargetControllerPci.Segment) &&
                       (Record->Bus == Config->TargetControllerPci.Bus) &&
                       (Record->Device == Config->TargetControllerPci.Device) &&
                       (Record->Function == Config->TargetControllerPci.Function));
    LogPrint (
      Logger,
      L"  %04x:%02x:%02x.%x vendor:device=%04x:%04x class=%02x:%02x:%02x%s\r\n",
      (UINT32)Record->Segment,
      (UINT32)Record->Bus,
      (UINT32)Record->Device,
      (UINT32)Record->Function,
      Read16 (&Record->Config[PCI_VENDOR_ID_OFFSET]),
      Read16 (&Record->Config[PCI_DEVICE_ID_OFFSET]),
      Record->Config[PCI_CLASS_OFFSET],
      Record->Config[PCI_SUBCLASS_OFFSET],
      Record->Config[PCI_PROGIF_OFFSET],
      Target ? L" <configured target>" : L""
      );
    if (Target) {
      *TargetFound = TRUE;
    }
  }

  if ((Config != NULL) && Config->HasTargetControllerPci && !*TargetFound) {
    LogPrint (Logger, L"Configured boot controller was not found; no replacement is selected\r\n");
  }
}

STATIC
VOID
ReportGopOwnership (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory,
  IN OUT VIDEO_DISCOVERY  *Discovery
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    *Handles;
  UINTN                         HandleCount;
  UINTN                         Index;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *ConsoleGop;
  BOOLEAN                       ScanComplete;

  if ((Logger == NULL) || (Inventory == NULL) || (Discovery == NULL)) {
    return;
  }

  Handles = NULL;
  HandleCount = 0;
  ConsoleGop = NULL;
  ScanComplete = FALSE;
  if ((gST != NULL) && (gST->ConsoleOutHandle != NULL)) {
    gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&ConsoleGop);
  }

  LogPrint (Logger, L"\r\n=== Active GOP ownership (read-only association) ===\r\n");
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"LocateHandleBuffer(EFI_GRAPHICS_OUTPUT_PROTOCOL): 0x%016lx (%s)\r\n", (UINT64)Status, EfiStatusName (Status));
    return;
  }
  ScanComplete = TRUE;

  for (Index = 0; Index < HandleCount; ++Index) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
    EFI_DEVICE_PATH_PROTOCOL      *Path;
    CHAR16                        *Text;
    CONST PCI_DEVICE_RECORD       *Match;
    UINTN                         MatchCount;
    UINTN                         RecordIndex;
    UINTN                         PathSize;
    BOOLEAN                       Active;
    BOOLEAN                       PathUsable;

    Gop = NULL;
    Path = NULL;
    Text = NULL;
    Match = NULL;
    MatchCount = 0;
    PathSize = 0;
    PathUsable = FALSE;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
    if (EFI_ERROR (Status) || (Gop == NULL)) {
      LogPrint (Logger, L"  GOP handle %p protocol unavailable: 0x%016lx (%s)\r\n", Handles[Index], (UINT64)Status, EfiStatusName (Status));
      ScanComplete = FALSE;
      continue;
    }

    Status = gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path);
    if (!EFI_ERROR (Status) && (Path != NULL) && IsDevicePathValid (Path, 4096U)) {
      PathSize = GetDevicePathSize (Path);
      Text = ConvertDevicePathToText (Path, FALSE, FALSE);
      PathUsable = TRUE;
    }
    for (RecordIndex = 0; RecordIndex < Inventory->Count; ++RecordIndex) {
      CONST PCI_DEVICE_RECORD  *Record;

      Record = &Inventory->Devices[RecordIndex];
      if (!RecordIsDisplay (Record)) {
        continue;
      }

      if ((Record->Handle == Handles[Index]) ||
          (PathUsable &&
           DevicePathPrefix (
             Record->DevicePath,
             Record->DevicePathSize,
             Path,
             PathSize
             )))
      {
        Match = Record;
        ++MatchCount;
      }
    }

    Active = (BOOLEAN)((Handles[Index] == ((gST != NULL) ? gST->ConsoleOutHandle : NULL)) ||
                       ((ConsoleGop != NULL) && (ConsoleGop == Gop)));
    if (MatchCount == 1) {
      UINTN CandidateIndex;
      for (CandidateIndex = 0;
           CandidateIndex < Discovery->Displays.Count;
           ++CandidateIndex)
      {
        NCV_DISPLAY_CANDIDATE *Candidate;
        Candidate = &Discovery->Displays.Items[CandidateIndex];
        if (Candidate->AddressValid &&
            (Candidate->Address.Segment == Match->Segment) &&
            (Candidate->Address.Bus == Match->Bus) &&
            (Candidate->Address.Device == Match->Device) &&
            (Candidate->Address.Function == Match->Function))
        {
          Candidate->GopAssociated = 1;
          Candidate->GopAssociationKnown = 1;
          break;
        }
      }
      if (CandidateIndex == Discovery->Displays.Count) {
        ScanComplete = FALSE;
      }
      LogPrint (
        Logger,
        L"  GOP[%u] handle=%p PCI=%04x:%02x:%02x.%x active-console=%s path=%s\r\n",
        (UINT32)Index,
        Handles[Index],
        (UINT32)Match->Segment,
        (UINT32)Match->Bus,
        (UINT32)Match->Device,
        (UINT32)Match->Function,
        Active ? L"yes" : L"no",
        (Text != NULL) ? Text : L"<device path unavailable>"
        );
    } else {
      ScanComplete = FALSE;
      LogPrint (
        Logger,
        L"  GOP[%u] handle=%p PCI-association=%s active-console=%s path=%s\r\n",
        (UINT32)Index,
        Handles[Index],
        (MatchCount == 0) ? L"unavailable" : L"ambiguous",
        Active ? L"yes" : L"no",
        (Text != NULL) ? Text : L"<device path unavailable>"
        );
    }

    if (Text != NULL) {
      FreePool (Text);
    }
  }

  if (ScanComplete) {
    for (Index = 0; Index < Discovery->Displays.Count; ++Index) {
      Discovery->Displays.Items[Index].GopAssociationKnown = 1;
    }
  } else {
    LogPrint (
      Logger,
      L"At least one GOP association was unavailable or ambiguous; "
      L"unmatched display candidates remain unknown and rank conservatively\r\n"
      );
  }
  LogPrint (Logger, L"GOP handle count=%u; active-console=yes only when the console exposes this GOP directly\r\n", (UINT32)HandleCount);
  gBS->FreePool (Handles);
}

STATIC
VOID
ReportActiveVgaPaths (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory
  )
{
  UINTN  Index;
  UINTN  Owners;

  if ((Logger == NULL) || (Inventory == NULL)) {
    return;
  }

  Owners = 0;
  LogPrint (Logger, L"\r\n=== Active legacy VGA owner/path candidates ===\r\n");
  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Record;

    Record = &Inventory->Devices[Index];
    if (!RecordIsDisplay (Record) || EFI_ERROR (Record->CurrentAttributesStatus) ||
        ((Record->CurrentAttributes & (EFI_PCI_IO_ATTRIBUTE_VGA_IO | EFI_PCI_IO_ATTRIBUTE_VGA_IO_16 | EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY)) == 0))
    {
      continue;
    }

    ++Owners;
    LogPrint (Logger, L"  active legacy-VGA attribute owner candidate=%04x:%02x:%02x.%x attrs=0x%016lx\r\n", (UINT32)Record->Segment, (UINT32)Record->Bus, (UINT32)Record->Device, (UINT32)Record->Function, Record->CurrentAttributes);
    PciPrintSelectedBridgePath (Logger, Inventory, Record->Segment, Record->Bus, Record->Device, Record->Function);
  }

  if (Owners == 0) {
    LogPrint (Logger, L"No display handle reported enabled legacy VGA I/O or memory attributes; active path is firmware-opaque\r\n");
  }
}

STATIC
VOID
ReportCompatibility16Tables (
  IN APP_LOGGER  *Logger
  )
{
  EFI_STATUS                  Status;
  MEMORY_MAP_SNAPSHOT         Map;
  UINTN                       Address;
  CONST volatile UINT8        *Bytes;
  UINTN                       TableLength;
  UINTN                       Index;
  UINT8                       Checksum;
  UINTN                       Count;
  EFI_COMPATIBILITY16_TABLE   Table;

  if (Logger == NULL) {
    return;
  }

  LogPrint (Logger, L"\r\n=== Resident Compatibility16 table candidates (read-only scan) ===\r\n");
  ZeroMem (&Map, sizeof (Map));
  Status = MemoryMapCapture (&Map);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"GetMemoryMap for Compatibility16 scan: 0x%016lx (%s)\r\n", (UINT64)Status, EfiStatusName (Status));
    return;
  }

  Count = 0;
  for (Address = PROBE_COMPAT_SCAN_START;
       Address + 6U <= PROBE_COMPAT_SCAN_END;
       Address += PROBE_COMPAT_ALIGNMENT)
  {
    if (!MemoryMapRangeIsReadable (&Map, Address, 6, NULL)) {
      continue;
    }

    Bytes = (CONST volatile UINT8 *)(UINTN)Address;
    if ((Bytes[0] != 'I') || (Bytes[1] != 'F') || (Bytes[2] != 'E') || (Bytes[3] != '$')) {
      continue;
    }

    TableLength = Bytes[5];
    if ((TableLength < OFFSET_OF (EFI_COMPATIBILITY16_TABLE, Compatibility16CallOffset) + sizeof (UINT16)) ||
        (TableLength > PROBE_COMPAT_MAX_TABLE_LENGTH) ||
        (TableLength > PROBE_COMPAT_SCAN_END - Address) ||
        !MemoryMapRangeIsReadable (&Map, Address, TableLength, NULL))
    {
      LogPrint (Logger, L"  $EFI candidate at 0x%05lx rejected: table-length=0x%02x is unsafe\r\n", (UINT64)Address, (UINT32)TableLength);
      continue;
    }

    Checksum = 0;
    for (Index = 0; Index < TableLength; ++Index) {
      Checksum = (UINT8)(Checksum + Bytes[Index]);
    }

    ZeroMem (&Table, sizeof (Table));
    CopyMem (
      &Table,
      (CONST VOID *)(UINTN)Address,
      (TableLength < sizeof (Table)) ? TableLength : sizeof (Table)
      );
    LogPrint (
      Logger,
      L"  $EFI[%u] physical=0x%05lx length=0x%02x checksum=%s revision=%u.%u call=%04x:%04x\r\n",
      (UINT32)Count,
      (UINT64)Address,
      (UINT32)TableLength,
      (Checksum == 0) ? L"valid" : L"invalid",
      Table.TableMajorRevision,
      Table.TableMinorRevision,
      Table.Compatibility16CallSegment,
      Table.Compatibility16CallOffset
      );
    ++Count;
  }

  if (Count == 0) {
    LogPrint (Logger, L"No readable $EFI Compatibility16 table candidates found in E0000-FFFFF\r\n");
  }

  MemoryMapRelease (&Map);
}

STATIC
BOOLEAN
ReportNativeCsmCapabilities (
  IN  APP_LOGGER                    *Logger,
  OUT EFI_LEGACY_BIOS_PROTOCOL      **LegacyBios,
  OUT BOOLEAN                       *LegacyRegionAvailable
  )
{
  EFI_STATUS                  Status;
  EFI_LEGACY_BIOS_PROTOCOL    *Bios;
  EFI_LEGACY_REGION2_PROTOCOL *Region;
  EFI_LEGACY_REGION_DESCRIPTOR *Descriptors;
  UINT32                      DescriptorCount;
  UINT32                      Index;

  if ((Logger == NULL) || (LegacyBios == NULL) || (LegacyRegionAvailable == NULL)) {
    return FALSE;
  }

  *LegacyBios = NULL;
  *LegacyRegionAvailable = FALSE;
  Bios = NULL;
  Region = NULL;
  LogPrint (Logger, L"\r\n=== Native CSM capability discovery ===\r\n");
  Status = gBS->LocateProtocol (&gEfiLegacyBiosProtocolGuid, NULL, (VOID **)&Bios);
  LogPrint (Logger, L"LocateProtocol(EFI_LEGACY_BIOS_PROTOCOL): 0x%016lx (%s), interface=%p\r\n", (UINT64)Status, EfiStatusName (Status), Bios);
  if (EFI_ERROR (Status) || (Bios == NULL)) {
    return FALSE;
  }

  *LegacyBios = Bios;
  LogPrint (Logger, L"  FarCall86 pointer=%p availability=%s (not invoked)\r\n", Bios->FarCall86, (Bios->FarCall86 != NULL) ? L"yes" : L"no");
  LogPrint (Logger, L"  LegacyBoot pointer=%p availability=%s (not invoked)\r\n", Bios->LegacyBoot, (Bios->LegacyBoot != NULL) ? L"yes" : L"no");
  LogPrint (Logger, L"  GetBbsInfo pointer=%p availability=%s (query-only path)\r\n", Bios->GetBbsInfo, (Bios->GetBbsInfo != NULL) ? L"yes" : L"no");
  Status = gBS->LocateProtocol (&gEfiLegacyRegion2ProtocolGuid, NULL, (VOID **)&Region);
  LogPrint (Logger, L"LocateProtocol(EFI_LEGACY_REGION2_PROTOCOL): 0x%016lx (%s), interface=%p\r\n", (UINT64)Status, EfiStatusName (Status), Region);
  if (EFI_ERROR (Status) || (Region == NULL)) {
    return TRUE;
  }

  *LegacyRegionAvailable = TRUE;
  if (Region->GetInfo == NULL) {
    LogPrint (Logger, L"  LegacyRegion2 GetInfo pointer is NULL; availability is opaque\r\n");
    return TRUE;
  }

  Descriptors = NULL;
  DescriptorCount = 0;
  Status = Region->GetInfo (Region, &DescriptorCount, &Descriptors);
  LogPrint (Logger, L"  LegacyRegion2 GetInfo: 0x%016lx (%s), descriptors=%u pointer=%p\r\n", (UINT64)Status, EfiStatusName (Status), DescriptorCount, Descriptors);
  if (!EFI_ERROR (Status) && (Descriptors != NULL) && (DescriptorCount <= 128U)) {
    for (Index = 0; Index < DescriptorCount; ++Index) {
      LogPrint (Logger, L"    region[%u] start=0x%08x length=0x%08x attribute=%u granularity=0x%08x\r\n", Index, Descriptors[Index].Start, Descriptors[Index].Length, Descriptors[Index].Attribute, Descriptors[Index].Granularity);
    }
  } else if (!EFI_ERROR (Status)) {
    LogPrint (Logger, L"  LegacyRegion2 descriptors were null or exceeded safety cap; not dereferenced\r\n");
  }

  if (!EFI_ERROR (Status) && (Descriptors != NULL)) {
    gBS->FreePool (Descriptors);
  }

  return TRUE;
}

STATIC
CONST CHAR16 *
CompatibilityClassification (
  IN CONST PROBE_CONFIG           *Config,
  IN CONST VIDEO_DISCOVERY        *Video,
  IN BOOLEAN                      ControllerFound,
  IN BOOLEAN                      NativeCsmAvailable,
  IN BOOLEAN                      LegacyRegionAvailable,
  IN CONST BBS_PROBE_SUMMARY      *Bbs,
  IN CONST BOOT_OPTION_PROBE_SUMMARY *Boots
  )
{
  if (!NativeCsmAvailable) {
    return L"NCV_PROBE_BLOCKED_NO_NATIVE_CSM_INTERFACE";
  }

  if (!LegacyRegionAvailable) {
    return L"NCV_PROBE_BLOCKED_NO_LEGACY_REGION_PROTOCOL";
  }

  if ((Config != NULL) && !Config->DiscoveryMode) {
    if ((Video == NULL) || !Video->TargetFound || !Video->TargetIsDisplay) {
      return L"NCV_PROBE_BLOCKED_NO_TARGET_VGA";
    }

    if (!Video->TargetIdentityMatches || !Video->TargetRomLegacyValid) {
      return L"NCV_PROBE_BLOCKED_TARGET_ROM_UNSUPPORTED";
    }

    if (EFI_ERROR (Video->TargetPathStatus)) {
      return L"NCV_PROBE_BLOCKED_BRIDGE_PATH_AMBIGUOUS";
    }

    if (!ControllerFound || (Bbs == NULL) || !Bbs->CountsValid ||
        (Bbs->TargetHardDiskMatches != 1) || (Boots == NULL) ||
        EFI_ERROR (Boots->BootOrderStatus) ||
        (Config->HasLegacyOptionDescription &&
         (Boots->ConfiguredDescriptionMatches != 1)))
    {
      return L"NCV_PROBE_BLOCKED_BOOT_TARGET_AMBIGUOUS";
    }
  }

  if ((Bbs != NULL) && EFI_ERROR (Bbs->Result)) {
    return L"NCV_PROBE_COMPATIBLE_WITH_FIRMWARE_QUIRK";
  }

  return L"NCV_PROBE_COMPATIBLE";
}

/* Probe-only UI; never reachable after the native CSM boundary. */
STATIC
VOID
FirstRunNotice (
  IN EFI_STATUS Result,
  IN BOOLEAN Complete
  )
{
  EFI_INPUT_KEY Key;
  EFI_STATUS InputStatus;
  UINTN EventIndex;

  if (Result == EFI_ABORTED) {
    Print (L"Setup cancelled; saved settings were not changed.\r\n");
    return;
  }
  Print (L"\r\n=== First-run hardware discovery ===\r\n");
  if (EFI_ERROR (Result)) {
    Print (L"Discovery or file output failed: %r. Check the probe log.\r\n", Result);
    Print (L"A usable configuration has not been confirmed.\r\n");
  } else if (Complete) {
    Print (L"Config.ini saved beside the EFI application.\r\n");
    Print (L"Your selected GPU and boot disk have been saved. Continuing to boot.\r\n");
    return;
  } else {
    Print (L"Probe.ini created beside the core EFI application.\r\n");
    Print (L"Suitable GPU and disk targets were not both found; no boot-ready INI was created.\r\n");
    Print (L"Review the candidate INI and probe log before configuring targets.\r\n");
  }
  Print (L"Press any key to return to firmware.\r\n");
  if ((gST->ConIn != NULL) && (gST->ConIn->WaitForKey != NULL)) {
    gST->ConIn->Reset (gST->ConIn, FALSE);
    do {
      InputStatus = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &EventIndex);
      if (EFI_ERROR (InputStatus)) { break; }
      InputStatus = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    } while (InputStatus == EFI_NOT_READY);
    if (!EFI_ERROR (InputStatus)) { return; }
  }
  Print (L"Console input unavailable; returning in 10 seconds.\r\n");
  gBS->Stall (10000000);
}

EFI_STATUS
EFIAPI
NativeCsmVgaProbeRun (
  IN EFI_HANDLE          ImageHandle,
  IN CONST PROBE_CONFIG  *SharedConfig,
  OUT BOOLEAN            *BootReady
  )
{
  EFI_STATUS                   Status;
  EFI_STATUS                   CloseStatus;
  APP_FILE_CONTEXT             Files;
  APP_LOGGER                   Log;
  PROBE_CONFIG                 Config;
  PCI_INVENTORY                Inventory;
  VIDEO_DISCOVERY              Video;
  BOOLEAN                      ControllerFound;
  EFI_LEGACY_BIOS_PROTOCOL     *LegacyBios;
  BOOLEAN                      LegacyRegionAvailable;
  BBS_CONTROLLER_TARGET        BbsTarget;
  BBS_PROBE_SUMMARY            Bbs;
  BOOT_OPTION_PROBE_SUMMARY    Boots;
  UINTN                        SelectedStorage;
  EFI_STATUS                   ProbeOutputStatus;
  BOOLEAN                      NativeCsmAvailable;
  CONST CHAR16                 *Classification;
  BOOLEAN                      SetupConfirmed = FALSE;

  if (BootReady == NULL) { return EFI_INVALID_PARAMETER; }
  *BootReady = FALSE;
  ZeroMem (&Files, sizeof (Files));
  ZeroMem (&Log, sizeof (Log));
  ZeroMem (&Config, sizeof (Config));
  ZeroMem (&Inventory, sizeof (Inventory));
  ZeroMem (&Video, sizeof (Video));
  ZeroMem (&Bbs, sizeof (Bbs));
  ZeroMem (&Boots, sizeof (Boots));
  SelectedStorage = NCV_NO_SELECTION;
  ProbeOutputStatus = EFI_SUCCESS;
  ControllerFound = FALSE;
  LegacyBios = NULL;
  LegacyRegionAvailable = FALSE;
  NativeCsmAvailable = FALSE;

  Status = AppFileInitialize (ImageHandle, &Files);
  if (EFI_ERROR (Status)) {
    Print (L"%s: executable-relative filesystem initialization failed: %r\r\n", PROBE_VERSION, Status);
    return Status;
  }

  Status = LogInitialize (&Log, &Files, TRUE, PROBE_LOG_FILE);
  if (EFI_ERROR (Status) || (Log.File == NULL)) {
    Print (L"%s: could not open %s: %r\r\n", PROBE_VERSION, PROBE_LOG_FILE, Status);
    AppFileClose (&Files);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  LogPrint (&Log, L"%s -- native CSM and hardware discovery; no GPU handoff\r\n", PROBE_VERSION);
  LogPrint (&Log, L"Safety contract: no PCI writes/attribute changes, controller disconnects, legacy-memory writes, ROM dispatch, BBS writes, LegacyBoot, or UEFI-variable writes.\r\n");
  LogPrint (&Log, L"Executable directory: %s\r\n", Files.ImageDirectoryPath);

  if ((SharedConfig == NULL) || !SharedConfig->Probe) { Status = EFI_INVALID_PARAMETER; goto Done; }
  CopyMem (&Config, SharedConfig, sizeof (Config));
  LogPrint (&Log, L"NCV_MODE=PROBE\r\n");
  if (Config.FirstRun) {
    LogPrint (&Log, L"NCV_INI_NOT_FOUND_FIRST_RUN_PROBE\r\n");
  }

  LogSetVerbose (&Log, Config.DiscoveryMode ? TRUE : Config.VerboseLog);
  Status = PciInventoryBuild (&Log, &Inventory);
  if (EFI_ERROR (Status)) {
    LogPrint (&Log, L"PCI inventory failed: 0x%016lx (%s)\r\n", (UINT64)Status, EfiStatusName (Status));
    goto Done;
  }

  PciPrintDisplayDevices (&Log, &Inventory);
  ReportRomCandidates (&Log, &Inventory, &Config, &Video);
  PciPrintAllDisplayBridgePaths (&Log, &Inventory);
  ReportGopOwnership (&Log, &Inventory, &Video);
  Video.SelectedCandidate = NCV_NO_SELECTION;
  if (!Video.Displays.Overflow &&
      (NcvRankDisplays (
         Video.Displays.Items,
         Video.Displays.Count,
         &Video.SelectedCandidate
         ) == 0) &&
      (Video.SelectedCandidate != NCV_NO_SELECTION))
  {
    NCV_DISPLAY_CANDIDATE *Selected;
    Selected = &Video.Displays.Items[Video.SelectedCandidate];
    LogPrint (
      &Log,
      L"Deterministic display selection=%04x:%02x:%02x.%x\r\n",
      Selected->Address.Segment,
      Selected->Address.Bus,
      Selected->Address.Device,
      Selected->Address.Function
      );
  } else {
    LogPrint (
      &Log,
      L"No eligible legacy x86 ROM display was discovered; selection remains incomplete\r\n"
      );
  }
  LogPrint (
    &Log,
    L"Display candidate collection: stored=%u observed=%u selected=%s\r\n",
    (UINT32)Video.Displays.Count,
    (UINT32)Video.Displays.ObservedCount,
    (Video.SelectedCandidate == NCV_NO_SELECTION) ? L"none" : L"one"
    );
  ReportActiveVgaPaths (&Log, &Inventory);

  if (!Config.DiscoveryMode && Config.HasTargetPci) {
    CONST PCI_DEVICE_RECORD  *Target;

    Target = FindRecord (&Inventory, &Config.TargetPci);
    if (Target == NULL) {
      Video.TargetFound = FALSE;
      Video.TargetPathStatus = EFI_NOT_FOUND;
      LogPrint (&Log, L"Configured target VGA is absent from the PCI inventory; no substitution is made\r\n");
    } else if (!RecordIsDisplay (Target)) {
      Video.TargetFound = TRUE;
      Video.TargetIsDisplay = FALSE;
      Video.TargetPathStatus = EFI_INVALID_PARAMETER;
      LogPrint (&Log, L"Configured target VGA exists but is not display-class; no substitution is made\r\n");
    } else {
      Video.TargetPathStatus = ReportTargetBridgePath (&Log, &Inventory, Target);
    }
  }

  ReportStorageControllers (&Log, &Inventory, &Config, &ControllerFound);
  NativeCsmAvailable = ReportNativeCsmCapabilities (&Log, &LegacyBios, &LegacyRegionAvailable);
  ReportCompatibility16Tables (&Log);

  if (NativeCsmAvailable && (LegacyBios != NULL)) {
    ZeroMem (&BbsTarget, sizeof (BbsTarget));
    if (!Config.DiscoveryMode && Config.HasTargetControllerPci) {
      BbsTarget.Segment = Config.TargetControllerPci.Segment;
      BbsTarget.Bus = Config.TargetControllerPci.Bus;
      BbsTarget.Device = Config.TargetControllerPci.Device;
      BbsTarget.Function = Config.TargetControllerPci.Function;
      Status = BbsProbeAndPrintEx (&Log, LegacyBios, &BbsTarget, &Bbs);
    } else {
      Status = BbsProbeAndPrintEx (&Log, LegacyBios, NULL, &Bbs);
    }
    LogPrint (&Log, L"GetBbsInfo probe result retained as read-only diagnostic: 0x%016lx (%s)\r\n", (UINT64)Status, EfiStatusName (Status));
  } else {
    Bbs.Result = EFI_UNSUPPORTED;
  }

  Status = BootOptionsProbe (&Log, Config.DiscoveryMode ? NULL : &Config, &Boots);
  LogPrint (&Log, L"Boot-order probe result: 0x%016lx (%s)\r\n", (UINT64)Status, EfiStatusName (Status));
  PciVerifyReadOnlySnapshot (&Log, &Inventory);

  if (Config.DiscoveryMode) {
    if (Video.Displays.Overflow || Bbs.Storage.Overflow) {
      ProbeOutputStatus = EFI_BUFFER_TOO_SMALL;
      LogPrint (&Log, L"Candidate collection overflow; refusing Probe INI output\r\n");
    } else {
      if (NcvRankStorage (
            Bbs.Storage.Items,
            Bbs.Storage.Count,
            &SelectedStorage
            ) != 0)
      {
        ProbeOutputStatus = EFI_INVALID_PARAMETER;
      } else {
        LogPrint (
          &Log,
          L"Storage candidate collection: stored=%u observed=%u selected=%s\r\n",
          (UINT32)Bbs.Storage.Count,
          (UINT32)Bbs.Storage.ObservedCount,
          (SelectedStorage == NCV_NO_SELECTION) ? L"none" : L"one"
          );
        {
        BOOLEAN SavedTargetMissing = FALSE;
        if (ProbeConfigIsEditing()) {
          UINTN Choice;
          Video.SelectedCandidate=NCV_NO_SELECTION; SelectedStorage=NCV_NO_SELECTION;
          for(Choice=0; Choice<Video.Displays.Count; Choice++) {
            NCV_PCI_ADDRESS *A=&Video.Displays.Items[Choice].Address;
            if(Video.Displays.Items[Choice].Eligible && A->Segment==Config.TargetPci.Segment && A->Bus==Config.TargetPci.Bus && A->Device==Config.TargetPci.Device && A->Function==Config.TargetPci.Function) Video.SelectedCandidate=Choice;
          }
          for(Choice=0; Choice<Bbs.Storage.Count; Choice++) {
            NCV_PCI_ADDRESS *A=&Bbs.Storage.Items[Choice].Address; CHAR16 Description[PROBE_CONFIG_TEXT_CHARS];
            AsciiStrToUnicodeStrS(Bbs.Storage.Items[Choice].Description,Description,PROBE_CONFIG_TEXT_CHARS);
            if(Bbs.Storage.Items[Choice].Eligible && A->Segment==Config.TargetControllerPci.Segment && A->Bus==Config.TargetControllerPci.Bus && A->Device==Config.TargetControllerPci.Device && A->Function==Config.TargetControllerPci.Function && StrCmp(Description,Config.TargetBbsDescription)==0) SelectedStorage=Choice;
          }
          if(Video.SelectedCandidate==NCV_NO_SELECTION || SelectedStorage==NCV_NO_SELECTION) {
            SavedTargetMissing = TRUE;
            if(Video.SelectedCandidate==NCV_NO_SELECTION) NcvRankDisplays(Video.Displays.Items,Video.Displays.Count,&Video.SelectedCandidate);
            if(SelectedStorage==NCV_NO_SELECTION) NcvRankStorage(Bbs.Storage.Items,Bbs.Storage.Count,&SelectedStorage);
          }
        }
        if (Config.FirstRun && Video.SelectedCandidate != NCV_NO_SELECTION &&
            SelectedStorage != NCV_NO_SELECTION) {
          Status = SelectBootTargets (
                     Video.Displays.Items, Video.Displays.Count, &Video.SelectedCandidate,
                     Bbs.Storage.Items, Bbs.Storage.Count, &SelectedStorage,
                     ProbeConfigIsEditing () ? ProbeConfigReviewSettings () : NULL, SavedTargetMissing
                     );
          if (EFI_ERROR (Status)) {
            LogPrint (&Log, L"First-run target selection did not save: %r\r\n", Status);
            goto Done;
          }
          SetupConfirmed = TRUE;
        }
        }
        ProbeOutputStatus = ProbeConfigWriteCandidateIni (
                              &Files,
                              &Log,
                              Video.Displays.Items,
                              Video.Displays.Count,
                              Video.SelectedCandidate,
                              Bbs.Storage.Items,
                              Bbs.Storage.Count,
                              SelectedStorage,
                              Config.FirstRun
                              );
      }
    }
    LogPrint (
      &Log,
      L"Candidate INI write: 0x%016lx (%s)\r\n",
      (UINT64)ProbeOutputStatus,
      EfiStatusName (ProbeOutputStatus)
      );
  }

  Classification = CompatibilityClassification (
                     &Config,
                     &Video,
                     ControllerFound,
                     NativeCsmAvailable,
                     LegacyRegionAvailable,
                     &Bbs,
                     &Boots
                     );
  LogPrint (&Log, L"\r\n=== Final compatibility classification ===\r\n%s\r\n", Classification);
  LogPrint (&Log, L"Probe completed normally without GPU handoff. Settings and diagnostic files may have been written.\r\n");
  Status = EFI_ERROR (ProbeOutputStatus) ?
           ProbeOutputStatus : EFI_SUCCESS;

Done:
  if (Inventory.Devices != NULL) {
    PciInventoryRelease (&Inventory);
  }
  CloseStatus = LogFlush (&Log);
  if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
    Status = CloseStatus;
  }
  CloseStatus = LogClose (&Log);
  if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
    Status = CloseStatus;
  }
  CloseStatus = AppFileClose (&Files);
  if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
    Status = CloseStatus;
  }

  *BootReady = (BOOLEAN)(!EFI_ERROR (Status) && SetupConfirmed);
  if (Config.FirstRun) {
    FirstRunNotice (
      Status,
      *BootReady
      );
  }

  return Status;
}

/** @file
  Probe-to-native CSM boot execution runtime-plan construction and validation.
**/

#include "RuntimePlan.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "MemoryMap.h"
#include "LegacyDispatch.h"
#include "LegacyRomGuard.h"
#include "StatusPrint.h"

#define PCI_COMMAND_OFFSET               0x04U
#define PCI_SUBCLASS_OFFSET              0x0AU
#define PCI_BASE_CLASS_OFFSET            0x0BU
#define PCI_SUBSYSTEM_VENDOR_ID_OFFSET   0x2CU
#define PCI_SUBSYSTEM_ID_OFFSET          0x2EU
#define PCI_BRIDGE_CONTROL_OFFSET        0x3EU

#define PCI_BASE_CLASS_DISPLAY           0x03U
#define PCI_BASE_CLASS_STORAGE           0x01U
#define PCI_BASE_CLASS_SERIAL            0x0CU
#define PCI_SUBCLASS_USB                 0x03U

#define COMPAT_SCAN_START                0xE0000U
#define COMPAT_SCAN_END                  0x100000U
#define COMPAT_SCAN_ALIGNMENT            16U
#define COMPAT_CALL_SAMPLE_BYTES         32U

STATIC
UINT16
ReadLe16 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8));
}

STATIC
BOOLEAN
AddressEqualRecord (
  IN CONST PCI_DEVICE_RECORD  *Record,
  IN CONST PROBE_PCI_ADDRESS  *Address
  )
{
  return (BOOLEAN)(
    (Record != NULL) && (Address != NULL) &&
    !EFI_ERROR (Record->LocationStatus) &&
    (Record->Segment == Address->Segment) &&
    (Record->Bus == Address->Bus) &&
    (Record->Device == Address->Device) &&
    (Record->Function == Address->Function)
    );
}

STATIC
BOOLEAN
RecordIsDisplay (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (BOOLEAN)(
    (Record != NULL) && !EFI_ERROR (Record->ConfigStatus) &&
    (Record->Config[PCI_BASE_CLASS_OFFSET] == PCI_BASE_CLASS_DISPLAY)
    );
}

STATIC
BOOLEAN
RecordIsUsbController (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (BOOLEAN)(
    (Record != NULL) && !EFI_ERROR (Record->ConfigStatus) &&
    (Record->Config[PCI_BASE_CLASS_OFFSET] == PCI_BASE_CLASS_SERIAL) &&
    (Record->Config[PCI_SUBCLASS_OFFSET] == PCI_SUBCLASS_USB)
    );
}

STATIC
BOOLEAN
RecordIsBootController (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (BOOLEAN)(
    (Record != NULL) && !EFI_ERROR (Record->ConfigStatus) &&
    ((Record->Config[PCI_BASE_CLASS_OFFSET] == PCI_BASE_CLASS_STORAGE) ||
     RecordIsUsbController (Record))
    );
}

STATIC
EFI_STATUS
CopyEndpoint (
  IN  CONST PCI_DEVICE_RECORD         *Record,
  OUT NATIVE_CSM_VGA_PLAN_ENDPOINT    *Endpoint
  )
{
  if ((Record == NULL) || (Endpoint == NULL) ||
      EFI_ERROR (Record->LocationStatus) ||
      EFI_ERROR (Record->ConfigStatus) ||
      (Record->PciIo == NULL) ||
      (Record->Segment > MAX_UINT16) ||
      (Record->Bus > MAX_UINT8) ||
      (Record->Device > 31U) ||
      (Record->Function > 7U))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Endpoint, sizeof (*Endpoint));
  Endpoint->InventoryRecord         = Record;
  Endpoint->Handle                  = Record->Handle;
  Endpoint->PciIo                   = Record->PciIo;
  Endpoint->Segment                 = (UINT16)Record->Segment;
  Endpoint->Bus                     = (UINT8)Record->Bus;
  Endpoint->Device                  = (UINT8)Record->Device;
  Endpoint->Function                = (UINT8)Record->Function;
  Endpoint->Vendor                  = ReadLe16 (&Record->Config[0]);
  Endpoint->DeviceId                = ReadLe16 (&Record->Config[2]);
  Endpoint->SubsystemVendor         =
    ReadLe16 (&Record->Config[PCI_SUBSYSTEM_VENDOR_ID_OFFSET]);
  Endpoint->SubsystemDevice         =
    ReadLe16 (&Record->Config[PCI_SUBSYSTEM_ID_OFFSET]);
  Endpoint->RootBridgeHandle        = Record->RootBridgeHandle;
  Endpoint->RootBridgeIo            = Record->RootBridgeIo;
  Endpoint->ParentHostHandle        =
    (Record->RootBridgeIo != NULL) ?
    Record->RootBridgeIo->ParentHandle : NULL;
  Endpoint->OriginalCommand         =
    ReadLe16 (&Record->Config[PCI_COMMAND_OFFSET]);
  Endpoint->OriginalAttributes      = Record->CurrentAttributes;
  Endpoint->OriginalAttributesStatus = Record->CurrentAttributesStatus;
  return EFI_SUCCESS;
}

STATIC
VOID
LogEndpoint (
  IN APP_LOGGER                          *Logger,
  IN CONST CHAR16                        *Label,
  IN CONST NATIVE_CSM_VGA_PLAN_ENDPOINT  *Endpoint
  )
{
  LogPrint (
    Logger,
    L"%s=%04x:%02x:%02x.%x vendor:device=%04x:%04x "
    L"subsystem=%04x:%04x handle=%p root=%p parent-host=%p\r\n",
    Label,
    Endpoint->Segment,
    Endpoint->Bus,
    Endpoint->Device,
    Endpoint->Function,
    Endpoint->Vendor,
    Endpoint->DeviceId,
    Endpoint->SubsystemVendor,
    Endpoint->SubsystemDevice,
    Endpoint->Handle,
    Endpoint->RootBridgeHandle,
    Endpoint->ParentHostHandle
    );
}

STATIC
EFI_STATUS
FindVideoEndpoints (
  IN  APP_LOGGER                   *Logger,
  IN  NATIVE_CSM_VGA_RUNTIME_PLAN  *Plan
  )
{
  UINTN                    Index;
  UINTN                    TargetCount;
  UINTN                    ActiveCount;
  CONST PCI_DEVICE_RECORD  *Target;
  CONST PCI_DEVICE_RECORD  *Active;
  EFI_STATUS               Status;

  TargetCount = 0;
  ActiveCount = 0;
  Target      = NULL;
  Active      = NULL;
  for (Index = 0; Index < Plan->Inventory.Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Record;

    Record = &Plan->Inventory.Devices[Index];
    if (!RecordIsDisplay (Record)) {
      continue;
    }

    if (AddressEqualRecord (Record, &Plan->Config.TargetPci)) {
      Target = Record;
      ++TargetCount;
    }

    if (!EFI_ERROR (Record->CurrentAttributesStatus) &&
        ((Record->CurrentAttributes &
          (EFI_PCI_IO_ATTRIBUTE_VGA_IO |
           EFI_PCI_IO_ATTRIBUTE_VGA_IO_16 |
           EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY)) != 0))
    {
      Active = Record;
      ++ActiveCount;
    }
  }

  LogPrint (
    Logger,
    L"Configured target endpoint matches=%u; active legacy-VGA owners=%u\r\n",
    (UINT32)TargetCount,
    (UINT32)ActiveCount
    );
  if ((TargetCount != 1U) || (Target == NULL)) {
    return EFI_NOT_FOUND;
  }

  if ((ActiveCount != 1U) || (Active == NULL)) {
    return EFI_NO_MAPPING;
  }

  if (Target == Active) {
    return EFI_ALREADY_STARTED;
  }

  Status = CopyEndpoint (Target, &Plan->Target);
  if (!EFI_ERROR (Status)) {
    Status = CopyEndpoint (Active, &Plan->Active);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Plan->Target.Segment != 0) {
    LogPrint (
      Logger,
      L"Configured target segment=%04x cannot be represented by the "
      L"Framework EFI_DISPATCH_OPROM_TABLE PciBus-only ABI\r\n",
      Plan->Target.Segment
      );
    return EFI_UNSUPPORTED;
  }

  if ((Plan->Config.HasExpectedVendor &&
       (Plan->Target.Vendor != Plan->Config.ExpectedVendor)) ||
      (Plan->Config.HasExpectedDevice &&
       (Plan->Target.DeviceId != Plan->Config.ExpectedDevice)) ||
      (Plan->Config.HasExpectedSubsystemVendor &&
       (Plan->Target.SubsystemVendor !=
        Plan->Config.ExpectedSubsystemVendor)) ||
      (Plan->Config.HasExpectedSubsystemDevice &&
       (Plan->Target.SubsystemDevice !=
        Plan->Config.ExpectedSubsystemDevice)))
  {
    LogPrint (
      Logger,
      L"Configured target identity did not match the optional INI binding\r\n"
      );
    return EFI_SECURITY_VIOLATION;
  }

  if (EFI_ERROR (Target->RootBridgeStatus) ||
      EFI_ERROR (Active->RootBridgeStatus) ||
      (Plan->Target.RootBridgeHandle == NULL) ||
      (Plan->Active.RootBridgeHandle == NULL) ||
      (Plan->Target.RootBridgeIo == NULL) ||
      (Plan->Active.RootBridgeIo == NULL) ||
      (Plan->Target.ParentHostHandle == NULL) ||
      (Plan->Active.ParentHostHandle == NULL) ||
      (Plan->Target.RootBridgeHandle != Plan->Active.RootBridgeHandle) ||
      (Plan->Target.ParentHostHandle != Plan->Active.ParentHostHandle))
  {
    LogPrint (
      Logger,
      L"Target/active topology gate failed: same-root=%s "
      L"same-parent-host=%s\r\n",
      (Plan->Target.RootBridgeHandle != NULL) &&
      (Plan->Target.RootBridgeHandle == Plan->Active.RootBridgeHandle) ?
        L"yes" : L"no",
      (Plan->Target.ParentHostHandle != NULL) &&
      (Plan->Target.ParentHostHandle == Plan->Active.ParentHostHandle) ?
        L"yes" : L"no"
      );
    return EFI_NO_MAPPING;
  }

  LogEndpoint (Logger, L"Target VGA", &Plan->Target);
  LogPrint (Logger, L"NCV_TARGET_VGA_SELECTED\r\n");
  LogEndpoint (Logger, L"Active VGA/GOP", &Plan->Active);
  LogPrint (Logger, L"NCV_ACTIVE_VGA_SELECTED\r\n");
  LogPrint (
    Logger,
    L"Target and active VGA share root=%p parent-host=%p\r\n",
    Plan->Target.RootBridgeHandle,
    Plan->Target.ParentHostHandle
    );
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
FindStorageController (
  IN  APP_LOGGER                   *Logger,
  IN  NATIVE_CSM_VGA_RUNTIME_PLAN  *Plan
  )
{
  UINTN                    Index;
  UINTN                    ExactCount;
  UINTN                    SegmentlessCount;
  CONST PCI_DEVICE_RECORD  *Selected;
  EFI_STATUS               Status;

  ExactCount       = 0;
  SegmentlessCount = 0;
  Selected         = NULL;
  for (Index = 0; Index < Plan->Inventory.Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Record;

    Record = &Plan->Inventory.Devices[Index];
    if (!RecordIsBootController (Record) ||
        EFI_ERROR (Record->LocationStatus))
    {
      continue;
    }

    if ((Record->Bus == Plan->Config.TargetControllerPci.Bus) &&
        (Record->Device == Plan->Config.TargetControllerPci.Device) &&
        (Record->Function == Plan->Config.TargetControllerPci.Function))
    {
      ++SegmentlessCount;
      if (AddressEqualRecord (
            Record,
            &Plan->Config.TargetControllerPci
            ))
      {
        Selected = Record;
        ++ExactCount;
      }
    }
  }

  LogPrint (
    Logger,
    L"Configured storage controller exact matches=%u; segmentless BBS "
    L"mapping matches=%u\r\n",
    (UINT32)ExactCount,
    (UINT32)SegmentlessCount
    );
  if ((ExactCount != 1U) || (SegmentlessCount != 1U) ||
      (Selected == NULL))
  {
    return EFI_NO_MAPPING;
  }

  Status = CopyEndpoint (Selected, &Plan->StorageController);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // A USB controller can host several disks. Never authorize a USB target
  // from the controller address alone; the BBS discovery below must match
  // exactly one live disk description on that controller.
  if (RecordIsUsbController (Selected) &&
      (!Plan->Config.HasTargetBbsDescription ||
       (Plan->Config.TargetBbsDescription[0] == L'\0')))
  {
    LogPrint (Logger, L"USB boot requires an exact TargetBbsDescription\r\n");
    return EFI_INVALID_PARAMETER;
  }

  // Resolve defaults against the live selected controller on every boot, so
  // changing targets in setup also changes the generic firmware boot option.
  // Explicit overrides still support firmware with different option names.
  if (!Plan->Config.HasLegacyOptionDescription) {
    StrCpyS (
      Plan->Config.LegacyOptionDescription,
      ARRAY_SIZE (Plan->Config.LegacyOptionDescription),
      RecordIsUsbController (Selected) ? L"USB" : L"Hard Drive"
      );
  }
  if (!Plan->Config.HasExcludeDescription) {
    StrCpyS (
      Plan->Config.ExcludeDescription,
      ARRAY_SIZE (Plan->Config.ExcludeDescription),
      RecordIsUsbController (Selected) ? L"Hard Drive" : L"USB"
      );
  }
  LogPrint (
    Logger,
    L"Legacy disk boot policy: option=\"%s\" exclude=\"%s\" controller=%s\r\n",
    Plan->Config.LegacyOptionDescription,
    Plan->Config.ExcludeDescription,
    RecordIsUsbController (Selected) ? L"USB" : L"mass storage"
    );

  CopyMem (
    &Plan->StorageControllerAddress,
    &Plan->Config.TargetControllerPci,
    sizeof (Plan->StorageControllerAddress)
    );
  LogEndpoint (
    Logger,
    L"Boot controller",
    &Plan->StorageController
    );
  LogPrint (Logger, L"NCV_BOOT_CONTROLLER_SELECTED\r\n");
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
BridgeIdentityEqual (
  IN CONST NATIVE_CSM_VGA_PLAN_BRIDGE  *Left,
  IN CONST NATIVE_CSM_VGA_PLAN_BRIDGE  *Right
  )
{
  return (BOOLEAN)(
    (Left != NULL) && (Right != NULL) &&
    (Left->Record.Handle == Right->Record.Handle) &&
    (Left->Record.PciIo == Right->Record.PciIo) &&
    (Left->Record.Segment == Right->Record.Segment) &&
    (Left->Record.Bus == Right->Record.Bus) &&
    (Left->Record.Device == Right->Record.Device) &&
    (Left->Record.Function == Right->Record.Function)
    );
}

STATIC
EFI_STATUS
CopyAndJournalPath (
  IN  APP_LOGGER                    *Logger,
  IN  CONST PCI_DEVICE_RECORD       *RawPath,
  IN  UINTN                         PathCount,
  OUT NATIVE_CSM_VGA_PLAN_BRIDGE    *PlanPath
  )
{
  UINTN       Index;
  EFI_STATUS  Status;
  UINT16      LiveControl;
  UINT16      SnapshotControl;

  for (Index = 0; Index < PathCount; ++Index) {
    if ((RawPath[Index].PciIo == NULL) ||
        (RawPath[Index].PciIo->Pci.Read == NULL) ||
        EFI_ERROR (RawPath[Index].ConfigStatus))
    {
      return EFI_UNSUPPORTED;
    }

    LiveControl = 0;
    Status = RawPath[Index].PciIo->Pci.Read (
                                         RawPath[Index].PciIo,
                                         EfiPciIoWidthUint16,
                                         PCI_BRIDGE_CONTROL_OFFSET,
                                         1,
                                         &LiveControl
                                         );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    SnapshotControl = ReadLe16 (
                        &RawPath[Index].Config[
                          PCI_BRIDGE_CONTROL_OFFSET
                          ]
                        );
    if (LiveControl != SnapshotControl) {
      LogPrint (
        Logger,
        L"Bridge changed during preflight at %04x:%02x:%02x.%x: "
        L"inventory=0x%04x live=0x%04x\r\n",
        (UINT32)RawPath[Index].Segment,
        (UINT32)RawPath[Index].Bus,
        (UINT32)RawPath[Index].Device,
        (UINT32)RawPath[Index].Function,
        SnapshotControl,
        LiveControl
        );
      return EFI_NOT_READY;
    }

    CopyMem (
      &PlanPath[Index].Record,
      &RawPath[Index],
      sizeof (PlanPath[Index].Record)
      );
    PlanPath[Index].OriginalBridgeControl = LiveControl;
    LogPrint (
      Logger,
      L"  path[%u]=%04x:%02x:%02x.%x original-bridge-control=0x%04x\r\n",
      (UINT32)Index,
      (UINT32)RawPath[Index].Segment,
      (UINT32)RawPath[Index].Bus,
      (UINT32)RawPath[Index].Device,
      (UINT32)RawPath[Index].Function,
      LiveControl
      );
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
BuildAndClassifyPaths (
  IN APP_LOGGER                   *Logger,
  IN NATIVE_CSM_VGA_RUNTIME_PLAN  *Plan
  )
{
  PCI_DEVICE_RECORD  RawPath[NATIVE_CSM_VGA_BOOT_MAX_BRIDGES];
  UINTN              RawCount;
  UINTN              Index;
  UINTN              OtherIndex;
  EFI_STATUS         Status;

  ZeroMem (RawPath, sizeof (RawPath));
  RawCount = 0;
  Status = PciBuildValidatedDevicePathBridgePath (
             Logger,
             &Plan->Inventory,
             Plan->Active.InventoryRecord,
             RawPath,
             ARRAY_SIZE (RawPath),
             &RawCount
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Plan->ActivePathCount = RawCount;
  Status = CopyAndJournalPath (
             Logger,
             RawPath,
             RawCount,
             Plan->ActivePath
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  LogPrint (Logger, L"NCV_ACTIVE_PATH_DISCOVERED\r\n");
  ZeroMem (RawPath, sizeof (RawPath));
  RawCount = 0;
  Status = PciBuildValidatedDevicePathBridgePath (
             Logger,
             &Plan->Inventory,
             Plan->Target.InventoryRecord,
             RawPath,
             ARRAY_SIZE (RawPath),
             &RawCount
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Plan->TargetPathCount = RawCount;
  Status = CopyAndJournalPath (
             Logger,
             RawPath,
             RawCount,
             Plan->TargetPath
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  LogPrint (Logger, L"NCV_TARGET_PATH_DISCOVERED\r\n");
  Plan->SharedPathCount = 0;
  while ((Plan->SharedPathCount < Plan->ActivePathCount) &&
         (Plan->SharedPathCount < Plan->TargetPathCount) &&
         BridgeIdentityEqual (
           &Plan->ActivePath[Plan->SharedPathCount],
           &Plan->TargetPath[Plan->SharedPathCount]
           ))
  {
    if (Plan->ActivePath[Plan->SharedPathCount].OriginalBridgeControl !=
        Plan->TargetPath[Plan->SharedPathCount].OriginalBridgeControl)
    {
      return EFI_NOT_READY;
    }

    CopyMem (
      &Plan->SharedPath[Plan->SharedPathCount],
      &Plan->ActivePath[Plan->SharedPathCount],
      sizeof (Plan->SharedPath[Plan->SharedPathCount])
      );
    ++Plan->SharedPathCount;
  }

  for (Index = Plan->SharedPathCount;
       Index < Plan->ActivePathCount;
       ++Index)
  {
    for (OtherIndex = Plan->SharedPathCount;
         OtherIndex < Plan->TargetPathCount;
         ++OtherIndex)
    {
      if (BridgeIdentityEqual (
            &Plan->ActivePath[Index],
            &Plan->TargetPath[OtherIndex]
            ))
      {
        LogPrint (
          Logger,
          L"Topology is not a valid shared-prefix tree: bridge "
          L"%04x:%02x:%02x.%x reappears after path divergence\r\n",
          (UINT32)Plan->ActivePath[Index].Record.Segment,
          (UINT32)Plan->ActivePath[Index].Record.Bus,
          (UINT32)Plan->ActivePath[Index].Record.Device,
          (UINT32)Plan->ActivePath[Index].Record.Function
          );
        return EFI_COMPROMISED_DATA;
      }
    }
  }

  Plan->ActiveExclusiveCount =
    Plan->ActivePathCount - Plan->SharedPathCount;
  Plan->TargetExclusiveCount =
    Plan->TargetPathCount - Plan->SharedPathCount;
  for (Index = 0; Index < Plan->ActiveExclusiveCount; ++Index) {
    CopyMem (
      &Plan->ActiveExclusivePath[Index],
      &Plan->ActivePath[Plan->SharedPathCount + Index],
      sizeof (Plan->ActiveExclusivePath[Index])
      );
  }

  for (Index = 0; Index < Plan->TargetExclusiveCount; ++Index) {
    CopyMem (
      &Plan->TargetExclusivePath[Index],
      &Plan->TargetPath[Plan->SharedPathCount + Index],
      sizeof (Plan->TargetExclusivePath[Index])
      );
  }

  LogPrint (
    Logger,
    L"Route classification: active=%u target=%u shared=%u "
    L"active-exclusive=%u target-exclusive=%u\r\n",
    (UINT32)Plan->ActivePathCount,
    (UINT32)Plan->TargetPathCount,
    (UINT32)Plan->SharedPathCount,
    (UINT32)Plan->ActiveExclusiveCount,
    (UINT32)Plan->TargetExclusiveCount
    );
  return EFI_SUCCESS;
}

STATIC
EFI_PHYSICAL_ADDRESS
FarAddress (
  IN UINT16  Segment,
  IN UINT16  Offset
  )
{
  return ((EFI_PHYSICAL_ADDRESS)Segment << 4) + Offset;
}

STATIC
BOOLEAN
ExecutableLooking (
  IN CONST UINT8  *Bytes,
  IN UINTN        ByteCount
  )
{
  UINTN  Index;
  UINTN  CodeBytes;

  if ((Bytes == NULL) || (ByteCount == 0)) {
    return FALSE;
  }

  CodeBytes = 0;
  for (Index = 0; Index < ByteCount; ++Index) {
    if ((Bytes[Index] != 0x00U) && (Bytes[Index] != 0xFFU)) {
      ++CodeBytes;
    }
  }

  return CodeBytes >= 4U;
}

// Only fields used by the dispatcher are mandatory. Later CSM revisions added
// optional suffix fields; a checksummed future tail must not overflow our copy.
#define COMPAT_REQUIRED_LENGTH \
  (OFFSET_OF (EFI_COMPATIBILITY16_TABLE, EfiSystemTable) + sizeof (UINT32))
#define COMPAT_PCIE_END \
  (OFFSET_OF (EFI_COMPATIBILITY16_TABLE, PciExpressBase) + sizeof (UINT32))
#define COMPAT_LAST_BUS_END \
  (OFFSET_OF (EFI_COMPATIBILITY16_TABLE, LastPciBus) + sizeof (UINT8))

STATIC
EFI_STATUS
SelectCompatibility16 (
  IN  APP_LOGGER                   *Logger,
  OUT NATIVE_CSM_VGA_PLAN_COMPATIBILITY16  *Selected
  )
{
  MEMORY_MAP_SNAPSHOT       Map;
  EFI_STATUS                Status;
  UINTN                     Address;
  UINTN                     Index;
  UINTN                     AcceptedCount;
  CONST volatile UINT8      *Bytes;
  EFI_COMPATIBILITY16_TABLE Candidate;
  EFI_PHYSICAL_ADDRESS      CallAddress;
  EFI_PHYSICAL_ADDRESS      PnpAddress;
  UINT8                     TableLength;
  UINT8                     Checksum;
  BOOLEAN                   RevisionSane;
  BOOLEAN                   SystemTableRelationship;
  BOOLEAN                   ExactSystemTableRelationship;
  BOOLEAN                   PciExpressSane;
  BOOLEAN                   LastBusSane;
  BOOLEAN                   CallLooksExecutable;
  BOOLEAN                   PnpSignatureValid;
  BOOLEAN                   Structural;
  BOOLEAN                   ChecksumValid;
  BOOLEAN                   Accepted;

  if ((Logger == NULL) || (Selected == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Selected, sizeof (*Selected));
  ZeroMem (&Map, sizeof (Map));
  Status = MemoryMapCapture (&Map);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  AcceptedCount = 0;
  for (Address = COMPAT_SCAN_START;
       Address + 6U <= COMPAT_SCAN_END;
       Address += COMPAT_SCAN_ALIGNMENT)
  {
    if (!MemoryMapRangeIsReadable (&Map, Address, 6U, NULL)) {
      continue;
    }

    Bytes = (CONST volatile UINT8 *)(UINTN)Address;
    if ((Bytes[0] != 'I') || (Bytes[1] != 'F') ||
        (Bytes[2] != 'E') || (Bytes[3] != '$'))
    {
      continue;
    }

    TableLength = Bytes[5];
    if ((TableLength < COMPAT_REQUIRED_LENGTH) ||
        (TableLength > COMPAT_SCAN_END - Address) ||
        !MemoryMapRangeIsReadable (
           &Map,
           Address,
           TableLength,
           NULL
           ))
    {
      LogPrint (
        Logger,
        L"$EFI candidate=0x%05lx rejected: unsafe table length=0x%02x\r\n",
        (UINT64)Address,
        TableLength
        );
      continue;
    }

    ZeroMem (&Candidate, sizeof (Candidate));
    CopyMem (&Candidate, (CONST VOID *)(UINTN)Address,
             MIN (TableLength, sizeof (Candidate)));
    CallAddress = FarAddress (
                    Candidate.Compatibility16CallSegment,
                    Candidate.Compatibility16CallOffset
                    );
    PnpAddress = FarAddress (
                   Candidate.PnPInstallationCheckSegment,
                   Candidate.PnPInstallationCheckOffset
                   );
    RevisionSane = (BOOLEAN)(
      (Candidate.TableMajorRevision != 0xFFU) &&
      (Candidate.TableMinorRevision != 0xFFU)
      );
    SystemTableRelationship = (BOOLEAN)(
      (Candidate.EfiSystemTable == 0) ||
      (Candidate.EfiSystemTable == (UINT32)(UINTN)gST)
      );
    ExactSystemTableRelationship = (BOOLEAN)(
      Candidate.EfiSystemTable == (UINT32)(UINTN)gST
      );
    PciExpressSane = (BOOLEAN)(
      (TableLength <= OFFSET_OF (EFI_COMPATIBILITY16_TABLE, PciExpressBase)) ||
      ((TableLength >= COMPAT_PCIE_END) &&
       ((Candidate.PciExpressBase & EFI_PAGE_MASK) == 0))
      );
    // These nonzero values corroborate only the stale-checksum workaround.
    LastBusSane = (BOOLEAN)(
      (TableLength >= COMPAT_LAST_BUS_END) &&
      (TableLength <= sizeof (Candidate)) &&
      (Candidate.PciExpressBase != 0) && (Candidate.LastPciBus != 0)
      );
    CallLooksExecutable = (BOOLEAN)(
      (CallAddress >= COMPAT_SCAN_START) &&
      (CallAddress <=
       (COMPAT_SCAN_END - COMPAT_CALL_SAMPLE_BYTES)) &&
      MemoryMapRangeIsReadable (
        &Map,
        (UINTN)CallAddress,
        COMPAT_CALL_SAMPLE_BYTES,
        NULL
        ) &&
      ExecutableLooking (
        (CONST UINT8 *)(UINTN)CallAddress,
        COMPAT_CALL_SAMPLE_BYTES
        )
      );
    PnpSignatureValid = (BOOLEAN)(
      (PnpAddress >= COMPAT_SCAN_START) &&
      (PnpAddress <= (COMPAT_SCAN_END - 4U)) &&
      MemoryMapRangeIsReadable (&Map, (UINTN)PnpAddress, 4U, NULL) &&
      (CompareMem ((CONST VOID *)(UINTN)PnpAddress, "$PnP", 4U) == 0)
      );

    Checksum = 0;
    for (Index = 0; Index < TableLength; ++Index) {
      Checksum = (UINT8)(Checksum + Bytes[Index]);
    }

    ChecksumValid = (BOOLEAN)(Checksum == 0);
    Structural = (BOOLEAN)(
      RevisionSane &&
      ExactSystemTableRelationship &&
      PciExpressSane &&
      LastBusSane &&
      CallLooksExecutable &&
      PnpSignatureValid
      );
    Accepted = (BOOLEAN)(
      RevisionSane &&
      SystemTableRelationship &&
      PciExpressSane &&
      CallLooksExecutable &&
      PnpSignatureValid &&
      (ChecksumValid || Structural)
      );
    LogPrint (
      Logger,
      L"$EFI candidate=0x%05lx length=0x%02x revision=%u.%u "
      L"call=%04x:%04x checksum=0x%02x structural=%s accepted=%s\r\n",
      (UINT64)Address,
      TableLength,
      Candidate.TableMajorRevision,
      Candidate.TableMinorRevision,
      Candidate.Compatibility16CallSegment,
      Candidate.Compatibility16CallOffset,
      Checksum,
      Structural ? L"yes" : L"no",
      Accepted ? L"yes" : L"no"
      );
    if (!Accepted) {
      LogPrint (Logger, L"  revision=%s system-table=%s PCIe-field=%s call-bytes=%s PnP-signature=%s\r\n",
                RevisionSane ? L"valid" : L"invalid",
                SystemTableRelationship ? L"valid" : L"invalid",
                PciExpressSane ? L"valid" : L"invalid",
                CallLooksExecutable ? L"valid" : L"invalid",
                PnpSignatureValid ? L"valid" : L"invalid");
      continue;
    }

    ++AcceptedCount;
    Selected->Table                  =
      (EFI_COMPATIBILITY16_TABLE *)(UINTN)Address;
    Selected->Address                = (EFI_PHYSICAL_ADDRESS)Address;
    Selected->TableLength            = TableLength;
    Selected->StoredChecksum         = Candidate.TableChecksum;
    Selected->CalculatedChecksum     = Checksum;
    Selected->CallSegment            =
      Candidate.Compatibility16CallSegment;
    Selected->CallOffset             =
      Candidate.Compatibility16CallOffset;
    Selected->CallAddress            = CallAddress;
    Selected->PnpSegment             =
      Candidate.PnPInstallationCheckSegment;
    Selected->PnpOffset              =
      Candidate.PnPInstallationCheckOffset;
    Selected->ChecksumValid          = ChecksumValid;
    Selected->StructuralCorroboration = Structural;
    Selected->StaleChecksumAccepted  =
      (BOOLEAN)(!ChecksumValid && Structural);
  }

  MemoryMapRelease (&Map);
  if ((AcceptedCount != 1U) || (Selected->Table == NULL)) {
    LogPrint (
      Logger,
      L"Compatibility16 accepted-candidate count=%u; exactly one is required\r\n",
      (UINT32)AcceptedCount
      );
    ZeroMem (Selected, sizeof (*Selected));
    return (AcceptedCount == 0) ? EFI_NOT_FOUND : EFI_NO_MAPPING;
  }

  if (Selected->StaleChecksumAccepted) {
    LogPrint (
      Logger,
      L"CHECKSUM_MISMATCH_ACCEPTED_ONLY_BY_EXACT_STRUCTURAL_CORROBORATION\r\n"
      );
  }

  LogPrint (
    Logger,
    L"NCV_COMPATIBILITY16_SELECTED table=0x%05lx "
    L"call=%04x:%04x checksum-valid=%s structural=%s stale-quirk=%s\r\n",
    (UINT64)Selected->Address,
    Selected->CallSegment,
    Selected->CallOffset,
    Selected->ChecksumValid ? L"yes" : L"no",
    Selected->StructuralCorroboration ? L"yes" : L"no",
    Selected->StaleChecksumAccepted ? L"yes" : L"no"
    );
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SelectRom (
  IN APP_LOGGER                   *Logger,
  IN NATIVE_CSM_VGA_RUNTIME_PLAN  *Plan
  )
{
  EFI_STATUS  Status;

  if ((Plan->Target.PciIo == NULL) ||
      (Plan->Target.PciIo->RomImage == NULL) ||
      (Plan->Target.PciIo->RomSize == 0))
  {
    return EFI_NOT_FOUND;
  }

  Plan->ExposedRom     = Plan->Target.PciIo->RomImage;
  Plan->ExposedRomSize = Plan->Target.PciIo->RomSize;
  Status = OptionRomParseAndValidate (
             Logger,
             Plan->ExposedRom,
             Plan->ExposedRomSize,
             Plan->ExposedRomSize,
             Plan->Target.Vendor,
             Plan->Target.DeviceId,
             &Plan->RomValidation
             );
  if (EFI_ERROR (Status) ||
      !Plan->RomValidation.MatchingLegacyImageFound ||
      (Plan->RomValidation.MatchingLegacyCandidateCount != 1U) ||
      (Plan->RomValidation.MatchingLegacyImageSize == 0) ||
      (Plan->RomValidation.MatchingLegacyImageSize > 0x10000U) ||
      (Plan->RomValidation.MatchingLegacyImageOffset >
       Plan->ExposedRomSize) ||
      (Plan->RomValidation.MatchingLegacyImageSize >
       Plan->ExposedRomSize -
       Plan->RomValidation.MatchingLegacyImageOffset))
  {
    return EFI_COMPROMISED_DATA;
  }

  Plan->SelectedRomOffset = Plan->RomValidation.MatchingLegacyImageOffset;
  Plan->SelectedRomSize   =
    (UINTN)Plan->RomValidation.MatchingLegacyImageSize;
  Plan->SelectedRom       =
    Plan->ExposedRom + Plan->SelectedRomOffset;
  Plan->SelectedRomVendor = Plan->Target.Vendor;
  Plan->SelectedRomDevice = Plan->Target.DeviceId;
  LogPrint (
    Logger,
    L"NCV_TARGET_ROM_VALIDATED exposed=%p/0x%lx "
    L"selected=%p offset=0x%lx size=0x%lx identity=%04x:%04x\r\n",
    Plan->ExposedRom,
    Plan->ExposedRomSize,
    Plan->SelectedRom,
    Plan->SelectedRomOffset,
    (UINT64)Plan->SelectedRomSize,
    Plan->SelectedRomVendor,
    Plan->SelectedRomDevice
    );
  return LegacyRomGuardCheck (Logger, Plan);
}

STATIC
EFI_STATUS
JournalEndpoint (
  IN APP_LOGGER                   *Logger,
  IN NATIVE_CSM_VGA_RUNTIME_PLAN  *Plan
  )
{
  EFI_STATUS  Status;
  UINT16      LiveCommand;
  UINT64      LiveAttributes;

  if ((Plan->Target.PciIo == NULL) ||
      (Plan->Target.PciIo->Pci.Read == NULL) ||
      (Plan->Target.PciIo->Attributes == NULL))
  {
    return EFI_UNSUPPORTED;
  }

  LiveCommand = 0;
  Status = Plan->Target.PciIo->Pci.Read (
                                     Plan->Target.PciIo,
                                     EfiPciIoWidthUint16,
                                     PCI_COMMAND_OFFSET,
                                     1,
                                     &LiveCommand
                                     );
  if (EFI_ERROR (Status) ||
      (LiveCommand != Plan->Target.OriginalCommand))
  {
    return EFI_NOT_READY;
  }

  LiveAttributes = 0;
  Status = Plan->Target.PciIo->Attributes (
                                     Plan->Target.PciIo,
                                     EfiPciIoAttributeOperationGet,
                                     0,
                                     &LiveAttributes
                                     );
  if (EFI_ERROR (Status) ||
      EFI_ERROR (Plan->Target.OriginalAttributesStatus) ||
      (LiveAttributes != Plan->Target.OriginalAttributes))
  {
    return EFI_NOT_READY;
  }

  LogPrint (
    Logger,
    L"Target endpoint journal: command=0x%04x EFI-attributes=0x%016lx\r\n",
    Plan->Target.OriginalCommand,
    Plan->Target.OriginalAttributes
    );
  return EFI_SUCCESS;
}

/* The retired reference-machine mode is rejected, never silently weakened.
 * Generic plan validation above derives all targets from configuration/discovery.
 * Keep this ABI entry point and plan fields for compatibility and stack stability.
 */
EFI_STATUS
NativeCsmVgaRuntimePlanFinalizeValidation (
  IN APP_LOGGER *Logger,
  IN OUT NATIVE_CSM_VGA_RUNTIME_PLAN *Plan
  )
{
  if ((Logger == NULL) || (Plan == NULL) || !Plan->Complete) {
    return EFI_INVALID_PARAMETER;
  }
  Plan->ValidationCompleted = TRUE;
  Plan->ValidationPassed = FALSE;
  if (Plan->Config.RequireReferenceSnapshotMatch) {
    LogPrint (Logger, L"RequireReferenceSnapshotMatch=true is unsupported; use INI targets and generic validation.\r\n");
    return EFI_UNSUPPORTED;
  }
  Plan->ValidationPassed = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
NativeCsmVgaRuntimePlanBuild (
  IN  APP_FILE_CONTEXT                 *Files,
  IN  APP_LOGGER                       *Logger,
  IN  CONST PROBE_CONFIG               *Config,
  OUT NATIVE_CSM_VGA_RUNTIME_PLAN      *Plan
  )
{
  EFI_STATUS  Status;

  if ((Files == NULL) || (Logger == NULL) || (Config == NULL) || (Plan == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Plan, sizeof (*Plan));
  CopyMem (&Plan->Config, Config, sizeof (Plan->Config));
  if (Config->Probe ||
      !Config->HasTargetPci ||
      !Config->HasTargetControllerPci ||
      !Config->HasIoDecoding ||
      !Config->HasMemoryDecoding ||
      !Config->HasBusMastering)
  {
    if (!Config->HasTargetPci) { LogPrint (Logger, L"Missing required Boot key: Video.TargetPci\r\n"); }
    if (!Config->HasIoDecoding) { LogPrint (Logger, L"Missing required Boot key: Endpoint.IoDecoding\r\n"); }
    if (!Config->HasMemoryDecoding) { LogPrint (Logger, L"Missing required Boot key: Endpoint.MemoryDecoding\r\n"); }
    if (!Config->HasBusMastering) { LogPrint (Logger, L"Missing required Boot key: Endpoint.BusMastering\r\n"); }
    if (!Config->HasTargetControllerPci) { LogPrint (Logger, L"Missing required Boot key: Boot.TargetControllerPci\r\n"); }
    LogPrint (Logger, L"NCV_BLOCKED_CONFIG; run Probe=true\r\n");
    return EFI_NOT_FOUND;
  }

  LogSetVerbose (Logger, Plan->Config.VerboseLog);
  LogPrint (Logger, L"NCV_CONFIG_PARSED\r\n");

  Status = SelectCompatibility16 (Logger, &Plan->Compatibility16);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_CSM_CAPABILITY\r\n");
    return Status;
  }

  Status = gBS->LocateProtocol (
                  &gEfiLegacyBiosProtocolGuid,
                  NULL,
                  (VOID **)&Plan->LegacyBios
                  );
  if (EFI_ERROR (Status) || (Plan->LegacyBios == NULL) ||
      (Plan->LegacyBios->FarCall86 == NULL) ||
      (Plan->LegacyBios->LegacyBoot == NULL) ||
      (Plan->LegacyBios->GetBbsInfo == NULL))
  {
    LogPrint (Logger, L"NCV_BLOCKED_CSM_CAPABILITY\r\n");
    return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
  }

  Status = gBS->LocateProtocol (
                  &gEfiLegacyRegion2ProtocolGuid,
                  NULL,
                  (VOID **)&Plan->LegacyRegion2
                  );
  if (EFI_ERROR (Status) || (Plan->LegacyRegion2 == NULL) ||
      (Plan->LegacyRegion2->UnLock == NULL))
  {
    LogPrint (Logger, L"NCV_BLOCKED_CSM_CAPABILITY\r\n");
    return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
  }

  Status = PciInventoryBuild (Logger, &Plan->Inventory);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_OTHER\r\n");
    return Status;
  }

  Status = FindVideoEndpoints (Logger, Plan);
  if (EFI_ERROR (Status)) {
    LogPrint (
      Logger,
      (Status == EFI_SECURITY_VIOLATION) ?
        L"NCV_BLOCKED_TARGET_IDENTITY\r\n" :
        L"NCV_BLOCKED_ACTIVE_VGA\r\n"
      );
    return Status;
  }

  LogPrint (Logger, L"Endpoint policy comes exclusively from Config.ini\r\n");

  Status = BuildAndClassifyPaths (Logger, Plan);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_TARGET_PATH\r\n");
    return Status;
  }

  Status = JournalEndpoint (Logger, Plan);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_TARGET_VGA\r\n");
    return Status;
  }

  Status = SelectRom (Logger, Plan);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_TARGET_ROM\r\n");
    return Status;
  }

  Status = FindStorageController (Logger, Plan);
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_BOOT_CONTROLLER\r\n");
    return Status;
  }

  Status = LegacyBootBootTargetDiscover (
             Logger,
             Plan->LegacyBios,
             Plan->StorageControllerAddress.Bus,
             Plan->StorageControllerAddress.Device,
             Plan->StorageControllerAddress.Function,
             Plan->Config.HasTargetBbsDescription ? Plan->Config.TargetBbsDescription : NULL,
             Plan->Config.LegacyOptionDescription,
             Plan->Config.ExcludeDescription,
             &Plan->BootTarget
             );
  if (EFI_ERROR (Status)) {
    LogPrint (
      Logger,
      Plan->BootTarget.GetBbsInfoCalled ?
        (Status == EFI_ACCESS_DENIED ? L"NCV_BLOCKED_BBS_ELIGIBILITY\r\n" : L"NCV_BLOCKED_BBS_TARGET\r\n") :
        L"NCV_BLOCKED_LEGACY_OPTION\r\n"
      );
    return Status;
  }

  if ((Plan->BootTarget.BbsCount > MAX_UINT8) ||
      ((UINTN)Plan->BootTarget.FirmwareBbsTable > MAX_UINT32))
  {
    LogPrint (
      Logger,
      L"Native BBS result cannot be represented in the Framework dispatch "
      L"table: count=%u pointer=%p\r\n",
      Plan->BootTarget.BbsCount,
      Plan->BootTarget.FirmwareBbsTable
      );
    LogPrint (Logger, L"NCV_BLOCKED_BBS_TARGET\r\n");
    return EFI_UNSUPPORTED;
  }

  Status = LegacyBootBootTargetValidateOwnedCopies (
             Logger,
             &Plan->BootTarget
             );
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_LEGACY_OPTION\r\n");
    return Status;
  }

  Plan->UniqueBbsIndex = Plan->BootTarget.LiveBbsIndex;
  CopyMem (
    &Plan->UniqueBbsEntry,
    &Plan->BootTarget.LiveBbsEntry,
    sizeof (Plan->UniqueBbsEntry)
    );
  LogPrint (
    Logger,
    L"NCV_BBS_TARGET_SELECTED index=%u BDF=%02x:%02x.%x "
    L"description=\"%a\"\r\n",
    Plan->UniqueBbsIndex,
    Plan->UniqueBbsEntry.Bus,
    Plan->UniqueBbsEntry.Device,
    Plan->UniqueBbsEntry.Function,
    Plan->BootTarget.LiveDescription
    );
  LogPrint (
    Logger,
    L"NCV_LEGACY_OPTION_SELECTED Boot%04x description=\"%s\" "
    L"path=%p/0x%lx opaque=%p/0x%x\r\n",
    Plan->BootTarget.BootOptionNumber,
    Plan->BootTarget.BootOptionDescription,
    Plan->BootTarget.BbsDevicePath,
    (UINT64)Plan->BootTarget.BbsDevicePathSize,
    Plan->BootTarget.LoadOptions,
    (UINT32)Plan->BootTarget.LoadOptionsSize
    );

  Status = MemoryMapValidateBootRanges ();
  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"NCV_BLOCKED_LOW_MEMORY_RANGE\r\nFull IVT/BDA/video/firmware ranges are not readable: %r\r\n", Status);
    return Status;
  }
  Plan->InitialInt10Offset  = ((volatile UINT16 *)(UINTN)(0x10U * 4U))[0];
  Plan->InitialInt10Segment = ((volatile UINT16 *)(UINTN)(0x10U * 4U))[1];
  LogPrint (
    Logger,
    L"Initial INT10 vector discovered=%04x:%04x\r\n",
    Plan->InitialInt10Segment,
    Plan->InitialInt10Offset
    );

  Plan->Complete = TRUE;
  LogPrint (
    Logger,
    L"NCV_RUNTIME_PLAN_COMPLETE pointers retained: "
    L"inventory=%p boot-context=%p\r\n",
    Plan->Inventory.Devices,
    &Plan->BootTarget
    );
  Status = NativeCsmVgaRuntimePlanFinalizeValidation (Logger, Plan);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
NativeCsmVgaRuntimePlanRelease (
  IN OUT NATIVE_CSM_VGA_RUNTIME_PLAN  *Plan
  )
{
  EFI_STATUS  Status, DispatchStatus;

  if (Plan == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Plan->RouteDirty || Plan->EndpointWriteApplied) {
    return EFI_ACCESS_DENIED;
  }

  Status = LegacyBootBootTargetRelease (&Plan->BootTarget);
  DispatchStatus = LegacyDispatchRelease ();
  if (EFI_ERROR (Status)) { return Status; }
  PciInventoryRelease (&Plan->Inventory);
  ZeroMem (Plan, sizeof (*Plan));
  return DispatchStatus;
}

/** @file
  Read-only PCI enumeration and selected-GPU topology reporting.

  This module deliberately contains no PCI write helper and never requests an
  EFI PCI attribute Set/Enable/Disable operation.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include <Uefi.h>

#include <IndustryStandard/Acpi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>

#include "PciEnumerate.h"
#include "MemoryMap.h"
#include "StatusPrint.h"

#define PCI_VENDOR_ID_OFFSET             0x00
#define PCI_DEVICE_ID_OFFSET             0x02
#define PCI_COMMAND_OFFSET               0x04
#define PCI_PROGIF_OFFSET                0x09
#define PCI_SUBCLASS_OFFSET              0x0A
#define PCI_BASE_CLASS_OFFSET            0x0B
#define PCI_HEADER_TYPE_OFFSET           0x0E
#define PCI_BAR0_OFFSET                  0x10
#define PCI_PRIMARY_BUS_OFFSET           0x18
#define PCI_SECONDARY_BUS_OFFSET         0x19
#define PCI_SUBORDINATE_BUS_OFFSET       0x1A
#define PCI_SUBSYSTEM_VENDOR_ID_OFFSET   0x2C
#define PCI_SUBSYSTEM_ID_OFFSET          0x2E
#define PCI_BRIDGE_CONTROL_OFFSET        0x3E

#define PCI_HEADER_TYPE_MASK             0x7F
#define PCI_HEADER_TYPE_DEVICE           0x00
#define PCI_HEADER_TYPE_BRIDGE           0x01
#define PCI_BASE_CLASS_DISPLAY           0x03
#define PCI_BASE_CLASS_BRIDGE             0x06
#define PCI_SUBCLASS_P2P_BRIDGE          0x04

#define PCI_COMMAND_IO                   BIT0
#define PCI_COMMAND_MEMORY               BIT1
#define PCI_COMMAND_BUS_MASTER           BIT2
#define PCI_COMMAND_VGA_PALETTE_SNOOP    BIT5
#define PCI_BRIDGE_CONTROL_VGA           BIT3
#define PCI_BRIDGE_CONTROL_VGA_16        BIT4

#define DEVICE_PATH_VALIDATION_LIMIT     4096

// GetBarAttributes() does not return its allocation size.  Bound every walk,
// validate every read against the UEFI memory map, and stop at the first
// non-spec descriptor rather than trusting an unknown length.
#define BAR_RESOURCE_SCAN_LIMIT          4096U
#define BAR_RESOURCE_DESCRIPTOR_LIMIT    64U
#define ACPI_LARGE_DESCRIPTOR_HEADER_SIZE  3U
#define ACPI_END_TAG_SIZE                2U

STATIC
UINT16
ReadLe16 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8));
}

STATIC
UINT32
ReadLe32 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT32)(Bytes[0] |
                  ((UINT32)Bytes[1] << 8) |
                  ((UINT32)Bytes[2] << 16) |
                  ((UINT32)Bytes[3] << 24));
}

STATIC
UINT64
ReadLe64 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT64)ReadLe32 (Bytes) |
         ((UINT64)ReadLe32 (Bytes + sizeof (UINT32)) << 32);
}

STATIC
VOID
LogStatus (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Operation,
  IN EFI_STATUS    Status
  )
{
  LogPrint (
    Logger,
    L"%s: 0x%016lx (%s)\r\n",
    Operation,
    (UINT64)Status,
    EfiStatusName (Status)
    );
}

STATIC
BOOLEAN
RecordLocationIsValid (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (Record != NULL) && !EFI_ERROR (Record->LocationStatus) &&
         (Record->Segment <= MAX_UINT16) && (Record->Bus <= MAX_UINT8) &&
         (Record->Device <= 31) && (Record->Function <= 7);
}

STATIC
BOOLEAN
RecordIsDisplay (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (Record != NULL) && !EFI_ERROR (Record->ConfigStatus) &&
         (Record->Config[PCI_BASE_CLASS_OFFSET] == PCI_BASE_CLASS_DISPLAY);
}

STATIC
BOOLEAN
RecordIsP2pBridge (
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  return (Record != NULL) && !EFI_ERROR (Record->ConfigStatus) &&
         ((Record->Config[PCI_HEADER_TYPE_OFFSET] & PCI_HEADER_TYPE_MASK) ==
          PCI_HEADER_TYPE_BRIDGE) &&
         (Record->Config[PCI_BASE_CLASS_OFFSET] == PCI_BASE_CLASS_BRIDGE) &&
         (Record->Config[PCI_SUBCLASS_OFFSET] == PCI_SUBCLASS_P2P_BRIDGE);
}

STATIC
BOOLEAN
DevicePathOffsetIsNodeBoundary (
  IN CONST PCI_DEVICE_RECORD  *Record,
  IN UINTN                    TargetOffset
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Node;
  UINTN                           Offset;
  UINTN                           NodeSize;

  if ((Record == NULL) || (Record->DevicePath == NULL) ||
      (Record->DevicePathSize < END_DEVICE_PATH_LENGTH) ||
      (TargetOffset > (Record->DevicePathSize - END_DEVICE_PATH_LENGTH)))
  {
    return FALSE;
  }

  Offset = 0;
  while (Offset <= (Record->DevicePathSize - END_DEVICE_PATH_LENGTH)) {
    if (Offset == TargetOffset) {
      return TRUE;
    }

    Node = (CONST EFI_DEVICE_PATH_PROTOCOL *)
           ((CONST UINT8 *)Record->DevicePath + Offset);
    NodeSize = DevicePathNodeLength (Node);
    if ((NodeSize < sizeof (EFI_DEVICE_PATH_PROTOCOL)) ||
        (NodeSize > (Record->DevicePathSize - Offset)))
    {
      return FALSE;
    }

    Offset += NodeSize;
  }

  return FALSE;
}

STATIC
CONST PCI_DEVICE_RECORD *
FindDevice (
  IN CONST PCI_INVENTORY  *Inventory,
  IN UINTN                Segment,
  IN UINTN                Bus,
  IN UINTN                Device,
  IN UINTN                Function
  )
{
  UINTN  Index;

  if ((Inventory == NULL) || (Inventory->Devices == NULL)) {
    return NULL;
  }

  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Record;

    Record = &Inventory->Devices[Index];
    if (RecordLocationIsValid (Record) &&
        (Record->Segment == Segment) && (Record->Bus == Bus) &&
        (Record->Device == Device) && (Record->Function == Function))
    {
      return Record;
    }
  }

  return NULL;
}

EFI_STATUS
PciInventoryBuild (
  IN  APP_LOGGER     *Logger,
  OUT PCI_INVENTORY  *Inventory
  )
{
  EFI_STATUS             Status;
  EFI_STATUS             FreeStatus;
  EFI_HANDLE             *Handles;
  UINTN                  HandleCount;
  UINTN                  HandleIndex;
  PCI_DEVICE_RECORD      *Record;
  EFI_PCI_IO_PROTOCOL    *PciIo;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *RemainingDevicePath;

  if ((Logger == NULL) || (Inventory == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Inventory, sizeof (*Inventory));
  Handles     = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    LogStatus (Logger, L"LocateHandleBuffer(EFI_PCI_IO_PROTOCOL)", Status);
    return Status;
  }

  Inventory->HandleCount = HandleCount;
  LogPrint (Logger, L"PCI I/O handles: %u\r\n", HandleCount);
  if (HandleCount == 0) {
    if (Handles != NULL) {
      FreeStatus = gBS->FreePool (Handles);
      if (EFI_ERROR (FreeStatus)) {
        LogStatus (Logger, L"FreePool(PCI handle buffer)", FreeStatus);
      }
    }

    return EFI_SUCCESS;
  }

  if (HandleCount > (MAX_UINTN / sizeof (*Inventory->Devices))) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Inventory->Devices = AllocateZeroPool (HandleCount * sizeof (*Inventory->Devices));
  if (Inventory->Devices == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  for (HandleIndex = 0; HandleIndex < HandleCount; ++HandleIndex) {
    PciIo  = NULL;
    Status = gBS->HandleProtocol (
                    Handles[HandleIndex],
                    &gEfiPciIoProtocolGuid,
                    (VOID **)&PciIo
                    );
    if (EFI_ERROR (Status) || (PciIo == NULL)) {
      LogPrint (
        Logger,
        L"PCI handle %u HandleProtocol failed: 0x%016lx (%s)\r\n",
        HandleIndex,
        (UINT64)Status,
        EfiStatusName (Status)
        );
      continue;
    }

    Record = &Inventory->Devices[Inventory->Count++];
    Record->Handle                    = Handles[HandleIndex];
    Record->PciIo                     = PciIo;
    Record->LocationStatus            = EFI_NOT_READY;
    Record->ConfigStatus              = EFI_NOT_READY;
    Record->DevicePathStatus          = EFI_NOT_READY;
    Record->SupportedAttributesStatus = EFI_NOT_READY;
    Record->CurrentAttributesStatus   = EFI_NOT_READY;
    Record->RootBridgeStatus          = EFI_NOT_READY;
    Record->RootAttributesStatus      = EFI_NOT_READY;

    if (PciIo->GetLocation == NULL) {
      Record->LocationStatus = EFI_UNSUPPORTED;
    } else {
      Record->LocationStatus = PciIo->GetLocation (
                                        PciIo,
                                        &Record->Segment,
                                        &Record->Bus,
                                        &Record->Device,
                                        &Record->Function
                                        );
    }

    if (!RecordLocationIsValid (Record)) {
      LogPrint (
        Logger,
        L"PCI handle %u returned invalid/unavailable location: "
        L"0x%016lx (%s), raw %lx:%lx:%lx.%lx\r\n",
        HandleIndex,
        (UINT64)Record->LocationStatus,
        EfiStatusName (Record->LocationStatus),
        (UINT64)Record->Segment,
        (UINT64)Record->Bus,
        (UINT64)Record->Device,
        (UINT64)Record->Function
        );
    }

    if (PciIo->Pci.Read == NULL) {
      Record->ConfigStatus = EFI_UNSUPPORTED;
    } else {
      Record->ConfigStatus = PciIo->Pci.Read (
                                           PciIo,
                                           EfiPciIoWidthUint8,
                                           0,
                                           PCI_PROBE_CONFIG_BYTES,
                                           Record->Config
                                           );
    }

    if (EFI_ERROR (Record->ConfigStatus)) {
      LogPrint (
        Logger,
        L"PCI %04x:%02x:%02x.%x 64-byte config read failed: "
        L"0x%016lx (%s)\r\n",
        (UINT32)Record->Segment,
        (UINT32)Record->Bus,
        (UINT32)Record->Device,
        (UINT32)Record->Function,
        (UINT64)Record->ConfigStatus,
        EfiStatusName (Record->ConfigStatus)
        );
    }

    if (PciIo->Attributes == NULL) {
      Record->SupportedAttributesStatus = EFI_UNSUPPORTED;
      Record->CurrentAttributesStatus   = EFI_UNSUPPORTED;
    } else {
      Record->SupportedAttributesStatus = PciIo->Attributes (
                                                    PciIo,
                                                    EfiPciIoAttributeOperationSupported,
                                                    0,
                                                    &Record->SupportedAttributes
                                                    );
      Record->CurrentAttributesStatus = PciIo->Attributes (
                                                  PciIo,
                                                  EfiPciIoAttributeOperationGet,
                                                  0,
                                                  &Record->CurrentAttributes
                                                  );
    }

    DevicePath = NULL;
    Record->DevicePathStatus = gBS->HandleProtocol (
                                      Handles[HandleIndex],
                                      &gEfiDevicePathProtocolGuid,
                                      (VOID **)&DevicePath
                                      );
    if (!EFI_ERROR (Record->DevicePathStatus) && (DevicePath != NULL) &&
        IsDevicePathValid (DevicePath, DEVICE_PATH_VALIDATION_LIMIT))
    {
      Record->DevicePath     = DevicePath;
      Record->DevicePathSize = GetDevicePathSize (DevicePath);
      if ((Record->DevicePathSize < END_DEVICE_PATH_LENGTH) ||
          (Record->DevicePathSize > DEVICE_PATH_VALIDATION_LIMIT))
      {
        Record->DevicePath       = NULL;
        Record->DevicePathSize   = 0;
        Record->DevicePathStatus = EFI_COMPROMISED_DATA;
      }
    } else if (!EFI_ERROR (Record->DevicePathStatus)) {
      Record->DevicePathStatus = EFI_COMPROMISED_DATA;
    }

    if (RecordIsDisplay (Record) && (Record->DevicePath != NULL)) {
      RemainingDevicePath = Record->DevicePath;
      Record->RootBridgeStatus = gBS->LocateDevicePath (
                                          &gEfiPciRootBridgeIoProtocolGuid,
                                          &RemainingDevicePath,
                                          &Record->RootBridgeHandle
                                          );
      if (!EFI_ERROR (Record->RootBridgeStatus) &&
          (Record->RootBridgeHandle == NULL))
      {
        Record->RootBridgeStatus = EFI_COMPROMISED_DATA;
      }

      if (!EFI_ERROR (Record->RootBridgeStatus)) {
        UINTN  DevicePathAddress;
        UINTN  RemainingAddress;

        DevicePathAddress = (UINTN)Record->DevicePath;
        RemainingAddress  = (UINTN)RemainingDevicePath;
        if ((DevicePathAddress > (MAX_UINTN - Record->DevicePathSize)) ||
            (RemainingAddress < DevicePathAddress) ||
            (RemainingAddress > (DevicePathAddress + Record->DevicePathSize -
                                 END_DEVICE_PATH_LENGTH)))
        {
          Record->RootBridgeStatus = EFI_COMPROMISED_DATA;
        } else {
          Record->RootBridgePrefixSize = RemainingAddress - DevicePathAddress;
          if (!DevicePathOffsetIsNodeBoundary (
                 Record,
                 Record->RootBridgePrefixSize
                 ))
          {
            Record->RootBridgeStatus = EFI_COMPROMISED_DATA;
          }
        }
      }

      if (!EFI_ERROR (Record->RootBridgeStatus)) {
        Record->RootBridgeStatus = gBS->HandleProtocol (
                                          Record->RootBridgeHandle,
                                          &gEfiPciRootBridgeIoProtocolGuid,
                                          (VOID **)&Record->RootBridgeIo
                                          );
        if (!EFI_ERROR (Record->RootBridgeStatus) &&
            ((Record->RootBridgeIo == NULL) ||
             (Record->RootBridgeIo->ParentHandle == NULL)))
        {
          Record->RootBridgeStatus = EFI_COMPROMISED_DATA;
        }
      }

      if (!EFI_ERROR (Record->RootBridgeStatus) &&
          (Record->RootBridgeIo != NULL))
      {
        if (Record->RootBridgeIo->SegmentNumber != Record->Segment) {
          Record->RootBridgeStatus = EFI_NO_MAPPING;
        } else if (Record->RootBridgeIo->GetAttributes == NULL) {
          Record->RootAttributesStatus = EFI_UNSUPPORTED;
        } else {
          Record->RootAttributesStatus = Record->RootBridgeIo->GetAttributes (
                                                               Record->RootBridgeIo,
                                                               &Record->RootSupportedAttributes,
                                                               &Record->RootCurrentAttributes
                                                               );
        }
      }
    }
  }

  Status = EFI_SUCCESS;

Done:
  if (Handles != NULL) {
    FreeStatus = gBS->FreePool (Handles);
    if (EFI_ERROR (FreeStatus)) {
      LogStatus (Logger, L"FreePool(PCI handle buffer)", FreeStatus);
      if (!EFI_ERROR (Status)) {
        Status = FreeStatus;
      }
    }
  }

  if (EFI_ERROR (Status)) {
    PciInventoryRelease (Inventory);
  }

  return Status;
}

VOID
PciInventoryRelease (
  IN OUT PCI_INVENTORY  *Inventory
  )
{
  if (Inventory == NULL) {
    return;
  }

  if (Inventory->Devices != NULL) {
    FreePool (Inventory->Devices);
  }

  ZeroMem (Inventory, sizeof (*Inventory));
}

STATIC
EFI_STATUS
PrintBarResourceList (
  IN APP_LOGGER   *Logger,
  IN CONST VOID   *Resources,
  IN UINT64       Supports,
  IN UINT8        LogicalBarIndex,
  IN BOOLEAN      ExpectedIo,
  IN BOOLEAN      Expected64Bit,
  IN UINT64       ConfigBusBase
  )
{
  EFI_STATUS           Status;
  MEMORY_MAP_SNAPSHOT  MemoryMap;
  CONST UINT8          *Descriptor;
  UINTN                ResourceAddress;
  UINTN                Offset;
  UINTN                DescriptorCount;
  UINTN                ResourceCount;
  UINTN                ByteIndex;
  UINT16               DescriptorLength;
  UINTN                TotalLength;
  UINT8                Tag;
  UINT8                Checksum;
  UINT8                ResourceType;
  UINT64               Granularity;
  UINT64               HostBase;
  UINT64               HostEnd;
  UINT64               BusBase;
  UINT64               BusEnd;
  UINT64               RangeMaximum;
  UINT64               Translation;
  UINT64               Length;
  UINT64               FirstHostBase;
  UINT64               FirstBusBase;
  UINT64               PreviousHostEnd;
  UINT64               PreviousBusEnd;
  UINT64               CombinedLength;
  BOOLEAN              EndTagFound;
  BOOLEAN              ListValid;
  BOOLEAN              RangeMaximumIsAlignment;
  BOOLEAN              RangeMaximumIsLiteralEnd;

  if ((Logger == NULL) || (Resources == NULL) || (LogicalBarIndex >= 6))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&MemoryMap, sizeof (MemoryMap));
  Status = MemoryMapCapture (&MemoryMap);
  if (EFI_ERROR (Status)) {
    LogPrint (
      Logger,
      L"      GetBarAttributes logical BAR%u: cannot validate resource "
      L"buffer, memory-map capture=0x%016lx (%s)\r\n",
      (UINT32)LogicalBarIndex,
      (UINT64)Status,
      EfiStatusName (Status)
      );
    LogPrint (
      Logger,
      L"      fallback: read-only PCI config bus-base=0x%016lx; "
      L"host-base and size unknown (no BAR sizing writes)\r\n",
      ConfigBusBase
      );
    return Status;
  }

  LogPrint (
    Logger,
    L"      GetBarAttributes logical BAR%u: supports=0x%016lx\r\n",
    (UINT32)LogicalBarIndex,
    Supports
    );

  ResourceAddress  = (UINTN)Resources;
  Offset           = 0;
  DescriptorCount  = 0;
  ResourceCount    = 0;
  Checksum         = 0;
  EndTagFound      = FALSE;
  ListValid        = TRUE;
  FirstHostBase    = 0;
  FirstBusBase     = 0;
  PreviousHostEnd  = 0;
  PreviousBusEnd   = 0;
  CombinedLength   = 0;
  Status           = EFI_SUCCESS;

  while ((Offset < BAR_RESOURCE_SCAN_LIMIT) &&
         (DescriptorCount < BAR_RESOURCE_DESCRIPTOR_LIMIT))
  {
    if ((ResourceAddress > (MAX_UINTN - Offset)) ||
        !MemoryMapRangeIsReadable (
           &MemoryMap,
           ResourceAddress + Offset,
           sizeof (UINT8),
           NULL
           ))
    {
      LogPrint (
        Logger,
        L"      BAR resource list unreadable at offset 0x%lx; "
        L"using config-space fallback\r\n",
        (UINT64)Offset
        );
      Status    = EFI_COMPROMISED_DATA;
      ListValid = FALSE;
      break;
    }

    Descriptor = (CONST UINT8 *)(ResourceAddress + Offset);
    Tag        = Descriptor[0];
    if (Tag == ACPI_END_TAG_DESCRIPTOR) {
      if ((Offset > (BAR_RESOURCE_SCAN_LIMIT - ACPI_END_TAG_SIZE)) ||
          !MemoryMapRangeIsReadable (
             &MemoryMap,
             ResourceAddress + Offset,
             ACPI_END_TAG_SIZE,
             NULL
             ))
      {
        LogPrint (
          Logger,
          L"      Truncated ACPI End Tag at resource-list offset 0x%lx; "
          L"using config-space fallback\r\n",
          (UINT64)Offset
          );
        Status    = EFI_COMPROMISED_DATA;
        ListValid = FALSE;
        break;
      }

      Checksum = (UINT8)(Checksum + Descriptor[0] + Descriptor[1]);
      if ((Descriptor[1] != 0) && (Checksum != 0)) {
        LogPrint (
          Logger,
          L"      ACPI End Tag checksum is invalid (0x%02x); "
          L"using config-space fallback\r\n",
          Checksum
          );
        Status    = EFI_COMPROMISED_DATA;
        ListValid = FALSE;
      }

      EndTagFound = TRUE;
      break;
    }

    ++DescriptorCount;
    if (Tag != ACPI_ADDRESS_SPACE_DESCRIPTOR) {
      if ((Tag & BIT7) != 0) {
        if ((Offset <= (BAR_RESOURCE_SCAN_LIMIT -
                        ACPI_LARGE_DESCRIPTOR_HEADER_SIZE)) &&
            MemoryMapRangeIsReadable (
              &MemoryMap,
              ResourceAddress + Offset,
              ACPI_LARGE_DESCRIPTOR_HEADER_SIZE,
              NULL
              ))
        {
          DescriptorLength = ReadLe16 (Descriptor + 1);
          LogPrint (
            Logger,
            L"      Unexpected ACPI large descriptor type=0x%02x "
            L"length=0x%04x at offset 0x%lx; stopping and using "
            L"config-space fallback\r\n",
            Tag,
            DescriptorLength,
            (UINT64)Offset
            );
        } else {
          LogPrint (
            Logger,
            L"      Truncated unexpected ACPI large descriptor type=0x%02x "
            L"at offset 0x%lx; using config-space fallback\r\n",
            Tag,
            (UINT64)Offset
            );
        }
      } else {
        LogPrint (
          Logger,
          L"      Unexpected ACPI small descriptor type=0x%02x "
          L"length=0x%02x at offset 0x%lx; stopping and using "
          L"config-space fallback\r\n",
          Tag,
          (UINT32)(Tag & 0x07U),
          (UINT64)Offset
          );
      }

      Status    = EFI_COMPROMISED_DATA;
      ListValid = FALSE;
      break;
    }

    if ((Offset > (BAR_RESOURCE_SCAN_LIMIT -
                   ACPI_LARGE_DESCRIPTOR_HEADER_SIZE)) ||
        !MemoryMapRangeIsReadable (
           &MemoryMap,
           ResourceAddress + Offset,
           ACPI_LARGE_DESCRIPTOR_HEADER_SIZE,
           NULL
           ))
    {
      LogPrint (
        Logger,
        L"      Truncated QWORD resource descriptor header at offset 0x%lx; "
        L"using config-space fallback\r\n",
        (UINT64)Offset
        );
      Status = EFI_COMPROMISED_DATA;
      ListValid = FALSE;
      break;
    }

    DescriptorLength = ReadLe16 (Descriptor + 1);
    if (DescriptorLength !=
        (sizeof (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR) -
         ACPI_LARGE_DESCRIPTOR_HEADER_SIZE))
    {
      LogPrint (
        Logger,
        L"      QWORD resource descriptor has invalid length=0x%04x "
        L"at offset 0x%lx (expected 0x%04x); using config-space fallback\r\n",
        DescriptorLength,
        (UINT64)Offset,
        (UINT32)(sizeof (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR) -
                 ACPI_LARGE_DESCRIPTOR_HEADER_SIZE)
        );
      Status    = EFI_COMPROMISED_DATA;
      ListValid = FALSE;
      break;
    }

    TotalLength = ACPI_LARGE_DESCRIPTOR_HEADER_SIZE + DescriptorLength;
    if ((Offset > (BAR_RESOURCE_SCAN_LIMIT - TotalLength)) ||
        !MemoryMapRangeIsReadable (
           &MemoryMap,
           ResourceAddress + Offset,
           TotalLength,
           NULL
           ))
    {
      LogPrint (
        Logger,
        L"      Truncated QWORD resource descriptor at offset 0x%lx; "
        L"using config-space fallback\r\n",
        (UINT64)Offset
        );
      Status    = EFI_COMPROMISED_DATA;
      ListValid = FALSE;
      break;
    }

    for (ByteIndex = 0; ByteIndex < TotalLength; ++ByteIndex) {
      Checksum = (UINT8)(Checksum + Descriptor[ByteIndex]);
    }

    ResourceType = Descriptor[OFFSET_OF (
                                EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR,
                                ResType
                                )];
    Granularity = ReadLe64 (
                    Descriptor + OFFSET_OF (
                                   EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR,
                                   AddrSpaceGranularity
                                   )
                    );
    HostBase = ReadLe64 (
                 Descriptor + OFFSET_OF (
                                EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR,
                                AddrRangeMin
                                )
                 );
    RangeMaximum = ReadLe64 (
                     Descriptor + OFFSET_OF (
                                    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR,
                                    AddrRangeMax
                                    )
                     );
    Translation = ReadLe64 (
                    Descriptor + OFFSET_OF (
                                   EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR,
                                   AddrTranslationOffset
                                   )
                    );
    Length = ReadLe64 (
               Descriptor + OFFSET_OF (
                              EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR,
                              AddrLen
                              )
               );

    BusBase = 0;
    HostEnd = 0;
    BusEnd  = 0;
    if ((Length == 0) || (HostBase > (MAX_UINT64 - (Length - 1U))) ||
        (HostBase > (MAX_UINT64 - Translation)))
    {
      ListValid = FALSE;
    } else {
      HostEnd = HostBase + Length - 1U;
      BusBase = HostBase + Translation;
      if (BusBase > (MAX_UINT64 - (Length - 1U))) {
        ListValid = FALSE;
      } else {
        BusEnd = BusBase + Length - 1U;
      }
    }

    LogPrint (
      Logger,
      L"      resource[%u]: host-base=0x%016lx bus-base=0x%016lx "
      L"range-max/alignment=0x%016lx translation=0x%016lx size=0x%016lx "
      L"type=%s granularity=%lu\r\n",
      (UINT32)ResourceCount,
      HostBase,
      BusBase,
      RangeMaximum,
      Translation,
      Length,
      (ResourceType == ACPI_ADDRESS_SPACE_TYPE_IO) ? L"I/O" :
      ((ResourceType == ACPI_ADDRESS_SPACE_TYPE_MEM) ? L"memory" : L"unexpected"),
      Granularity
      );

    if ((ResourceType != ACPI_ADDRESS_SPACE_TYPE_MEM) &&
        (ResourceType != ACPI_ADDRESS_SPACE_TYPE_IO))
    {
      LogPrint (
        Logger,
        L"        warning: unsupported resource type 0x%02x\r\n",
        ResourceType
        );
      ListValid = FALSE;
    } else if (((ResourceType == ACPI_ADDRESS_SPACE_TYPE_IO) ? TRUE : FALSE) !=
               ExpectedIo)
    {
      LogPrint (
        Logger,
        L"        warning: descriptor type does not match the PCI config BAR\r\n"
        );
      ListValid = FALSE;
    }

    if (ResourceType == ACPI_ADDRESS_SPACE_TYPE_MEM) {
      if ((Granularity != 32U) && (Granularity != 64U)) {
        LogPrint (
          Logger,
          L"        warning: memory descriptor granularity is neither 32 nor 64\r\n"
          );
        ListValid = FALSE;
      } else if (((Granularity == 64U) ? TRUE : FALSE) != Expected64Bit) {
        LogPrint (
          Logger,
          L"        warning: memory descriptor granularity does not match the "
          L"PCI config BAR width\r\n"
          );
        ListValid = FALSE;
      }
    }

    if (Length == 0) {
      LogPrint (Logger, L"        warning: descriptor length is zero\r\n");
      ListValid = FALSE;
    } else if ((HostBase > (MAX_UINT64 - (Length - 1U))) ||
               (HostBase > (MAX_UINT64 - Translation)) ||
               (BusBase > (MAX_UINT64 - (Length - 1U))))
    {
      LogPrint (Logger, L"        warning: descriptor address arithmetic overflows\r\n");
      ListValid = FALSE;
    } else {
      // EDK II's PCI bus driver uses AddrRangeMax as an alignment mask here,
      // including a minimum 4 KiB mask for small memory BARs.  Some firmware
      // follows the ACPI field name literally and returns the range end.
      RangeMaximumIsLiteralEnd = (RangeMaximum == HostEnd);
      RangeMaximumIsAlignment =
        (RangeMaximum >= (Length - 1U)) &&
        ((RangeMaximum & (RangeMaximum + 1U)) == 0);
      if (!RangeMaximumIsLiteralEnd && !RangeMaximumIsAlignment) {
        LogPrint (
          Logger,
          L"        warning: range-max is neither the range end nor a valid "
          L"alignment mask\r\n"
        );
        ListValid = FALSE;
      } else if (!RangeMaximumIsLiteralEnd &&
                 ((BusBase & RangeMaximum) != 0))
      {
        LogPrint (
          Logger,
          L"        warning: PCI config bus-base is not aligned to the "
          L"descriptor alignment mask\r\n"
          );
        ListValid = FALSE;
      }

      if (ResourceCount == 0) {
        FirstHostBase = HostBase;
        FirstBusBase  = BusBase;
        if (BusBase != ConfigBusBase) {
          LogPrint (
            Logger,
            L"        warning: descriptor bus-base does not match the "
            L"read-only PCI config BAR (0x%016lx)\r\n",
            ConfigBusBase
            );
          ListValid = FALSE;
        }
      } else if ((PreviousHostEnd == MAX_UINT64) ||
                 (PreviousBusEnd == MAX_UINT64) ||
                 (HostBase != (PreviousHostEnd + 1U)) ||
                 (BusBase != (PreviousBusEnd + 1U)))
      {
        LogPrint (
          Logger,
          L"        warning: multiple resource descriptors are not contiguous\r\n"
          );
        ListValid = FALSE;
      }

      PreviousHostEnd = HostEnd;
      PreviousBusEnd  = BusEnd;

      if (CombinedLength > (MAX_UINT64 - Length)) {
        LogPrint (
          Logger,
          L"        warning: combined BAR resource length overflows 64 bits\r\n"
          );
        ListValid = FALSE;
      } else {
        CombinedLength += Length;
      }
    }

    ++ResourceCount;
    Offset += TotalLength;
  }

  if (!EndTagFound) {
    if ((Offset >= BAR_RESOURCE_SCAN_LIMIT) ||
        (DescriptorCount >= BAR_RESOURCE_DESCRIPTOR_LIMIT))
    {
      LogPrint (
        Logger,
        L"      BAR resource list exceeded the scan limit without an End Tag; "
        L"using config-space fallback\r\n"
        );
    }

    Status    = EFI_COMPROMISED_DATA;
    ListValid = FALSE;
  } else if (ResourceCount == 0) {
    if (ConfigBusBase == 0) {
      LogPrint (
        Logger,
        L"      GetBarAttributes logical BAR%u: End Tag only; "
        L"no allocated BAR resource\r\n",
        (UINT32)LogicalBarIndex
        );
    } else {
      LogPrint (
        Logger,
        L"      End Tag reached without a QWORD descriptor for nonzero "
        L"config BAR base 0x%016lx; using config-space fallback\r\n",
        ConfigBusBase
        );
      Status    = EFI_COMPROMISED_DATA;
      ListValid = FALSE;
    }
  }

  if (!ListValid && !EFI_ERROR (Status)) {
    Status = EFI_COMPROMISED_DATA;
  }

  if (ListValid && (ResourceCount != 0)) {
    LogPrint (
      Logger,
      L"      validated BAR resource list: descriptors=%u "
      L"host-base=0x%016lx bus-base=0x%016lx size=0x%016lx\r\n",
      (UINT32)ResourceCount,
      FirstHostBase,
      FirstBusBase,
      CombinedLength
      );
  } else if (EFI_ERROR (Status)) {
    LogPrint (
      Logger,
      L"      fallback: read-only PCI config bus-base=0x%016lx; "
      L"host-base and size unknown (no BAR sizing writes)\r\n",
      ConfigBusBase
      );
  }

  MemoryMapRelease (&MemoryMap);
  return Status;
}

STATIC
EFI_STATUS
PrintBar (
  IN APP_LOGGER               *Logger,
  IN CONST PCI_DEVICE_RECORD  *Record,
  IN UINT8                    ConfigBarIndex,
  IN UINT8                    LogicalBarIndex,
  OUT BOOLEAN                 *ConsumesNextBar
  )
{
  EFI_STATUS    Status;
  EFI_STATUS    GetBarStatus;
  EFI_STATUS    FreeStatus;
  UINT32        RawLow;
  UINT32        RawHigh;
  UINT64        RawBase;
  UINT64        Supports;
  VOID          *Resources;
  BOOLEAN       IsIo;
  BOOLEAN       Is64;
  BOOLEAN       Prefetchable;
  BOOLEAN       ResourcesOwned;
  CONST CHAR16  *Kind;

  if ((Logger == NULL) || (Record == NULL) || (ConsumesNextBar == NULL) ||
      (ConfigBarIndex >= 6) || (LogicalBarIndex >= 6) ||
      EFI_ERROR (Record->ConfigStatus))
  {
    return EFI_INVALID_PARAMETER;
  }

  *ConsumesNextBar = FALSE;
  RawLow = ReadLe32 (
             &Record->Config[PCI_BAR0_OFFSET + (ConfigBarIndex * 4)]
             );
  RawHigh      = 0;
  IsIo         = (RawLow & BIT0) != 0;
  Is64         = FALSE;
  Prefetchable = FALSE;

  if (IsIo) {
    RawBase = (UINT64)(RawLow & 0xFFFFFFFCU);
    Kind    = L"I/O";
  } else {
    Is64         = (((RawLow >> 1) & 0x3) == 0x2);
    Prefetchable = (RawLow & BIT3) != 0;
    if (Is64 && (ConfigBarIndex < 5)) {
      RawHigh = ReadLe32 (
                  &Record->Config[
                    PCI_BAR0_OFFSET + ((ConfigBarIndex + 1U) * 4)
                    ]
                  );
      RawBase          = ((UINT64)RawHigh << 32) | (RawLow & 0xFFFFFFF0U);
      *ConsumesNextBar = TRUE;
      Kind             = L"Memory64";
    } else {
      RawBase = (UINT64)(RawLow & 0xFFFFFFF0U);
      Kind    = Is64 ? L"Memory64-truncated" : L"Memory32";
    }
  }

  Supports  = 0;
  Resources = NULL;
  ResourcesOwned = FALSE;
  if (Record->PciIo->GetBarAttributes == NULL) {
    GetBarStatus = EFI_UNSUPPORTED;
  } else {
    // UEFI BAR indices are logical resource numbers, not PCI config dword
    // slots.  A 64-bit BAR consumes two config slots but one logical index.
    GetBarStatus = Record->PciIo->GetBarAttributes (
                                     Record->PciIo,
                                     LogicalBarIndex,
                                     &Supports,
                                     &Resources
                                     );
  }

  Status = GetBarStatus;
  ResourcesOwned = !EFI_ERROR (GetBarStatus);

  LogPrint (
    Logger,
    L"    BAR%u (EFI logical BAR%u): %s raw=0x%08x",
    (UINT32)ConfigBarIndex,
    (UINT32)LogicalBarIndex,
    Kind,
    RawLow
    );
  if (*ConsumesNextBar) {
    LogPrint (Logger, L" high=0x%08x", RawHigh);
  }

  LogPrint (
    Logger,
    L" bus-base=0x%016lx prefetchable=%s\r\n",
    RawBase,
    Prefetchable ? L"yes" : L"no"
    );

  if (!EFI_ERROR (Status) && (Resources != NULL)) {
    Status = PrintBarResourceList (
               Logger,
               Resources,
               Supports,
               LogicalBarIndex,
               IsIo,
               Is64,
               RawBase
               );
  } else {
    if (!EFI_ERROR (Status)) {
      Status = EFI_COMPROMISED_DATA;
      LogPrint (
        Logger,
        L"      GetBarAttributes logical BAR%u returned success with a null "
        L"resource list\r\n",
        (UINT32)LogicalBarIndex
        );
    } else {
      LogPrint (
        Logger,
        L"      GetBarAttributes logical BAR%u: 0x%016lx (%s)\r\n",
        (UINT32)LogicalBarIndex,
        (UINT64)Status,
        EfiStatusName (Status)
        );
      if (Resources != NULL) {
        LogPrint (
          Logger,
          L"      warning: firmware returned an error with a non-null, "
          L"untrusted resource pointer %p; it will not be dereferenced or freed\r\n",
          Resources
          );
      }
    }

    LogPrint (
      Logger,
      L"      fallback: read-only PCI config bus-base=0x%016lx; "
      L"host-base and size unknown (no BAR sizing writes)\r\n",
      RawBase
      );
  }

  if (ResourcesOwned && (Resources != NULL)) {
    FreeStatus = gBS->FreePool (Resources);
    if (EFI_ERROR (FreeStatus)) {
      LogStatus (Logger, L"FreePool(BAR resource descriptor list)", FreeStatus);
      if (!EFI_ERROR (Status)) {
        Status = FreeStatus;
      }
    }
  }

  return Status;
}

STATIC
VOID
PrintDisplay (
  IN APP_LOGGER               *Logger,
  IN CONST PCI_DEVICE_RECORD  *Record
  )
{
  UINT16   VendorId;
  UINT16   DeviceId;
  UINT16   Command;
  UINT16   SubsystemVendorId;
  UINT16   SubsystemId;
  UINT8    HeaderType;
  UINT8    BarIndex;
  UINT8    LogicalBarIndex;
  BOOLEAN  ConsumesNext;

  VendorId  = ReadLe16 (&Record->Config[PCI_VENDOR_ID_OFFSET]);
  DeviceId  = ReadLe16 (&Record->Config[PCI_DEVICE_ID_OFFSET]);
  Command   = ReadLe16 (&Record->Config[PCI_COMMAND_OFFSET]);
  HeaderType = Record->Config[PCI_HEADER_TYPE_OFFSET] & PCI_HEADER_TYPE_MASK;

  if (RecordLocationIsValid (Record)) {
    LogPrint (
      Logger,
      L"Display %04x:%02x:%02x.%x\r\n",
      (UINT32)Record->Segment,
      (UINT32)Record->Bus,
      (UINT32)Record->Device,
      (UINT32)Record->Function
      );
  } else {
    LogPrint (
      Logger,
      L"Display <location unavailable: 0x%016lx (%s)>\r\n",
      (UINT64)Record->LocationStatus,
      EfiStatusName (Record->LocationStatus)
      );
  }
  LogPrint (
    Logger,
    L"  vendor:device=%04x:%04x class=%02x:%02x:%02x header-type=%02x\r\n",
    VendorId,
    DeviceId,
    Record->Config[PCI_BASE_CLASS_OFFSET],
    Record->Config[PCI_SUBCLASS_OFFSET],
    Record->Config[PCI_PROGIF_OFFSET],
    HeaderType
    );

  if (HeaderType == PCI_HEADER_TYPE_DEVICE) {
    SubsystemVendorId = ReadLe16 (&Record->Config[PCI_SUBSYSTEM_VENDOR_ID_OFFSET]);
    SubsystemId       = ReadLe16 (&Record->Config[PCI_SUBSYSTEM_ID_OFFSET]);
    LogPrint (
      Logger,
      L"  subsystem=%04x:%04x\r\n",
      SubsystemVendorId,
      SubsystemId
      );
  } else {
    LogPrint (Logger, L"  subsystem=<not present in non-Type-0 header>\r\n");
  }

  LogPrint (
    Logger,
    L"  command=0x%04x [io=%s memory=%s bus-master=%s palette-snoop=%s]\r\n",
    Command,
    (Command & PCI_COMMAND_IO) != 0 ? L"on" : L"off",
    (Command & PCI_COMMAND_MEMORY) != 0 ? L"on" : L"off",
    (Command & PCI_COMMAND_BUS_MASTER) != 0 ? L"on" : L"off",
    (Command & PCI_COMMAND_VGA_PALETTE_SNOOP) != 0 ? L"on" : L"off"
    );

  if (EFI_ERROR (Record->SupportedAttributesStatus)) {
    LogPrint (
      Logger,
      L"  supported EFI PCI attributes: unknown, 0x%016lx (%s)\r\n",
      (UINT64)Record->SupportedAttributesStatus,
      EfiStatusName (Record->SupportedAttributesStatus)
      );
    LogPrint (Logger, L"  legacy VGA I/O support: unknown\r\n");
    LogPrint (Logger, L"  16-bit VGA I/O support: unknown\r\n");
    LogPrint (Logger, L"  legacy VGA memory support: unknown\r\n");
  } else {
    LogPrint (
      Logger,
      L"  supported EFI PCI attributes=0x%016lx\r\n",
      Record->SupportedAttributes
      );
    LogPrint (
      Logger,
      L"  legacy VGA I/O support=%s\r\n",
      (Record->SupportedAttributes & EFI_PCI_IO_ATTRIBUTE_VGA_IO) != 0 ?
      L"yes" : L"no"
      );
    LogPrint (
      Logger,
      L"  16-bit VGA I/O support=%s\r\n",
      (Record->SupportedAttributes & EFI_PCI_IO_ATTRIBUTE_VGA_IO_16) != 0 ?
      L"yes" : L"no"
      );
    LogPrint (
      Logger,
      L"  legacy VGA memory support=%s\r\n",
      (Record->SupportedAttributes & EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY) != 0 ?
      L"yes" : L"no"
      );
  }

  if (EFI_ERROR (Record->CurrentAttributesStatus)) {
    LogPrint (
      Logger,
      L"  enabled EFI PCI attributes: unknown, 0x%016lx (%s)\r\n",
      (UINT64)Record->CurrentAttributesStatus,
      EfiStatusName (Record->CurrentAttributesStatus)
      );
  } else {
    LogPrint (
      Logger,
      L"  enabled EFI PCI attributes=0x%016lx\r\n",
      Record->CurrentAttributes
      );
    LogPrint (
      Logger,
      L"  enabled legacy VGA attributes: I/O=%s I/O-16=%s memory=%s\r\n",
      (Record->CurrentAttributes & EFI_PCI_IO_ATTRIBUTE_VGA_IO) != 0 ?
      L"yes" : L"no",
      (Record->CurrentAttributes & EFI_PCI_IO_ATTRIBUTE_VGA_IO_16) != 0 ?
      L"yes" : L"no",
      (Record->CurrentAttributes & EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY) != 0 ?
      L"yes" : L"no"
      );
  }

  LogPrint (
    Logger,
    L"  option ROM exposed=%s image=%p size=0x%016lx\r\n",
    ((Record->PciIo->RomImage != NULL) && (Record->PciIo->RomSize != 0)) ?
    L"yes" : L"no",
    Record->PciIo->RomImage,
    Record->PciIo->RomSize
    );

  LogicalBarIndex = 0;
  for (BarIndex = 0; BarIndex < 6; ++BarIndex, ++LogicalBarIndex) {
    ConsumesNext = FALSE;
    PrintBar (
      Logger,
      Record,
      BarIndex,
      LogicalBarIndex,
      &ConsumesNext
      );
    if (ConsumesNext) {
      ++BarIndex;
    }
  }
}

EFI_STATUS
PciPrintDisplayDevices (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory
  )
{
  UINTN  Index;
  UINTN  DisplayCount;

  if ((Logger == NULL) || (Inventory == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  LogPrint (Logger, L"\r\n=== Display-class PCI devices ===\r\n");
  DisplayCount = 0;
  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Record;

    Record = &Inventory->Devices[Index];
    if (!RecordIsDisplay (Record)) {
      continue;
    }

    ++DisplayCount;
    PrintDisplay (Logger, Record);
  }

  LogPrint (Logger, L"Display-class device count: %u\r\n", DisplayCount);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
PrintRootBridge (
  IN APP_LOGGER               *Logger,
  IN CONST PCI_DEVICE_RECORD  *Display
  )
{
  if (EFI_ERROR (Display->RootBridgeStatus) ||
      (Display->RootBridgeIo == NULL))
  {
    LogPrint (
      Logger,
      L"Root bridge association unavailable: 0x%016lx (%s)\r\n",
      (UINT64)Display->RootBridgeStatus,
      EfiStatusName (Display->RootBridgeStatus)
      );
    return EFI_ERROR (Display->RootBridgeStatus) ?
           Display->RootBridgeStatus : EFI_COMPROMISED_DATA;
  }

  LogPrint (
    Logger,
    L"Root bridge: handle=%p parent-host=%p segment=%04x "
    L"device-path-prefix-bytes=%u\r\n",
    Display->RootBridgeHandle,
    Display->RootBridgeIo->ParentHandle,
    Display->RootBridgeIo->SegmentNumber,
    Display->RootBridgePrefixSize
    );
  if (EFI_ERROR (Display->RootAttributesStatus)) {
    LogPrint (
      Logger,
      L"  root supported/current attributes unavailable: 0x%016lx (%s)\r\n",
      (UINT64)Display->RootAttributesStatus,
      EfiStatusName (Display->RootAttributesStatus)
      );
    return Display->RootAttributesStatus;
  }

  LogPrint (
    Logger,
    L"  root supported attributes=0x%016lx; enabled=0x%016lx\r\n",
    Display->RootSupportedAttributes,
    Display->RootCurrentAttributes
    );
  LogPrint (
    Logger,
    L"  root enabled legacy VGA attributes: I/O=%s I/O-16=%s "
    L"memory=%s palette=%s palette-16=%s\r\n",
    (Display->RootCurrentAttributes & EFI_PCI_ATTRIBUTE_VGA_IO) != 0 ?
    L"yes" : L"no",
    (Display->RootCurrentAttributes & EFI_PCI_ATTRIBUTE_VGA_IO_16) != 0 ?
    L"yes" : L"no",
    (Display->RootCurrentAttributes & EFI_PCI_ATTRIBUTE_VGA_MEMORY) != 0 ?
    L"yes" : L"no",
    (Display->RootCurrentAttributes & EFI_PCI_ATTRIBUTE_VGA_PALETTE_IO) != 0 ?
    L"yes" : L"no",
    (Display->RootCurrentAttributes & EFI_PCI_ATTRIBUTE_VGA_PALETTE_IO_16) != 0 ?
    L"yes" : L"no"
    );
  return EFI_SUCCESS;
}

STATIC
VOID
PrintBridge (
  IN APP_LOGGER               *Logger,
  IN CONST PCI_DEVICE_RECORD  *Bridge,
  IN UINTN                    Depth
  )
{
  UINT16  Command;
  UINT16  BridgeControl;

  Command       = ReadLe16 (&Bridge->Config[PCI_COMMAND_OFFSET]);
  BridgeControl = ReadLe16 (&Bridge->Config[PCI_BRIDGE_CONTROL_OFFSET]);
  LogPrint (
    Logger,
    L"  [%u] %04x:%02x:%02x.%x buses primary=%02x secondary=%02x "
    L"subordinate=%02x\r\n",
    Depth,
    (UINT32)Bridge->Segment,
    (UINT32)Bridge->Bus,
    (UINT32)Bridge->Device,
    (UINT32)Bridge->Function,
    Bridge->Config[PCI_PRIMARY_BUS_OFFSET],
    Bridge->Config[PCI_SECONDARY_BUS_OFFSET],
    Bridge->Config[PCI_SUBORDINATE_BUS_OFFSET]
    );
  LogPrint (
    Logger,
    L"      command=0x%04x [io=%s memory=%s bus-master=%s palette-snoop=%s]\r\n",
    Command,
    (Command & PCI_COMMAND_IO) != 0 ? L"on" : L"off",
    (Command & PCI_COMMAND_MEMORY) != 0 ? L"on" : L"off",
    (Command & PCI_COMMAND_BUS_MASTER) != 0 ? L"on" : L"off",
    (Command & PCI_COMMAND_VGA_PALETTE_SNOOP) != 0 ? L"on" : L"off"
    );
  LogPrint (
    Logger,
    L"      bridge-control=0x%04x [VGA-route=%s VGA-16=%s]\r\n",
    BridgeControl,
    (BridgeControl & PCI_BRIDGE_CONTROL_VGA) != 0 ? L"on" : L"off",
    (BridgeControl & PCI_BRIDGE_CONTROL_VGA_16) != 0 ? L"on" : L"off"
    );
  if (!EFI_ERROR (Bridge->SupportedAttributesStatus)) {
    LogPrint (
      Logger,
      L"      supported EFI PCI attributes=0x%016lx\r\n",
      Bridge->SupportedAttributes
      );
  } else {
    LogPrint (
      Logger,
      L"      supported EFI PCI attributes unavailable: 0x%016lx (%s)\r\n",
      (UINT64)Bridge->SupportedAttributesStatus,
      EfiStatusName (Bridge->SupportedAttributesStatus)
      );
  }

  if (!EFI_ERROR (Bridge->CurrentAttributesStatus)) {
    LogPrint (
      Logger,
      L"      enabled EFI PCI attributes=0x%016lx\r\n",
      Bridge->CurrentAttributes
      );
  } else {
    LogPrint (
      Logger,
      L"      enabled EFI PCI attributes unavailable: 0x%016lx (%s)\r\n",
      (UINT64)Bridge->CurrentAttributesStatus,
      EfiStatusName (Bridge->CurrentAttributesStatus)
      );
  }
}

STATIC
BOOLEAN
IsStrictDevicePathPrefix (
  IN CONST PCI_DEVICE_RECORD  *Candidate,
  IN CONST PCI_DEVICE_RECORD  *Selected
  )
{
  UINTN  PrefixSize;

  if ((Candidate->DevicePath == NULL) || (Selected->DevicePath == NULL) ||
      (Candidate->DevicePathSize <= END_DEVICE_PATH_LENGTH) ||
      (Selected->DevicePathSize <= Candidate->DevicePathSize))
  {
    return FALSE;
  }

  PrefixSize = Candidate->DevicePathSize - END_DEVICE_PATH_LENGTH;
  return CompareMem (Candidate->DevicePath, Selected->DevicePath, PrefixSize) == 0;
}

STATIC
VOID
SortPathByDevicePathSize (
  IN CONST PCI_INVENTORY  *Inventory,
  IN OUT UINTN            *Indices,
  IN UINTN                Count
  )
{
  UINTN  Outer;
  UINTN  Inner;
  UINTN  Value;

  for (Outer = 1; Outer < Count; ++Outer) {
    Value = Indices[Outer];
    Inner = Outer;
    while ((Inner > 0) &&
           (Inventory->Devices[Indices[Inner - 1]].DevicePathSize >
            Inventory->Devices[Value].DevicePathSize))
    {
      Indices[Inner] = Indices[Inner - 1];
      --Inner;
    }

    Indices[Inner] = Value;
  }
}

STATIC
EFI_STATUS
BuildDevicePathBridgePath (
  IN  CONST PCI_INVENTORY      *Inventory,
  IN  CONST PCI_DEVICE_RECORD  *Selected,
  OUT UINTN                    *Path,
  OUT UINTN                    *PathCount
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Node;
  UINTN                           Index;
  UINTN                           Offset;
  UINTN                           NodeSize;
  UINTN                           PciNodeCount;

  *PathCount = 0;
  if (Selected->DevicePath == NULL) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Candidate;

    Candidate = &Inventory->Devices[Index];
    if (RecordLocationIsValid (Candidate) && RecordIsP2pBridge (Candidate) &&
        (Candidate->Segment == Selected->Segment) &&
        IsStrictDevicePathPrefix (Candidate, Selected))
    {
      Path[(*PathCount)++] = Index;
    }
  }

  SortPathByDevicePathSize (Inventory, Path, *PathCount);

  // A device directly below a PCI root bridge legitimately has no upstream
  // PCI-to-PCI bridge.  Count the selected handle's PCI device-path nodes so
  // that this valid empty path is distinguishable from missing bridge handles.
  PciNodeCount = 0;
  Offset       = 0;
  while (Offset + sizeof (EFI_DEVICE_PATH_PROTOCOL) <= Selected->DevicePathSize) {
    Node     = (CONST EFI_DEVICE_PATH_PROTOCOL *)
               ((CONST UINT8 *)Selected->DevicePath + Offset);
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
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
BuildBusBridgePath (
  IN  CONST PCI_INVENTORY      *Inventory,
  IN  CONST PCI_DEVICE_RECORD  *Selected,
  OUT UINTN                    *Path,
  OUT UINTN                    *PathCount
  )
{
  UINTN  CurrentBus;
  UINTN  CandidateIndex;
  UINTN  CandidateCount;
  UINTN  Index;
  UINTN  ReverseIndex;

  *PathCount = 0;
  CurrentBus = Selected->Bus;
  while ((CurrentBus != 0) && (*PathCount < Inventory->Count)) {
    CandidateIndex = 0;
    CandidateCount = 0;
    for (Index = 0; Index < Inventory->Count; ++Index) {
      CONST PCI_DEVICE_RECORD  *Bridge;

      Bridge = &Inventory->Devices[Index];
      if (!RecordLocationIsValid (Bridge) || !RecordIsP2pBridge (Bridge) ||
          (Bridge->Segment != Selected->Segment))
      {
        continue;
      }

      if ((Bridge->Config[PCI_SECONDARY_BUS_OFFSET] == CurrentBus) &&
          (Bridge->Config[PCI_PRIMARY_BUS_OFFSET] == Bridge->Bus))
      {
        CandidateIndex = Index;
        ++CandidateCount;
      }
    }

    if (CandidateCount == 0) {
      return EFI_NOT_FOUND;
    }

    if (CandidateCount != 1) {
      return EFI_NO_MAPPING;
    }

    if (Inventory->Devices[CandidateIndex].Bus == CurrentBus) {
      return EFI_COMPROMISED_DATA;
    }

    for (ReverseIndex = 0; ReverseIndex < *PathCount; ++ReverseIndex) {
      if (Path[ReverseIndex] == CandidateIndex) {
        return EFI_COMPROMISED_DATA;
      }
    }

    Path[(*PathCount)++] = CandidateIndex;
    CurrentBus = Inventory->Devices[CandidateIndex].Bus;
  }

  for (Index = 0; Index < (*PathCount / 2); ++Index) {
    UINTN  Temporary;

    ReverseIndex       = *PathCount - 1 - Index;
    Temporary          = Path[Index];
    Path[Index]        = Path[ReverseIndex];
    Path[ReverseIndex] = Temporary;
  }

  return (*PathCount == 0) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

STATIC
EFI_STATUS
CrossCheckBridgePath (
  IN CONST PCI_INVENTORY      *Inventory,
  IN CONST PCI_DEVICE_RECORD  *Selected,
  IN CONST UINTN              *Path,
  IN UINTN                    PathCount
  )
{
  UINTN  Index;
  UINTN  ChildBus;

  if (PathCount == 0) {
    return EFI_SUCCESS;
  }

  for (Index = 0; Index < PathCount; ++Index) {
    CONST PCI_DEVICE_RECORD  *Bridge;

    Bridge = &Inventory->Devices[Path[Index]];
    ChildBus = (Index + 1 < PathCount) ?
               Inventory->Devices[Path[Index + 1]].Bus : Selected->Bus;
    if ((Bridge->Config[PCI_PRIMARY_BUS_OFFSET] != Bridge->Bus) ||
        (Bridge->Config[PCI_SECONDARY_BUS_OFFSET] != ChildBus) ||
        (ChildBus > Bridge->Config[PCI_SUBORDINATE_BUS_OFFSET]))
    {
      return EFI_NO_MAPPING;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
PciProbeDevicePathBridgePathStatus (
  IN  APP_LOGGER                    *Logger,
  IN  CONST PCI_INVENTORY           *Inventory,
  IN  CONST PCI_DEVICE_RECORD       *Selected,
  OUT BOOLEAN                       *PathDiscovered,
  OUT BOOLEAN                       *PathValidated
  )
{
  UINTN       *Path;
  UINTN       PathCount;
  EFI_STATUS  Status;

  if ((Logger == NULL) || (Inventory == NULL) ||
      (Inventory->Devices == NULL) || (Selected == NULL) ||
      (PathDiscovered == NULL) || (PathValidated == NULL) ||
      !RecordLocationIsValid (Selected) || !RecordIsDisplay (Selected))
  {
    return EFI_INVALID_PARAMETER;
  }

  *PathDiscovered = FALSE;
  *PathValidated = FALSE;
  if (EFI_ERROR (Selected->RootBridgeStatus) ||
      (Selected->RootBridgeHandle == NULL) ||
      (Selected->RootBridgeIo == NULL) ||
      (Selected->RootBridgeIo->ParentHandle == NULL))
  {
    return EFI_NO_MAPPING;
  }
  if (Inventory->Count > (MAX_UINTN / sizeof (*Path))) {
    return EFI_OUT_OF_RESOURCES;
  }

  Path = AllocateZeroPool (Inventory->Count * sizeof (*Path));
  if (Path == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  PathCount = 0;
  Status = BuildDevicePathBridgePath (
             Inventory,
             Selected,
             Path,
             &PathCount
             );
  if (!EFI_ERROR (Status)) {
    *PathDiscovered = TRUE;
    Status = CrossCheckBridgePath (
               Inventory,
               Selected,
               Path,
               PathCount
               );
    if (!EFI_ERROR (Status)) {
      *PathValidated = TRUE;
    }
  }

  LogPrint (
    Logger,
    L"Probe path %04x:%02x:%02x.%x: discovered=%s validation=%s "
    L"bridge-count=%u status=0x%016lx (%s)\r\n",
    (UINT32)Selected->Segment,
    (UINT32)Selected->Bus,
    (UINT32)Selected->Device,
    (UINT32)Selected->Function,
    *PathDiscovered ? L"yes" : L"no",
    *PathValidated ? L"passed" : L"failed",
    (UINT32)PathCount,
    (UINT64)Status,
    EfiStatusName (Status)
    );
  FreePool (Path);
  return Status;
}

EFI_STATUS
PciBuildValidatedDevicePathBridgePath (
  IN  APP_LOGGER                    *Logger,
  IN  CONST PCI_INVENTORY           *Inventory,
  IN  CONST PCI_DEVICE_RECORD       *Selected,
  OUT PCI_DEVICE_RECORD             *Bridges OPTIONAL,
  IN  UINTN                         BridgeCapacity,
  OUT UINTN                         *BridgeCount
  )
{
  UINTN       *Path;
  UINTN       PathCount;
  UINTN       Index;
  EFI_STATUS  Status;

  if ((Logger == NULL) || (Inventory == NULL) ||
      (Inventory->Devices == NULL) || (Selected == NULL) ||
      (BridgeCount == NULL) ||
      ((BridgeCapacity != 0) && (Bridges == NULL)) ||
      !RecordLocationIsValid (Selected) || !RecordIsDisplay (Selected))
  {
    return EFI_INVALID_PARAMETER;
  }

  *BridgeCount = 0;
  if (EFI_ERROR (Selected->RootBridgeStatus) ||
      (Selected->RootBridgeHandle == NULL) ||
      (Selected->RootBridgeIo == NULL) ||
      (Selected->RootBridgeIo->ParentHandle == NULL))
  {
    LogPrint (
      Logger,
      L"Device-path ancestry rejected for %04x:%02x:%02x.%x: "
      L"root/parent-host association is incomplete\r\n",
      (UINT32)Selected->Segment,
      (UINT32)Selected->Bus,
      (UINT32)Selected->Device,
      (UINT32)Selected->Function
      );
    return EFI_NO_MAPPING;
  }

  if (Inventory->Count > (MAX_UINTN / sizeof (*Path))) {
    return EFI_OUT_OF_RESOURCES;
  }

  Path = AllocateZeroPool (Inventory->Count * sizeof (*Path));
  if (Path == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  PathCount = 0;
  Status = BuildDevicePathBridgePath (
             Inventory,
             Selected,
             Path,
             &PathCount
             );
  if (!EFI_ERROR (Status)) {
    Status = CrossCheckBridgePath (
               Inventory,
               Selected,
               Path,
               PathCount
               );
  }

  if (!EFI_ERROR (Status) && (PathCount > BridgeCapacity)) {
    Status = EFI_BAD_BUFFER_SIZE;
  }

  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < PathCount; ++Index) {
      CopyMem (
        &Bridges[Index],
        &Inventory->Devices[Path[Index]],
        sizeof (Bridges[Index])
        );
    }

    *BridgeCount = PathCount;
    LogPrint (
      Logger,
      L"Operating path %04x:%02x:%02x.%x: source=device-path ancestry "
      L"bridge-count=%u bus-register-cross-check=passed\r\n",
      (UINT32)Selected->Segment,
      (UINT32)Selected->Bus,
      (UINT32)Selected->Device,
      (UINT32)Selected->Function,
      (UINT32)PathCount
      );
  } else {
    LogPrint (
      Logger,
      L"Operating path %04x:%02x:%02x.%x rejected: strict device-path "
      L"ancestry/bus cross-check status=0x%016lx (%s)\r\n",
      (UINT32)Selected->Segment,
      (UINT32)Selected->Bus,
      (UINT32)Selected->Device,
      (UINT32)Selected->Function,
      (UINT64)Status,
      EfiStatusName (Status)
      );
  }

  FreePool (Path);
  return Status;
}

STATIC
EFI_STATUS
PrintBridgePathForRecord (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory,
  IN CONST PCI_DEVICE_RECORD  *Display
  )
{
  UINTN                    *Path;
  UINTN                    PathCount;
  UINTN                    Index;
  EFI_STATUS               Status;
  EFI_STATUS               CrossCheckStatus;
  EFI_STATUS               RootStatus;
  BOOLEAN                  UsedDevicePath;

  if ((Logger == NULL) || (Inventory == NULL) ||
      !RecordLocationIsValid (Display) || !RecordIsDisplay (Display))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Inventory->Count > (MAX_UINTN / sizeof (*Path))) {
    return EFI_OUT_OF_RESOURCES;
  }

  Path = AllocateZeroPool (Inventory->Count * sizeof (*Path));
  if (Path == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  PathCount      = 0;
  UsedDevicePath = TRUE;
  RootStatus = PrintRootBridge (Logger, Display);
  Status = BuildDevicePathBridgePath (Inventory, Display, Path, &PathCount);
  if (EFI_ERROR (Status)) {
    UsedDevicePath = FALSE;
    LogPrint (
      Logger,
      L"Device-path ancestry unavailable (0x%016lx, %s); "
      L"trying exact secondary-bus fallback\r\n",
      (UINT64)Status,
      EfiStatusName (Status)
      );
    Status = BuildBusBridgePath (Inventory, Display, Path, &PathCount);
  }

  if (EFI_ERROR (Status)) {
    LogPrint (
      Logger,
      L"Could not derive an unambiguous bridge path: 0x%016lx (%s)\r\n",
      (UINT64)Status,
      EfiStatusName (Status)
      );
    FreePool (Path);
    return Status;
  }

  CrossCheckStatus = CrossCheckBridgePath (Inventory, Display, Path, PathCount);
  LogPrint (
    Logger,
    L"Path source: %s; bridge count=%u; bus-register cross-check=%s\r\n",
    UsedDevicePath ? L"device-path ancestry" : L"exact secondary-bus fallback",
    PathCount,
    EFI_ERROR (CrossCheckStatus) ? L"FAILED/ambiguous" : L"passed"
    );

  for (Index = 0; Index < PathCount; ++Index) {
    PrintBridge (Logger, &Inventory->Devices[Path[Index]], Index);
  }

  FreePool (Path);
  if (EFI_ERROR (CrossCheckStatus)) {
    return CrossCheckStatus;
  }

  if (!UsedDevicePath) {
    LogPrint (
      Logger,
      L"Exact-bus fallback is diagnostic only and is insufficient for the "
      L"VGA-routing hardware gate\r\n"
      );
    return EFI_NO_MAPPING;
  }

  return RootStatus;
}

EFI_STATUS
PciPrintAllDisplayBridgePaths (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory
  )
{
  EFI_STATUS  OverallStatus;
  EFI_STATUS  Status;
  UINTN       Index;
  UINTN       DisplayCount;
  UINTN       OtherIndex;

  if ((Logger == NULL) || (Inventory == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  LogPrint (Logger, L"\r\n=== All display-class upstream paths ===\r\n");
  OverallStatus = EFI_SUCCESS;
  DisplayCount  = 0;
  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Display;

    Display = &Inventory->Devices[Index];
    if (!RecordIsDisplay (Display)) {
      continue;
    }

    ++DisplayCount;
    if (RecordLocationIsValid (Display)) {
      LogPrint (
        Logger,
        L"\r\nDisplay path %u: %04x:%02x:%02x.%x vendor:device=%04x:%04x\r\n",
        DisplayCount,
        (UINT32)Display->Segment,
        (UINT32)Display->Bus,
        (UINT32)Display->Device,
        (UINT32)Display->Function,
        ReadLe16 (&Display->Config[PCI_VENDOR_ID_OFFSET]),
        ReadLe16 (&Display->Config[PCI_DEVICE_ID_OFFSET])
        );
    } else {
      LogPrint (
        Logger,
        L"\r\nDisplay path %u: location unavailable, 0x%016lx (%s)\r\n",
        DisplayCount,
        (UINT64)Display->LocationStatus,
        EfiStatusName (Display->LocationStatus)
        );
    }

    Status = PrintBridgePathForRecord (Logger, Inventory, Display);
    if (EFI_ERROR (Status)) {
      LogPrint (
        Logger,
        L"Display path %u is incomplete/invalid: 0x%016lx (%s)\r\n",
        DisplayCount,
        (UINT64)Status,
        EfiStatusName (Status)
        );
      if (!EFI_ERROR (OverallStatus)) {
        OverallStatus = Status;
      }
    }
  }

  if (DisplayCount == 0) {
    LogPrint (Logger, L"No display-class devices were available for path enumeration\r\n");
    return EFI_NOT_FOUND;
  }

  LogPrint (Logger, L"\r\nDisplay root-bridge relationships:\r\n");
  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Left;

    Left = &Inventory->Devices[Index];
    if (!RecordIsDisplay (Left) || !RecordLocationIsValid (Left)) {
      continue;
    }

    for (OtherIndex = Index + 1; OtherIndex < Inventory->Count; ++OtherIndex) {
      CONST PCI_DEVICE_RECORD  *Right;

      Right = &Inventory->Devices[OtherIndex];
      if (!RecordIsDisplay (Right) || !RecordLocationIsValid (Right)) {
        continue;
      }

      LogPrint (
        Logger,
        L"  %04x:%02x:%02x.%x vs %04x:%02x:%02x.%x: "
        L"same-root=%s same-parent-host=%s\r\n",
        (UINT32)Left->Segment,
        (UINT32)Left->Bus,
        (UINT32)Left->Device,
        (UINT32)Left->Function,
        (UINT32)Right->Segment,
        (UINT32)Right->Bus,
        (UINT32)Right->Device,
        (UINT32)Right->Function,
        (!EFI_ERROR (Left->RootBridgeStatus) &&
         !EFI_ERROR (Right->RootBridgeStatus)) ?
        ((Left->RootBridgeHandle == Right->RootBridgeHandle) ? L"yes" : L"no") :
        L"unknown",
        (!EFI_ERROR (Left->RootBridgeStatus) &&
         !EFI_ERROR (Right->RootBridgeStatus) &&
         (Left->RootBridgeIo != NULL) && (Right->RootBridgeIo != NULL)) ?
        ((Left->RootBridgeIo->ParentHandle == Right->RootBridgeIo->ParentHandle) ?
         L"yes" : L"no") : L"unknown"
        );
    }
  }

  LogPrint (
    Logger,
    L"All-display path enumeration attempted for %u device(s)\r\n",
    DisplayCount
    );
  return OverallStatus;
}

EFI_STATUS
PciPrintSelectedBridgePath (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory,
  IN UINTN                Segment,
  IN UINTN                Bus,
  IN UINTN                Device,
  IN UINTN                Function
  )
{
  CONST PCI_DEVICE_RECORD  *Selected;

  if ((Logger == NULL) || (Inventory == NULL) ||
      (Segment > MAX_UINT16) || (Bus > MAX_UINT8) ||
      (Device > 31) || (Function > 7))
  {
    return EFI_INVALID_PARAMETER;
  }

  LogPrint (Logger, L"\r\n=== Selected VGA upstream path ===\r\n");
  Selected = FindDevice (Inventory, Segment, Bus, Device, Function);
  if (Selected == NULL) {
    LogPrint (
      Logger,
      L"Configured selected_vga %04x:%02x:%02x.%x was not found\r\n",
      (UINT32)Segment,
      (UINT32)Bus,
      (UINT32)Device,
      (UINT32)Function
      );
    return EFI_NOT_FOUND;
  }

  LogPrint (
    Logger,
    L"Selected VGA: %04x:%02x:%02x.%x class=%02x:%02x:%02x\r\n",
    (UINT32)Selected->Segment,
    (UINT32)Selected->Bus,
    (UINT32)Selected->Device,
    (UINT32)Selected->Function,
    Selected->Config[PCI_BASE_CLASS_OFFSET],
    Selected->Config[PCI_SUBCLASS_OFFSET],
    Selected->Config[PCI_PROGIF_OFFSET]
    );
  if (!RecordIsDisplay (Selected)) {
    LogPrint (Logger, L"Configured selected_vga is not a display-class device\r\n");
    return EFI_INVALID_PARAMETER;
  }

  return PrintBridgePathForRecord (Logger, Inventory, Selected);
}

EFI_STATUS
PciVerifyReadOnlySnapshot (
  IN APP_LOGGER           *Logger,
  IN CONST PCI_INVENTORY  *Inventory
  )
{
  EFI_STATUS  OverallStatus;
  EFI_STATUS  Status;
  UINTN       Index;
  UINT8       Current[PCI_PROBE_CONFIG_BYTES];
  UINT16      Before;
  UINT16      After;
  UINTN       BarOffset;
  UINT64      CurrentAttributes;
  UINT64      RootSupportedAttributes;
  UINT64      RootCurrentAttributes;

  if ((Logger == NULL) || (Inventory == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  LogPrint (Logger, L"\r\n=== Read-only PCI state verification ===\r\n");
  OverallStatus = EFI_SUCCESS;
  for (Index = 0; Index < Inventory->Count; ++Index) {
    CONST PCI_DEVICE_RECORD  *Record;

    Record = &Inventory->Devices[Index];
    if (EFI_ERROR (Record->ConfigStatus) || !RecordLocationIsValid (Record) ||
        (Record->PciIo == NULL) || (Record->PciIo->Pci.Read == NULL))
    {
      continue;
    }

    ZeroMem (Current, sizeof (Current));
    Status = Record->PciIo->Pci.Read (
                                  Record->PciIo,
                                  EfiPciIoWidthUint8,
                                  0,
                                  sizeof (Current),
                                  Current
                                  );
    if (EFI_ERROR (Status)) {
      LogPrint (
        Logger,
        L"Verification read failed for %04x:%02x:%02x.%x: 0x%016lx (%s)\r\n",
        (UINT32)Record->Segment,
        (UINT32)Record->Bus,
        (UINT32)Record->Device,
        (UINT32)Record->Function,
        (UINT64)Status,
        EfiStatusName (Status)
        );
      OverallStatus = Status;
      continue;
    }

    Before = ReadLe16 (&Record->Config[PCI_COMMAND_OFFSET]);
    After  = ReadLe16 (&Current[PCI_COMMAND_OFFSET]);
    if (Before != After) {
      LogPrint (
        Logger,
        L"ERROR: command changed at %04x:%02x:%02x.%x: 0x%04x -> 0x%04x\r\n",
        (UINT32)Record->Segment,
        (UINT32)Record->Bus,
        (UINT32)Record->Device,
        (UINT32)Record->Function,
        Before,
        After
        );
      OverallStatus = EFI_COMPROMISED_DATA;
    }

    if (RecordIsDisplay (Record)) {
      for (BarOffset = PCI_BAR0_OFFSET; BarOffset < (PCI_BAR0_OFFSET + 24);
           BarOffset += sizeof (UINT32))
      {
        UINT32  BeforeBar;
        UINT32  AfterBar;

        BeforeBar = ReadLe32 (&Record->Config[BarOffset]);
        AfterBar  = ReadLe32 (&Current[BarOffset]);
        if (BeforeBar != AfterBar) {
          LogPrint (
            Logger,
            L"ERROR: BAR dword 0x%02x changed at %04x:%02x:%02x.%x: "
            L"0x%08x -> 0x%08x\r\n",
            (UINT32)BarOffset,
            (UINT32)Record->Segment,
            (UINT32)Record->Bus,
            (UINT32)Record->Device,
            (UINT32)Record->Function,
            BeforeBar,
            AfterBar
            );
          OverallStatus = EFI_COMPROMISED_DATA;
        }
      }
    }

    if (RecordIsP2pBridge (Record)) {
      Before = ReadLe16 (&Record->Config[PCI_BRIDGE_CONTROL_OFFSET]);
      After  = ReadLe16 (&Current[PCI_BRIDGE_CONTROL_OFFSET]);
      if (Before != After) {
        LogPrint (
          Logger,
          L"ERROR: bridge control changed at %04x:%02x:%02x.%x: "
          L"0x%04x -> 0x%04x\r\n",
          (UINT32)Record->Segment,
          (UINT32)Record->Bus,
          (UINT32)Record->Device,
          (UINT32)Record->Function,
          Before,
          After
          );
        OverallStatus = EFI_COMPROMISED_DATA;
      }
    }

    if (!EFI_ERROR (Record->CurrentAttributesStatus) &&
        (Record->PciIo->Attributes != NULL))
    {
      CurrentAttributes = 0;
      Status = Record->PciIo->Attributes (
                                Record->PciIo,
                                EfiPciIoAttributeOperationGet,
                                0,
                                &CurrentAttributes
                                );
      if (EFI_ERROR (Status)) {
        LogPrint (
          Logger,
          L"Verification attribute query failed for %04x:%02x:%02x.%x: "
          L"0x%016lx (%s)\r\n",
          (UINT32)Record->Segment,
          (UINT32)Record->Bus,
          (UINT32)Record->Device,
          (UINT32)Record->Function,
          (UINT64)Status,
          EfiStatusName (Status)
          );
        if (!EFI_ERROR (OverallStatus)) {
          OverallStatus = Status;
        }
      } else if (CurrentAttributes != Record->CurrentAttributes) {
        LogPrint (
          Logger,
          L"ERROR: enabled EFI PCI attributes changed at %04x:%02x:%02x.%x: "
          L"0x%016lx -> 0x%016lx\r\n",
          (UINT32)Record->Segment,
          (UINT32)Record->Bus,
          (UINT32)Record->Device,
          (UINT32)Record->Function,
          Record->CurrentAttributes,
          CurrentAttributes
          );
        OverallStatus = EFI_COMPROMISED_DATA;
      }
    }

    if (RecordIsDisplay (Record) &&
        !EFI_ERROR (Record->RootBridgeStatus) &&
        !EFI_ERROR (Record->RootAttributesStatus) &&
        (Record->RootBridgeIo != NULL) &&
        (Record->RootBridgeIo->GetAttributes != NULL))
    {
      RootSupportedAttributes = 0;
      RootCurrentAttributes   = 0;
      Status = Record->RootBridgeIo->GetAttributes (
                                      Record->RootBridgeIo,
                                      &RootSupportedAttributes,
                                      &RootCurrentAttributes
                                      );
      if (EFI_ERROR (Status)) {
        LogPrint (
          Logger,
          L"Verification root-attribute query failed for %04x:%02x:%02x.%x: "
          L"0x%016lx (%s)\r\n",
          (UINT32)Record->Segment,
          (UINT32)Record->Bus,
          (UINT32)Record->Device,
          (UINT32)Record->Function,
          (UINT64)Status,
          EfiStatusName (Status)
          );
        if (!EFI_ERROR (OverallStatus)) {
          OverallStatus = Status;
        }
      } else if ((RootSupportedAttributes != Record->RootSupportedAttributes) ||
                 (RootCurrentAttributes != Record->RootCurrentAttributes))
      {
        LogPrint (
          Logger,
          L"ERROR: root attributes changed for %04x:%02x:%02x.%x: "
          L"supported 0x%016lx -> 0x%016lx, enabled 0x%016lx -> 0x%016lx\r\n",
          (UINT32)Record->Segment,
          (UINT32)Record->Bus,
          (UINT32)Record->Device,
          (UINT32)Record->Function,
          Record->RootSupportedAttributes,
          RootSupportedAttributes,
          Record->RootCurrentAttributes,
          RootCurrentAttributes
          );
        OverallStatus = EFI_COMPROMISED_DATA;
      }
    }
  }

  if (!EFI_ERROR (OverallStatus)) {
    LogPrint (
      Logger,
      L"Verified: all captured PCI command registers, display BARs, "
      L"bridge-control registers, enabled EFI PCI attributes, and display "
      L"root-bridge attributes are unchanged.\r\n"
      );
  } else {
    LogPrint (
      Logger,
      L"PCI state verification failed: 0x%016lx (%s)\r\n",
      (UINT64)OverallStatus,
      EfiStatusName (OverallStatus)
      );
  }

  return OverallStatus;
}

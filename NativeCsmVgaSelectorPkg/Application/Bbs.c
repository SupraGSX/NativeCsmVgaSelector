/** @file
  Bounded reporting for firmware-owned Legacy BIOS BBS data.

  EFI_LEGACY_BIOS_PROTOCOL.GetBbsInfo() is a query whose reference
  implementation may lazily initialize internal CSM state. The read-only
  probe may call it when explicitly requested. Compatibility16 discovery
  calls it only behind the separate
  stage3_preinstall_bbs_snapshot diagnostic switch. That firmware-specific
  caveat is logged immediately before every call.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include <Uefi.h>

#include <IndustryStandard/Pci22.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/LegacyBios.h>

#include "Bbs.h"
#include "MemoryMap.h"
#include "StatusPrint.h"

#define BBS_MAX_TABLE_ENTRIES  256
#define BBS_MAX_STRING_BYTES   128
#define REAL_MODE_MAX_ADDRESS  0x10FFEFU

STATIC_ASSERT (sizeof (BBS_STATUS_FLAGS) == 2, "Unexpected BBS status ABI");
STATIC_ASSERT (sizeof (BBS_TABLE) == 69, "Unexpected BBS table ABI");
STATIC_ASSERT (sizeof (HDD_INFO) == 1045, "Unexpected HDD info ABI");
STATIC_ASSERT (
  OFFSET_OF (BBS_TABLE, AssignedDriveNumber) == 52,
  "Unexpected BBS assigned-drive offset"
  );

typedef enum {
  BbsStringPresent,
  BbsStringAbsent,
  BbsStringUnsafe,
  BbsStringTruncated
} BBS_STRING_RESULT;

STATIC
BOOLEAN
BbsBufferHasNonzeroByte (
  IN CONST UINT8  *Bytes,
  IN UINTN        ByteCount
  )
{
  UINTN  Index;

  for (Index = 0; Index < ByteCount; ++Index) {
    if (Bytes[Index] != 0) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
UINT16
BbsRead16 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8));
}

STATIC
UINT32
BbsRead32 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT32)(Bytes[0] |
                  ((UINT32)Bytes[1] << 8) |
                  ((UINT32)Bytes[2] << 16) |
                  ((UINT32)Bytes[3] << 24));
}

STATIC
CONST CHAR16 *
BbsDeviceTypeName (
  IN UINT16  DeviceType
  )
{
  switch (DeviceType) {
    case BBS_FLOPPY:
      return L"floppy";
    case BBS_HARDDISK:
      return L"hard disk";
    case BBS_CDROM:
      return L"CD-ROM";
    case BBS_PCMCIA:
      return L"PCMCIA";
    case BBS_USB:
      return L"USB";
    case BBS_EMBED_NETWORK:
      return L"embedded network";
    case BBS_BEV_DEVICE:
      return L"BEV";
    case BBS_UNKNOWN:
      return L"unknown";
    default:
      return L"vendor-specific";
  }
}

STATIC
CONST CHAR16 *
BbsPriorityName (
  IN UINT16  Priority
  )
{
  switch (Priority) {
    case BBS_DO_NOT_BOOT_FROM:
      return L"do-not-boot";
    case BBS_LOWEST_PRIORITY:
      return L"lowest";
    case BBS_UNPRIORITIZED_ENTRY:
      return L"unprioritized";
    case BBS_IGNORE_ENTRY:
      return L"ignore";
    default:
      return L"ordered";
  }
}

STATIC
BBS_STRING_RESULT
BbsCopyFarString (
  IN  CONST MEMORY_MAP_SNAPSHOT  *MemoryMap,
  IN  UINT16                     Segment,
  IN  UINT16                     Offset,
  OUT CHAR16                     *Output,
  IN  UINTN                      OutputChars
  )
{
  UINTN                 Address;
  UINTN                 ReadableBytes;
  UINTN                 Limit;
  UINTN                 Index;
  CONST volatile UINT8  *Source;
  UINT8                 Value;

  if ((Output == NULL) || (OutputChars == 0)) {
    return BbsStringUnsafe;
  }

  Output[0] = L'\0';
  if ((Segment == 0) && (Offset == 0)) {
    return BbsStringAbsent;
  }

  Address = ((UINTN)Segment << 4) + Offset;
  if ((Address < EFI_PAGE_SIZE) || (Address > REAL_MODE_MAX_ADDRESS) ||
      !MemoryMapRangeIsReadable (MemoryMap, Address, 1, &ReadableBytes))
  {
    return BbsStringUnsafe;
  }

  Limit = ReadableBytes;
  if (Limit > BBS_MAX_STRING_BYTES) {
    Limit = BBS_MAX_STRING_BYTES;
  }

  if (Limit > (REAL_MODE_MAX_ADDRESS - Address + 1)) {
    Limit = REAL_MODE_MAX_ADDRESS - Address + 1;
  }

  if (Limit >= OutputChars) {
    Limit = OutputChars - 1;
  }

  Source = (CONST volatile UINT8 *)(UINTN)Address;
  for (Index = 0; Index < Limit; ++Index) {
    Value = Source[Index];
    if (Value == 0) {
      Output[Index] = L'\0';
      return BbsStringPresent;
    }

    if ((Value >= 0x20) && (Value <= 0x7E)) {
      Output[Index] = (CHAR16)Value;
    } else if (Value == '\t') {
      Output[Index] = L' ';
    } else {
      Output[Index] = L'.';
    }
  }

  Output[Limit] = L'\0';
  return BbsStringTruncated;
}

STATIC
EFI_STATUS
BbsPrintString (
  IN APP_LOGGER                 *Logger,
  IN CONST MEMORY_MAP_SNAPSHOT  *MemoryMap,
  IN CONST CHAR16               *Label,
  IN UINT16                     Segment,
  IN UINT16                     Offset
  )
{
  CHAR16             Text[BBS_MAX_STRING_BYTES + 1];
  BBS_STRING_RESULT  Result;

  Result = BbsCopyFarString (
             MemoryMap,
             Segment,
             Offset,
             Text,
             ARRAY_SIZE (Text)
             );
  switch (Result) {
    case BbsStringPresent:
      LogPrint (
        Logger,
        L"      %s %04x:%04x = \"%s\"\r\n",
        Label,
        Segment,
        Offset,
        Text
        );
      return EFI_SUCCESS;

    case BbsStringAbsent:
      LogPrint (Logger, L"      %s 0000:0000 = <absent>\r\n", Label);
      return EFI_SUCCESS;

    case BbsStringTruncated:
      LogPrint (
        Logger,
        L"      %s %04x:%04x = \"%s\" <unterminated/truncated>\r\n",
        Label,
        Segment,
        Offset,
        Text
        );
      return EFI_COMPROMISED_DATA;

    default:
      LogPrint (
        Logger,
        L"      %s %04x:%04x = <not safely readable>\r\n",
        Label,
        Segment,
        Offset
        );
      return EFI_SECURITY_VIOLATION;
  }
}

STATIC
 BBS_STRING_RESULT
BbsCopyFarAsciiString (
  IN  CONST MEMORY_MAP_SNAPSHOT  *MemoryMap,
  IN  UINT16                     Segment,
  IN  UINT16                     Offset,
  OUT CHAR8                      *Output,
  IN  UINTN                      OutputBytes
  )
{
  UINTN                 Address;
  UINTN                 ReadableBytes;
  UINTN                 Limit;
  UINTN                 Index;
  CONST volatile UINT8  *Source;
  UINT8                 Value;

  if ((Output == NULL) || (OutputBytes == 0)) {
    return BbsStringUnsafe;
  }

  Output[0] = '\0';
  if ((Segment == 0) && (Offset == 0)) {
    return BbsStringAbsent;
  }

  Address = ((UINTN)Segment << 4) + Offset;
  if ((Address < EFI_PAGE_SIZE) || (Address > REAL_MODE_MAX_ADDRESS) ||
      !MemoryMapRangeIsReadable (MemoryMap, Address, 1, &ReadableBytes))
  {
    return BbsStringUnsafe;
  }

  Limit = ReadableBytes;
  if (Limit > BBS_MAX_STRING_BYTES) {
    Limit = BBS_MAX_STRING_BYTES;
  }
  if (Limit > (REAL_MODE_MAX_ADDRESS - Address + 1U)) {
    Limit = REAL_MODE_MAX_ADDRESS - Address + 1U;
  }
  if (Limit >= OutputBytes) {
    Limit = OutputBytes - 1U;
  }

  Source = (CONST volatile UINT8 *)(UINTN)Address;
  for (Index = 0; Index < Limit; ++Index) {
    Value = Source[Index];
    if (Value == 0) {
      Output[Index] = '\0';
      return BbsStringPresent;
    }
    Output[Index] = (CHAR8)Value;
  }

  Output[Limit] = '\0';
  return BbsStringTruncated;
}

STATIC
EFI_STATUS
BbsPrintEntry (
  IN APP_LOGGER                 *Logger,
  IN CONST MEMORY_MAP_SNAPSHOT  *MemoryMap,
  IN UINTN                      Index,
  IN CONST BBS_TABLE            *PackedEntry
  )
{
  CONST UINT8  *Bytes;
  UINT16       Priority;
  UINT32       Bus;
  UINT32       Device;
  UINT32       Function;
  UINT16       MfgOffset;
  UINT16       MfgSegment;
  UINT16       DeviceType;
  UINT16       StatusFlags;
  UINT16       DescOffset;
  UINT16       DescSegment;
  UINT16       BootHandlerOffset;
  UINT16       BootHandlerSegment;
  UINT8        AssignedDrive;
  UINT32       InitPerReserved;
  UINT32       AdditionalIrq13Handler;
  UINT32       AdditionalIrq18Handler;
  UINT32       AdditionalIrq19Handler;
  UINT32       AdditionalIrq40Handler;
  UINT32       AdditionalIrq41Handler;
  UINT32       AdditionalIrq46Handler;
  UINT32       Ibv1;
  UINT32       Ibv2;
  EFI_STATUS   Status;
  EFI_STATUS   StringStatus;

  Bytes         = (CONST UINT8 *)PackedEntry;
  Priority      = BbsRead16 (Bytes + OFFSET_OF (BBS_TABLE, BootPriority));
  Bus           = BbsRead32 (Bytes + OFFSET_OF (BBS_TABLE, Bus));
  Device        = BbsRead32 (Bytes + OFFSET_OF (BBS_TABLE, Device));
  Function      = BbsRead32 (Bytes + OFFSET_OF (BBS_TABLE, Function));
  MfgOffset     = BbsRead16 (Bytes + OFFSET_OF (BBS_TABLE, MfgStringOffset));
  MfgSegment    = BbsRead16 (Bytes + OFFSET_OF (BBS_TABLE, MfgStringSegment));
  DeviceType    = BbsRead16 (Bytes + OFFSET_OF (BBS_TABLE, DeviceType));
  StatusFlags   = BbsRead16 (Bytes + OFFSET_OF (BBS_TABLE, StatusFlags));
  DescOffset    = BbsRead16 (Bytes + OFFSET_OF (BBS_TABLE, DescStringOffset));
  DescSegment   = BbsRead16 (Bytes + OFFSET_OF (BBS_TABLE, DescStringSegment));
  BootHandlerOffset = BbsRead16 (
                        Bytes + OFFSET_OF (BBS_TABLE, BootHandlerOffset)
                        );
  BootHandlerSegment = BbsRead16 (
                         Bytes + OFFSET_OF (BBS_TABLE, BootHandlerSegment)
                         );
  AssignedDrive = Bytes[OFFSET_OF (BBS_TABLE, AssignedDriveNumber)];
  InitPerReserved = BbsRead32 (Bytes + OFFSET_OF (BBS_TABLE, InitPerReserved));
  AdditionalIrq13Handler = BbsRead32 (
                             Bytes + OFFSET_OF (BBS_TABLE, AdditionalIrq13Handler)
                             );
  AdditionalIrq18Handler = BbsRead32 (
                             Bytes + OFFSET_OF (BBS_TABLE, AdditionalIrq18Handler)
                             );
  AdditionalIrq19Handler = BbsRead32 (
                             Bytes + OFFSET_OF (BBS_TABLE, AdditionalIrq19Handler)
                             );
  AdditionalIrq40Handler = BbsRead32 (
                             Bytes + OFFSET_OF (BBS_TABLE, AdditionalIrq40Handler)
                             );
  AdditionalIrq41Handler = BbsRead32 (
                             Bytes + OFFSET_OF (BBS_TABLE, AdditionalIrq41Handler)
                             );
  AdditionalIrq46Handler = BbsRead32 (
                             Bytes + OFFSET_OF (BBS_TABLE, AdditionalIrq46Handler)
                             );
  Ibv1 = BbsRead32 (Bytes + OFFSET_OF (BBS_TABLE, IBV1));
  Ibv2 = BbsRead32 (Bytes + OFFSET_OF (BBS_TABLE, IBV2));

  LogPrint (
    Logger,
    L"  BBS[%u] priority=0x%04x (%s) type=0x%04x (%s) "
    L"BDF(no segment)=%02x:%02x.%x drive=0x%02x\r\n",
    Index,
    Priority,
    BbsPriorityName (Priority),
    DeviceType,
    BbsDeviceTypeName (DeviceType),
    Bus,
    Device,
    Function,
    AssignedDrive
    );
  LogPrint (
    Logger,
    L"      class=%02x:%02x status=0x%04x "
    L"[old-position=%u reserved1=0x%x enabled=%s failed=%s "
    L"media=%u reserved2=0x%x]\r\n",
    Bytes[OFFSET_OF (BBS_TABLE, Class)],
    Bytes[OFFSET_OF (BBS_TABLE, SubClass)],
    StatusFlags,
    StatusFlags & 0x0FU,
    (StatusFlags >> 4) & 0x0FU,
    (StatusFlags & BIT8) != 0 ? L"yes" : L"no",
    (StatusFlags & BIT9) != 0 ? L"yes" : L"no",
    (StatusFlags >> 10) & 0x3,
    (StatusFlags >> 12) & 0x0FU
    );
  LogPrint (
    Logger,
    L"      boot-handler=%04x:%04x init-per-reserved=0x%08x\r\n",
    BootHandlerSegment,
    BootHandlerOffset,
    InitPerReserved
    );
  LogPrint (
    Logger,
    L"      additional IRQ handlers: 13=0x%08x 18=0x%08x 19=0x%08x "
    L"40=0x%08x 41=0x%08x 46=0x%08x\r\n",
    AdditionalIrq13Handler,
    AdditionalIrq18Handler,
    AdditionalIrq19Handler,
    AdditionalIrq40Handler,
    AdditionalIrq41Handler,
    AdditionalIrq46Handler
    );
  LogPrint (Logger, L"      IBV1=0x%08x IBV2=0x%08x\r\n", Ibv1, Ibv2);

  Status = BbsPrintString (
             Logger,
             MemoryMap,
             L"manufacturer",
             MfgSegment,
             MfgOffset
             );
  StringStatus = BbsPrintString (
                   Logger,
                   MemoryMap,
                   L"description",
                   DescSegment,
                   DescOffset
                   );
  if (!EFI_ERROR (Status) && EFI_ERROR (StringStatus)) {
    Status = StringStatus;
  }

  return Status;
}

STATIC
VOID
BbsPrintHddInfo (
  IN APP_LOGGER       *Logger,
  IN UINTN            Index,
  IN CONST HDD_INFO   *PackedInfo
  )
{
  CONST UINT8  *Bytes;
  UINT16       HddStatus;
  CONST UINT8  *IdentifyBytes;

  Bytes         = (CONST UINT8 *)PackedInfo;
  HddStatus     = BbsRead16 (Bytes + OFFSET_OF (HDD_INFO, Status));
  IdentifyBytes = Bytes + OFFSET_OF (HDD_INFO, IdentifyDrive);
  LogPrint (
    Logger,
    L"  HDD_INFO[%u] status=0x%04x BDF(no segment)=%02x:%02x.%x "
    L"command=0x%04x control=0x%04x bus-master=0x%04x IRQ=%u\r\n",
    Index,
    HddStatus,
    BbsRead32 (Bytes + OFFSET_OF (HDD_INFO, Bus)),
    BbsRead32 (Bytes + OFFSET_OF (HDD_INFO, Device)),
    BbsRead32 (Bytes + OFFSET_OF (HDD_INFO, Function)),
    BbsRead16 (Bytes + OFFSET_OF (HDD_INFO, CommandBaseAddress)),
    BbsRead16 (Bytes + OFFSET_OF (HDD_INFO, ControlBaseAddress)),
    BbsRead16 (Bytes + OFFSET_OF (HDD_INFO, BusMasterAddress)),
    Bytes[OFFSET_OF (HDD_INFO, HddIrq)]
    );
  LogPrint (
    Logger,
    L"      primary=%s secondary=%s master-IDE=%s slave-IDE=%s "
    L"master-CD=%s slave-CD=%s master-ZIP=%s slave-ZIP=%s\r\n",
    (HddStatus & HDD_PRIMARY) != 0 ? L"yes" : L"no",
    (HddStatus & HDD_SECONDARY) != 0 ? L"yes" : L"no",
    (HddStatus & HDD_MASTER_IDE) != 0 ? L"yes" : L"no",
    (HddStatus & HDD_SLAVE_IDE) != 0 ? L"yes" : L"no",
    (HddStatus & HDD_MASTER_ATAPI_CDROM) != 0 ? L"yes" : L"no",
    (HddStatus & HDD_SLAVE_ATAPI_CDROM) != 0 ? L"yes" : L"no",
    (HddStatus & HDD_MASTER_ATAPI_ZIPDISK) != 0 ? L"yes" : L"no",
    (HddStatus & HDD_SLAVE_ATAPI_ZIPDISK) != 0 ? L"yes" : L"no"
    );
  LogPrint (
    Logger,
    L"      identify payload: master=%s slave=%s "
    L"(raw storage-identify words intentionally not printed)\r\n",
    BbsBufferHasNonzeroByte (IdentifyBytes, sizeof (ATAPI_IDENTIFY)) ?
    L"present" : L"zero",
    BbsBufferHasNonzeroByte (
      IdentifyBytes + sizeof (ATAPI_IDENTIFY),
      sizeof (ATAPI_IDENTIFY)
      ) ? L"present" : L"zero"
    );
}

STATIC
EFI_STATUS
BbsFlushAndReturn (
  IN APP_LOGGER  *Logger,
  IN EFI_STATUS  CurrentStatus
  )
{
  EFI_STATUS  FlushStatus;

  FlushStatus = LogFlush (Logger);
  if (EFI_ERROR (FlushStatus)) {
    LogPrint (
      Logger,
      L"BBS log flush failed: 0x%016lx (%s); prior result=0x%016lx (%s)\r\n",
      (UINT64)FlushStatus,
      EfiStatusName (FlushStatus),
      (UINT64)CurrentStatus,
      EfiStatusName (CurrentStatus)
      );
    return FlushStatus;
  }

  return CurrentStatus;
}

STATIC
EFI_STATUS
BbsSummaryReturn (
  OUT BBS_PROBE_SUMMARY  *Summary OPTIONAL,
  IN  EFI_STATUS         Result
  )
{
  if (Summary != NULL) {
    Summary->Result = Result;
  }

  return Result;
}

EFI_STATUS
BbsProbeAndPrintEx (
  IN  APP_LOGGER                *Logger,
  IN  EFI_LEGACY_BIOS_PROTOCOL  *LegacyBios,
  IN  CONST BBS_CONTROLLER_TARGET *Target OPTIONAL,
  OUT BBS_PROBE_SUMMARY         *Summary OPTIONAL
  )
{
  EFI_STATUS           Status;
  UINT16               HddCount;
  HDD_INFO             *HddInfo;
  UINT16               BbsCount;
  BBS_TABLE            *BbsTable;
  UINTN                TableBytes;
  UINTN                HddBytes;
  UINTN                Index;
  HDD_INFO             HddEntry;
  BBS_TABLE            Entry;
  MEMORY_MAP_SNAPSHOT  MemoryMap;
  EFI_STATUS           OverallStatus;
  EFI_STATUS           EntryStatus;

  if (Summary != NULL) {
    ZeroMem (Summary, sizeof (*Summary));
    Summary->Result = EFI_NOT_READY;
    if (Target != NULL) {
      Summary->TargetRequested = TRUE;
      Summary->TargetSegmentRepresentable = (BOOLEAN)(Target->Segment == 0);
    }
  }

  if ((Logger == NULL) || (LegacyBios == NULL)) {
    return BbsSummaryReturn (Summary, EFI_INVALID_PARAMETER);
  }

  if ((Logger->File == NULL) || EFI_ERROR (Logger->FileOpenStatus)) {
    LogPrint (
      Logger,
      L"Refusing GetBbsInfo because no writable file log is open\r\n"
      );
    return BbsSummaryReturn (Summary, EFI_NOT_READY);
  }

  LogPrint (Logger, L"\r\n=== Native CSM BBS information ===\r\n");
  if (LegacyBios->GetBbsInfo == NULL) {
    LogPrint (Logger, L"GetBbsInfo function pointer is NULL\r\n");
    return BbsSummaryReturn (Summary, EFI_UNSUPPORTED);
  }

  LogPrint (
    Logger,
    L"CAUTION: GetBbsInfo is a standard query, but firmware may lazily connect "
    L"controllers or update CSM-internal BBS priorities while servicing it.\r\n"
    );
  Status = LogFlush (Logger);
  if (EFI_ERROR (Status)) {
    LogPrint (
      Logger,
      L"Refusing GetBbsInfo because the pre-query caution could not be "
      L"flushed: 0x%016lx (%s)\r\n",
      (UINT64)Status,
      EfiStatusName (Status)
      );
    return BbsSummaryReturn (Summary, Status);
  }

  HddCount = 0;
  HddInfo  = NULL;
  BbsCount = 0;
  BbsTable = NULL;
  if (Summary != NULL) {
    Summary->Called = TRUE;
  }

  Status = LegacyBios->GetBbsInfo (
                         LegacyBios,
                         &HddCount,
                         &HddInfo,
                         &BbsCount,
                         &BbsTable
                         );
  LogPrint (
    Logger,
    L"GetBbsInfo: 0x%016lx (%s), HDD count=%u table=%p, "
    L"BBS count=%u table=%p\r\n",
    (UINT64)Status,
    EfiStatusName (Status),
    HddCount,
    HddInfo,
    BbsCount,
    BbsTable
    );
  EntryStatus = LogFlush (Logger);
  if (EFI_ERROR (EntryStatus)) {
    LogPrint (
      Logger,
      L"Could not durably log the GetBbsInfo result: 0x%016lx (%s)\r\n",
      (UINT64)EntryStatus,
      EfiStatusName (EntryStatus)
      );
    return BbsSummaryReturn (Summary, EntryStatus);
  }

  if (EFI_ERROR (Status)) {
    return BbsSummaryReturn (Summary, Status);
  }

  if (((HddCount != 0) && (HddInfo == NULL)) ||
      (HddCount > MAX_IDE_CONTROLLER))
  {
    LogPrint (
      Logger,
      L"Malformed HDD_INFO result: table=%p count=%u (safety cap=%u)\r\n",
      HddInfo,
      HddCount,
      MAX_IDE_CONTROLLER
      );
    Status = BbsFlushAndReturn (Logger, EFI_COMPROMISED_DATA);
    return BbsSummaryReturn (Summary, Status);
  }

  if (((BbsCount != 0) && (BbsTable == NULL)) ||
      (BbsCount > BBS_MAX_TABLE_ENTRIES))
  {
    LogPrint (
      Logger,
      L"Malformed BBS result: table=%p count=%u (safety cap=%u)\r\n",
      BbsTable,
      BbsCount,
      BBS_MAX_TABLE_ENTRIES
      );
    Status = BbsFlushAndReturn (Logger, EFI_COMPROMISED_DATA);
    return BbsSummaryReturn (Summary, Status);
  }

  if (Summary != NULL) {
    Summary->CountsValid = TRUE;
    Summary->HddCount    = HddCount;
    Summary->BbsCount    = BbsCount;
  }

  if ((HddCount == 0) && (BbsCount == 0)) {
    LogPrint (Logger, L"GetBbsInfo returned empty HDD_INFO and BBS tables\r\n");
    Status = BbsFlushAndReturn (Logger, EFI_SUCCESS);
    return BbsSummaryReturn (Summary, Status);
  }

  HddBytes   = (UINTN)HddCount * sizeof (HDD_INFO);
  TableBytes = (UINTN)BbsCount * sizeof (BBS_TABLE);
  Status = MemoryMapCapture (&MemoryMap);
  if (EFI_ERROR (Status)) {
    LogPrint (
      Logger,
      L"GetMemoryMap for BBS pointer validation failed: 0x%016lx (%s)\r\n",
      (UINT64)Status,
      EfiStatusName (Status)
      );
    Status = BbsFlushAndReturn (Logger, Status);
    return BbsSummaryReturn (Summary, Status);
  }

  if ((HddCount != 0) && !MemoryMapRangeIsReadable (
                           &MemoryMap,
                           (UINTN)HddInfo,
                           HddBytes,
                           NULL
                           ))
  {
    LogPrint (
      Logger,
      L"HDD_INFO range %p + 0x%lx is not in a readable UEFI memory descriptor; "
      L"refusing to dereference it\r\n",
      HddInfo,
      (UINT64)HddBytes
      );
    MemoryMapRelease (&MemoryMap);
    Status = BbsFlushAndReturn (Logger, EFI_SECURITY_VIOLATION);
    return BbsSummaryReturn (Summary, Status);
  }

  if ((BbsCount != 0) && !MemoryMapRangeIsReadable (
         &MemoryMap,
         (UINTN)BbsTable,
         TableBytes,
         NULL
         ))
  {
    LogPrint (
      Logger,
      L"BBS table range %p + 0x%lx is not in a readable UEFI memory descriptor; "
      L"refusing to dereference it\r\n",
      BbsTable,
      (UINT64)TableBytes
      );
    MemoryMapRelease (&MemoryMap);
    Status = BbsFlushAndReturn (Logger, EFI_SECURITY_VIOLATION);
    return BbsSummaryReturn (Summary, Status);
  }

  OverallStatus = EFI_SUCCESS;
  for (Index = 0; Index < HddCount; ++Index) {
    CopyMem (
      &HddEntry,
      (CONST UINT8 *)HddInfo + (Index * sizeof (HDD_INFO)),
      sizeof (HddEntry)
      );
    BbsPrintHddInfo (Logger, Index, &HddEntry);
  }

  if (BbsCount == 0) {
    LogPrint (Logger, L"GetBbsInfo returned an empty BBS table\r\n");
  }

  for (Index = 0; Index < BbsCount; ++Index) {
    CopyMem (
      &Entry,
      (CONST UINT8 *)BbsTable + (Index * sizeof (BBS_TABLE)),
      sizeof (Entry)
      );
    EntryStatus = BbsPrintEntry (Logger, &MemoryMap, Index, &Entry);
    if (EFI_ERROR (EntryStatus) && !EFI_ERROR (OverallStatus)) {
      OverallStatus = EntryStatus;
    }

    if ((Summary != NULL) &&
        (BbsRead16 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, DeviceType)) == BBS_HARDDISK))
    {
      NCV_STORAGE_CANDIDATE Candidate;
      BBS_STRING_RESULT MfgResult;
      BBS_STRING_RESULT DescResult;
      UINT32 RawBus;
      UINT32 RawDevice;
      UINT32 RawFunction;

      ZeroMem (&Candidate, sizeof (Candidate));
      RawBus = BbsRead32 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, Bus));
      RawDevice = BbsRead32 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, Device));
      RawFunction = BbsRead32 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, Function));
      Candidate.BbsIndex = Index;
      Candidate.FirmwareStatusKnown = 1;
      Candidate.FirmwareStatus = BbsRead16 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, StatusFlags));
      Candidate.FirmwarePriority = BbsRead16 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, BootPriority));
      Candidate.RawBus = RawBus;
      Candidate.RawDevice = RawDevice;
      Candidate.RawFunction = RawFunction;
      Candidate.AddressValid = (NCV_BOOL)(
                                 (RawBus <= MAX_UINT8) &&
                                 (RawDevice <= 31U) &&
                                 (RawFunction <= 7U)
                                 );
      if (Candidate.AddressValid) {
        Candidate.Address.Segment = 0;
        Candidate.Address.Bus = (NCV_U8)RawBus;
        Candidate.Address.Device = (NCV_U8)RawDevice;
        Candidate.Address.Function = (NCV_U8)RawFunction;
      }
      Candidate.BaseClass = Entry.Class;
      Candidate.SubClass = Entry.SubClass;
      MfgResult = BbsCopyFarAsciiString (
                    &MemoryMap,
                    Entry.MfgStringSegment,
                    Entry.MfgStringOffset,
                    Candidate.Manufacturer,
                    sizeof (Candidate.Manufacturer)
                    );
      DescResult = BbsCopyFarAsciiString (
                     &MemoryMap,
                     Entry.DescStringSegment,
                     Entry.DescStringOffset,
                     Candidate.Description,
                     sizeof (Candidate.Description)
                     );
      Candidate.Usb = (NCV_BOOL)(
                        (Candidate.BaseClass == PCI_CLASS_SERIAL) &&
                        (Candidate.SubClass == PCI_CLASS_SERIAL_USB)
                        );
      Candidate.Eligible = (NCV_BOOL)(
                             Candidate.AddressValid &&
                             (DescResult == BbsStringPresent) &&
                             NcvIniValueIsExactlyRepresentable (
                               Candidate.Description,
                               sizeof (Candidate.Description)
                               )
                             );
      (VOID)MfgResult;
      NcvStorageCollectionAppend (&Summary->Storage, &Candidate);
    }

    if ((Target != NULL) && (Target->Segment == 0) &&
        (BbsRead32 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, Bus)) == Target->Bus) &&
        (BbsRead32 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, Device)) == Target->Device) &&
        (BbsRead32 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, Function)) == Target->Function))
    {
      CHAR16             TargetDescription[BBS_PROBE_DESCRIPTION_CHARS];
      BBS_STRING_RESULT  DescriptionResult;
      UINT16             DeviceType;

      DeviceType = BbsRead16 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, DeviceType));
      if (Summary != NULL) {
        ++Summary->TargetBbsMatches;
        if (DeviceType == BBS_HARDDISK) {
          ++Summary->TargetHardDiskMatches;
        }
        Summary->TargetBbsIndex = Index;
      }

      DescriptionResult = BbsCopyFarString (
                            &MemoryMap,
                            BbsRead16 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, DescStringSegment)),
                            BbsRead16 ((CONST UINT8 *)&Entry + OFFSET_OF (BBS_TABLE, DescStringOffset)),
                            TargetDescription,
                            ARRAY_SIZE (TargetDescription)
                            );
      if ((Summary != NULL) && (DescriptionResult == BbsStringPresent)) {
        CopyMem (
          Summary->TargetDescription,
          TargetDescription,
          sizeof (Summary->TargetDescription)
          );
      }

      LogPrint (
        Logger,
        L"  configured boot-controller match: BBS[%u] type=0x%04x description=%s\r\n",
        (UINT32)Index,
        DeviceType,
        (DescriptionResult == BbsStringPresent) ? TargetDescription : L"<unavailable>"
        );
    }
  }

  if ((Summary != NULL) && Summary->Storage.Overflow) {
    LogPrint (
      Logger,
      L"BBS storage candidate collection overflow: maximum=%u observed=%u\r\n",
      NCV_MAX_STORAGE_CANDIDATES,
      (UINT32)Summary->Storage.ObservedCount
      );
    if (!EFI_ERROR (OverallStatus)) {
      OverallStatus = EFI_BUFFER_TOO_SMALL;
    }
  }

  if ((Target != NULL) && (Target->Segment != 0)) {
    LogPrint (
      Logger,
      L"Configured boot controller %04x:%02x:%02x.%x cannot be uniquely mapped to BBS: BBS exposes no PCI segment field\r\n",
      Target->Segment,
      Target->Bus,
      Target->Device,
      Target->Function
      );
  } else if (Target != NULL) {
    LogPrint (
      Logger,
      L"Configured boot-controller BBS matches=%u hard-disk matches=%u\r\n",
      (Summary != NULL) ? (UINT32)Summary->TargetBbsMatches : 0,
      (Summary != NULL) ? (UINT32)Summary->TargetHardDiskMatches : 0
      );
  }

  MemoryMapRelease (&MemoryMap);
  LogPrint (
    Logger,
    L"BBS enumeration completed: HDD entries=%u BBS entries=%u "
    L"result=0x%016lx (%s)\r\n",
    HddCount,
    BbsCount,
    (UINT64)OverallStatus,
    EfiStatusName (OverallStatus)
    );
  Status = BbsFlushAndReturn (Logger, OverallStatus);
  return BbsSummaryReturn (Summary, Status);
}

EFI_STATUS
BbsProbeAndPrint (
  IN APP_LOGGER                *Logger,
  IN EFI_LEGACY_BIOS_PROTOCOL  *LegacyBios
  )
{
  return BbsProbeAndPrintEx (Logger, LegacyBios, NULL, NULL);
}

/** @file
  Strict legacy boot-target Boot####/BBS discovery and reversible BootPriority
  transaction support.

  The only firmware-owned writes in this file target the two-byte
  BBS_TABLE.BootPriority field.  No variable, PCI register, option ROM, or
  native CSM operation other than the one read-only GetBbsInfo query is
  performed here.  LegacyBoot is intentionally owned by the higher-level
  irreversible-boundary code.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include <Uefi.h>

#include <Guid/GlobalVariable.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LegacyBios.h>

#include "MemoryMap.h"
#include "LegacyBootTarget.h"
#include "LegacyStorageRom.h"
#include "LegacyFirmwareDisk.h"
#include "LegacyRomGuard.h"
#include "StatusPrint.h"

#define LEGACY_BOOT_BOOT_TARGET_SIGNATURE       SIGNATURE_32 ('S', '4', 'B', 'T')
#define LEGACY_BOOT_BOOT_ORDER_MAX_BYTES        4096U
#define LEGACY_BOOT_BOOT_OPTION_MAX_BYTES       65536U
#define LEGACY_BOOT_LOAD_OPTION_FIXED_BYTES     6U
#define LEGACY_BOOT_BBS_NODE_FIXED_BYTES        8U
#define LEGACY_BOOT_DEVICE_PATH_END_BYTES       4U
#define LEGACY_BOOT_REAL_MODE_MAX_ADDRESS       0x10FFEFU
#define LEGACY_BOOT_VARIABLE_READ_ATTEMPTS      3U
#define LEGACY_BOOT_LEGACY_DEV_ORDER_MAX_BYTES  4096U
#define LEGACY_BOOT_LEGACY_DEV_ORDER_NAME       L"LegacyDevOrder"

// Pinned IntelFrameworkModulePkg/Include/Guid/LegacyDevOrder.h ABI.  The
// active build deliberately does not import IntelFrameworkModulePkg, so the
// variable is decoded manually and this GUID is kept local to legacy boot-target discovery.
STATIC CONST EFI_GUID  mLegacyBootLegacyDevOrderVariableGuid = {
  0xa56074db,
  0x65fe,
  0x45f7,
  { 0xbd, 0x21, 0x2d, 0x2b, 0xdd, 0x8e, 0x96, 0x52 }
};

STATIC_ASSERT (sizeof (BBS_STATUS_FLAGS) == 2, "Unexpected BBS status ABI");
STATIC_ASSERT (sizeof (BBS_TABLE) == 69, "Unexpected BBS table ABI");
STATIC_ASSERT (
  OFFSET_OF (BBS_TABLE, BootPriority) == 0,
  "Unexpected BBS BootPriority offset"
  );
STATIC_ASSERT (
  OFFSET_OF (BBS_BBS_DEVICE_PATH, String) == LEGACY_BOOT_BBS_NODE_FIXED_BYTES,
  "Unexpected BBS device-path ABI"
  );
STATIC_ASSERT (
  sizeof (BBS_BBS_DEVICE_PATH) == (LEGACY_BOOT_BBS_NODE_FIXED_BYTES + 1U),
  "Unexpected packed BBS device-path size"
  );

typedef enum {
  LegacyBootFarStringPresent,
  LegacyBootFarStringAbsent,
  LegacyBootFarStringUnsafe,
  LegacyBootFarStringTruncated
} LEGACY_BOOT_FAR_STRING_RESULT;

typedef struct {
  BOOLEAN  IsLegacy;
  BOOLEAN  IsActive;

  UINT16  Number;
  UINT32  Attributes;
  CHAR16  *Description;
  UINTN   DescriptionBytes;

  UINT8   *DevicePath;
  UINTN   DevicePathSize;
  UINT32  DevicePathCrc32;
  UINT16  BbsDeviceType;
  UINT16  BbsStatusFlag;
  CHAR8   BbsDescription[LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U];

  UINT8   *OptionalData;
  UINTN   OptionalDataSize;
  UINT32  OptionalDataCrc32;
} LEGACY_BOOT_BOOT_CANDIDATE;

STATIC
UINT16
LegacyBootReadLe16 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8));
}

STATIC
UINT32
LegacyBootReadLe32 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT32)(Bytes[0] |
                  ((UINT32)Bytes[1] << 8) |
                  ((UINT32)Bytes[2] << 16) |
                  ((UINT32)Bytes[3] << 24));
}

STATIC
VOID
LegacyBootWriteLe16 (
  OUT UINT8   *Bytes,
  IN  UINT16  Value
  )
{
  Bytes[0] = (UINT8)(Value & 0xFFU);
  Bytes[1] = (UINT8)(Value >> 8);
}

STATIC
UINT8
LegacyBootAsciiUpper (
  IN UINT8  Character
  )
{
  if ((Character >= 'a') && (Character <= 'z')) {
    return (UINT8)(Character - ('a' - 'A'));
  }

  return Character;
}

STATIC
CHAR16
LegacyBootUnicodeUpperAscii (
  IN CHAR16  Character
  )
{
  if ((Character >= L'a') && (Character <= L'z')) {
    return (CHAR16)(Character - (L'a' - L'A'));
  }

  return Character;
}

STATIC
BOOLEAN
LegacyBootUnicodeContainsInsensitive (
  IN CONST CHAR16  *Text,
  IN CONST CHAR16  *Needle
  )
{
  UINTN  TextIndex;
  UINTN  NeedleIndex;

  if ((Text == NULL) || (Needle == NULL) || (Needle[0] == L'\0')) {
    return FALSE;
  }

  for (TextIndex = 0; Text[TextIndex] != L'\0'; ++TextIndex) {
    for (NeedleIndex = 0; Needle[NeedleIndex] != L'\0'; ++NeedleIndex) {
      if (Text[TextIndex + NeedleIndex] == L'\0') {
        break;
      }

      if (LegacyBootUnicodeUpperAscii (Text[TextIndex + NeedleIndex]) !=
          LegacyBootUnicodeUpperAscii (Needle[NeedleIndex]))
      {
        break;
      }
    }

    if (Needle[NeedleIndex] == L'\0') {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
LegacyBootUnicodeEqualsInsensitive (
  IN CONST CHAR16  *Left,
  IN CONST CHAR16  *Right
  )
{
  UINTN  Index;

  if ((Left == NULL) || (Right == NULL)) {
    return FALSE;
  }

  for (Index = 0; ; ++Index) {
    if (LegacyBootUnicodeUpperAscii (Left[Index]) !=
        LegacyBootUnicodeUpperAscii (Right[Index]))
    {
      return FALSE;
    }

    if (Left[Index] == L'\0') {
      return TRUE;
    }
  }
}

STATIC
BOOLEAN
LegacyBootAsciiEqualsUnicodeInsensitive (
  IN CONST CHAR8   *Text,
  IN CONST CHAR16  *Needle
  )
{
  UINTN  Index;
  if ((Text == NULL) || (Needle == NULL)) { return FALSE; }
  for (Index = 0; ; ++Index) {
    if ((Needle[Index] > 0x7FU) ||
        (LegacyBootAsciiUpper ((UINT8)Text[Index]) != (UINT8)LegacyBootUnicodeUpperAscii (Needle[Index])))
    { return FALSE; }
    if (Text[Index] == '\0') { return TRUE; }
  }
}

STATIC
UINT32
LegacyBootCrc32Update (
  IN UINT32       RunningCrc,
  IN CONST UINT8  *Bytes,
  IN UINTN        ByteCount
  )
{
  UINTN   ByteIndex;
  UINTN   BitIndex;
  UINT32  Mask;

  for (ByteIndex = 0; ByteIndex < ByteCount; ++ByteIndex) {
    RunningCrc ^= Bytes[ByteIndex];
    for (BitIndex = 0; BitIndex < 8; ++BitIndex) {
      Mask       = (UINT32)(0U - (RunningCrc & 1U));
      RunningCrc = (RunningCrc >> 1) ^ (0xEDB88320U & Mask);
    }
  }

  return RunningCrc;
}

STATIC
UINT32
LegacyBootCrc32 (
  IN CONST VOID  *Buffer OPTIONAL,
  IN UINTN       BufferSize
  )
{
  UINT32  Crc;

  Crc = MAX_UINT32;
  if ((Buffer != NULL) && (BufferSize != 0)) {
    Crc = LegacyBootCrc32Update (Crc, Buffer, BufferSize);
  }

  return ~Crc;
}

STATIC
UINT32
LegacyBootBbsIdentityCrc32 (
  IN CONST BBS_TABLE  *Table,
  IN UINT16           Count
  )
{
  UINT32  Crc;
  UINTN   Index;

  Crc = MAX_UINT32;
  for (Index = 0; Index < Count; ++Index) {
    Crc = LegacyBootCrc32Update (
            Crc,
            (CONST UINT8 *)&Table[Index] + sizeof (UINT16),
            sizeof (BBS_TABLE) - sizeof (UINT16)
            );
  }

  return ~Crc;
}

STATIC
UINT32
LegacyBootBbsPriorityCrc32FromTable (
  IN CONST BBS_TABLE  *Table,
  IN UINT16           Count
  )
{
  UINT32  Crc;
  UINTN   Index;
  UINT8   PriorityBytes[2];

  Crc = MAX_UINT32;
  for (Index = 0; Index < Count; ++Index) {
    LegacyBootWriteLe16 (PriorityBytes, Table[Index].BootPriority);
    Crc = LegacyBootCrc32Update (Crc, PriorityBytes, sizeof (PriorityBytes));
  }

  return ~Crc;
}

STATIC
UINT32
LegacyBootBbsPriorityCrc32FromVector (
  IN CONST UINT16  *Priorities,
  IN UINT16        Count
  )
{
  UINT32  Crc;
  UINTN   Index;
  UINT8   PriorityBytes[2];

  Crc = MAX_UINT32;
  for (Index = 0; Index < Count; ++Index) {
    LegacyBootWriteLe16 (PriorityBytes, Priorities[Index]);
    Crc = LegacyBootCrc32Update (Crc, PriorityBytes, sizeof (PriorityBytes));
  }

  return ~Crc;
}

#include "LegacyBootPolicy.inc"

STATIC
CONST CHAR16 *
LegacyBootFarStringResultName (
  IN LEGACY_BOOT_FAR_STRING_RESULT  Result
  )
{
  switch (Result) {
    case LegacyBootFarStringPresent:
      return L"present";
    case LegacyBootFarStringAbsent:
      return L"absent";
    case LegacyBootFarStringUnsafe:
      return L"unsafe";
    case LegacyBootFarStringTruncated:
      return L"truncated";
    default:
      return L"unknown";
  }
}

STATIC
LEGACY_BOOT_FAR_STRING_RESULT
LegacyBootCopyFarAsciiString (
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
    return LegacyBootFarStringUnsafe;
  }

  Output[0] = '\0';
  if ((Segment == 0) && (Offset == 0)) {
    return LegacyBootFarStringAbsent;
  }

  Address = ((UINTN)Segment << 4) + Offset;
  if ((Address < EFI_PAGE_SIZE) || (Address > LEGACY_BOOT_REAL_MODE_MAX_ADDRESS) ||
      !MemoryMapRangeIsReadable (MemoryMap, Address, 1, &ReadableBytes))
  {
    return LegacyBootFarStringUnsafe;
  }

  Limit = ReadableBytes;
  if (Limit > LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES) {
    Limit = LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES;
  }

  if (Limit > (LEGACY_BOOT_REAL_MODE_MAX_ADDRESS - Address + 1U)) {
    Limit = LEGACY_BOOT_REAL_MODE_MAX_ADDRESS - Address + 1U;
  }

  if (Limit >= OutputBytes) {
    Limit = OutputBytes - 1U;
  }

  Source = (CONST volatile UINT8 *)(UINTN)Address;
  for (Index = 0; Index < Limit; ++Index) {
    Value = Source[Index];
    if (Value == 0) {
      Output[Index] = '\0';
      return LegacyBootFarStringPresent;
    }

    Output[Index] = (CHAR8)Value;
  }

  Output[Limit] = '\0';
  return LegacyBootFarStringTruncated;
}

/* PnP offset zero means absent, even when firmware keeps the ROM segment.
   Do not reinterpret arbitrary segment:0 pointers: require the full proof. */
STATIC
LEGACY_BOOT_FAR_STRING_RESULT EFIAPI __attribute__((noinline))
LegacyBootCopyManufacturer (
  CONST MEMORY_MAP_SNAPSHOT *MemoryMap, CONST BBS_TABLE *Entry,
  CHAR8 *Output, UINTN OutputBytes)
{
  BOOLEAN Missing;
  if (Output == NULL || OutputBytes == 0) { return LegacyBootFarStringUnsafe; }
  if (LegacyStorageRomMatch (Entry, &Missing) && Missing) {
    Output[0] = 0;
    return LegacyBootFarStringAbsent;
  }
  return LegacyBootCopyFarAsciiString (MemoryMap, Entry->MfgStringSegment,
    Entry->MfgStringOffset, Output, OutputBytes);
}

STATIC
VOID
LegacyBootSanitizeAsciiForLog (
  IN  CONST CHAR8  *Input,
  OUT CHAR8        *Output,
  IN  UINTN        OutputBytes
  )
{
  UINTN  Index;
  UINT8  Value;

  if ((Input == NULL) || (Output == NULL) || (OutputBytes == 0)) {
    return;
  }

  for (Index = 0; Index + 1U < OutputBytes; ++Index) {
    Value = (UINT8)Input[Index];
    if (Value == 0) {
      break;
    }

    Output[Index] = ((Value >= 0x20U) && (Value <= 0x7EU)) ?
                    (CHAR8)Value : '.';
  }

  Output[Index] = '\0';
}

STATIC
EFI_STATUS
LegacyBootReadVariableBounded (
  IN  CONST CHAR16  *Name,
  IN  CONST EFI_GUID *Guid,
  IN  UINTN         MaximumSize,
  OUT UINT32        *Attributes OPTIONAL,
  OUT UINT8         **Buffer,
  OUT UINTN         *BufferSize
  )
{
  EFI_STATUS  Status;
  UINTN       RequiredSize;
  UINT32      LocalAttributes;
  UINT8       *LocalBuffer;
  UINTN       Attempt;

  if ((Name == NULL) || (Guid == NULL) || (Buffer == NULL) ||
      (BufferSize == NULL) || (MaximumSize == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Buffer     = NULL;
  *BufferSize = 0;
  if (Attributes != NULL) {
    *Attributes = 0;
  }

  RequiredSize    = 0;
  LocalAttributes = 0;
  Status = gRT->GetVariable (
                  (CHAR16 *)Name,
                  (EFI_GUID *)Guid,
                  &LocalAttributes,
                  &RequiredSize,
                  NULL
                  );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
  }

  for (Attempt = 0; Attempt < LEGACY_BOOT_VARIABLE_READ_ATTEMPTS; ++Attempt) {
    if ((RequiredSize == 0) || (RequiredSize > MaximumSize)) {
      return EFI_BAD_BUFFER_SIZE;
    }

    LocalBuffer = AllocateZeroPool (RequiredSize);
    if (LocalBuffer == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    *BufferSize     = RequiredSize;
    LocalAttributes = 0;
    Status = gRT->GetVariable (
                    (CHAR16 *)Name,
                    (EFI_GUID *)Guid,
                    &LocalAttributes,
                    BufferSize,
                    LocalBuffer
                    );
    if (Status == EFI_SUCCESS) {
      if ((*BufferSize == 0) || (*BufferSize > RequiredSize)) {
        FreePool (LocalBuffer);
        *BufferSize = 0;
        return EFI_COMPROMISED_DATA;
      }

      *Buffer = LocalBuffer;
      if (Attributes != NULL) {
        *Attributes = LocalAttributes;
      }

      return EFI_SUCCESS;
    }

    FreePool (LocalBuffer);
    if ((Status != EFI_BUFFER_TOO_SMALL) ||
        (*BufferSize == 0) || (*BufferSize > MaximumSize))
    {
      *BufferSize = 0;
      return Status;
    }

    RequiredSize = *BufferSize;
  }

  *BufferSize = 0;
  return EFI_BUFFER_TOO_SMALL;
}

#include "LegacyBootOrder.inc"

STATIC
EFI_STATUS
LegacyBootVerifyLegacyDevOrderSnapshot (
  IN APP_LOGGER                        *Logger,
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  IN CONST CHAR16                      *Label,
  IN BOOLEAN                           FlushResult
  )
{
  EFI_STATUS  Status;
  UINT8       *Variable;
  UINTN       VariableSize;
  UINT32      Attributes;
  BOOLEAN     Match;

  if ((Logger == NULL) || (Context == NULL) || (Label == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Variable     = NULL;
  VariableSize = 0;
  Attributes   = 0;
  Status = LegacyBootReadVariableBounded (
             LEGACY_BOOT_LEGACY_DEV_ORDER_NAME,
             &mLegacyBootLegacyDevOrderVariableGuid,
             LEGACY_BOOT_LEGACY_DEV_ORDER_MAX_BYTES,
             &Attributes,
             &Variable,
             &VariableSize
             );
  if (!Context->LegacyDevOrderUsable) {
    if (Variable != NULL) {
      FreePool (Variable);
    }

    Status = LogPrint (
               Logger,
               L"%s: LegacyDevOrder is not an ordering prerequisite; "
               L"status=0x%016lx (%s); "
               L"NCV_BOOT_LEGACY_DEV_ORDER_FALLBACK=LIVE_BBS_INDEX_ORDER\r\n",
               Label,
               (UINT64)Status,
               EfiStatusName (Status)
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    return FlushResult ? LogFlush (Logger) : EFI_SUCCESS;
  }

  Match = (BOOLEAN)(
    !EFI_ERROR (Status) &&
    Context->LegacyDevOrderUsable &&
    (Variable != NULL) &&
    (VariableSize == Context->LegacyDevOrderSize) &&
    (Attributes == Context->LegacyDevOrderAttributes) &&
    (LegacyBootCrc32 (Variable, VariableSize) ==
     Context->LegacyDevOrderCrc32) &&
    (CompareMem (
       Variable,
       Context->LegacyDevOrderData,
       VariableSize
       ) == 0)
    );
  if (Variable != NULL) {
    FreePool (Variable);
  }

  if (!Match) {
    LogPrint (
      Logger,
      L"%s failed: pinned LegacyDevOrder changed or became unreadable; "
      L"status=0x%016lx (%s) attributes=0x%08x size=0x%lx\r\n",
      Label,
      (UINT64)Status,
      EfiStatusName (Status),
      Attributes,
      (UINT64)VariableSize
      );
    if (FlushResult) {
      LogFlush (Logger);
    }

    return EFI_ABORTED;
  }

  Status = LogPrint (
             Logger,
             L"%s passed: LegacyDevOrder attributes=0x%08x size=0x%lx "
             L"CRC32=0x%08x unchanged\r\n",
             Label,
             Context->LegacyDevOrderAttributes,
             (UINT64)Context->LegacyDevOrderSize,
             Context->LegacyDevOrderCrc32
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return FlushResult ? LogFlush (Logger) : EFI_SUCCESS;
}

STATIC
VOID
LegacyBootReleaseCandidate (
  IN OUT LEGACY_BOOT_BOOT_CANDIDATE  *Candidate
  )
{
  if (Candidate == NULL) {
    return;
  }

  if (Candidate->Description != NULL) {
    FreePool (Candidate->Description);
  }

  if (Candidate->DevicePath != NULL) {
    FreePool (Candidate->DevicePath);
  }

  if (Candidate->OptionalData != NULL) {
    FreePool (Candidate->OptionalData);
  }

  ZeroMem (Candidate, sizeof (*Candidate));
}

STATIC
EFI_STATUS
LegacyBootValidateDevicePathList (
  IN CONST UINT8  *Path,
  IN UINTN        PathSize
  )
{
  UINTN   Offset;
  UINT16  NodeLength;
  UINT8   Type;
  UINT8   SubType;
  BOOLEAN SawEndEntire;

  if ((Path == NULL) || (PathSize < LEGACY_BOOT_DEVICE_PATH_END_BYTES)) {
    return EFI_COMPROMISED_DATA;
  }

  Offset       = 0;
  SawEndEntire = FALSE;
  while (Offset < PathSize) {
    if ((PathSize - Offset) < sizeof (EFI_DEVICE_PATH_PROTOCOL)) {
      return EFI_COMPROMISED_DATA;
    }

    Type       = Path[Offset];
    SubType    = Path[Offset + 1U];
    NodeLength = LegacyBootReadLe16 (Path + Offset + 2U);
    if ((NodeLength < sizeof (EFI_DEVICE_PATH_PROTOCOL)) ||
        (NodeLength > (PathSize - Offset)))
    {
      return EFI_COMPROMISED_DATA;
    }

    Offset += NodeLength;
    if (Type == END_DEVICE_PATH_TYPE) {
      if ((NodeLength != LEGACY_BOOT_DEVICE_PATH_END_BYTES) ||
          ((SubType != END_INSTANCE_DEVICE_PATH_SUBTYPE) &&
           (SubType != END_ENTIRE_DEVICE_PATH_SUBTYPE)))
      {
        return EFI_COMPROMISED_DATA;
      }

      if (SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE) {
        SawEndEntire = TRUE;
        if (Offset != PathSize) {
          return EFI_COMPROMISED_DATA;
        }
      }
    }
  }

  return SawEndEntire ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

STATIC
EFI_STATUS
LegacyBootValidateExactBbsDevicePath (
  IN  CONST UINT8  *Path,
  IN  UINTN        PathSize,
  OUT UINT16       *DeviceType,
  OUT UINT16       *StatusFlag,
  OUT CHAR8        *Description,
  IN  UINTN        DescriptionBytes
  )
{
  EFI_STATUS  Status;
  UINT16      NodeLength;
  UINTN       StringBytes;
  UINTN       Index;

  if ((Path == NULL) || (DeviceType == NULL) || (StatusFlag == NULL) ||
      (Description == NULL) || (DescriptionBytes == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Description[0] = '\0';
  Status = LegacyBootValidateDevicePathList (Path, PathSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Path[0] != BBS_DEVICE_PATH) || (Path[1] != BBS_BBS_DP)) {
    return EFI_UNSUPPORTED;
  }

  if (PathSize < (sizeof (BBS_BBS_DEVICE_PATH) +
                  LEGACY_BOOT_DEVICE_PATH_END_BYTES))
  {
    return EFI_COMPROMISED_DATA;
  }

  NodeLength = LegacyBootReadLe16 (Path + 2U);
  if ((NodeLength < sizeof (BBS_BBS_DEVICE_PATH)) ||
      ((UINTN)NodeLength + LEGACY_BOOT_DEVICE_PATH_END_BYTES != PathSize) ||
      (Path[NodeLength] != END_DEVICE_PATH_TYPE) ||
      (Path[NodeLength + 1U] != END_ENTIRE_DEVICE_PATH_SUBTYPE) ||
      (LegacyBootReadLe16 (Path + NodeLength + 2U) !=
       LEGACY_BOOT_DEVICE_PATH_END_BYTES))
  {
    return EFI_COMPROMISED_DATA;
  }

  StringBytes = NodeLength - LEGACY_BOOT_BBS_NODE_FIXED_BYTES;
  if ((StringBytes == 0) ||
      (Path[LEGACY_BOOT_BBS_NODE_FIXED_BYTES + StringBytes - 1U] != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = 0; Index + 1U < StringBytes; ++Index) {
    if (Path[LEGACY_BOOT_BBS_NODE_FIXED_BYTES + Index] == 0) {
      return EFI_COMPROMISED_DATA;
    }
  }

  if (StringBytes > DescriptionBytes) {
    return EFI_BAD_BUFFER_SIZE;
  }

  for (Index = 0; Index < StringBytes; ++Index) {
    Description[Index] =
      (CHAR8)Path[LEGACY_BOOT_BBS_NODE_FIXED_BYTES + Index];
  }

  *DeviceType = LegacyBootReadLe16 (Path + 4U);
  *StatusFlag = LegacyBootReadLe16 (Path + 6U);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS __attribute__((noinline))
LegacyBootParseLegacyBootOption (
  IN  UINT16                  Number,
  IN  CONST UINT8             *Variable,
  IN  UINTN                   VariableSize,
  OUT LEGACY_BOOT_BOOT_CANDIDATE   *Candidate
  )
{
  EFI_STATUS  Status;
  UINTN       DescriptionOffset;
  UINTN       DescriptionCharacters;
  UINTN       DescriptionBytes;
  UINTN       FilePathOffset;
  UINTN       FilePathSize;
  UINTN       OptionalOffset;
  UINTN       Index;
  BOOLEAN     FoundTerminator;

  if ((Variable == NULL) || (Candidate == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Candidate, sizeof (*Candidate));
  Candidate->Number = Number;
  if (VariableSize < (LEGACY_BOOT_LOAD_OPTION_FIXED_BYTES + sizeof (CHAR16))) {
    return EFI_COMPROMISED_DATA;
  }

  Candidate->Attributes = LegacyBootReadLe32 (Variable);
  Candidate->IsActive   = (BOOLEAN)(
                                    (Candidate->Attributes &
                                     LOAD_OPTION_ACTIVE) != 0
                                    );
  FilePathSize = LegacyBootReadLe16 (Variable + sizeof (UINT32));

  DescriptionOffset     = LEGACY_BOOT_LOAD_OPTION_FIXED_BYTES;
  DescriptionCharacters = 0;
  FoundTerminator       = FALSE;
  while ((DescriptionOffset +
          ((DescriptionCharacters + 1U) * sizeof (CHAR16))) <= VariableSize)
  {
    if (LegacyBootReadLe16 (
          Variable + DescriptionOffset +
          (DescriptionCharacters * sizeof (CHAR16))
          ) == 0)
    {
      FoundTerminator = TRUE;
      break;
    }

    ++DescriptionCharacters;
  }

  if (!FoundTerminator ||
      (DescriptionCharacters > ((MAX_UINTN / sizeof (CHAR16)) - 1U)))
  {
    return EFI_COMPROMISED_DATA;
  }

  DescriptionBytes = (DescriptionCharacters + 1U) * sizeof (CHAR16);
  if (DescriptionOffset > (MAX_UINTN - DescriptionBytes)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  FilePathOffset = DescriptionOffset + DescriptionBytes;
  if ((FilePathSize == 0) || (FilePathOffset > VariableSize) ||
      (FilePathSize > (VariableSize - FilePathOffset)))
  {
    return EFI_COMPROMISED_DATA;
  }

  OptionalOffset = FilePathOffset + FilePathSize;
  Status = LegacyBootValidateDevicePathList (
             Variable + FilePathOffset,
             FilePathSize
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Variable[FilePathOffset] != BBS_DEVICE_PATH) ||
      (Variable[FilePathOffset + 1U] != BBS_BBS_DP))
  {
    return EFI_UNSUPPORTED;
  }

  Candidate->Description = AllocateZeroPool (DescriptionBytes);
  Candidate->DevicePath  = AllocateZeroPool (FilePathSize);
  Candidate->OptionalDataSize = VariableSize - OptionalOffset;
  if (Candidate->OptionalDataSize != 0) {
    Candidate->OptionalData = AllocateZeroPool (Candidate->OptionalDataSize);
  }

  if ((Candidate->Description == NULL) || (Candidate->DevicePath == NULL) ||
      ((Candidate->OptionalDataSize != 0) &&
       (Candidate->OptionalData == NULL)))
  {
    LegacyBootReleaseCandidate (Candidate);
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index <= DescriptionCharacters; ++Index) {
    Candidate->Description[Index] = LegacyBootReadLe16 (
                                      Variable + DescriptionOffset +
                                      (Index * sizeof (CHAR16))
                                      );
  }

  CopyMem (
    Candidate->DevicePath,
    Variable + FilePathOffset,
    FilePathSize
    );
  if (Candidate->OptionalDataSize != 0) {
    CopyMem (
      Candidate->OptionalData,
      Variable + OptionalOffset,
      Candidate->OptionalDataSize
      );
  }

  Candidate->DescriptionBytes = DescriptionBytes;
  Candidate->DevicePathSize   = FilePathSize;
  Status = LegacyBootValidateExactBbsDevicePath (
             Candidate->DevicePath,
             Candidate->DevicePathSize,
             &Candidate->BbsDeviceType,
             &Candidate->BbsStatusFlag,
             Candidate->BbsDescription,
             sizeof (Candidate->BbsDescription)
             );
  if (EFI_ERROR (Status)) {
    LegacyBootReleaseCandidate (Candidate);
    return Status;
  }

  Candidate->IsLegacy          = TRUE;
  Candidate->DevicePathCrc32   = LegacyBootCrc32 (
                                   Candidate->DevicePath,
                                   Candidate->DevicePathSize
                                   );
  Candidate->OptionalDataCrc32 = LegacyBootCrc32 (
                                   Candidate->OptionalData,
                                   Candidate->OptionalDataSize
                                   );

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
LegacyBootLogHexBytes (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Label,
  IN CONST UINT8   *Bytes OPTIONAL,
  IN UINTN         ByteCount
  )
{
  CHAR16      Line[96];
  UINTN       Offset;
  UINTN       Chunk;
  UINTN       Index;
  UINTN       Used;
  UINTN       Added;
  EFI_STATUS  Status;

  if ((Logger == NULL) || (Label == NULL) ||
      ((ByteCount != 0) && (Bytes == NULL)))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (ByteCount == 0) {
    return LogPrint (Logger, L"      %s: <empty>\r\n", Label);
  }

  Status = EFI_SUCCESS;
  for (Offset = 0; Offset < ByteCount; Offset += Chunk) {
    Chunk = ByteCount - Offset;
    if (Chunk > 16U) {
      Chunk = 16U;
    }

    Used = UnicodeSPrint (
             Line,
             sizeof (Line),
             L"      %s[%04lx]:",
             Label,
             (UINT64)Offset
             );
    for (Index = 0; Index < Chunk; ++Index) {
      if (Used >= (ARRAY_SIZE (Line) - 4U)) {
        return EFI_BAD_BUFFER_SIZE;
      }

      Added = UnicodeSPrint (
                Line + Used,
                (ARRAY_SIZE (Line) - Used) * sizeof (CHAR16),
                L" %02x",
                Bytes[Offset + Index]
                );
      Used += Added;
    }

    UnicodeSPrint (
      Line + Used,
      (ARRAY_SIZE (Line) - Used) * sizeof (CHAR16),
      L"\r\n"
      );
    Status = LogPrint (Logger, L"%s", Line);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return Status;
}

STATIC
EFI_STATUS __attribute__((noinline))
LegacyBootLogBootCandidate (
  IN APP_LOGGER                  *Logger,
  IN CONST LEGACY_BOOT_BOOT_CANDIDATE *Candidate
  )
{
  EFI_STATUS  Status;
  CHAR16      *PathText;
  CHAR8       BbsDescriptionDisplay[
                LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U
                ];

  if ((Logger == NULL) || (Candidate == NULL) || !Candidate->IsLegacy) {
    return EFI_INVALID_PARAMETER;
  }

  PathText = ConvertDevicePathToText (
               (CONST EFI_DEVICE_PATH_PROTOCOL *)Candidate->DevicePath,
               FALSE,
               FALSE
               );
  ZeroMem (BbsDescriptionDisplay, sizeof (BbsDescriptionDisplay));
  LegacyBootSanitizeAsciiForLog (
    Candidate->BbsDescription,
    BbsDescriptionDisplay,
    sizeof (BbsDescriptionDisplay)
    );
  Status = LogPrint (
             Logger,
             L"  Boot%04x active=%s attributes=0x%08x description=\"%s\"\r\n",
             Candidate->Number,
             Candidate->IsActive ? L"yes" : L"no",
             Candidate->Attributes,
             Candidate->Description
             );
  if (!EFI_ERROR (Status)) {
    Status = LogPrint (
               Logger,
               L"      complete device path: %s\r\n",
               (PathText != NULL) ? PathText : L"<conversion failed>"
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = LogPrint (
               Logger,
               L"      BBS type=0x%04x status=0x%04x string=\"%a\" "
               L"path-size=0x%lx path-CRC32=0x%08x\r\n",
               Candidate->BbsDeviceType,
               Candidate->BbsStatusFlag,
               BbsDescriptionDisplay,
               (UINT64)Candidate->DevicePathSize,
               Candidate->DevicePathCrc32
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = LogPrint (
               Logger,
               L"      OptionalData size=0x%lx CRC32=0x%08x "
               L"(opaque; not interpreted)\r\n",
               (UINT64)Candidate->OptionalDataSize,
               Candidate->OptionalDataCrc32
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = LegacyBootLogHexBytes (
               Logger,
               L"BBS-path",
               Candidate->DevicePath,
               Candidate->DevicePathSize
               );
  }

  if (PathText != NULL) {
    FreePool (PathText);
  }

  return Status;
}

STATIC
BOOLEAN
LegacyBootBootOptionStatusIsSkippable (
  IN EFI_STATUS  Status
  )
{
  switch (Status) {
    case EFI_NOT_FOUND:
    case EFI_UNSUPPORTED:
    case EFI_COMPROMISED_DATA:
    case EFI_BAD_BUFFER_SIZE:
    case EFI_BUFFER_TOO_SMALL:
      return TRUE;
    default:
      return FALSE;
  }
}

// Retain the reviewed GCC LTO call boundaries: discovery is inlined, while
// its larger parser/logger helpers and BBS scratch arrays stay out of Boot.
// The production disassembly check remains the authority; do not add padding.
STATIC
inline EFI_STATUS __attribute__((always_inline))
LegacyBootDiscoverBootCandidate (
  IN  APP_LOGGER                 *Logger,
  IN  CONST CHAR16               *LegacyOptionDescription,
  IN  CONST CHAR16               *ExcludeDescription,
  OUT LEGACY_BOOT_BOOT_CANDIDATE      *Selected
  )
{
  EFI_STATUS              Status;
  EFI_STATUS              LogStatus;
  EFI_STATUS              EntryStatus;
  UINT8                   *BootOrder;
  UINTN                   BootOrderSize;
  UINT32                  BootOrderAttributes;
  UINTN                   BootOrderCount;
  UINTN                   OrderIndex;
  UINTN                   PreviousIndex;
  UINT16                  OptionNumber;
  CHAR16                  OptionName[9];
  UINT8                   *OptionVariable;
  UINTN                   OptionVariableSize;
  UINT32                  OptionVariableAttributes;
  LEGACY_BOOT_BOOT_CANDIDATE   Candidate;
  UINTN                   MatchCount;
  BOOLEAN                 GenericDisk;
  BOOLEAN                 RequestedDescription;
  BOOLEAN                 ExcludedDescription;
  BOOLEAN                 SelectedDescription;

  if ((Logger == NULL) || (Selected == NULL) ||
      (LegacyOptionDescription == NULL) || (LegacyOptionDescription[0] == L'\0') ||
      (ExcludeDescription == NULL) || (ExcludeDescription[0] == L'\0')) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Selected, sizeof (*Selected));
  BootOrder           = NULL;
  BootOrderSize       = 0;
  BootOrderAttributes = 0;
  LogPrint (
    Logger,
    L"\r\n=== legacy boot-target discovery read-only BootOrder/Boot#### discovery ===\r\n"
    );
  Status = LegacyBootReadVariableBounded (
             L"BootOrder",
             &gEfiGlobalVariableGuid,
             LEGACY_BOOT_BOOT_ORDER_MAX_BYTES,
             &BootOrderAttributes,
             &BootOrder,
             &BootOrderSize
             );
  LogPrint (
    Logger,
    L"BootOrder GetVariable result=0x%016lx (%s) attributes=0x%08x "
    L"size=0x%lx\r\n",
    (UINT64)Status,
    EfiStatusName (Status),
    BootOrderAttributes,
    (UINT64)BootOrderSize
    );
  if (EFI_ERROR (Status)) {
    LogFlush (Logger);
    return Status;
  }

  if (((BootOrderSize % sizeof (UINT16)) != 0) ||
      (BootOrderSize == 0))
  {
    FreePool (BootOrder);
    LogPrint (Logger, L"BootOrder has a malformed packed UINT16 size\r\n");
    LogFlush (Logger);
    return EFI_COMPROMISED_DATA;
  }

  BootOrderCount = BootOrderSize / sizeof (UINT16);
  MatchCount     = 0;
  Status         = EFI_NOT_FOUND;
  for (OrderIndex = 0; OrderIndex < BootOrderCount; ++OrderIndex) {
    OptionNumber = LegacyBootReadLe16 (
                     BootOrder + (OrderIndex * sizeof (UINT16))
                     );
    for (PreviousIndex = 0; PreviousIndex < OrderIndex; ++PreviousIndex) {
      if (OptionNumber == LegacyBootReadLe16 (
                            BootOrder +
                            (PreviousIndex * sizeof (UINT16))
                            ))
      {
        LogPrint (
          Logger,
          L"BootOrder contains duplicate Boot%04x; refusing ambiguous order\r\n",
          OptionNumber
          );
        Status = EFI_COMPROMISED_DATA;
        goto Done;
      }
    }

    UnicodeSPrint (OptionName, sizeof (OptionName), L"Boot%04x", OptionNumber);
    OptionVariable           = NULL;
    OptionVariableSize       = 0;
    OptionVariableAttributes = 0;
    EntryStatus = LegacyBootReadVariableBounded (
                    OptionName,
                    &gEfiGlobalVariableGuid,
                    LEGACY_BOOT_BOOT_OPTION_MAX_BYTES,
                    &OptionVariableAttributes,
                    &OptionVariable,
                    &OptionVariableSize
                    );
    if (EFI_ERROR (EntryStatus)) {
      LogStatus = LogPrint (
                    Logger,
                    L"  Boot%04x SKIPPED: GetVariable status=0x%016lx "
                    L"(%s)\r\n",
                    OptionNumber,
                    (UINT64)EntryStatus,
                    EfiStatusName (EntryStatus)
                    );
      if (EFI_ERROR (LogStatus)) {
        Status = LogStatus;
        goto Done;
      }

      if (LegacyBootBootOptionStatusIsSkippable (EntryStatus)) {
        continue;
      }

      Status = EntryStatus;
      goto Done;
    }

    ZeroMem (&Candidate, sizeof (Candidate));
    EntryStatus = LegacyBootParseLegacyBootOption (
                    OptionNumber,
                    OptionVariable,
                    OptionVariableSize,
                    &Candidate
                    );
    FreePool (OptionVariable);
    if (EFI_ERROR (EntryStatus)) {
      LogStatus = LogPrint (
                    Logger,
                    L"  Boot%04x SKIPPED: strict EFI_LOAD_OPTION status="
                    L"0x%016lx (%s)\r\n",
                    OptionNumber,
                    (UINT64)EntryStatus,
                    EfiStatusName (EntryStatus)
                    );
      if (EFI_ERROR (LogStatus)) {
        Status = LogStatus;
        goto Done;
      }

      if (LegacyBootBootOptionStatusIsSkippable (EntryStatus)) {
        continue;
      }

      Status = EntryStatus;
      goto Done;
    }

    LogStatus = LegacyBootLogBootCandidate (Logger, &Candidate);
    if (EFI_ERROR (LogStatus)) {
      LegacyBootReleaseCandidate (&Candidate);
      Status = LogStatus;
      goto Done;
    }

    GenericDisk = (BOOLEAN)(
      Candidate.IsActive &&
      ((Candidate.BbsDeviceType == BBS_TYPE_HARDDRIVE) ||
       (Candidate.BbsDeviceType == BBS_TYPE_USB))
      );
    RequestedDescription = (BOOLEAN)(
      GenericDisk &&
      LegacyBootUnicodeEqualsInsensitive (
        Candidate.Description,
        LegacyOptionDescription
        )
      );
    ExcludedDescription = (BOOLEAN)(
      GenericDisk &&
      LegacyBootUnicodeContainsInsensitive (
        Candidate.Description,
        ExcludeDescription
        )
      );
    SelectedDescription = (BOOLEAN)(
      GenericDisk &&
      RequestedDescription &&
      !ExcludedDescription
      );
    if (GenericDisk) {
      LogStatus = LogPrint (
                    Logger,
                    L"      Configured Boot#### selector: requested=%s excluded=%s selected=%s\r\n",
                    RequestedDescription ? L"yes" : L"no",
                    ExcludedDescription ? L"yes" : L"no",
                    SelectedDescription ? L"yes" : L"no"
                    );
      if (EFI_ERROR (LogStatus)) {
        LegacyBootReleaseCandidate (&Candidate);
        Status = LogStatus;
        goto Done;
      }
    }

    if (SelectedDescription)
    {
      ++MatchCount;
      if (MatchCount == 1U) {
        CopyMem (Selected, &Candidate, sizeof (Candidate));
        ZeroMem (&Candidate, sizeof (Candidate));
      }
    }

    LegacyBootReleaseCandidate (&Candidate);
  }

  if (MatchCount != 1U) {
    LogPrint (
      Logger,
      L"Active generic hard-drive/USB Boot#### candidate count with configured "
      L"description and no configured exclusion=%u; exactly one is required\r\n",
      MatchCount
      );
    Status = (MatchCount == 0) ? EFI_NOT_FOUND : EFI_ABORTED;
    goto Done;
  }

  Status = EFI_SUCCESS;

Done:
  FreePool (BootOrder);
  if (EFI_ERROR (Status)) {
    LegacyBootReleaseCandidate (Selected);
  }

  LogStatus = LogFlush (Logger);
  if (!EFI_ERROR (Status) && EFI_ERROR (LogStatus)) {
    Status = LogStatus;
    LegacyBootReleaseCandidate (Selected);
  }

  return Status;
}

STATIC
EFI_STATUS
LegacyBootValidateFirmwareBbsRange (
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  IN BOOLEAN                           RequireWritable
  )
{
  EFI_STATUS           Status;
  MEMORY_MAP_SNAPSHOT  MemoryMap;
  BOOLEAN              Valid;

  if ((Context == NULL) || (Context->FirmwareBbsTable == NULL) ||
      (Context->BbsTableBytes == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&MemoryMap, sizeof (MemoryMap));
  Status = MemoryMapCapture (&MemoryMap);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (RequireWritable) {
    Valid = MemoryMapRangeIsWritable (
              &MemoryMap,
              (UINTN)Context->FirmwareBbsTable,
              Context->BbsTableBytes
              );
  } else {
    Valid = MemoryMapRangeIsReadable (
              &MemoryMap,
              (UINTN)Context->FirmwareBbsTable,
              Context->BbsTableBytes,
              NULL
              );
  }

  MemoryMapRelease (&MemoryMap);
  return Valid ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}

STATIC
EFI_STATUS
LegacyBootLogBbsEntry (
  IN  APP_LOGGER                 *Logger,
  IN  CONST MEMORY_MAP_SNAPSHOT  *MemoryMap,
  IN  UINTN                      Index,
  IN  CONST BBS_TABLE            *Entry,
  OUT CHAR8                      *Manufacturer,
  IN  UINTN                      ManufacturerBytes,
  OUT CHAR8                      *Description,
  IN  UINTN                      DescriptionBytes,
  OUT LEGACY_BOOT_FAR_STRING_RESULT   *ManufacturerReadResult OPTIONAL,
  OUT LEGACY_BOOT_FAR_STRING_RESULT   *DescriptionReadResult OPTIONAL
  )
{
  CONST UINT8              *Bytes;
  UINT16                   StatusFlags;
  LEGACY_BOOT_FAR_STRING_RESULT ManufacturerResult;
  LEGACY_BOOT_FAR_STRING_RESULT DescriptionResult;
  EFI_STATUS               Status;
  CHAR8                    ManufacturerDisplay[
                             LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U
                             ];
  CHAR8                    DescriptionDisplay[
                             LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U
                             ];

  if ((Logger == NULL) || (MemoryMap == NULL) || (Entry == NULL) ||
      (Manufacturer == NULL) || (ManufacturerBytes == 0) ||
      (Description == NULL) || (DescriptionBytes == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Bytes       = (CONST UINT8 *)Entry;
  StatusFlags = LegacyBootReadLe16 (
                  Bytes + OFFSET_OF (BBS_TABLE, StatusFlags)
                  );
  ManufacturerResult = LegacyBootCopyManufacturer (
                         MemoryMap, Entry, Manufacturer, ManufacturerBytes);
  DescriptionResult = LegacyBootCopyFarAsciiString (
                        MemoryMap,
                        LegacyBootReadLe16 (
                          Bytes + OFFSET_OF (BBS_TABLE, DescStringSegment)
                          ),
                        LegacyBootReadLe16 (
                          Bytes + OFFSET_OF (BBS_TABLE, DescStringOffset)
                          ),
                        Description,
                        DescriptionBytes
                        );
  if (ManufacturerReadResult != NULL) {
    *ManufacturerReadResult = ManufacturerResult;
  }

  if (DescriptionReadResult != NULL) {
    *DescriptionReadResult = DescriptionResult;
  }

  ZeroMem (ManufacturerDisplay, sizeof (ManufacturerDisplay));
  ZeroMem (DescriptionDisplay, sizeof (DescriptionDisplay));
  LegacyBootSanitizeAsciiForLog (
    Manufacturer,
    ManufacturerDisplay,
    sizeof (ManufacturerDisplay)
    );
  LegacyBootSanitizeAsciiForLog (
    Description,
    DescriptionDisplay,
    sizeof (DescriptionDisplay)
    );

  Status = LogPrint (
             Logger,
             L"  BBS[%u] priority=0x%04x type=0x%04x status=0x%04x "
             L"BDF(no segment)=%02x:%02x.%x class=%02x:%02x\r\n",
             Index,
             LegacyBootReadLe16 (
               Bytes + OFFSET_OF (BBS_TABLE, BootPriority)
               ),
             LegacyBootReadLe16 (
               Bytes + OFFSET_OF (BBS_TABLE, DeviceType)
               ),
             StatusFlags,
             LegacyBootReadLe32 (Bytes + OFFSET_OF (BBS_TABLE, Bus)),
             LegacyBootReadLe32 (Bytes + OFFSET_OF (BBS_TABLE, Device)),
             LegacyBootReadLe32 (Bytes + OFFSET_OF (BBS_TABLE, Function)),
             Bytes[OFFSET_OF (BBS_TABLE, Class)],
             Bytes[OFFSET_OF (BBS_TABLE, SubClass)]
             );
  if (!EFI_ERROR (Status) && StatusFlags == 0 && LegacyStorageRomMatch (Entry, NULL)) {
    Status = LogPrint (Logger, L"      NCV_BBS_RESIDENT_ROM_VERIFIED PCI/PnP/handler/strings corroborated; zero status accepted\r\n");
  }
  if (!EFI_ERROR (Status) && StatusFlags == 0 && LegacyFirmwareDiskMatch (Entry)) {
    Status = LogPrint (Logger, L"      NCV_BBS_FIRMWARE_AHCI_VERIFIED PCI identity and stable BIOS handler/strings corroborated; zero status accepted\r\n");
  }
  if (!EFI_ERROR (Status)) {
    Status = LogPrint (
               Logger,
               L"      manufacturer[%s]=\"%a\" description[%s]=\"%a\"\r\n",
               LegacyBootFarStringResultName (ManufacturerResult),
               ManufacturerDisplay,
               LegacyBootFarStringResultName (DescriptionResult),
               DescriptionDisplay
               );
  }

  return Status;
}

STATIC
EFI_STATUS __attribute__((noinline))
LegacyBootLogAndCorrelateLiveBbs (
  IN     APP_LOGGER                    *Logger,
  IN     CONST MEMORY_MAP_SNAPSHOT     *MemoryMap,
  IN     UINT32                        TargetBus,
  IN     UINT32                        TargetDevice,
  IN     UINT32                        TargetFunction,
  IN     CONST CHAR16                  *TargetDescription,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT    *Context
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  UINTN       MatchCount;
  UINTN       IdentityMatches;
  BBS_TABLE   Entry;
  CHAR8       Manufacturer[LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U];
  CHAR8       Description[LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U];
  BOOLEAN     BdfMatch;
  BOOLEAN     StorageMatch;
  BOOLEAN     TextMatch;
  BOOLEAN     ValidEntry;
  LEGACY_BOOT_FAR_STRING_RESULT ManufacturerResult;
  LEGACY_BOOT_FAR_STRING_RESULT DescriptionResult;

  if ((Logger == NULL) || (MemoryMap == NULL) ||
      (Context == NULL) ||
      (Context->BbsDiscoverySnapshot == NULL) ||
      (Context->BbsDiscoveryStrings == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  MatchCount = 0;
  IdentityMatches = 0;
  for (Index = 0; Index < Context->BbsCount; ++Index) {
    CopyMem (
      &Entry,
      (CONST UINT8 *)Context->FirmwareBbsTable +
      (Index * sizeof (BBS_TABLE)),
      sizeof (Entry)
      );
    ZeroMem (Manufacturer, sizeof (Manufacturer));
    ZeroMem (Description, sizeof (Description));
    Status = LegacyBootLogBbsEntry (
               Logger,
               MemoryMap,
               Index,
               &Entry,
               Manufacturer,
               sizeof (Manufacturer),
               Description,
               sizeof (Description),
               &ManufacturerResult,
               &DescriptionResult
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Context->BbsDiscoveryStrings[Index].ManufacturerReadResult =
      (UINT8)ManufacturerResult;
    Context->BbsDiscoveryStrings[Index].DescriptionReadResult =
      (UINT8)DescriptionResult;
    CopyMem (
      Context->BbsDiscoveryStrings[Index].Manufacturer,
      Manufacturer,
      sizeof (Context->BbsDiscoveryStrings[Index].Manufacturer)
      );
    CopyMem (
      Context->BbsDiscoveryStrings[Index].Description,
      Description,
      sizeof (Context->BbsDiscoveryStrings[Index].Description)
      );

    BdfMatch = (BOOLEAN)(
      (Entry.Bus == TargetBus) &&
      (Entry.Device == TargetDevice) &&
      (Entry.Function == TargetFunction)
      );
    StorageMatch = (BOOLEAN)(Entry.DeviceType == BBS_HARDDISK);
    TextMatch = (BOOLEAN)((TargetDescription == NULL) || (TargetDescription[0] == L'\0') ||
      LegacyBootAsciiEqualsUnicodeInsensitive (Description, TargetDescription));
    ValidEntry = LegacyBootSelectedDiskCanAttempt (&Entry);
    if (BdfMatch && StorageMatch && TextMatch) {
      ++IdentityMatches;
      if (!ValidEntry) {
        LogPrint (Logger, L"      Matching disk rejected: %s\r\n", LegacyBootBbsEligibilityReason (&Entry));
      }
    }
    if (BdfMatch && StorageMatch && TextMatch && ValidEntry) {
      ++MatchCount;
      Context->LiveBbsIndex = (UINT16)Index;
      CopyMem (&Context->LiveBbsEntry, &Entry, sizeof (Entry));
      CopyMem (
        Context->LiveManufacturer,
        Manufacturer,
        sizeof (Context->LiveManufacturer)
        );
      CopyMem (
        Context->LiveDescription,
        Description,
        sizeof (Context->LiveDescription)
        );
    }

    Status = LogPrint (
               Logger,
               L"      target predicates: BDF=%s storage/type=%s "
               L"configured-text=%s transaction-eligible=%s candidate=%s\r\n",
               BdfMatch ? L"yes" : L"no",
               StorageMatch ? L"yes" : L"no",
               TextMatch ? L"yes" : L"no",
               ValidEntry ? L"yes" : L"no",
               (BdfMatch && StorageMatch && TextMatch && ValidEntry) ?
                 L"yes" : L"no"
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  LogPrint (
    Logger,
    L"Live BBS target match count=%u for %02x:%02x.%x and token \"%s\"\r\n",
    MatchCount,
    TargetBus,
    TargetDevice,
    TargetFunction,
    (TargetDescription != NULL) ? TargetDescription : L"<BDF-only>"
    );
  if (IdentityMatches > 1) {
    LogPrint (Logger, L"Multiple live disk identities match the configured controller/name; refusing ambiguous selection.\r\n");
    return EFI_ABORTED;
  }
  if (MatchCount == 0) {
    if (IdentityMatches == 1) {
      LogPrint (Logger, L"NCV_BLOCKED_BBS_ELIGIBILITY\r\nThe selected disk was found but its boot eligibility could not be established. See the matching-disk reason above.\r\n");
      return EFI_ACCESS_DENIED;
    }
    return EFI_NOT_FOUND;
  }

  if (MatchCount != 1U) {
    return EFI_ABORTED;
  }

  if (!LegacyBootBbsEntryIsTransactionEligible (&Context->LiveBbsEntry)) {
    Status = LogPrint (Logger,
      L"NCV_DISK_FIRMWARE_WARNING_BBS\r\n"
      L"Firmware boot warning: selected BBS[%u] reports %s; status=0x%04x priority=0x%04x. "
      L"User selection overrides firmware boot policy if all preflight checks pass.\r\n",
      Context->LiveBbsIndex, LegacyBootBbsEligibilityReason (&Context->LiveBbsEntry),
      LegacyBootBbsStatusFlagsRaw (&Context->LiveBbsEntry), Context->LiveBbsEntry.BootPriority);
    if (EFI_ERROR (Status)) { return Status; }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS __attribute__((noinline))
LegacyBootMoveCandidateToContext (
  IN     APP_LOGGER                    *Logger,
  IN OUT LEGACY_BOOT_BOOT_CANDIDATE        *Candidate,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT   *Context
  )
{
  EFI_STATUS  Status;
  UINT16      LiveStatusFlags;

  if ((Logger == NULL) || (Candidate == NULL) || (Context == NULL) ||
      !Candidate->IsLegacy || !Candidate->IsActive ||
      ((Candidate->OptionalDataSize != 0) &&
       (Candidate->OptionalData == NULL)))
  {
    return EFI_INVALID_PARAMETER;
  }

  // The generic boot group and individual disk need not have the same type:
  // some firmware uses BBS(USB) for USB disks listed as BBS_HARDDISK. Only
  // permit that pairing for a uniquely matched, eligible USB-class live disk.
  // Keep the firmware's original path and opaque OptionalData unchanged.
  if ((Context->LiveBbsEntry.DeviceType != BBS_HARDDISK) ||
      !((Candidate->BbsDeviceType == BBS_TYPE_HARDDRIVE) ||
        ((Candidate->BbsDeviceType == BBS_TYPE_USB) &&
         (Context->LiveBbsEntry.Class == 0x0CU) &&
         (Context->LiveBbsEntry.SubClass == 0x03U))) ||
      !LegacyBootSelectedDiskCanAttempt (&Context->LiveBbsEntry))
  {
    LogPrint (
      Logger,
      L"Boot%04x generic disk path cannot be paired with eligible "
      L"live configured BBS[%u]: path-type=0x%04x live-type=0x%04x\r\n",
      Candidate->Number,
      Context->LiveBbsIndex,
      Candidate->BbsDeviceType,
      Context->LiveBbsEntry.DeviceType
      );
    return EFI_COMPROMISED_DATA;
  }

  Context->BootOptionNumber           = Candidate->Number;
  Context->BootOptionAttributes       = Candidate->Attributes;
  Context->BootOptionDescription      = Candidate->Description;
  Context->BootOptionDescriptionBytes = Candidate->DescriptionBytes;
  Context->BootOptionDescriptionCrc32 = LegacyBootCrc32 (
                                          Candidate->Description,
                                          Candidate->DescriptionBytes
                                          );
  Context->BbsDevicePath              = Candidate->DevicePath;
  Context->BbsDevicePathSize          = Candidate->DevicePathSize;
  Context->BbsDevicePathCrc32         = Candidate->DevicePathCrc32;
  Context->BbsDeviceType              = Candidate->BbsDeviceType;
  Context->BbsStatusFlag              = Candidate->BbsStatusFlag;
  CopyMem (
    Context->BbsDescription,
    Candidate->BbsDescription,
    sizeof (Context->BbsDescription)
    );
  Context->LoadOptions                = Candidate->OptionalData;
  Context->LoadOptionsSize            = Candidate->OptionalDataSize;
  Context->OriginalLoadOptionsCrc32   = Candidate->OptionalDataCrc32;
  Context->LoadOptionsCrc32           = Candidate->OptionalDataCrc32;

  Candidate->Description  = NULL;
  Candidate->DevicePath   = NULL;
  Candidate->OptionalData = NULL;

  LiveStatusFlags = LegacyBootReadLe16 (
                      (CONST UINT8 *)&Context->LiveBbsEntry +
                      OFFSET_OF (BBS_TABLE, StatusFlags)
                      );
  if (Context->BbsStatusFlag != LiveStatusFlags) {
    LogPrint (
      Logger,
      L"Diagnostic: Boot%04x BBS path status 0x%04x differs from live "
      L"BBS[%u] status 0x%04x; reference matching does not use this field\r\n",
      Context->BootOptionNumber,
      Context->BbsStatusFlag,
      Context->LiveBbsIndex,
      LiveStatusFlags
      );
  }

  Status = LogPrint (
      Logger,
      L"Selected generic hard-drive Boot%04x; independently selected live "
      L"Configured BBS[%u] %02x:%02x.%x manufacturer=\"%a\" "
      L"description=\"%a\"\r\n",
             Context->BootOptionNumber,
             Context->LiveBbsIndex,
             Context->LiveBbsEntry.Bus,
             Context->LiveBbsEntry.Device,
             Context->LiveBbsEntry.Function,
             Context->LiveManufacturer,
             Context->LiveDescription
             );
  if (!EFI_ERROR (Status)) {
    Status = LogPrint (
               Logger,
               L"Owned copies: BBS path size=0x%lx CRC32=0x%08x; "
               L"opaque OptionalData size=0x%lx original-CRC32=0x%08x "
               L"effective-CRC32=0x%08x byte-identical=yes\r\n",
               (UINT64)Context->BbsDevicePathSize,
               Context->BbsDevicePathCrc32,
               (UINT64)Context->LoadOptionsSize,
               Context->OriginalLoadOptionsCrc32,
               Context->LoadOptionsCrc32
               );
  }

  return Status;
}

inline EFI_STATUS __attribute__((always_inline))
LegacyBootBootTargetDiscover (
  IN  APP_LOGGER                  *Logger,
  IN  EFI_LEGACY_BIOS_PROTOCOL    *LegacyBios,
  IN  UINT32                      TargetBus,
  IN  UINT32                      TargetDevice,
  IN  UINT32                      TargetFunction,
  IN  CONST CHAR16                *TargetDescription,
  IN  CONST CHAR16                *LegacyOptionDescription,
  IN  CONST CHAR16                *ExcludeDescription,
  OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context
  )
{
  EFI_STATUS             Status;
  EFI_STATUS             FlushStatus;
  LEGACY_BOOT_BOOT_CANDIDATE  Candidate;
  UINT16                 HddCount;
  HDD_INFO               *HddInfo;
  UINT16                 BbsCount;
  BBS_TABLE              *BbsTable;
  UINTN                  HddBytes;
  MEMORY_MAP_SNAPSHOT    MemoryMap;

  if ((Logger == NULL) || (LegacyBios == NULL) || (Context == NULL) ||
      (LegacyOptionDescription == NULL) || (LegacyOptionDescription[0] == L'\0') ||
      (ExcludeDescription == NULL) || (ExcludeDescription[0] == L'\0'))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Context->Signature == LEGACY_BOOT_BOOT_TARGET_SIGNATURE) {
    return EFI_ALREADY_STARTED;
  }

  ZeroMem (Context, sizeof (*Context));
  Context->Signature        = LEGACY_BOOT_BOOT_TARGET_SIGNATURE;
  Context->GetBbsInfoStatus = EFI_NOT_READY;
  ZeroMem (&Candidate, sizeof (Candidate));
  ZeroMem (&MemoryMap, sizeof (MemoryMap));

  Status = LegacyBootDiscoverBootCandidate (
             Logger,
             LegacyOptionDescription,
             ExcludeDescription,
             &Candidate
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Logger->File == NULL) || EFI_ERROR (Logger->FileOpenStatus)) {
    LegacyBootReleaseCandidate (&Candidate);
    LogPrint (
      Logger,
      L"Refusing the one legacy boot-target discovery GetBbsInfo call because no writable file "
      L"log is open\r\n"
      );
    return EFI_NOT_READY;
  }

  if (LegacyBios->GetBbsInfo == NULL) {
    LegacyBootReleaseCandidate (&Candidate);
    LogPrint (Logger, L"GetBbsInfo function pointer is NULL\r\n");
    return EFI_UNSUPPORTED;
  }

  LogPrint (
    Logger,
    L"\r\n=== legacy boot-target discovery one-shot native BBS discovery ===\r\n"
    L"CAUTION: GetBbsInfo is a query, but firmware may lazily initialize "
    L"CSM state. BootOrder/Boot#### enumeration and owned candidate copying "
    L"completed before this call.\r\n"
    );
  Status = LogFlush (Logger);
  if (EFI_ERROR (Status)) {
    LegacyBootReleaseCandidate (&Candidate);
    return Status;
  }

  HddCount = 0;
  HddInfo  = NULL;
  BbsCount = 0;
  BbsTable = NULL;
  Context->GetBbsInfoCalled = TRUE;
  Status = LegacyBios->GetBbsInfo (
                         LegacyBios,
                         &HddCount,
                         &HddInfo,
                         &BbsCount,
                         &BbsTable
                         );
  Context->GetBbsInfoStatus = Status;
  LogPrint (
    Logger,
    L"GetBbsInfo one-shot result=0x%016lx (%s), HDD count=%u table=%p, "
    L"BBS count=%u table=%p\r\n",
    (UINT64)Status,
    EfiStatusName (Status),
    HddCount,
    HddInfo,
    BbsCount,
    BbsTable
    );
  FlushStatus = LogFlush (Logger);
  if (EFI_ERROR (FlushStatus)) {
    LegacyBootReleaseCandidate (&Candidate);
    return FlushStatus;
  }

  if (EFI_ERROR (Status)) {
    LegacyBootReleaseCandidate (&Candidate);
    return Status;
  }

  if ((HddCount > MAX_IDE_CONTROLLER) ||
      ((HddCount != 0) && (HddInfo == NULL)) ||
      (BbsCount == 0) ||
      (BbsCount > LEGACY_BOOT_BOOT_TARGET_MAX_BBS_ENTRIES) ||
      (BbsTable == NULL))
  {
    LegacyBootReleaseCandidate (&Candidate);
    LogPrint (
      Logger,
      L"Malformed bounded GetBbsInfo result; HDD count zero is accepted, "
      L"but BBS count must be 1..%u\r\n",
      LEGACY_BOOT_BOOT_TARGET_MAX_BBS_ENTRIES
      );
    LogFlush (Logger);
    return EFI_COMPROMISED_DATA;
  }

  // Some CSM implementations shadow ROMs lazily while answering GetBbsInfo.
  // Recheck after its one and only invocation, before accepting BBS entries.
  Status = LegacyRomGuardRecheckAfterBbs (Logger);
  if (EFI_ERROR (Status)) {
    LegacyBootReleaseCandidate (&Candidate);
    return Status;
  }

  Context->HddCount        = HddCount;
  Context->BbsCount        = BbsCount;
  Context->FirmwareBbsTable = BbsTable;
  Context->BbsTableBytes   = (UINTN)BbsCount * sizeof (BBS_TABLE);

  Status = MemoryMapCapture (&MemoryMap);
  if (EFI_ERROR (Status)) {
    LegacyBootReleaseCandidate (&Candidate);
    return Status;
  }

  HddBytes = (UINTN)HddCount * sizeof (HDD_INFO);
  if (((HddCount != 0) &&
       !MemoryMapRangeIsReadable (
          &MemoryMap,
          (UINTN)HddInfo,
          HddBytes,
          NULL
          )) ||
      !MemoryMapRangeIsReadable (
         &MemoryMap,
         (UINTN)BbsTable,
         Context->BbsTableBytes,
         NULL
         ))
  {
    MemoryMapRelease (&MemoryMap);
    LegacyBootReleaseCandidate (&Candidate);
    LogPrint (Logger, L"GetBbsInfo returned a BBS/HDD range that is not safely readable\r\n");
    LogFlush (Logger);
    return EFI_SECURITY_VIOLATION;
  }

  Context->BbsDiscoverySnapshot = AllocateCopyPool (
                                    Context->BbsTableBytes,
                                    BbsTable
                                    );
  Context->BbsDiscoveryStrings = AllocateZeroPool (
                                   (UINTN)Context->BbsCount *
                                   sizeof (LEGACY_BOOT_BBS_STRING_SNAPSHOT)
                                   );
  if ((Context->BbsDiscoverySnapshot == NULL) ||
      (Context->BbsDiscoveryStrings == NULL))
  {
    MemoryMapRelease (&MemoryMap);
    LegacyBootReleaseCandidate (&Candidate);
    return EFI_OUT_OF_RESOURCES;
  }

  Context->BbsDiscoveryCrc32 = LegacyBootCrc32 (
                                 Context->BbsDiscoverySnapshot,
                                 Context->BbsTableBytes
                                 );
  Context->BbsDiscoveryIdentityCrc32 = LegacyBootBbsIdentityCrc32 (
                                         Context->BbsDiscoverySnapshot,
                                         Context->BbsCount
                                         );
  LogPrint (
    Logger,
    L"Complete read-only BBS discovery snapshot: entries=%u bytes=0x%lx "
    L"raw-CRC32=0x%08x identity-CRC32=0x%08x\r\n",
    Context->BbsCount,
    (UINT64)Context->BbsTableBytes,
    Context->BbsDiscoveryCrc32,
    Context->BbsDiscoveryIdentityCrc32
    );
  Status = LegacyBootLogAndCorrelateLiveBbs (
             Logger,
             &MemoryMap,
             TargetBus,
             TargetDevice,
             TargetFunction,
             TargetDescription,
             Context
             );
  if (!EFI_ERROR (Status)) {
    Context->BbsDiscoveryStringsCrc32 = LegacyBootCrc32 (
                                          Context->BbsDiscoveryStrings,
                                          (UINTN)Context->BbsCount *
                                          sizeof (LEGACY_BOOT_BBS_STRING_SNAPSHOT)
                                          );
    LogPrint (
      Logger,
      L"Bounded discovery manufacturer/description snapshot CRC32=0x%08x\r\n",
      Context->BbsDiscoveryStringsCrc32
      );
  }
  if (!EFI_ERROR (Status) &&
      (CompareMem (
         Context->BbsDiscoverySnapshot,
         Context->FirmwareBbsTable,
         Context->BbsTableBytes
         ) != 0))
  {
    LogPrint (
      Logger,
      L"Live BBS table changed while it was being enumerated; refusing "
      L"an unstable target\r\n"
      );
    Status = EFI_ABORTED;
  }

  if (!EFI_ERROR (Status)) {
    Status = LegacyBootMoveCandidateToContext (
               Logger,
               &Candidate,
               Context
               );
  }

  MemoryMapRelease (&MemoryMap);
  LegacyBootReleaseCandidate (&Candidate);
  if (EFI_ERROR (Status)) {
    LogFlush (Logger);
    return Status;
  }

  Context->DiscoveryComplete = TRUE;
  Status = LegacyBootBootTargetValidateOwnedCopies (Logger, Context);
  if (EFI_ERROR (Status)) {
    Context->DiscoveryComplete = FALSE;
    return Status;
  }

  LogPrint (
    Logger,
    L"legacy boot-target discovery target discovery passed. No BBS priority is journaled for "
    L"mutation until LegacyBootBootTargetJournalAndValidate is called after "
    L"the PCI journal.\r\n"
    );
  Status = LogFlush (Logger);
  if (EFI_ERROR (Status)) {
    Context->DiscoveryComplete = FALSE;
  }

  return Status;
}

EFI_STATUS
LegacyBootBootTargetValidateOwnedCopies (
  IN APP_LOGGER                        *Logger,
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;
  UINT16      DeviceType;
  UINT16      StatusFlag;
  CHAR8       Description[LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U];

  if ((Logger == NULL) || (Context == NULL) ||
      (Context->Signature != LEGACY_BOOT_BOOT_TARGET_SIGNATURE) ||
      !Context->DiscoveryComplete ||
      (Context->BootOptionDescription == NULL) ||
      (Context->BootOptionDescriptionBytes < sizeof (CHAR16)) ||
      ((Context->BootOptionDescriptionBytes % sizeof (CHAR16)) != 0) ||
      (Context->BbsDevicePath == NULL) ||
      (Context->BbsDevicePathSize == 0) ||
      ((Context->LoadOptionsSize != 0) &&
       (Context->LoadOptions == NULL)) ||
      (Context->LoadOptionsSize > MAX_UINT32) ||
      (Context->BbsDiscoverySnapshot == NULL) ||
      (Context->BbsDiscoveryStrings == NULL) ||
      (Context->LiveBbsIndex >= Context->BbsCount))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = LegacyStorageRomVerify (Logger);
  if (EFI_ERROR (Status)) { return Status; }

  if (Context->BootOptionDescription[
        (Context->BootOptionDescriptionBytes / sizeof (CHAR16)) - 1U
        ] != L'\0')
  {
    return EFI_COMPROMISED_DATA;
  }

  if ((LegacyBootCrc32 (
         Context->BootOptionDescription,
         Context->BootOptionDescriptionBytes
         ) != Context->BootOptionDescriptionCrc32) ||
      (LegacyBootCrc32 (
         Context->BbsDevicePath,
         Context->BbsDevicePathSize
         ) != Context->BbsDevicePathCrc32) ||
      (LegacyBootCrc32 (
         Context->LoadOptions,
         Context->LoadOptionsSize
         ) != Context->LoadOptionsCrc32) ||
      (Context->LoadOptionsCrc32 != Context->OriginalLoadOptionsCrc32) ||
      (LegacyBootCrc32 (
         Context->BbsDiscoverySnapshot,
         Context->BbsTableBytes
         ) != Context->BbsDiscoveryCrc32) ||
      (LegacyBootBbsIdentityCrc32 (
         Context->BbsDiscoverySnapshot,
         Context->BbsCount
         ) != Context->BbsDiscoveryIdentityCrc32) ||
      (LegacyBootCrc32 (
         Context->BbsDiscoveryStrings,
         (UINTN)Context->BbsCount *
         sizeof (LEGACY_BOOT_BBS_STRING_SNAPSHOT)
         ) != Context->BbsDiscoveryStringsCrc32))
  {
    LogPrint (Logger, L"legacy boot-target discovery owned-copy CRC validation failed\r\n");
    LogFlush (Logger);
    return EFI_CRC_ERROR;
  }

  ZeroMem (Description, sizeof (Description));
  Status = LegacyBootValidateExactBbsDevicePath (
             Context->BbsDevicePath,
             Context->BbsDevicePathSize,
             &DeviceType,
             &StatusFlag,
             Description,
             sizeof (Description)
             );
  if (EFI_ERROR (Status) ||
      (DeviceType != Context->BbsDeviceType) ||
      (StatusFlag != Context->BbsStatusFlag) ||
      (AsciiStrCmp (Description, Context->BbsDescription) != 0) ||
      ((Context->BootOptionAttributes & LOAD_OPTION_ACTIVE) == 0) ||
      (CompareMem (
         &Context->LiveBbsEntry,
         &Context->BbsDiscoverySnapshot[Context->LiveBbsIndex],
         sizeof (BBS_TABLE)
         ) != 0) ||
      (CompareMem (
         Context->LiveManufacturer,
         Context->BbsDiscoveryStrings[
           Context->LiveBbsIndex
           ].Manufacturer,
         sizeof (Context->LiveManufacturer)
         ) != 0) ||
      (CompareMem (
         Context->LiveDescription,
         Context->BbsDiscoveryStrings[
           Context->LiveBbsIndex
           ].Description,
         sizeof (Context->LiveDescription)
         ) != 0))
  {
    LogPrint (
      Logger,
      L"legacy boot-target discovery owned Boot####/BBS argument structural validation failed: "
      L"0x%016lx (%s)\r\n",
      (UINT64)Status,
      EfiStatusName (Status)
      );
    LogFlush (Logger);
    return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
  }

  Status = LogPrint (
             Logger,
             L"Owned legacy boot-target discovery boot arguments verified: Boot%04x, BBS[%u], "
             L"path-size=0x%lx path-CRC32=0x%08x, OptionalData-size=0x%lx "
             L"OptionalData-CRC32=0x%08x\r\n",
             Context->BootOptionNumber,
             Context->LiveBbsIndex,
             (UINT64)Context->BbsDevicePathSize,
             Context->BbsDevicePathCrc32,
             (UINT64)Context->LoadOptionsSize,
             Context->LoadOptionsCrc32
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return LogFlush (Logger);
}

STATIC
EFI_STATUS __attribute__((noinline))
LegacyBootBuildPriorityPlan (
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context
  )
{
  BOOLEAN  Ranked[LEGACY_BOOT_BOOT_TARGET_MAX_BBS_ENTRIES];
  UINTN    Index;
  UINTN    BestIndex;
  UINT16   BestOriginalPriority;
  UINT16   BestOrderRank;
  UINT16   CandidateOrderRank;
  UINT16   NextPriority;
  BOOLEAN  Found;
  UINTN    Pass;

  if ((Context == NULL) || (Context->BbsJournal == NULL) ||
      (Context->PlannedPriorities == NULL) ||
      (Context->ExpectedPriorities == NULL) ||
      (Context->LiveBbsIndex >= Context->BbsCount) ||
      (Context->LegacyDevOrderUsable &&
       ((Context->LegacyDevOrderRanks == NULL) ||
        (Context->LegacyDevOrderDisabled == NULL))))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Ranked, sizeof (Ranked));
  for (Index = 0; Index < Context->BbsCount; ++Index) {
    Context->ExpectedPriorities[Index] =
      Context->BbsJournal[Index].BootPriority;
    if (LegacyBootBbsIndexIsTransactionEligible (Context, Index))
    {
      Context->PlannedPriorities[Index] = BBS_UNPRIORITIZED_ENTRY;
    } else {
      Context->PlannedPriorities[Index] =
        Context->BbsJournal[Index].BootPriority;
      Ranked[Index] = TRUE;
    }
  }

  if (!LegacyBootBbsIndexIsTransactionEligible (
         Context,
         Context->LiveBbsIndex
         ))
  {
    return EFI_NOT_FOUND;
  }

  Context->PlannedPriorities[Context->LiveBbsIndex] = 0;
  Ranked[Context->LiveBbsIndex]                     = TRUE;
  NextPriority                                      = 1;

  // Pass zero ranks remaining hard disks. Pass one ranks every other eligible
  // entry. A strictly validated LegacyDevOrder supplies the current firmware
  // ordering key (and protects its 0xFF-high-byte disabled rows). When that
  // read-only variable is unavailable or malformed, remaining hard disks use
  // their ascending live BBS indices as the deterministic fallback.
  for (Pass = 0; Pass < 2U; ++Pass) {
    for (;;) {
      Found                = FALSE;
      BestIndex            = 0;
      BestOriginalPriority = MAX_UINT16;
      BestOrderRank        = MAX_UINT16;
      for (Index = 0; Index < Context->BbsCount; ++Index) {
        if (Ranked[Index] ||
            !LegacyBootBbsIndexIsTransactionEligible (Context, Index) ||
            ((Pass == 0) !=
             (Context->BbsJournal[Index].DeviceType == BBS_HARDDISK)))
        {
          continue;
        }

        CandidateOrderRank = Context->LegacyDevOrderUsable ?
                               Context->LegacyDevOrderRanks[Index] :
                               MAX_UINT16;
        if (Context->LegacyDevOrderUsable &&
            (CandidateOrderRank == MAX_UINT16))
        {
          return EFI_COMPROMISED_DATA;
        }

        if (!Found ||
            (Context->LegacyDevOrderUsable &&
             ((CandidateOrderRank < BestOrderRank) ||
              ((CandidateOrderRank == BestOrderRank) &&
               (Index < BestIndex)))) ||
            (!Context->LegacyDevOrderUsable && (Pass == 0) &&
             (Index < BestIndex)) ||
            (!Context->LegacyDevOrderUsable && (Pass != 0) &&
             ((Context->BbsJournal[Index].BootPriority <
               BestOriginalPriority) ||
              ((Context->BbsJournal[Index].BootPriority ==
                BestOriginalPriority) && (Index < BestIndex)))))
        {
          Found                = TRUE;
          BestIndex            = Index;
          BestOriginalPriority =
            Context->BbsJournal[Index].BootPriority;
          BestOrderRank = CandidateOrderRank;
        }
      }

      if (!Found) {
        break;
      }

      if (NextPriority >= BBS_DO_NOT_BOOT_FROM) {
        return EFI_OUT_OF_RESOURCES;
      }

      Context->PlannedPriorities[BestIndex] = NextPriority++;
      Ranked[BestIndex]                     = TRUE;
    }
  }

  // Ineligible rows must be preserved exactly.  If firmware already gave
  // such a non-target row priority zero, the authorized mutation set cannot
  // make selected disk uniquely priority zero without changing a protected row.
  // Fail the plan before any write instead of weakening either invariant.
  for (Index = 0; Index < Context->BbsCount; ++Index) {
    if ((Index != Context->LiveBbsIndex) &&
        (Context->PlannedPriorities[Index] == 0))
    {
      return EFI_ACCESS_DENIED;
    }
  }

  Context->PlannedPriorityCrc32 = LegacyBootBbsPriorityCrc32FromVector (
                                    Context->PlannedPriorities,
                                    Context->BbsCount
                                    );
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
LegacyBootVerifyLiveBbsStrings (
  IN APP_LOGGER                        *Logger,
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  IN CONST CHAR16                      *Label,
  IN BOOLEAN                           FlushResult
  )
{
  EFI_STATUS                  Status;
  MEMORY_MAP_SNAPSHOT         MemoryMap;
  LEGACY_BOOT_BBS_STRING_SNAPSHOT  Current;
  LEGACY_BOOT_FAR_STRING_RESULT    ManufacturerResult;
  LEGACY_BOOT_FAR_STRING_RESULT    DescriptionResult;
  BBS_TABLE                   Entry;
  UINTN                       Index;
  UINT32                      RunningCrc;
  UINT32                      CurrentCrc;

  if ((Logger == NULL) || (Context == NULL) || (Label == NULL) ||
      (Context->BbsJournalStrings == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (LegacyBootCrc32 (
        Context->BbsJournalStrings,
        (UINTN)Context->BbsCount *
        sizeof (LEGACY_BOOT_BBS_STRING_SNAPSHOT)
        ) != Context->BbsJournalStringsCrc32)
  {
    LogPrint (Logger, L"%s failed: owned BBS string-journal CRC changed\r\n", Label);
    if (FlushResult) {
      LogFlush (Logger);
    }

    return EFI_CRC_ERROR;
  }

  ZeroMem (&MemoryMap, sizeof (MemoryMap));
  Status = MemoryMapCapture (&MemoryMap);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!MemoryMapRangeIsReadable (
         &MemoryMap,
         (UINTN)Context->FirmwareBbsTable,
         Context->BbsTableBytes,
         NULL
         ))
  {
    MemoryMapRelease (&MemoryMap);
    return EFI_SECURITY_VIOLATION;
  }

  RunningCrc = MAX_UINT32;
  for (Index = 0; Index < Context->BbsCount; ++Index) {
    CopyMem (
      &Entry,
      (CONST UINT8 *)Context->FirmwareBbsTable +
      (Index * sizeof (BBS_TABLE)),
      sizeof (Entry)
      );
    ZeroMem (&Current, sizeof (Current));
    ManufacturerResult = LegacyBootCopyManufacturer (
                           &MemoryMap, &Entry, Current.Manufacturer, sizeof (Current.Manufacturer));
    DescriptionResult = LegacyBootCopyFarAsciiString (
                          &MemoryMap,
                          Entry.DescStringSegment,
                          Entry.DescStringOffset,
                          Current.Description,
                          sizeof (Current.Description)
                          );
    Current.ManufacturerReadResult = (UINT8)ManufacturerResult;
    Current.DescriptionReadResult  = (UINT8)DescriptionResult;
    RunningCrc = LegacyBootCrc32Update (
                   RunningCrc,
                   (CONST UINT8 *)&Current,
                   sizeof (Current)
                   );
    if (CompareMem (
          &Current,
          &Context->BbsJournalStrings[Index],
          sizeof (Current)
          ) != 0)
    {
      MemoryMapRelease (&MemoryMap);
      LogPrint (
        Logger,
        L"%s failed: bounded manufacturer/description content or read state "
        L"changed at BBS[%u]\r\n",
        Label,
        Index
        );
      if (FlushResult) {
        LogFlush (Logger);
      }

      return EFI_COMPROMISED_DATA;
    }
  }

  MemoryMapRelease (&MemoryMap);
  CurrentCrc = ~RunningCrc;
  Status = LogPrint (
             Logger,
             L"%s bounded BBS strings passed: CRC32=0x%08x\r\n",
             Label,
             CurrentCrc
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CurrentCrc != Context->BbsJournalStringsCrc32) {
    if (FlushResult) {
      LogFlush (Logger);
    }

    return EFI_CRC_ERROR;
  }

  return FlushResult ? LogFlush (Logger) : EFI_SUCCESS;
}

STATIC
EFI_STATUS
LegacyBootVerifyLiveAgainstPriorities (
  IN APP_LOGGER                        *Logger,
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  IN CONST UINT16                      *ExpectedPriorities,
  IN CONST CHAR16                      *Label,
  IN BOOLEAN                           RequireWritableRange,
  IN BOOLEAN                           FlushResult
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  BBS_TABLE   Entry;
  UINT32      IdentityCrc32;
  UINT32      PriorityCrc32;

  if ((Logger == NULL) || (Context == NULL) ||
      (ExpectedPriorities == NULL) || (Label == NULL) ||
      (Context->BbsJournal == NULL) ||
      (Context->VerificationScratch == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = LegacyBootValidateFirmwareBbsRange (Context, RequireWritableRange);
  if (EFI_ERROR (Status)) {
    LogPrint (
      Logger,
      L"%s BBS pointer/range validation failed: 0x%016lx (%s)\r\n",
      Label,
      (UINT64)Status,
      EfiStatusName (Status)
      );
    if (FlushResult) {
      LogFlush (Logger);
    }

    return Status;
  }

  CopyMem (
    Context->VerificationScratch,
    Context->FirmwareBbsTable,
    Context->BbsTableBytes
    );
  for (Index = 0; Index < Context->BbsCount; ++Index) {
    CopyMem (&Entry, &Context->VerificationScratch[Index], sizeof (Entry));
    if (CompareMem (
          (CONST UINT8 *)&Entry + sizeof (UINT16),
          (CONST UINT8 *)&Context->BbsJournal[Index] + sizeof (UINT16),
          sizeof (BBS_TABLE) - sizeof (UINT16)
          ) != 0)
    {
      LogPrint (
        Logger,
        L"%s failed: BBS[%u] identity changed outside BootPriority\r\n",
        Label,
        Index
        );
      if (FlushResult) {
        LogFlush (Logger);
      }

      return EFI_COMPROMISED_DATA;
    }

    if (Entry.BootPriority != ExpectedPriorities[Index]) {
      LogPrint (
        Logger,
        L"%s failed: BBS[%u] priority expected=0x%04x actual=0x%04x\r\n",
        Label,
        Index,
        ExpectedPriorities[Index],
        Entry.BootPriority
        );
      if (FlushResult) {
        LogFlush (Logger);
      }

      return EFI_COMPROMISED_DATA;
    }
  }

  IdentityCrc32 = LegacyBootBbsIdentityCrc32 (
                    Context->VerificationScratch,
                    Context->BbsCount
                    );
  PriorityCrc32 = LegacyBootBbsPriorityCrc32FromTable (
                    Context->VerificationScratch,
                    Context->BbsCount
                    );
  Status = LegacyBootVerifyLiveBbsStrings (
             Logger,
             Context,
             Label,
             FALSE
             );
  if (EFI_ERROR (Status)) {
    if (FlushResult) {
      LogFlush (Logger);
    }

    return Status;
  }

  for (Index = 0; Index < Context->BbsCount; ++Index) {
    CopyMem (
      &Entry,
      (CONST UINT8 *)Context->FirmwareBbsTable +
      (Index * sizeof (BBS_TABLE)),
      sizeof (Entry)
      );
    if (CompareMem (
          &Entry,
          &Context->VerificationScratch[Index],
          sizeof (Entry)
          ) != 0)
    {
      LogPrint (
        Logger,
        L"%s failed: BBS[%u] changed during bounded string revalidation\r\n",
        Label,
        Index
        );
      if (FlushResult) {
        LogFlush (Logger);
      }

      return EFI_ABORTED;
    }
  }

  Status = LogPrint (
             Logger,
             L"%s passed: entries=%u identity-CRC32=0x%08x "
             L"priority-CRC32=0x%08x\r\n",
             Label,
             Context->BbsCount,
             IdentityCrc32,
             PriorityCrc32
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return FlushResult ? LogFlush (Logger) : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI __attribute__((noinline))
LegacyBootReserveJournal (LEGACY_BOOT_BOOT_TARGET_CONTEXT *Context)
{
  if (Context->BbsJournal != NULL && Context->BbsJournalStrings != NULL &&
      Context->PlannedPriorities != NULL && Context->ExpectedPriorities != NULL &&
      Context->VerificationScratch != NULL) { return EFI_SUCCESS; }
  /* A partial allocation belongs to release, never retry over owned pointers. */
  if (Context->BbsJournal != NULL || Context->BbsJournalStrings != NULL ||
      Context->PlannedPriorities != NULL || Context->ExpectedPriorities != NULL ||
      Context->VerificationScratch != NULL) { return EFI_OUT_OF_RESOURCES; }
  Context->BbsJournal = AllocateZeroPool (Context->BbsTableBytes);
  Context->BbsJournalStrings = AllocateZeroPool (
                                 (UINTN)Context->BbsCount *
                                 sizeof (LEGACY_BOOT_BBS_STRING_SNAPSHOT)
                                 );
  Context->PlannedPriorities = AllocateZeroPool (
                                 (UINTN)Context->BbsCount * sizeof (UINT16)
                                 );
  Context->ExpectedPriorities = AllocateZeroPool (
                                  (UINTN)Context->BbsCount * sizeof (UINT16)
                                  );
  Context->VerificationScratch = AllocateZeroPool (Context->BbsTableBytes);
  if ((Context->BbsJournal == NULL) ||
      (Context->BbsJournalStrings == NULL) ||
      (Context->PlannedPriorities == NULL) ||
      (Context->ExpectedPriorities == NULL) ||
      (Context->VerificationScratch == NULL))
  {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI __attribute__((noinline))
LegacyBootBootTargetPrepare (APP_LOGGER *Logger, LEGACY_BOOT_BOOT_TARGET_CONTEXT *Context)
{
  EFI_STATUS Status;
  if (Logger == NULL || Context == NULL || !Context->DiscoveryComplete ||
      Context->JournalAttempted) { return EFI_INVALID_PARAMETER; }
  Status = LegacyBootValidateFirmwareBbsRange (Context, TRUE);
  if (EFI_ERROR (Status)) { return Status; }
  Status = LegacyBootReserveJournal (Context);
  if (EFI_ERROR (Status)) { return Status; }
  CopyMem (Context->BbsJournal, Context->BbsDiscoverySnapshot, Context->BbsTableBytes);
  Status = LegacyBootCaptureLegacyDevOrder (Logger, Context);
  if (EFI_ERROR (Status)) { return Status; }
  Status = LegacyBootBuildPriorityPlan (Context);
  if (EFI_ERROR (Status)) { return Status; }
  return LogPrint (Logger, L"NCV_BBS_PREFLIGHT_READY\r\nBBS writable range, priority feasibility and journal allocation passed before GPU handoff.\r\n");
}

inline EFI_STATUS __attribute__((always_inline))
LegacyBootBootTargetJournalAndValidate (
  IN     APP_LOGGER                   *Logger,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT   *Context
  )
{
  EFI_STATUS           Status;
  MEMORY_MAP_SNAPSHOT  MemoryMap;
  UINTN                Index;
  CHAR8                Manufacturer[
                         LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U
                         ];
  CHAR8                Description[
                         LEGACY_BOOT_BOOT_TARGET_MAX_STRING_BYTES + 1U
                         ];
  UINT32               EntryCrc32;
  LEGACY_BOOT_FAR_STRING_RESULT ManufacturerResult;
  LEGACY_BOOT_FAR_STRING_RESULT DescriptionResult;

  if ((Logger == NULL) || (Context == NULL) ||
      (Context->Signature != LEGACY_BOOT_BOOT_TARGET_SIGNATURE) ||
      !Context->DiscoveryComplete)
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Context->JournalAttempted || Context->JournalComplete ||
      Context->PriorityTransactionAttempted)
  {
    return EFI_ALREADY_STARTED;
  }

  Context->JournalAttempted = TRUE;

  Status = LegacyBootBootTargetValidateOwnedCopies (Logger, Context);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LegacyStorageRomReportVga (Logger);
  if (EFI_ERROR (Status)) { return Status; }

  Status = LegacyBootValidateFirmwareBbsRange (Context, TRUE);
  if (EFI_ERROR (Status)) {
    LogPrint (
      Logger,
      L"Definitive BBS journal refused because the live table is not in a "
      L"validated writable memory range: 0x%016lx (%s)\r\n",
      (UINT64)Status,
      EfiStatusName (Status)
      );
    LogFlush (Logger);
    return Status;
  }

  if (CompareMem (
        Context->BbsDiscoverySnapshot,
        Context->FirmwareBbsTable,
        Context->BbsTableBytes
        ) != 0)
  {
    LogPrint (
      Logger,
      L"Live BBS table differs from the discovery snapshot before durable "
      L"journaling; refusing mutation\r\n"
      );
    LogFlush (Logger);
    return EFI_ABORTED;
  }

  Status = LegacyBootReserveJournal (Context);
  if (EFI_ERROR (Status)) { return Status; }
  CopyMem (Context->BbsJournal, Context->FirmwareBbsTable, Context->BbsTableBytes);

  Context->BbsJournalCrc32 = LegacyBootCrc32 (
                               Context->BbsJournal,
                               Context->BbsTableBytes
                               );
  Context->BbsIdentityCrc32 = LegacyBootBbsIdentityCrc32 (
                                Context->BbsJournal,
                                Context->BbsCount
                                );
  Context->BbsPriorityCrc32 = LegacyBootBbsPriorityCrc32FromTable (
                                Context->BbsJournal,
                                Context->BbsCount
                                );
  Status = Context->LegacyDevOrderUsable ?
    LegacyBootVerifyLegacyDevOrderSnapshot (Logger, Context, L"Post-ROM ordering revalidation", TRUE) :
    LegacyBootCaptureLegacyDevOrder (Logger, Context);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LegacyBootBuildPriorityPlan (Context);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&MemoryMap, sizeof (MemoryMap));
  Status = MemoryMapCapture (&MemoryMap);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!MemoryMapRangeIsReadable (
         &MemoryMap,
         (UINTN)Context->FirmwareBbsTable,
         Context->BbsTableBytes,
         NULL
         ))
  {
    MemoryMapRelease (&MemoryMap);
    return EFI_SECURITY_VIOLATION;
  }

  LogPrint (
    Logger,
    L"\r\n=== Definitive legacy boot-target discovery durable BBS mutation journal ===\r\n"
    L"This journal was requested after the route engine's durable PCI "
    L"journal. Only BBS_TABLE.BootPriority may be written later.\r\n"
    );
  LogPrint (
    Logger,
    L"BBS table=%p entries=%u bytes=0x%lx raw-CRC32=0x%08x "
    L"identity-CRC32=0x%08x priority-CRC32=0x%08x "
    L"planned-priority-CRC32=0x%08x\r\n",
    Context->FirmwareBbsTable,
    Context->BbsCount,
    (UINT64)Context->BbsTableBytes,
    Context->BbsJournalCrc32,
    Context->BbsIdentityCrc32,
    Context->BbsPriorityCrc32,
    Context->PlannedPriorityCrc32
    );
  Status = LogPrint (
             Logger,
             L"Definitive selected target: matching Boot%04x; selected live "
             L"BBS index=%u; LegacyDevOrder source=%s size=0x%lx "
             L"CRC32=0x%08x\r\n",
             Context->BootOptionNumber,
             Context->LiveBbsIndex,
             Context->LegacyDevOrderUsable ?
               L"strict-read-only-variable" :
               L"live-BBS-index-fallback",
             (UINT64)Context->LegacyDevOrderSize,
             Context->LegacyDevOrderCrc32
             );
  if (EFI_ERROR (Status)) {
    MemoryMapRelease (&MemoryMap);
    return Status;
  }

  for (Index = 0; Index < Context->BbsCount; ++Index) {
    ZeroMem (Manufacturer, sizeof (Manufacturer));
    ZeroMem (Description, sizeof (Description));
    Status = LegacyBootLogBbsEntry (
               Logger,
               &MemoryMap,
               Index,
               &Context->BbsJournal[Index],
               Manufacturer,
               sizeof (Manufacturer),
               Description,
               sizeof (Description),
               &ManufacturerResult,
               &DescriptionResult
               );
    if (EFI_ERROR (Status)) {
      MemoryMapRelease (&MemoryMap);
      return Status;
    }

    Context->BbsJournalStrings[Index].ManufacturerReadResult =
      (UINT8)ManufacturerResult;
    Context->BbsJournalStrings[Index].DescriptionReadResult =
      (UINT8)DescriptionResult;
    CopyMem (
      Context->BbsJournalStrings[Index].Manufacturer,
      Manufacturer,
      sizeof (Context->BbsJournalStrings[Index].Manufacturer)
      );
    CopyMem (
      Context->BbsJournalStrings[Index].Description,
      Description,
      sizeof (Context->BbsJournalStrings[Index].Description)
      );

    EntryCrc32 = LegacyBootCrc32 (
                   &Context->BbsJournal[Index],
                   sizeof (BBS_TABLE)
                   );
    Status = LogPrint (
               Logger,
               L"      journal entry-CRC32=0x%08x original-priority=0x%04x "
               L"planned-priority=0x%04x selected=%s eligibility=%s\r\n",
               EntryCrc32,
               Context->BbsJournal[Index].BootPriority,
               Context->PlannedPriorities[Index],
               (Index == Context->LiveBbsIndex) ? L"yes" : L"no",
               LegacyBootBbsIndexEligibilityReason (Context, Index)
               );
    if (EFI_ERROR (Status)) {
      MemoryMapRelease (&MemoryMap);
      return Status;
    }
  }

  Context->BbsJournalStringsCrc32 = LegacyBootCrc32 (
                                      Context->BbsJournalStrings,
                                      (UINTN)Context->BbsCount *
                                      sizeof (LEGACY_BOOT_BBS_STRING_SNAPSHOT)
                                      );
  Status = LogPrint (
             Logger,
             L"Bounded journal manufacturer/description CRC32=0x%08x "
             L"discovery-CRC32=0x%08x exact-match=%s\r\n",
             Context->BbsJournalStringsCrc32,
             Context->BbsDiscoveryStringsCrc32,
             (CompareMem (
                Context->BbsJournalStrings,
                Context->BbsDiscoveryStrings,
                (UINTN)Context->BbsCount *
                sizeof (LEGACY_BOOT_BBS_STRING_SNAPSHOT)
                ) == 0) ? L"yes" : L"no"
             );
  if (EFI_ERROR (Status)) {
    MemoryMapRelease (&MemoryMap);
    return Status;
  }

  if (CompareMem (
        Context->BbsJournalStrings,
        Context->BbsDiscoveryStrings,
        (UINTN)Context->BbsCount *
        sizeof (LEGACY_BOOT_BBS_STRING_SNAPSHOT)
        ) != 0)
  {
    MemoryMapRelease (&MemoryMap);
    LogPrint (
      Logger,
      L"A bounded live BBS manufacturer/description changed between target "
      L"discovery and durable journaling; refusing mutation\r\n"
      );
    LogFlush (Logger);
    return EFI_ABORTED;
  }

  MemoryMapRelease (&MemoryMap);
  Status = LogFlush (Logger);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LegacyBootVerifyLegacyDevOrderSnapshot (
             Logger,
             Context,
             L"Final flushed LegacyDevOrder journal reread",
             TRUE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // File activity above is intentionally followed by a complete live reread.
  // No allocation is performed by this copy/compare step.
  CopyMem (
    Context->VerificationScratch,
    Context->FirmwareBbsTable,
    Context->BbsTableBytes
    );
  if (CompareMem (
        Context->VerificationScratch,
        Context->BbsJournal,
        Context->BbsTableBytes
        ) != 0)
  {
    LogPrint (
      Logger,
      L"Final live BBS reread differs from the flushed durable journal; "
      L"refusing mutation\r\n"
      );
    LogFlush (Logger);
    return EFI_ABORTED;
  }

  Status = LegacyBootVerifyLiveBbsStrings (
             Logger,
             Context,
             L"Final flushed BBS string-journal reread",
             TRUE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Context->JournalComplete = TRUE;
  Status = LegacyBootVerifyLiveAgainstPriorities (
             Logger,
             Context,
             Context->ExpectedPriorities,
             L"Final original BBS journal reread",
             TRUE,
             TRUE
             );
  if (EFI_ERROR (Status)) {
    Context->JournalComplete = FALSE;
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
LegacyBootBootTargetValidateOriginal (
  IN APP_LOGGER                        *Logger,
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;

  if ((Context == NULL) || !Context->JournalComplete ||
      Context->PrioritiesApplied || Context->PriorityTransactionDirty)
  {
    return EFI_NOT_READY;
  }

  Status = LegacyBootBootTargetValidateOwnedCopies (Logger, Context);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LegacyBootVerifyLegacyDevOrderSnapshot (
             Logger,
             Context,
             L"legacy boot-target discovery original LegacyDevOrder validation",
             TRUE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return LegacyBootVerifyLiveAgainstPriorities (
           Logger,
           Context,
           Context->ExpectedPriorities,
           L"legacy boot-target discovery original BBS state validation",
           TRUE,
           TRUE
           );
}

EFI_STATUS
LegacyBootBootTargetValidateApplied (
  IN APP_LOGGER                        *Logger,
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  UINTN       PriorityZeroCount;

  if ((Context == NULL) || !Context->JournalComplete ||
      !Context->PrioritiesApplied || !Context->PriorityTransactionDirty)
  {
    return EFI_NOT_READY;
  }

  Status = LegacyBootBootTargetValidateOwnedCopies (Logger, Context);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LegacyBootVerifyLegacyDevOrderSnapshot (
             Logger,
             Context,
             L"legacy boot-target discovery applied LegacyDevOrder validation",
             TRUE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LegacyBootVerifyLiveAgainstPriorities (
             Logger,
             Context,
             Context->PlannedPriorities,
             L"legacy boot-target discovery applied BBS state validation",
             TRUE,
             TRUE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  PriorityZeroCount = 0;
  for (Index = 0; Index < Context->BbsCount; ++Index) {
    if (Context->PlannedPriorities[Index] == 0) {
      ++PriorityZeroCount;
      if (Index != Context->LiveBbsIndex) {
        LogPrint (
          Logger,
          L"Applied BBS uniqueness failed: non-selected disk BBS[%u] has "
          L"BootPriority 0\r\n",
          Index
          );
        LogFlush (Logger);
        return EFI_COMPROMISED_DATA;
      }
    }
  }

  if ((PriorityZeroCount != 1U) ||
      (Context->PlannedPriorities[Context->LiveBbsIndex] != 0))
  {
    LogPrint (
      Logger,
      L"Applied BBS uniqueness failed: priority-zero-count=%u selected disk="
      L"0x%04x\r\n",
      PriorityZeroCount,
      Context->PlannedPriorities[Context->LiveBbsIndex]
      );
    LogFlush (Logger);
    return EFI_COMPROMISED_DATA;
  }

  Status = LogPrint (
             Logger,
             L"Applied BBS uniqueness passed: selected disk BBS[%u] is the one "
             L"and only BootPriority 0 entry\r\n",
             Context->LiveBbsIndex
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return LogFlush (Logger);
}

EFI_STATUS
LegacyBootBootTargetGetLegacyBootArguments (
  IN  CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  OUT BBS_BBS_DEVICE_PATH               **BootOption,
  OUT UINT32                            *LoadOptionsSize,
  OUT VOID                              **LoadOptions
  )
{
  if ((Context == NULL) || (BootOption == NULL) ||
      (LoadOptionsSize == NULL) || (LoadOptions == NULL) ||
      (Context->Signature != LEGACY_BOOT_BOOT_TARGET_SIGNATURE) ||
      !Context->DiscoveryComplete || !Context->JournalComplete ||
      !Context->PrioritiesApplied ||
      !Context->PriorityTransactionDirty ||
      (Context->BbsDevicePath == NULL) ||
      ((Context->LoadOptionsSize != 0) &&
       (Context->LoadOptions == NULL)) ||
      (Context->LoadOptionsSize > MAX_UINT32))
  {
    return EFI_INVALID_PARAMETER;
  }

  *BootOption     = (BBS_BBS_DEVICE_PATH *)Context->BbsDevicePath;
  *LoadOptionsSize = (UINT32)Context->LoadOptionsSize;
  *LoadOptions    = Context->LoadOptions;
  return EFI_SUCCESS;
}

EFI_STATUS
LegacyBootBootTargetGetOwnedCopyInfo (
  IN  CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  OUT CONST UINT8                       **BbsDevicePath,
  OUT UINTN                             *BbsDevicePathSize,
  OUT UINT32                            *BbsDevicePathCrc32,
  OUT CONST UINT8                       **LoadOptions,
  OUT UINTN                             *LoadOptionsSize,
  OUT UINT32                            *LoadOptionsCrc32
  )
{
  if ((Context == NULL) || (BbsDevicePath == NULL) ||
      (BbsDevicePathSize == NULL) || (BbsDevicePathCrc32 == NULL) ||
      (LoadOptions == NULL) || (LoadOptionsSize == NULL) ||
      (LoadOptionsCrc32 == NULL) ||
      (Context->Signature != LEGACY_BOOT_BOOT_TARGET_SIGNATURE) ||
      !Context->DiscoveryComplete)
  {
    return EFI_INVALID_PARAMETER;
  }

  *BbsDevicePath      = Context->BbsDevicePath;
  *BbsDevicePathSize  = Context->BbsDevicePathSize;
  *BbsDevicePathCrc32 = Context->BbsDevicePathCrc32;
  *LoadOptions        = Context->LoadOptions;
  *LoadOptionsSize    = Context->LoadOptionsSize;
  *LoadOptionsCrc32   = Context->LoadOptionsCrc32;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
LegacyBootWritePriority (
  IN     APP_LOGGER                   *Logger,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT   *Context,
  IN     UINT16                       Index,
  IN     UINT16                       RequestedPriority,
  IN     CONST CHAR16                 *Phase
  )
{
  EFI_STATUS                    Status;
  BBS_TABLE                    Before;
  BBS_TABLE                    PreWrite;
  BBS_TABLE                    After;
  UINT16                       OldPriority;
  UINT16                       ActualPriority;
  volatile UINT16              *PriorityAddress;
  LEGACY_BOOT_BBS_PRIORITY_WRITE    *WriteRecord;

  if ((Logger == NULL) || (Context == NULL) || (Phase == NULL) ||
      !Context->JournalComplete || (Index >= Context->BbsCount) ||
      (Context->ExpectedPriorities == NULL) ||
      (Context->BbsJournal == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (
    &Before,
    (CONST UINT8 *)Context->FirmwareBbsTable +
    ((UINTN)Index * sizeof (BBS_TABLE)),
    sizeof (Before)
    );
  OldPriority = Before.BootPriority;
  if ((CompareMem (
         (CONST UINT8 *)&Before + sizeof (UINT16),
         (CONST UINT8 *)&Context->BbsJournal[Index] + sizeof (UINT16),
         sizeof (BBS_TABLE) - sizeof (UINT16)
         ) != 0) ||
      (OldPriority != Context->ExpectedPriorities[Index]))
  {
    LogPrint (
      Logger,
      L"BBS priority %s precondition failed at index %u: expected=0x%04x "
      L"actual=0x%04x identity-match=%s\r\n",
      Phase,
      Index,
      Context->ExpectedPriorities[Index],
      OldPriority,
      (CompareMem (
         (CONST UINT8 *)&Before + sizeof (UINT16),
         (CONST UINT8 *)&Context->BbsJournal[Index] + sizeof (UINT16),
         sizeof (BBS_TABLE) - sizeof (UINT16)
         ) == 0) ? L"yes" : L"no"
      );
    LogFlush (Logger);
    return EFI_COMPROMISED_DATA;
  }

  if (OldPriority == RequestedPriority) {
    return EFI_SUCCESS;
  }

  if (Context->PriorityWriteCount >=
      LEGACY_BOOT_BOOT_TARGET_MAX_PRIORITY_WRITES)
  {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = LogPrint (
             Logger,
             L"BBS_PRIORITY_WRITE_INTENT phase=%s index=%u old=0x%04x "
             L"requested=0x%04x; field=BootPriority only\r\n",
             Phase,
             Index,
             OldPriority,
             RequestedPriority
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LogFlush (Logger);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // The durable prewrite record above is followed by another exact reread so
  // filesystem activity cannot create an unchecked write window.
  CopyMem (
    &PreWrite,
    (CONST UINT8 *)Context->FirmwareBbsTable +
    ((UINTN)Index * sizeof (BBS_TABLE)),
    sizeof (PreWrite)
    );
  if ((PreWrite.BootPriority != OldPriority) ||
      (CompareMem (&PreWrite, &Before, sizeof (BBS_TABLE)) != 0))
  {
    LogPrint (
      Logger,
      L"BBS[%u] changed after the flushed write intent; refusing write\r\n",
      Index
      );
    LogFlush (Logger);
    return EFI_ABORTED;
  }

  WriteRecord = &Context->PriorityWrites[Context->PriorityWriteCount++];
  WriteRecord->Index             = Index;
  WriteRecord->OldPriority       = OldPriority;
  WriteRecord->RequestedPriority = RequestedPriority;
  Context->PriorityTransactionDirty = TRUE;

  PriorityAddress = (volatile UINT16 *)(VOID *)(
    (UINT8 *)Context->FirmwareBbsTable +
    ((UINTN)Index * sizeof (BBS_TABLE)) +
    OFFSET_OF (BBS_TABLE, BootPriority)
    );
  *PriorityAddress = RequestedPriority;
  MemoryFence ();

  CopyMem (
    &After,
    (CONST UINT8 *)Context->FirmwareBbsTable +
    ((UINTN)Index * sizeof (BBS_TABLE)),
    sizeof (After)
    );
  ActualPriority = After.BootPriority;
  if ((ActualPriority != RequestedPriority) ||
      (CompareMem (
         (CONST UINT8 *)&After + sizeof (UINT16),
         (CONST UINT8 *)&Before + sizeof (UINT16),
         sizeof (BBS_TABLE) - sizeof (UINT16)
         ) != 0))
  {
    LogPrint (
      Logger,
      L"BBS_PRIORITY_WRITE_VERIFY_FAILED phase=%s index=%u old=0x%04x "
      L"requested=0x%04x actual=0x%04x unrelated-bytes-unchanged=%s\r\n",
      Phase,
      Index,
      OldPriority,
      RequestedPriority,
      ActualPriority,
      (CompareMem (
         (CONST UINT8 *)&After + sizeof (UINT16),
         (CONST UINT8 *)&Before + sizeof (UINT16),
         sizeof (BBS_TABLE) - sizeof (UINT16)
         ) == 0) ? L"yes" : L"no"
      );
    LogFlush (Logger);
    return EFI_DEVICE_ERROR;
  }

  Context->ExpectedPriorities[Index] = RequestedPriority;
  Status = LegacyBootVerifyLiveAgainstPriorities (
             Logger,
             Context,
             Context->ExpectedPriorities,
             L"Per-write complete BBS verification",
             TRUE,
             FALSE
             );
  if (EFI_ERROR (Status)) {
    LogFlush (Logger);
    return Status;
  }

  Status = LogPrint (
             Logger,
             L"BBS_PRIORITY_WRITE_VERIFIED phase=%s index=%u old=0x%04x "
             L"requested=0x%04x actual=0x%04x\r\n",
             Phase,
             Index,
             OldPriority,
             RequestedPriority,
             ActualPriority
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return LogFlush (Logger);
}

STATIC
EFI_STATUS
LegacyBootRollbackInternal (
  IN     APP_LOGGER                   *Logger,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT   *Context
  )
{
  EFI_STATUS                    Status;
  EFI_STATUS                    OverallStatus;
  EFI_STATUS                    LogStatus;
  UINTN                         HistoryIndex;
  LEGACY_BOOT_BBS_PRIORITY_WRITE     *Record;
  BBS_TABLE                     Before;
  BBS_TABLE                     After;
  volatile UINT16               *PriorityAddress;

  if ((Logger == NULL) || (Context == NULL) ||
      !Context->JournalComplete)
  {
    return EFI_INVALID_PARAMETER;
  }

  OverallStatus = LegacyBootValidateFirmwareBbsRange (Context, TRUE);
  if (EFI_ERROR (OverallStatus)) {
    LogPrint (
      Logger,
      L"BBS rollback cannot validate the firmware table range: "
      L"0x%016lx (%s)\r\n",
      (UINT64)OverallStatus,
      EfiStatusName (OverallStatus)
      );
    LogFlush (Logger);
    return OverallStatus;
  }

  LogStatus = LogPrint (
                Logger,
                L"Beginning exact reverse BBS BootPriority rollback; "
                L"successful-write records=%u\r\n",
                Context->PriorityWriteCount
                );
  if (EFI_ERROR (LogStatus)) {
    OverallStatus = LogStatus;
  }

  LogStatus = LogFlush (Logger);
  if (EFI_ERROR (LogStatus) && !EFI_ERROR (OverallStatus)) {
    OverallStatus = LogStatus;
  }

  for (HistoryIndex = Context->PriorityWriteCount;
       HistoryIndex > 0;
       --HistoryIndex)
  {
    Record = &Context->PriorityWrites[HistoryIndex - 1U];
    CopyMem (
      &Before,
      (CONST UINT8 *)Context->FirmwareBbsTable +
      ((UINTN)Record->Index * sizeof (BBS_TABLE)),
      sizeof (Before)
      );
    if (CompareMem (
          (CONST UINT8 *)&Before + sizeof (UINT16),
          (CONST UINT8 *)&Context->BbsJournal[Record->Index] +
          sizeof (UINT16),
          sizeof (BBS_TABLE) - sizeof (UINT16)
          ) != 0)
    {
      LogPrint (
        Logger,
        L"BBS rollback refused index %u because its identity changed\r\n",
        Record->Index
        );
      LogFlush (Logger);
      OverallStatus = EFI_COMPROMISED_DATA;
      continue;
    }

    LogStatus = LogPrint (
                  Logger,
                  L"BBS_PRIORITY_ROLLBACK_INTENT index=%u current=0x%04x "
                  L"expected-current=0x%04x restore=0x%04x\r\n",
                  Record->Index,
                  Before.BootPriority,
                  Record->RequestedPriority,
                  Record->OldPriority
                  );
    if (EFI_ERROR (LogStatus) && !EFI_ERROR (OverallStatus)) {
      OverallStatus = LogStatus;
    }

    LogStatus = LogFlush (Logger);
    if (EFI_ERROR (LogStatus) && !EFI_ERROR (OverallStatus)) {
      OverallStatus = LogStatus;
    }

    CopyMem (
      &After,
      (CONST UINT8 *)Context->FirmwareBbsTable +
      ((UINTN)Record->Index * sizeof (BBS_TABLE)),
      sizeof (After)
      );
    if (CompareMem (
          (CONST UINT8 *)&After + sizeof (UINT16),
          (CONST UINT8 *)&Context->BbsJournal[Record->Index] +
          sizeof (UINT16),
          sizeof (BBS_TABLE) - sizeof (UINT16)
          ) != 0)
    {
      LogPrint (
        Logger,
        L"BBS rollback refused index %u after the log flush because its "
        L"identity changed\r\n",
        Record->Index
        );
      LogFlush (Logger);
      OverallStatus = EFI_COMPROMISED_DATA;
      continue;
    }

    // Rollback must proceed even if its logging sink failed. Identity was
    // verified above, and only the recorded two-byte field is restored.
    PriorityAddress = (volatile UINT16 *)(VOID *)(
      (UINT8 *)Context->FirmwareBbsTable +
      ((UINTN)Record->Index * sizeof (BBS_TABLE)) +
      OFFSET_OF (BBS_TABLE, BootPriority)
      );
    *PriorityAddress = Record->OldPriority;
    MemoryFence ();

    CopyMem (
      &After,
      (CONST UINT8 *)Context->FirmwareBbsTable +
      ((UINTN)Record->Index * sizeof (BBS_TABLE)),
      sizeof (After)
      );
    if ((After.BootPriority != Record->OldPriority) ||
        (CompareMem (
           (CONST UINT8 *)&After + sizeof (UINT16),
           (CONST UINT8 *)&Before + sizeof (UINT16),
           sizeof (BBS_TABLE) - sizeof (UINT16)
           ) != 0))
    {
      LogPrint (
        Logger,
        L"BBS_PRIORITY_ROLLBACK_VERIFY_FAILED index=%u requested=0x%04x "
        L"actual=0x%04x\r\n",
        Record->Index,
        Record->OldPriority,
        After.BootPriority
        );
      LogFlush (Logger);
      OverallStatus = EFI_DEVICE_ERROR;
      continue;
    }

    Context->ExpectedPriorities[Record->Index] = Record->OldPriority;
    Status = LegacyBootVerifyLiveAgainstPriorities (
               Logger,
               Context,
               Context->ExpectedPriorities,
               L"Per-rollback-write complete BBS verification",
               TRUE,
               FALSE
               );
    if (EFI_ERROR (Status) && !EFI_ERROR (OverallStatus)) {
      OverallStatus = Status;
    }

    LogStatus = LogPrint (
                  Logger,
                  L"BBS_PRIORITY_ROLLBACK_VERIFIED index=%u value=0x%04x\r\n",
                  Record->Index,
                  After.BootPriority
                  );
    if (EFI_ERROR (LogStatus) && !EFI_ERROR (OverallStatus)) {
      OverallStatus = LogStatus;
    }

    LogStatus = LogFlush (Logger);
    if (EFI_ERROR (LogStatus) && !EFI_ERROR (OverallStatus)) {
      OverallStatus = LogStatus;
    }
  }

  Status = LegacyBootVerifyLiveAgainstPriorities (
             Logger,
             Context,
             Context->ExpectedPriorities,
             L"Post-rollback complete BBS verification",
             TRUE,
             TRUE
             );
  if (EFI_ERROR (Status) && !EFI_ERROR (OverallStatus)) {
    OverallStatus = Status;
  }

  if ((CompareMem (
         Context->FirmwareBbsTable,
         Context->BbsJournal,
         Context->BbsTableBytes
         ) != 0) ||
      (LegacyBootCrc32 (
         Context->FirmwareBbsTable,
         Context->BbsTableBytes
         ) != Context->BbsJournalCrc32))
  {
    LogPrint (
      Logger,
      L"Exact BBS journal restoration failed after reverse rollback\r\n"
      );
    LogFlush (Logger);
    OverallStatus = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (OverallStatus)) {
    Context->PriorityTransactionDirty = FALSE;
    Context->PrioritiesApplied        = FALSE;
    Context->PriorityWriteCount       = 0;
    ZeroMem (Context->PriorityWrites, sizeof (Context->PriorityWrites));
    LogPrint (
      Logger,
      L"Exact reverse BBS BootPriority restoration passed; every journaled "
      L"byte matches its original value\r\n"
      );
    return LogFlush (Logger);
  }

  return OverallStatus;
}

EFI_STATUS
LegacyBootBootPriorityApply (
  IN     APP_LOGGER                   *Logger,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT   *Context
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  RollbackStatus;
  UINTN       Index;
  UINTN       Priority;
  UINTN       ValidCount;

  if ((Logger == NULL) || (Context == NULL) ||
      (Context->Signature != LEGACY_BOOT_BOOT_TARGET_SIGNATURE) ||
      !Context->DiscoveryComplete || !Context->JournalComplete)
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Context->PriorityTransactionAttempted) {
    return EFI_ALREADY_STARTED;
  }

  Context->PriorityTransactionAttempted = TRUE;
  Status = LegacyBootBootTargetValidateOriginal (Logger, Context);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  LogPrint (
    Logger,
    L"\r\n=== Reversible legacy boot-target discovery BBS BootPriority transaction ===\r\n"
    L"Only the packed two-byte BootPriority field may change. The selected disk "
    L"follows explicit user choice despite any recorded firmware policy warning. "
    L"Unselected reserved-priority, disabled, failed, no-media, reserved-status "
    L"and zero-status rows remain preserved. A usable LegacyDevOrder also "
    L"preserves unselected 0xFF-disabled entries. Ordering source: %s.\r\n",
    Context->LegacyDevOrderUsable ?
      L"strict read-only LegacyDevOrder" :
      L"live-BBS-index deterministic fallback"
    );
  Status = LogFlush (Logger);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // The route coordinator may have closed and reopened the FAT log after the
  // definitive journal. Pin the read-only ordering source again here, after
  // that file activity and immediately before the first authorized BBS write.
  Status = LegacyBootVerifyLegacyDevOrderSnapshot (
             Logger,
             Context,
             L"Immediate pre-BBS-write LegacyDevOrder reread",
             FALSE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Reference-style first phase over the stricter transaction-eligible set:
  // make each eligible entry unprioritized while preserving every ineligible
  // entry, including the three reserved priority sentinels and every
  // LegacyDevOrder 0xFF-high-byte disabled entry, exactly.
  for (Index = 0; Index < Context->BbsCount; ++Index) {
    if (!LegacyBootBbsIndexIsTransactionEligible (Context, Index))
    {
      continue;
    }

    Status = LegacyBootWritePriority (
               Logger,
               Context,
               (UINT16)Index,
               BBS_UNPRIORITIZED_ENTRY,
               L"unprioritize-valid-entry"
               );
    if (EFI_ERROR (Status)) {
      goto Failed;
    }
  }

  ValidCount = 0;
  for (Index = 0; Index < Context->BbsCount; ++Index) {
    if (LegacyBootBbsIndexIsTransactionEligible (Context, Index))
    {
      ++ValidCount;
    }
  }

  // The plan is dense: priority zero is selected disk, priorities 1.. rank the
  // remaining eligible HDDs first and then all remaining eligible entries.
  for (Priority = 0; Priority < ValidCount; ++Priority) {
    for (Index = 0; Index < Context->BbsCount; ++Index) {
      if (LegacyBootBbsIndexIsTransactionEligible (Context, Index) &&
          Context->PlannedPriorities[Index] == Priority) {
        Status = LegacyBootWritePriority (
                   Logger,
                   Context,
                   (UINT16)Index,
                   (UINT16)Priority,
                   (Priority == 0) ?
                     L"select-unique-selected disk" : L"rank-remaining-entry"
                   );
        if (EFI_ERROR (Status)) {
          goto Failed;
        }

        break;
      }
    }

    if (Index == Context->BbsCount) {
      Status = EFI_COMPROMISED_DATA;
      goto Failed;
    }
  }

  Context->PrioritiesApplied = TRUE;
  Status = LegacyBootBootTargetValidateApplied (Logger, Context);
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  LogPrint (
    Logger,
    L"legacy boot-target discovery BBS priority transaction passed: selected live BBS[%u] has "
    L"priority 0; all complete-table invariants passed\r\n",
    Context->LiveBbsIndex
    );
  Status = LogFlush (Logger);
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  return EFI_SUCCESS;

Failed:
  LogPrint (
    Logger,
    L"legacy boot-target discovery BBS priority transaction failed: 0x%016lx (%s); initiating "
    L"immediate reverse rollback\r\n",
    (UINT64)Status,
    EfiStatusName (Status)
    );
  LogFlush (Logger);
  if (Context->PriorityTransactionDirty ||
      (Context->PriorityWriteCount != 0))
  {
    RollbackStatus = LegacyBootRollbackInternal (Logger, Context);
    if (EFI_ERROR (RollbackStatus)) {
      LogPrint (
        Logger,
        L"Emergency BBS rollback also failed: 0x%016lx (%s)\r\n",
        (UINT64)RollbackStatus,
        EfiStatusName (RollbackStatus)
        );
      LogFlush (Logger);
      return RollbackStatus;
    }
  }

  return Status;
}

EFI_STATUS
LegacyBootBootPriorityRollback (
  IN     APP_LOGGER                   *Logger,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT   *Context
  )
{
  if ((Logger == NULL) || (Context == NULL) ||
      (Context->Signature != LEGACY_BOOT_BOOT_TARGET_SIGNATURE) ||
      !Context->JournalComplete)
  {
    return EFI_INVALID_PARAMETER;
  }

  if (!Context->PriorityTransactionDirty &&
      (Context->PriorityWriteCount == 0))
  {
    return LegacyBootBootTargetValidateOriginal (Logger, Context);
  }

  return LegacyBootRollbackInternal (Logger, Context);
}

EFI_STATUS
LegacyBootBootTargetRelease (
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context
  )
{
  if (Context == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Context->Signature == LEGACY_BOOT_BOOT_TARGET_SIGNATURE) &&
      (Context->PriorityTransactionDirty ||
       (Context->PriorityWriteCount != 0)))
  {
    return EFI_ACCESS_DENIED;
  }

  if (Context->BbsDiscoverySnapshot != NULL) {
    FreePool (Context->BbsDiscoverySnapshot);
  }

  if (Context->BbsDiscoveryStrings != NULL) {
    FreePool (Context->BbsDiscoveryStrings);
  }

  if (Context->LegacyDevOrderData != NULL) {
    FreePool (Context->LegacyDevOrderData);
  }

  if (Context->LegacyDevOrderRanks != NULL) {
    FreePool (Context->LegacyDevOrderRanks);
  }

  if (Context->LegacyDevOrderDisabled != NULL) {
    FreePool (Context->LegacyDevOrderDisabled);
  }

  if (Context->BbsJournal != NULL) {
    FreePool (Context->BbsJournal);
  }

  if (Context->BbsJournalStrings != NULL) {
    FreePool (Context->BbsJournalStrings);
  }

  if (Context->PlannedPriorities != NULL) {
    FreePool (Context->PlannedPriorities);
  }

  if (Context->ExpectedPriorities != NULL) {
    FreePool (Context->ExpectedPriorities);
  }

  if (Context->VerificationScratch != NULL) {
    FreePool (Context->VerificationScratch);
  }

  if (Context->BootOptionDescription != NULL) {
    FreePool (Context->BootOptionDescription);
  }

  if (Context->BbsDevicePath != NULL) {
    FreePool (Context->BbsDevicePath);
  }

  if (Context->LoadOptions != NULL) {
    ZeroMem (Context->LoadOptions, Context->LoadOptionsSize);
    FreePool (Context->LoadOptions);
  }

  LegacyRomGuardReset ();
  ZeroMem (Context, sizeof (*Context));
  return EFI_SUCCESS;
}

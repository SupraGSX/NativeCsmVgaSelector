/** @file
  Read-only generic BBS Boot#### discovery.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include "BootOptions.h"

#include <Protocol/DevicePath.h>
#include <Protocol/LegacyBios.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "StatusPrint.h"

#define BOOT_PROBE_MAX_VARIABLE_BYTES  32768U
#define BOOT_PROBE_MAX_ORDER_BYTES      4096U
#define BOOT_PROBE_READ_ATTEMPTS        3U

STATIC
UINT16
Read16 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8));
}

STATIC
UINT32
Read32 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT32)(Bytes[0] | ((UINT32)Bytes[1] << 8) |
                  ((UINT32)Bytes[2] << 16) | ((UINT32)Bytes[3] << 24));
}

STATIC
EFI_STATUS
ReadVariableBounded (
  IN  CONST CHAR16  *Name,
  IN  UINTN         MaximumSize,
  OUT UINT8         **Buffer,
  OUT UINTN         *BufferSize
  )
{
  EFI_STATUS  Status;
  UINTN       RequiredSize;
  UINTN       Attempt;
  UINT8       *Result;

  if ((Name == NULL) || (Buffer == NULL) || (BufferSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Buffer = NULL;
  *BufferSize = 0;
  RequiredSize = 0;
  Status = gRT->GetVariable (
                  (CHAR16 *)Name,
                  &gEfiGlobalVariableGuid,
                  NULL,
                  &RequiredSize,
                  NULL
                  );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
  }

  for (Attempt = 0; Attempt < BOOT_PROBE_READ_ATTEMPTS; ++Attempt) {
    if ((RequiredSize == 0) || (RequiredSize > MaximumSize)) {
      return EFI_BAD_BUFFER_SIZE;
    }

    Result = AllocateZeroPool (RequiredSize);
    if (Result == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    *BufferSize = RequiredSize;
    Status = gRT->GetVariable (
                    (CHAR16 *)Name,
                    &gEfiGlobalVariableGuid,
                    NULL,
                    BufferSize,
                    Result
                    );
    if (Status == EFI_SUCCESS) {
      if ((*BufferSize == 0) || (*BufferSize > RequiredSize)) {
        FreePool (Result);
        *BufferSize = 0;
        return EFI_COMPROMISED_DATA;
      }

      *Buffer = Result;
      return EFI_SUCCESS;
    }

    FreePool (Result);
    if ((Status != EFI_BUFFER_TOO_SMALL) || (*BufferSize == 0) ||
        (*BufferSize > MaximumSize))
    {
      *BufferSize = 0;
      return Status;
    }

    RequiredSize = *BufferSize;
  }

  *BufferSize = 0;
  return EFI_BUFFER_TOO_SMALL;
}

STATIC
BOOLEAN
ContainsInsensitive (
  IN CONST CHAR16  *Text,
  IN CONST CHAR16  *Needle
  )
{
  UINTN  TextLength;
  UINTN  NeedleLength;
  UINTN  Offset;
  UINTN  Index;

  if ((Text == NULL) || (Needle == NULL)) {
    return FALSE;
  }

  TextLength = StrLen (Text);
  NeedleLength = StrLen (Needle);
  if ((NeedleLength == 0) || (NeedleLength > TextLength)) {
    return FALSE;
  }

  for (Offset = 0; Offset <= TextLength - NeedleLength; ++Offset) {
    for (Index = 0; Index < NeedleLength; ++Index) {
      if (CharToUpper (Text[Offset + Index]) != CharToUpper (Needle[Index])) {
        break;
      }
    }

    if (Index == NeedleLength) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
EqualInsensitive (
  IN CONST CHAR16  *Left,
  IN CONST CHAR16  *Right
  )
{
  UINTN  Index;

  if ((Left == NULL) || (Right == NULL)) {
    return FALSE;
  }

  for (Index = 0; (Left[Index] != L'\0') && (Right[Index] != L'\0'); ++Index) {
    if (CharToUpper (Left[Index]) != CharToUpper (Right[Index])) {
      return FALSE;
    }
  }

  return (BOOLEAN)(Left[Index] == Right[Index]);
}

STATIC
EFI_STATUS
ParseBbsOption (
  IN  CONST UINT8   *Data,
  IN  UINTN         DataSize,
  OUT BOOLEAN       *Active,
  OUT UINT16        *BbsDeviceType,
  OUT CHAR16        **Description
  )
{
  UINTN   DescriptionOffset;
  UINTN   DescriptionChars;
  UINTN   DescriptionBytes;
  UINTN   PathOffset;
  UINTN   PathSize;
  UINTN   Index;
  UINT16  NodeLength;
  CHAR16  *Text;

  if ((Data == NULL) || (Active == NULL) || (BbsDeviceType == NULL) ||
      (Description == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Description = NULL;
  if (DataSize < (sizeof (UINT32) + sizeof (UINT16) + sizeof (CHAR16))) {
    return EFI_COMPROMISED_DATA;
  }

  *Active = (BOOLEAN)((Read32 (Data) & LOAD_OPTION_ACTIVE) != 0);
  PathSize = Read16 (Data + sizeof (UINT32));
  DescriptionOffset = sizeof (UINT32) + sizeof (UINT16);
  DescriptionChars = 0;
  while ((DescriptionOffset + ((DescriptionChars + 1U) * sizeof (CHAR16))) <= DataSize) {
    if (Read16 (Data + DescriptionOffset + DescriptionChars * sizeof (CHAR16)) == 0) {
      break;
    }
    ++DescriptionChars;
  }

  if ((DescriptionOffset + ((DescriptionChars + 1U) * sizeof (CHAR16))) > DataSize) {
    return EFI_COMPROMISED_DATA;
  }

  DescriptionBytes = (DescriptionChars + 1U) * sizeof (CHAR16);
  PathOffset = DescriptionOffset + DescriptionBytes;
  if ((PathSize < END_DEVICE_PATH_LENGTH) || (PathOffset > DataSize) ||
      (PathSize > DataSize - PathOffset))
  {
    return EFI_COMPROMISED_DATA;
  }

  if ((Data[PathOffset] != BBS_DEVICE_PATH) ||
      (Data[PathOffset + 1U] != BBS_BBS_DP))
  {
    return EFI_UNSUPPORTED;
  }

  NodeLength = Read16 (Data + PathOffset + 2U);
  if ((NodeLength < sizeof (BBS_BBS_DEVICE_PATH)) ||
      ((UINTN)NodeLength + END_DEVICE_PATH_LENGTH != PathSize) ||
      (Data[PathOffset + NodeLength] != END_DEVICE_PATH_TYPE) ||
      (Data[PathOffset + NodeLength + 1U] != END_ENTIRE_DEVICE_PATH_SUBTYPE) ||
      (Read16 (Data + PathOffset + NodeLength + 2U) != END_DEVICE_PATH_LENGTH))
  {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = 0; Index < DescriptionChars; ++Index) {
    if (Read16 (Data + DescriptionOffset + Index * sizeof (CHAR16)) < 0x20) {
      return EFI_COMPROMISED_DATA;
    }
  }

  Text = AllocateZeroPool (DescriptionBytes);
  if (Text == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index <= DescriptionChars; ++Index) {
    Text[Index] = Read16 (Data + DescriptionOffset + Index * sizeof (CHAR16));
  }

  *BbsDeviceType = Read16 (Data + PathOffset + 4U);
  *Description = Text;
  return EFI_SUCCESS;
}

EFI_STATUS
BootOptionsProbe (
  IN  APP_LOGGER                 *Logger,
  IN  CONST PROBE_CONFIG         *Config OPTIONAL,
  OUT BOOT_OPTION_PROBE_SUMMARY  *Summary OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  OptionStatus;
  UINT8       *Order;
  UINTN       OrderSize;
  UINTN       Index;
  UINT16      Number;
  CHAR16      Name[12];
  UINT8       *Option;
  UINTN       OptionSize;
  BOOLEAN     Active;
  UINT16      BbsDeviceType;
  CHAR16      *Description;

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Summary != NULL) {
    ZeroMem (Summary, sizeof (*Summary));
    Summary->BootOrderStatus = EFI_NOT_READY;
  }

  LogPrint (Logger, L"\r\n=== Generic legacy BootOrder/Boot#### discovery (read-only) ===\r\n");
  Order = NULL;
  OrderSize = 0;
  Status = ReadVariableBounded (L"BootOrder", BOOT_PROBE_MAX_ORDER_BYTES, &Order, &OrderSize);
  if (Summary != NULL) {
    Summary->BootOrderStatus = Status;
  }

  if (EFI_ERROR (Status)) {
    LogPrint (Logger, L"BootOrder read: 0x%016lx (%s)\r\n", (UINT64)Status, EfiStatusName (Status));
    return Status;
  }

  if ((OrderSize == 0) || ((OrderSize % sizeof (UINT16)) != 0)) {
    FreePool (Order);
    LogPrint (Logger, L"BootOrder has malformed packed UINT16 size=0x%lx\r\n", (UINT64)OrderSize);
    return EFI_COMPROMISED_DATA;
  }

  if (Summary != NULL) {
    Summary->BootOrderEntries = OrderSize / sizeof (UINT16);
  }
  LogPrint (Logger, L"BootOrder entries=%u\r\n", (UINT32)(OrderSize / sizeof (UINT16)));
  for (Index = 0; Index < OrderSize / sizeof (UINT16); ++Index) {
    Number = Read16 (Order + Index * sizeof (UINT16));
    UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Number);
    Option = NULL;
    OptionSize = 0;
    OptionStatus = ReadVariableBounded (Name, BOOT_PROBE_MAX_VARIABLE_BYTES, &Option, &OptionSize);
    if (EFI_ERROR (OptionStatus)) {
      LogPrint (Logger, L"  %s skipped: 0x%016lx (%s)\r\n", Name, (UINT64)OptionStatus, EfiStatusName (OptionStatus));
      continue;
    }

    Description = NULL;
    OptionStatus = ParseBbsOption (Option, OptionSize, &Active, &BbsDeviceType, &Description);
    FreePool (Option);
    if (EFI_ERROR (OptionStatus)) {
      LogPrint (Logger, L"  %s skipped: bounded BBS legacy parse 0x%016lx (%s)\r\n", Name, (UINT64)OptionStatus, EfiStatusName (OptionStatus));
      continue;
    }

    if (Summary != NULL) {
      ++Summary->GenericBbsOptions;
      if (Active && (BbsDeviceType == BBS_HARDDISK)) {
        ++Summary->ActiveGenericHardDiskOptions;
      }
    }
    LogPrint (Logger, L"  %s active=%s BBS-type=0x%04x description=\"%s\"\r\n", Name, Active ? L"yes" : L"no", BbsDeviceType, Description);
    if ((Config != NULL) && Config->HasLegacyOptionDescription &&
        EqualInsensitive (Description, Config->LegacyOptionDescription))
    {
      if (Summary != NULL) {
        ++Summary->ConfiguredDescriptionMatches;
      }
      LogPrint (Logger, L"    matches configured LegacyOptionDescription\r\n");
    }

    if ((Config != NULL) && Config->HasExcludeDescription &&
        ContainsInsensitive (Description, Config->ExcludeDescription))
    {
      if (Summary != NULL) {
        ++Summary->ExcludedDescriptionMatches;
      }
      LogPrint (Logger, L"    matches configured ExcludeDescription; diagnostic exclusion only\r\n");
    }

    FreePool (Description);
  }

  FreePool (Order);
  return EFI_SUCCESS;
}

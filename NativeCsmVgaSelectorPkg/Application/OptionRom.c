/** @file
  Bounded, read-only PCI option-ROM parsing and target validation.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#include <Uefi.h>

#include <IndustryStandard/Pci22.h>
#include <IndustryStandard/Pci23.h>
#include <IndustryStandard/Pci30.h>
#include <Library/BaseMemoryLib.h>

#include "OptionRom.h"

#define OPTION_ROM_BLOCK_SIZE             512U
#define OPTION_ROM_LAST_IMAGE             BIT7
#define OPTION_ROM_INDICATOR_RESERVED     0x7FU
#define OPTION_ROM_DEVICE_LIST_TERMINATOR 0x0000U
#define OPTION_ROM_CONFIG_HEADER_SIZE     42U
#define OPTION_ROM_CONFIG_LENGTH_OFFSET   0U
#define OPTION_ROM_CONFIG_NAME_OFFSET     1U
#define OPTION_ROM_CONFIG_NAME_SIZE       40U
#define OPTION_ROM_CONFIG_REVISION_OFFSET 41U
#define OPTION_ROM_CONFIG_REVISION_1      1U

STATIC_ASSERT (
  sizeof (PCI_EXPANSION_ROM_HEADER) == 0x1A,
  "Unexpected PCI expansion ROM header declaration"
  );
STATIC_ASSERT (
  OFFSET_OF (PCI_EXPANSION_ROM_HEADER, PcirOffset) == 0x18,
  "Unexpected PCIR pointer offset"
  );
STATIC_ASSERT (
  sizeof (PCI_DATA_STRUCTURE) == 0x18,
  "Unexpected PCI 2.x data structure declaration"
  );
STATIC_ASSERT (
  sizeof (PCI_3_0_DATA_STRUCTURE) == 0x1C,
  "Unexpected PCI 3.0 data structure declaration"
  );

typedef struct {
  BOOLEAN  DirectMatch;
  BOOLEAN  ListMatch;
  UINTN    ListCount;
} OPTION_ROM_DEVICE_MATCH;

STATIC
BOOLEAN
RangeIsInside (
  IN UINTN  ContainerSize,
  IN UINTN  Offset,
  IN UINTN  Length
  )
{
  return (Offset <= ContainerSize) &&
         (Length <= (ContainerSize - Offset));
}

STATIC
BOOLEAN
ReadRomBytes (
  IN  CONST UINT8  *Rom,
  IN  UINTN        RomSize,
  IN  UINTN        Offset,
  OUT VOID         *Destination,
  IN  UINTN        Length
  )
{
  if ((Rom == NULL) || (Destination == NULL) || (Length == 0) ||
      !RangeIsInside (RomSize, Offset, Length))
  {
    return FALSE;
  }

  CopyMem (Destination, Rom + Offset, Length);
  return TRUE;
}

STATIC
BOOLEAN
ComputeChecksum (
  IN  CONST UINT8  *Rom,
  IN  UINTN        RomSize,
  IN  UINTN        Offset,
  IN  UINTN        Length,
  OUT UINT8        *Checksum
  )
{
  UINT8  Sum;
  UINTN  Index;

  if ((Checksum == NULL) || (Length == 0) ||
      !RangeIsInside (RomSize, Offset, Length))
  {
    return FALSE;
  }

  Sum = 0;
  for (Index = 0; Index < Length; ++Index) {
    Sum = (UINT8)(Sum + Rom[Offset + Index]);
  }

  *Checksum = Sum;
  return TRUE;
}

STATIC
CONST CHAR16 *
CodeTypeName (
  IN UINT8  CodeType
  )
{
  switch (CodeType) {
    case PCI_CODE_TYPE_PCAT_IMAGE:
      return L"PCAT/x86 legacy";

    case PCI_CODE_TYPE_EFI_IMAGE:
      return L"EFI/UEFI";

    default:
      return L"unknown";
  }
}

STATIC
EFI_STATUS
ParsePci30DeviceList (
  IN  APP_LOGGER                   *Logger,
  IN  CONST UINT8                  *Rom,
  IN  UINTN                        RomSize,
  IN  UINTN                        ImageOffset,
  IN  UINTN                        ImageSize,
  IN  UINTN                        PcirOffset,
  IN  CONST PCI_3_0_DATA_STRUCTURE *Pcir,
  IN  UINT16                       ExpectedDeviceId,
  OUT OPTION_ROM_DEVICE_MATCH      *Match
  )
{
  UINTN   RelativeOffset;
  UINTN   AbsoluteOffset;
  UINTN   ImageEnd;
  UINTN   MaximumEntries;
  UINTN   Index;
  UINT16  DeviceId;
  BOOLEAN Terminated;

  if ((Logger == NULL) || (Rom == NULL) || (Pcir == NULL) ||
      (Match == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Match->DirectMatch = (BOOLEAN)(Pcir->DeviceId == ExpectedDeviceId);
  Match->ListMatch   = FALSE;
  Match->ListCount   = 0;

  if (Pcir->DeviceListOffset == 0) {
    LogPrint (Logger, L"    PCI 3.0 device list: absent\r\n");
    return EFI_SUCCESS;
  }

  RelativeOffset = (UINTN)Pcir->DeviceListOffset;
  if (((RelativeOffset & (sizeof (UINT16) - 1U)) != 0) ||
      (RelativeOffset < (UINTN)Pcir->Length) ||
      (PcirOffset > (MAX_UINTN - RelativeOffset)))
  {
    LogPrint (
      Logger,
      L"    ERROR: malformed PCI 3.0 device-list offset 0x%04x "
      L"(PCIR length=0x%04x)\r\n",
      Pcir->DeviceListOffset,
      Pcir->Length
      );
    return EFI_COMPROMISED_DATA;
  }

  AbsoluteOffset = PcirOffset + RelativeOffset;
  if ((ImageOffset > (MAX_UINTN - ImageSize)) ||
      (AbsoluteOffset < ImageOffset) ||
      !RangeIsInside (RomSize, AbsoluteOffset, sizeof (UINT16)))
  {
    LogPrint (Logger, L"    ERROR: PCI 3.0 device list begins outside the image\r\n");
    return EFI_COMPROMISED_DATA;
  }

  ImageEnd = ImageOffset + ImageSize;
  if ((AbsoluteOffset > ImageEnd) ||
      (sizeof (UINT16) > (ImageEnd - AbsoluteOffset)))
  {
    LogPrint (Logger, L"    ERROR: PCI 3.0 device list is outside its containing image\r\n");
    return EFI_COMPROMISED_DATA;
  }

  MaximumEntries = (ImageEnd - AbsoluteOffset) / sizeof (UINT16);
  Terminated     = FALSE;
  for (Index = 0; Index < MaximumEntries; ++Index) {
    UINTN  EntryOffset;

    if (Index > ((MAX_UINTN - AbsoluteOffset) / sizeof (UINT16))) {
      LogPrint (Logger, L"    ERROR: PCI 3.0 device-list address overflow\r\n");
      return EFI_COMPROMISED_DATA;
    }

    EntryOffset = AbsoluteOffset + (Index * sizeof (UINT16));
    if (!ReadRomBytes (
           Rom,
           RomSize,
           EntryOffset,
           &DeviceId,
           sizeof (DeviceId)
           ))
    {
      LogPrint (Logger, L"    ERROR: PCI 3.0 device-list read escaped ROM bounds\r\n");
      return EFI_COMPROMISED_DATA;
    }

    if (DeviceId == OPTION_ROM_DEVICE_LIST_TERMINATOR) {
      Terminated = TRUE;
      LogPrint (
        Logger,
        L"    PCI 3.0 device list: terminator after %u entr%s\r\n",
        (UINT32)Match->ListCount,
        (Match->ListCount == 1) ? L"y" : L"ies"
        );
      break;
    }

    LogPrint (
      Logger,
      L"      device-list[%u]=0x%04x%s\r\n",
      (UINT32)Match->ListCount,
      DeviceId,
      (DeviceId == ExpectedDeviceId) ? L" (target)" : L""
      );
    ++Match->ListCount;
    if (DeviceId == ExpectedDeviceId) {
      Match->ListMatch = TRUE;
    }
  }

  if (!Terminated) {
    LogPrint (Logger, L"    ERROR: PCI 3.0 device list has no in-image zero terminator\r\n");
    return EFI_COMPROMISED_DATA;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ValidatePci30Metadata (
  IN APP_LOGGER                   *Logger,
  IN CONST UINT8                  *Rom,
  IN UINTN                        RomSize,
  IN UINTN                        ImageOffset,
  IN UINTN                        ImageSize,
  IN CONST PCI_3_0_DATA_STRUCTURE *Pcir
  )
{
  UINTN   MaximumRuntimeSize;
  UINTN   ConfigOffset;
  UINTN   ConfigAbsoluteOffset;
  UINTN   ConfigSize;
  UINTN   Index;
  UINTN   NameLength;
  UINTN   DmtfOffset;
  UINT8   ConfigHeader[OPTION_ROM_CONFIG_HEADER_SIZE];
  UINT8   ConfigBlocks;
  UINT8   ConfigRevision;
  BOOLEAN NameTerminated;

  if ((Logger == NULL) || (Rom == NULL) || (Pcir == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Pcir->MaxRuntimeImageLength > Pcir->ImageLength) {
    LogPrint (
      Logger,
      L"    ERROR: PCI 3.x Maximum Run-time Image Length %u blocks "
      L"exceeds Image Length %u blocks\r\n",
      (UINT32)Pcir->MaxRuntimeImageLength,
      (UINT32)Pcir->ImageLength
      );
    return EFI_COMPROMISED_DATA;
  }

  MaximumRuntimeSize =
    (UINTN)Pcir->MaxRuntimeImageLength * OPTION_ROM_BLOCK_SIZE;
  LogPrint (
    Logger,
    L"    PCI 3.x maximum run-time blocks=%u bytes=0x%lx\r\n",
    (UINT32)Pcir->MaxRuntimeImageLength,
    (UINT64)MaximumRuntimeSize
    );

  if (Pcir->ConfigUtilityCodeHeaderOffset != 0) {
    ConfigOffset = (UINTN)Pcir->ConfigUtilityCodeHeaderOffset;
    if (!RangeIsInside (
           ImageSize,
           ConfigOffset,
           OPTION_ROM_CONFIG_HEADER_SIZE
           ) ||
        (ImageOffset > (MAX_UINTN - ConfigOffset)))
    {
      LogPrint (
        Logger,
        L"    ERROR: Configuration Utility Code Header offset 0x%04x "
        L"does not contain a complete 42-byte header inside the image\r\n",
        Pcir->ConfigUtilityCodeHeaderOffset
        );
      return EFI_COMPROMISED_DATA;
    }

    ConfigAbsoluteOffset = ImageOffset + ConfigOffset;
    if (!ReadRomBytes (
           Rom,
           RomSize,
           ConfigAbsoluteOffset,
           ConfigHeader,
           sizeof (ConfigHeader)
           ))
    {
      LogPrint (Logger, L"    ERROR: Configuration Utility Code Header read escaped ROM bounds\r\n");
      return EFI_COMPROMISED_DATA;
    }

    ConfigBlocks   = ConfigHeader[OPTION_ROM_CONFIG_LENGTH_OFFSET];
    ConfigRevision = ConfigHeader[OPTION_ROM_CONFIG_REVISION_OFFSET];
    if (ConfigBlocks == 0) {
      LogPrint (Logger, L"    ERROR: Configuration Utility Code Length is zero\r\n");
      return EFI_COMPROMISED_DATA;
    }

    if (ConfigRevision != OPTION_ROM_CONFIG_REVISION_1) {
      LogPrint (
        Logger,
        L"    ERROR: Configuration Utility Code Header revision=%u; "
        L"PCI Firmware 3.0 requires revision 1\r\n",
        (UINT32)ConfigRevision
        );
      return EFI_COMPROMISED_DATA;
    }

    ConfigSize = (UINTN)ConfigBlocks * OPTION_ROM_BLOCK_SIZE;
    if (!RangeIsInside (ImageSize, ConfigOffset, ConfigSize)) {
      LogPrint (
        Logger,
        L"    ERROR: Configuration Utility extent offset=0x%04x "
        L"blocks=%u bytes=0x%lx escapes the image\r\n",
        Pcir->ConfigUtilityCodeHeaderOffset,
        (UINT32)ConfigBlocks,
        (UINT64)ConfigSize
        );
      return EFI_COMPROMISED_DATA;
    }

    NameTerminated = FALSE;
    NameLength     = 0;
    for (Index = 0; Index < OPTION_ROM_CONFIG_NAME_SIZE; ++Index) {
      UINT8  Character;

      Character = ConfigHeader[OPTION_ROM_CONFIG_NAME_OFFSET + Index];
      if (Character == 0) {
        NameTerminated = TRUE;
        NameLength     = Index;
        break;
      }

      if ((Character < 0x20U) || (Character > 0x7EU)) {
        LogPrint (
          Logger,
          L"    ERROR: Configuration Utility identifier has non-ASCII "
          L"text byte 0x%02x at index %u\r\n",
          Character,
          (UINT32)Index
          );
        return EFI_COMPROMISED_DATA;
      }
    }

    if (!NameTerminated) {
      LogPrint (Logger, L"    ERROR: Configuration Utility 40-byte identifier is not null-terminated\r\n");
      return EFI_COMPROMISED_DATA;
    }

    LogPrint (
      Logger,
      L"    Configuration Utility header offset=0x%04x revision=%u "
      L"identifier-bytes=%u blocks=%u contained-bytes=0x%lx\r\n",
      Pcir->ConfigUtilityCodeHeaderOffset,
      (UINT32)ConfigRevision,
      (UINT32)NameLength,
      (UINT32)ConfigBlocks,
      (UINT64)ConfigSize
      );
  } else {
    LogPrint (Logger, L"    PCI 3.x Configuration Utility Code Header: absent\r\n");
  }

  if (Pcir->DMTFCLPEntryPointOffset != 0) {
    DmtfOffset = (UINTN)Pcir->DMTFCLPEntryPointOffset;
    if (!RangeIsInside (ImageSize, DmtfOffset, 1U)) {
      LogPrint (
        Logger,
        L"    ERROR: DMTF CLP entry-point offset 0x%04x is outside the image\r\n",
        Pcir->DMTFCLPEntryPointOffset
        );
      return EFI_COMPROMISED_DATA;
    }

    LogPrint (
      Logger,
      L"    PCI 3.x DMTF CLP entry-point offset=0x%04x (in-image)\r\n",
      Pcir->DMTFCLPEntryPointOffset
      );
  } else {
    LogPrint (Logger, L"    PCI 3.x DMTF CLP entry point: absent\r\n");
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
LogAndValidateLegacyHeader (
  IN  APP_LOGGER  *Logger,
  IN  CONST UINT8  *Rom,
  IN  UINTN       RomSize,
  IN  UINTN       ImageOffset,
  IN  UINTN       ImageSize,
  OUT BOOLEAN     *LegacyChecksumValid,
  OUT UINTN       *LegacySizeResult
  )
{
  UINT8   Size512;
  UINTN   SizeFieldOffset;
  UINTN   LegacySize;
  UINT8   LegacyChecksum;

  if ((Logger == NULL) || (Rom == NULL) || (LegacyChecksumValid == NULL) ||
      (LegacySizeResult == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *LegacyChecksumValid = FALSE;
  *LegacySizeResult    = 0;
  if (ImageOffset >
      (MAX_UINTN - OFFSET_OF (EFI_LEGACY_EXPANSION_ROM_HEADER, Size512)))
  {
    LogPrint (Logger, L"    ERROR: legacy Size512 field address overflows\r\n");
    return EFI_COMPROMISED_DATA;
  }

  SizeFieldOffset = ImageOffset +
                    OFFSET_OF (EFI_LEGACY_EXPANSION_ROM_HEADER, Size512);
  if (!ReadRomBytes (
         Rom,
         RomSize,
         SizeFieldOffset,
         &Size512,
         sizeof (Size512)
         ))
  {
    LogPrint (Logger, L"    ERROR: legacy Size512 field is out of range\r\n");
    return EFI_COMPROMISED_DATA;
  }

  if (Size512 == 0) {
    LogPrint (Logger, L"    ERROR: PCAT image has legacy Size512=0\r\n");
    return EFI_COMPROMISED_DATA;
  }

  LegacySize = (UINTN)Size512 * OPTION_ROM_BLOCK_SIZE;
  if ((LegacySize > ImageSize) ||
      !RangeIsInside (RomSize, ImageOffset, LegacySize))
  {
    LogPrint (
      Logger,
      L"    ERROR: legacy Size512=%u (0x%lx bytes) exceeds image size 0x%lx\r\n",
      (UINT32)Size512,
      (UINT64)LegacySize,
      (UINT64)ImageSize
      );
    return EFI_COMPROMISED_DATA;
  }

  if (!ComputeChecksum (
         Rom,
         RomSize,
         ImageOffset,
         LegacySize,
         &LegacyChecksum
         ))
  {
    LogPrint (Logger, L"    ERROR: legacy checksum range validation failed\r\n");
    return EFI_COMPROMISED_DATA;
  }

  *LegacyChecksumValid = (BOOLEAN)(LegacyChecksum == 0);
  *LegacySizeResult    = LegacySize;
  LogPrint (
    Logger,
    L"    legacy Size512=%u bytes=0x%lx checksum-sum=0x%02x (%s)%s\r\n",
    (UINT32)Size512,
    (UINT64)LegacySize,
    LegacyChecksum,
    *LegacyChecksumValid ? L"valid" : L"INVALID",
    (LegacySize == ImageSize) ? L"" : L"; differs from PCIR image length"
    );
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ValidatePcatLayout (
  IN APP_LOGGER                    *Logger,
  IN UINTN                         RelativePcirOffset,
  IN UINTN                         PcirLength,
  IN UINTN                         LegacySize,
  IN CONST PCI_3_0_DATA_STRUCTURE  *Pcir30 OPTIONAL
  )
{
  UINTN  MaximumRuntimeSize;

  if (Logger == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!RangeIsInside (LegacySize, RelativePcirOffset, PcirLength)) {
    LogPrint (
      Logger,
      L"    ERROR: complete PCIR extent offset=0x%lx length=0x%lx "
      L"is not contained in legacy Size512 bytes=0x%lx\r\n",
      (UINT64)RelativePcirOffset,
      (UINT64)PcirLength,
      (UINT64)LegacySize
      );
    return EFI_COMPROMISED_DATA;
  }

  if (Pcir30 == NULL) {
    LogPrint (Logger, L"    PCIR declared extent is contained in the legacy initialization region\r\n");
    return EFI_SUCCESS;
  }

  MaximumRuntimeSize =
    (UINTN)Pcir30->MaxRuntimeImageLength * OPTION_ROM_BLOCK_SIZE;
  if (MaximumRuntimeSize > LegacySize) {
    LogPrint (
      Logger,
      L"    ERROR: PCI 3.x maximum run-time bytes=0x%lx exceed "
      L"legacy initialization bytes=0x%lx\r\n",
      (UINT64)MaximumRuntimeSize,
      (UINT64)LegacySize
      );
    return EFI_COMPROMISED_DATA;
  }

  if ((MaximumRuntimeSize != 0) &&
      !RangeIsInside (
         MaximumRuntimeSize,
         RelativePcirOffset,
         PcirLength
         ))
  {
    LogPrint (
      Logger,
      L"    ERROR: PCIR declared extent is not contained in the "
      L"nonzero PCI 3.x maximum run-time region (0x%lx bytes)\r\n",
      (UINT64)MaximumRuntimeSize
      );
    return EFI_COMPROMISED_DATA;
  }

  LogPrint (
    Logger,
    (MaximumRuntimeSize == 0) ?
      L"    PCI 3.x run-time size is zero; PCIR is contained in the initialization region\r\n" :
      L"    PCIR is contained in both the PCI 3.x run-time and initialization regions\r\n"
    );
  return EFI_SUCCESS;
}

EFI_STATUS
OptionRomParseAndValidate (
  IN  APP_LOGGER             *Logger,
  IN  CONST VOID             *RomImage,
  IN  UINT64                 RomSize,
  IN  UINT64                 ExpectedRomSize,
  IN  UINT16                 ExpectedVendorId,
  IN  UINT16                 ExpectedDeviceId,
  OUT OPTION_ROM_VALIDATION  *Validation
  )
{
  CONST UINT8           *Rom;
  OPTION_ROM_VALIDATION LocalValidation;
  UINTN                 BoundedRomSize;
  UINTN                 ImageOffset;
  UINTN                 MaximumImages;
  BOOLEAN               SawMatchingLegacy;
  BOOLEAN               SawMatchingLegacyChecksumFailure;

  if (Validation != NULL) {
    ZeroMem (Validation, sizeof (*Validation));
  }

  if ((Logger == NULL) || (RomImage == NULL) || (Validation == NULL) ||
      (ExpectedRomSize == 0) || (ExpectedVendorId == 0) ||
      (ExpectedDeviceId == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&LocalValidation, sizeof (LocalValidation));
  LogPrint (Logger, L"\r\n=== Read-only PCI option-ROM parser ===\r\n");
  LogPrint (
    Logger,
    L"Exposed ROM base=0x%016lx size=0x%016lx; required size=0x%016lx "
    L"target=%04x:%04x\r\n",
    (UINT64)(UINTN)RomImage,
    RomSize,
    ExpectedRomSize,
    ExpectedVendorId,
    ExpectedDeviceId
    );

  if (RomSize != ExpectedRomSize) {
    LogPrint (Logger, L"ERROR: exposed ROM size does not exactly match the hardware binding\r\n");
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((RomSize < OPTION_ROM_BLOCK_SIZE) ||
      ((RomSize & (OPTION_ROM_BLOCK_SIZE - 1U)) != 0) ||
      (RomSize > MAX_UINTN))
  {
    LogPrint (Logger, L"ERROR: exposed ROM size is not a bounded nonzero multiple of 512 bytes\r\n");
    return EFI_BAD_BUFFER_SIZE;
  }

  BoundedRomSize = (UINTN)RomSize;
  if ((UINTN)RomImage > (MAX_UINTN - (BoundedRomSize - 1U))) {
    LogPrint (Logger, L"ERROR: exposed ROM address range wraps native address space\r\n");
    return EFI_BAD_BUFFER_SIZE;
  }

  Rom                              = (CONST UINT8 *)RomImage;
  ImageOffset                      = 0;
  MaximumImages                    = BoundedRomSize / OPTION_ROM_BLOCK_SIZE;
  SawMatchingLegacy               = FALSE;
  SawMatchingLegacyChecksumFailure = FALSE;

  while (LocalValidation.ImageCount < MaximumImages) {
    PCI_EXPANSION_ROM_HEADER    Header;
    PCI_DATA_STRUCTURE         Pcir22;
    PCI_3_0_DATA_STRUCTURE     Pcir30;
    OPTION_ROM_DEVICE_MATCH    DeviceMatch;
    EFI_STATUS                 Status;
    UINTN                      RelativePcirOffset;
    UINTN                      AbsolutePcirOffset;
    UINTN                      ImageSize;
    UINTN                      LegacySize;
    UINTN                      NextImageOffset;
    UINT8                      ImageChecksum;
    BOOLEAN                    ImageChecksumValid;
    BOOLEAN                    LegacyChecksumValid;
    BOOLEAN                    VendorMatches;
    BOOLEAN                    DeviceMatches;
    BOOLEAN                    TargetPcat;
    BOOLEAN                    LastImage;

    ZeroMem (&Header, sizeof (Header));
    ZeroMem (&Pcir22, sizeof (Pcir22));
    ZeroMem (&Pcir30, sizeof (Pcir30));
    ZeroMem (&DeviceMatch, sizeof (DeviceMatch));
    LegacyChecksumValid = FALSE;
    LegacySize          = 0;

    if (!ReadRomBytes (
           Rom,
           BoundedRomSize,
           ImageOffset,
           &Header,
           sizeof (Header)
           ))
    {
      LogPrint (Logger, L"ERROR: image header at offset 0x%lx is truncated\r\n", (UINT64)ImageOffset);
      return EFI_COMPROMISED_DATA;
    }

    ++LocalValidation.ImageCount;
    LogPrint (
      Logger,
      L"\r\nROM image[%u] offset=0x%lx signature=0x%04x PCIR-relative=0x%04x\r\n",
      (UINT32)(LocalValidation.ImageCount - 1U),
      (UINT64)ImageOffset,
      Header.Signature,
      Header.PcirOffset
      );

    if (Header.Signature != PCI_EXPANSION_ROM_HEADER_SIGNATURE) {
      LogPrint (Logger, L"  ERROR: invalid option-ROM signature; expected 0x55AA\r\n");
      return EFI_COMPROMISED_DATA;
    }

    RelativePcirOffset = (UINTN)Header.PcirOffset;
    if ((RelativePcirOffset < sizeof (Header)) ||
        ((RelativePcirOffset & 3U) != 0) ||
        (ImageOffset > (MAX_UINTN - RelativePcirOffset)))
    {
      LogPrint (Logger, L"  ERROR: PCIR offset is zero, overlapping, unaligned, or overflowing\r\n");
      return EFI_COMPROMISED_DATA;
    }

    AbsolutePcirOffset = ImageOffset + RelativePcirOffset;
    if (!ReadRomBytes (
           Rom,
           BoundedRomSize,
           AbsolutePcirOffset,
           &Pcir22,
           sizeof (Pcir22)
           ))
    {
      LogPrint (Logger, L"  ERROR: base PCIR structure is outside the exposed ROM\r\n");
      return EFI_COMPROMISED_DATA;
    }

    if (Pcir22.Signature != PCI_DATA_STRUCTURE_SIGNATURE) {
      LogPrint (
        Logger,
        L"  ERROR: invalid PCIR signature 0x%08x; expected 'PCIR'\r\n",
        Pcir22.Signature
        );
      return EFI_COMPROMISED_DATA;
    }

    if (Pcir22.ImageLength == 0) {
      LogPrint (Logger, L"  ERROR: PCIR image length is zero\r\n");
      return EFI_COMPROMISED_DATA;
    }

    ImageSize = (UINTN)Pcir22.ImageLength * OPTION_ROM_BLOCK_SIZE;
    if (!RangeIsInside (BoundedRomSize, ImageOffset, ImageSize)) {
      LogPrint (
        Logger,
        L"  ERROR: PCIR image length %u blocks (0x%lx bytes) escapes the exposed ROM\r\n",
        (UINT32)Pcir22.ImageLength,
        (UINT64)ImageSize
        );
      return EFI_COMPROMISED_DATA;
    }

    if ((Pcir22.Length < sizeof (Pcir22)) ||
        !RangeIsInside (ImageSize, RelativePcirOffset, (UINTN)Pcir22.Length))
    {
      LogPrint (
        Logger,
        L"  ERROR: PCIR length 0x%04x is too small or escapes its image\r\n",
        Pcir22.Length
        );
      return EFI_COMPROMISED_DATA;
    }

    if (Pcir22.Revision >= 3) {
      if ((Pcir22.Length < sizeof (Pcir30)) ||
          !ReadRomBytes (
             Rom,
             BoundedRomSize,
             AbsolutePcirOffset,
             &Pcir30,
             sizeof (Pcir30)
             ))
      {
        LogPrint (Logger, L"  ERROR: PCI 3.x PCIR structure is truncated\r\n");
        return EFI_COMPROMISED_DATA;
      }

      Status = ParsePci30DeviceList (
                 Logger,
                 Rom,
                 BoundedRomSize,
                 ImageOffset,
                 ImageSize,
                 AbsolutePcirOffset,
                 &Pcir30,
                 ExpectedDeviceId,
                 &DeviceMatch
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }

      Status = ValidatePci30Metadata (
                 Logger,
                 Rom,
                 BoundedRomSize,
                 ImageOffset,
                 ImageSize,
                 &Pcir30
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }
    } else {
      DeviceMatch.DirectMatch = (BOOLEAN)(Pcir22.DeviceId == ExpectedDeviceId);
    }

    VendorMatches = (BOOLEAN)(Pcir22.VendorId == ExpectedVendorId);
    DeviceMatches = (BOOLEAN)(DeviceMatch.DirectMatch || DeviceMatch.ListMatch);
    LastImage     = (BOOLEAN)((Pcir22.Indicator & OPTION_ROM_LAST_IMAGE) != 0);
    if ((Pcir22.Indicator & OPTION_ROM_INDICATOR_RESERVED) != 0) {
      LogPrint (
        Logger,
        L"  ERROR: PCIR indicator 0x%02x has reserved bits set\r\n",
        Pcir22.Indicator
        );
      return EFI_COMPROMISED_DATA;
    }

    if (!ComputeChecksum (
           Rom,
           BoundedRomSize,
           ImageOffset,
           ImageSize,
           &ImageChecksum
           ))
    {
      LogPrint (Logger, L"  ERROR: image checksum range validation failed\r\n");
      return EFI_COMPROMISED_DATA;
    }

    ImageChecksumValid = (BOOLEAN)(ImageChecksum == 0);
    LogPrint (
      Logger,
      L"  PCIR absolute=0x%lx length=0x%04x revision=0x%02x "
      L"vendor:device=%04x:%04x\r\n",
      (UINT64)AbsolutePcirOffset,
      Pcir22.Length,
      Pcir22.Revision,
      Pcir22.VendorId,
      Pcir22.DeviceId
      );
    LogPrint (
      Logger,
      L"  CodeRevision=0x%04x CodeType=0x%02x (%s) "
      L"Indicator=0x%02x last=%s\r\n",
      Pcir22.CodeRevision,
      Pcir22.CodeType,
      CodeTypeName (Pcir22.CodeType),
      Pcir22.Indicator,
      LastImage ? L"yes" : L"no"
      );
    LogPrint (
      Logger,
      L"  image blocks=%u bytes=0x%lx checksum-sum=0x%02x (%s)\r\n",
      (UINT32)Pcir22.ImageLength,
      (UINT64)ImageSize,
      ImageChecksum,
      ImageChecksumValid ? L"valid" : L"INVALID"
      );

    if (Pcir22.Revision >= 3) {
      LogPrint (
        Logger,
        L"  PCI 3.x DeviceListOffset=0x%04x entries=%u direct-match=%s "
        L"list-match=%s MaxRuntimeImageLength=%u\r\n",
        Pcir30.DeviceListOffset,
        (UINT32)DeviceMatch.ListCount,
        DeviceMatch.DirectMatch ? L"yes" : L"no",
        DeviceMatch.ListMatch ? L"yes" : L"no",
        (UINT32)Pcir30.MaxRuntimeImageLength
        );
    }

    if (Pcir22.CodeType == PCI_CODE_TYPE_PCAT_IMAGE) {
      Status = LogAndValidateLegacyHeader (
                 Logger,
                 Rom,
                 BoundedRomSize,
                 ImageOffset,
                 ImageSize,
                 &LegacyChecksumValid,
                 &LegacySize
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }

      Status = ValidatePcatLayout (
                 Logger,
                 RelativePcirOffset,
                 (UINTN)Pcir22.Length,
                 LegacySize,
                 (Pcir22.Revision >= 3) ? &Pcir30 : NULL
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    TargetPcat = (BOOLEAN)(
                           (Pcir22.CodeType == PCI_CODE_TYPE_PCAT_IMAGE) &&
                           VendorMatches && DeviceMatches
                           );
    if (TargetPcat) {
      SawMatchingLegacy = TRUE;
      if (ImageChecksumValid && LegacyChecksumValid) {
        ++LocalValidation.MatchingLegacyCandidateCount;
        if (!LocalValidation.MatchingLegacyImageFound) {
          LocalValidation.MatchingLegacyImageFound    = TRUE;
          LocalValidation.MatchingLegacyImageOffset   = (UINT64)ImageOffset;
          LocalValidation.MatchingLegacyImageSize     = (UINT64)ImageSize;
          LocalValidation.MatchingLegacyPcirOffset    = (UINT64)AbsolutePcirOffset;
          LocalValidation.MatchingLegacyCodeRevision  = Pcir22.CodeRevision;
          LocalValidation.MatchingLegacyPcirRevision  = Pcir22.Revision;
        }

        LogPrint (Logger, L"  TARGET PCAT candidate: checksum-valid and accepted\r\n");
      } else {
        SawMatchingLegacyChecksumFailure = TRUE;
        LogPrint (Logger, L"  TARGET PCAT candidate: rejected because a checksum is invalid\r\n");
      }
    } else if ((Pcir22.CodeType == PCI_CODE_TYPE_EFI_IMAGE) &&
               VendorMatches && DeviceMatches)
    {
      LocalValidation.MatchingUefiImageSeen = TRUE;
      LogPrint (Logger, L"  Target EFI/UEFI image observed; it is enumeration-only and cannot be selected\r\n");
    } else {
      LogPrint (Logger, L"  Image does not satisfy the target PCAT vendor/device binding\r\n");
    }

    if (ImageOffset > (MAX_UINTN - ImageSize)) {
      LogPrint (Logger, L"  ERROR: next-image offset arithmetic overflow\r\n");
      return EFI_COMPROMISED_DATA;
    }

    NextImageOffset = ImageOffset + ImageSize;
    if (NextImageOffset <= ImageOffset) {
      LogPrint (Logger, L"  ERROR: image chain made no forward progress\r\n");
      return EFI_COMPROMISED_DATA;
    }

    if (LastImage) {
      if (NextImageOffset != BoundedRomSize) {
        LogPrint (
          Logger,
          L"  ERROR: final image ends at 0x%lx but exposed ROM ends at "
          L"0x%lx; trailing bytes are not permitted\r\n",
          (UINT64)NextImageOffset,
          (UINT64)BoundedRomSize
          );
        return EFI_COMPROMISED_DATA;
      }

      LocalValidation.FinalIndicatorSeen = TRUE;
      LogPrint (
        Logger,
        L"  Final image indicator reached at exact exposed-ROM end "
        L"offset 0x%lx\r\n",
        (UINT64)NextImageOffset
        );
      break;
    }

    if (NextImageOffset >= BoundedRomSize) {
      LogPrint (Logger, L"ERROR: image chain consumed the ROM without a final indicator\r\n");
      return EFI_COMPROMISED_DATA;
    }

    ImageOffset = NextImageOffset;
  }

  if (!LocalValidation.FinalIndicatorSeen) {
    LogPrint (Logger, L"ERROR: no final-image indicator was found within the bounded ROM\r\n");
    return EFI_COMPROMISED_DATA;
  }

  if (!LocalValidation.MatchingLegacyImageFound) {
    if (SawMatchingLegacyChecksumFailure || SawMatchingLegacy) {
      LogPrint (Logger, L"ERROR: target PCAT image exists but is not checksum-valid\r\n");
      return EFI_CRC_ERROR;
    }

    if (LocalValidation.MatchingUefiImageSeen) {
      LogPrint (Logger, L"ERROR: target ROM is UEFI-only; x86 legacy execution is forbidden\r\n");
      return EFI_UNSUPPORTED;
    }

    LogPrint (Logger, L"ERROR: no PCAT image matches the requested vendor/device pair\r\n");
    return EFI_NOT_FOUND;
  }

  LogPrint (
    Logger,
    L"Option-ROM validation passed: %u image(s), %u checksum-valid target "
    L"PCAT candidate(s), first candidate offset=0x%lx size=0x%lx.\r\n",
    (UINT32)LocalValidation.ImageCount,
    (UINT32)LocalValidation.MatchingLegacyCandidateCount,
    LocalValidation.MatchingLegacyImageOffset,
    LocalValidation.MatchingLegacyImageSize
    );
  *Validation = LocalValidation;
  return EFI_SUCCESS;
}

/* Synthetic firmware edge cases. Never install this harness on real hardware.
   SPDX-License-Identifier: GPL-3.0-only */
#include <Uefi.h>
STATIC UINTN CompatibilityTestStart;
STATIC UINTN CompatibilityTestEnd;

#define NCV_TEST_SCANNER 1
#define NativeCsmVgaRuntimePlanBuild HarnessRuntimePlanBuild
#define NativeCsmVgaRuntimePlanFinalizeValidation HarnessRuntimePlanFinalizeValidation
#define NativeCsmVgaRuntimePlanRelease HarnessRuntimePlanRelease
#include "RuntimePlan.c"

#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>

STATIC APP_LOGGER Logger;
STATIC UINTN Checks;
STATIC UINTN FailedLine;
STATIC NATIVE_CSM_VGA_RUNTIME_PLAN Plan;
STATIC PCI_DEVICE_RECORD Records[3];
STATIC EFI_PCI_IO_PROTOCOL Pci[3];
STATIC EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL Root;
STATIC EFI_BOOT_SERVICES TestBoot;
STATIC UINTN MapCalls;
STATIC UINTN MapMode;

#define REQUIRE(Expression) do { \
  ++Checks; \
  if (!(Expression)) { FailedLine = __LINE__; \
    LogPrint (&Logger, L"FAIL line %u: %a\r\n", (UINT32)__LINE__, #Expression); \
    return EFI_ABORTED; } \
} while (0)

STATIC EFI_STATUS MemoryTests (VOID)
{
  EFI_MEMORY_DESCRIPTOR D[4];
  struct { EFI_MEMORY_DESCRIPTOR Descriptor; UINT64 Tail[2]; } Padded[2];
  MEMORY_MAP_SNAPSHOT Map;
  UINTN Bytes, Index;
  CONST UINT32 Rejected[] = { EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
                             EfiUnusableMemory, EfiUnacceptedMemoryType };

  ZeroMem (D, sizeof (D));
  ZeroMem (&Map, sizeof (Map));
  Map.Map = D;
  Map.MapSize = 3 * sizeof (D[0]);
  Map.DescriptorSize = sizeof (D[0]);
  for (Index = 0; Index < 3; ++Index) {
    D[Index].PhysicalStart = 0x1000 + 0x1000 * Index;
    D[Index].NumberOfPages = 1;
    D[Index].Type = EfiBootServicesData;
  }
  REQUIRE (MemoryMapRangeIsWritable (&Map, 0x1800, 0x2000));
  D[1].Attribute = EFI_MEMORY_RO;
  REQUIRE (!MemoryMapRangeIsWritable (&Map, 0x1800, 0x2000));
  D[1].Attribute = EFI_MEMORY_WP;
  REQUIRE (!MemoryMapRangeIsWritable (&Map, 0x1800, 0x2000));
  D[1].Attribute = 0;
  D[1].Type = EfiBootServicesCode;
  REQUIRE (!MemoryMapRangeIsWritable (&Map, 0x1800, 0x2000));
  D[1].Type = EfiBootServicesData;
  REQUIRE (MemoryMapRangeIsReadable (&Map, 0x1800, 0x2000, &Bytes));
  REQUIRE (Bytes == 0x2800);
  REQUIRE (MemoryMapRangeIsReadable (&Map, 0x1000, 0x3000, NULL));
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1000, 0x3001, &Bytes));
  REQUIRE (Bytes == 0x3000);
  REQUIRE (MemoryMapRangeIsReadable (&Map, 0x3fff, 1, &Bytes) && Bytes == 1);
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x4000, 1, &Bytes) && Bytes == 0);
  // Deliberately unsorted map, mixed readable types and descriptor padding.
  D[0].PhysicalStart = 0x3000;
  D[2].PhysicalStart = 0x1000;
  D[1].Type = EfiACPIReclaimMemory;
  REQUIRE (MemoryMapRangeIsReadable (&Map, 0x1800, 0x2800, &Bytes) && Bytes == 0x2800);
  for (Index = 0; Index < ARRAY_SIZE (Rejected); ++Index) {
    D[1].Type = Rejected[Index];
    REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1800, 0x1000, &Bytes) && Bytes == 0x800);
    REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x2000, 1, NULL));
  }
  D[1].Type = EfiBootServicesData;
  D[1].Attribute = EFI_MEMORY_RP;
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1800, 0x1000, &Bytes) && Bytes == 0x800);
  D[1].Attribute = EFI_MEMORY_RO | EFI_MEMORY_XP;
  REQUIRE (MemoryMapRangeIsReadable (&Map, 0x1800, 0x1000, NULL));
  D[1].Attribute = 0;
  D[1].NumberOfPages = 0;
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1800, 0x1000, &Bytes) && Bytes == 0x800);
  D[1].NumberOfPages = 1;
  D[1].PhysicalStart = 0x4000;
  REQUIRE (!MemoryMapRangeIsWritable (&Map, 0x1800, 0x2000));
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1800, 0x1000, NULL));
  D[1].PhysicalStart = 0x2000;
  D[2].NumberOfPages = 2; // Overlap beginning at the next boundary.
  REQUIRE (!MemoryMapRangeIsWritable (&Map, 0x1800, 0x1000));
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1800, 0x1000, &Bytes) && Bytes == 0x800);
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x2000, 1, &Bytes) && Bytes == 0);
  D[1].Attribute = EFI_MEMORY_RP;
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x2000, 1, NULL));
  D[2].NumberOfPages = 1;
  D[1].Attribute = 0;
  Map.MapSize--;
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1000, 1, NULL));
  Map.MapSize++;
  Map.DescriptorSize = sizeof (D[0]) - 1;
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1000, 1, NULL));
  Map.DescriptorSize = sizeof (D[0]);
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1000, 0, &Bytes) && Bytes == 0);
  REQUIRE (!MemoryMapRangeIsReadable (&Map, MAX_UINTN, 2, NULL));
  REQUIRE (!MemoryMapRangeIsReadable (NULL, 0, 1, NULL));
  D[1].NumberOfPages = MAX_UINT64 / EFI_PAGE_SIZE + 1;
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1000, 1, NULL));
  D[1].NumberOfPages = 1;
  D[1].PhysicalStart = MAX_UINT64 - 1;
  REQUIRE (!MemoryMapRangeIsReadable (&Map, 0x1000, 1, NULL));
  D[1].PhysicalStart = MAX_UINT64 - EFI_PAGE_MASK;
  REQUIRE (MemoryMapRangeIsReadable (&Map, MAX_UINTN, 1, &Bytes) && Bytes == 1);
  ZeroMem (Padded, sizeof (Padded));
  Padded[0].Descriptor = D[2];
  Padded[1].Descriptor = D[0];
  Padded[1].Descriptor.PhysicalStart = 0x2000;
  SetMem (Padded[0].Tail, sizeof (Padded[0].Tail), 0xff);
  Map.Map = &Padded[0].Descriptor;
  Map.DescriptorSize = sizeof (Padded[0]);
  Map.MapSize = sizeof (Padded);
  REQUIRE (MemoryMapRangeIsReadable (&Map, 0x1800, 0x1800, &Bytes) && Bytes == 0x1800);
  LogPrint (&Logger, L"PASS contiguous/unsorted memory, gaps, protection, overlap and overflow\r\n");
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI GetTestMap (
  UINTN *Size, EFI_MEMORY_DESCRIPTOR *Map, UINTN *Key, UINTN *Stride, UINT32 *Version
  )
{
  UINTN Capacity = *Size;
  ++MapCalls;
  *Key = 1;
  *Version = EFI_MEMORY_DESCRIPTOR_VERSION;
  *Stride = sizeof (*Map);
  *Size = sizeof (*Map);
  if (Map == NULL) {
    if (MapMode == 1) { *Stride = 0; }
    if (MapMode == 2) { *Stride = sizeof (*Map) - 1; }
    if (MapMode == 3) { *Size = MAX_UINTN; }
    return EFI_BUFFER_TOO_SMALL;
  }
  if ((MapMode == 9) || ((MapMode == 10) && (MapCalls == 2))) {
    *Size = Capacity + sizeof (*Map);
    return EFI_BUFFER_TOO_SMALL;
  }
  ZeroMem (Map, sizeof (*Map));
  Map->PhysicalStart = MapMode >= 11 ? 0 : 0x1000;
  Map->NumberOfPages = MapMode >= 11 ? 256 : 1;
  if (MapMode == 12) { Map->Attribute = EFI_MEMORY_RP; }
  if (MapMode == 13) { Map->NumberOfPages = 0xc0; }
  Map->Type = EfiBootServicesData;
  if (MapMode == 4) { *Stride = 0; }
  if (MapMode == 5) { *Stride = sizeof (*Map) - 1; }
  if (MapMode == 6) { *Size = 0; }
  if (MapMode == 7) { *Size = sizeof (*Map) + 1; }
  if (MapMode == 8) { *Size = Capacity + 1; }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS CaptureTests (VOID)
{
  EFI_BOOT_SERVICES *Original = gBS;
  EFI_STATUS Status;
  MEMORY_MAP_SNAPSHOT Map;
  CopyMem (&TestBoot, gBS, sizeof (TestBoot));
  TestBoot.GetMemoryMap = GetTestMap;
  for (MapMode = 0; MapMode <= 10; ++MapMode) {
    MapCalls = 0;
    gBS = &TestBoot; // Application-local services pointer; no firmware patch.
    Status = MemoryMapCapture (&Map);
    gBS = Original;
    if ((MapMode == 0) || (MapMode == 10)) {
      REQUIRE (Status == EFI_SUCCESS);
      REQUIRE (MemoryMapRangeIsReadable (&Map, 0x1000, 1, NULL));
      MemoryMapRelease (&Map);
      REQUIRE (Map.Map == NULL);
    } else {
      REQUIRE (Status == (MapMode == 9 ? EFI_BUFFER_TOO_SMALL : EFI_BAD_BUFFER_SIZE));
      REQUIRE (Map.Map == NULL);
    }
  }
  for (MapMode = 11; MapMode <= 13; ++MapMode) {
    gBS = &TestBoot;
    Status = MemoryMapValidateBootRanges ();
    gBS = Original;
    REQUIRE (Status == (MapMode == 11 ? EFI_SUCCESS : EFI_SECURITY_VIOLATION));
  }
  LogPrint (&Logger, L"PASS memory-map capture validation and bounded growth retries\r\n");
  return EFI_SUCCESS;
}

STATIC EFI_COMPATIBILITY16_TABLE *TestTable;
STATIC NATIVE_CSM_VGA_PLAN_COMPATIBILITY16 Selected;

STATIC VOID TableChecksum (UINTN Length, BOOLEAN Damage)
{
  UINTN Index;
  UINT8 Sum = 0;
  UINT8 *Bytes = (UINT8 *)TestTable;
  TestTable->TableLength = (UINT8)Length;
  TestTable->TableChecksum = 0;
  for (Index = 0; Index < Length; ++Index) { Sum = (UINT8)(Sum + Bytes[Index]); }
  TestTable->TableChecksum = (UINT8)(0 - Sum + (Damage ? 1 : 0));
}

STATIC VOID ConfigureTable (VOID)
{
  ZeroMem ((VOID *)CompatibilityTestStart, CompatibilityTestEnd - CompatibilityTestStart);
  TestTable = (EFI_COMPATIBILITY16_TABLE *)CompatibilityTestStart;
  TestTable->Signature = SIGNATURE_32 ('I', 'F', 'E', '$');
  TestTable->TableMinorRevision = 98;
  TestTable->EfiSystemTable = (UINT32)(UINTN)gST;
  TestTable->PciExpressBase = 0xe0000000;
  TestTable->LastPciBus = 8;
  TestTable->Compatibility16CallSegment = (UINT16)((CompatibilityTestStart + 0x1000) >> 4);
  TestTable->PnPInstallationCheckSegment = (UINT16)((CompatibilityTestStart + 0x1200) >> 4);
  // Recognizable bytes, never executed. We invoke only the real table scanner.
  SetMem ((VOID *)(CompatibilityTestStart + 0x1000), COMPAT_CALL_SAMPLE_BYTES, 0x90);
  CopyMem ((VOID *)(CompatibilityTestStart + 0x1200), "$PnP", 4);
  TableChecksum (sizeof (*TestTable), FALSE);
}

STATIC EFI_STATUS CompatibilityAssertions (VOID)
{
  UINTN Length;
  ConfigureTable ();
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_SUCCESS);
  REQUIRE (Selected.Address == CompatibilityTestStart && Selected.ChecksumValid);
  REQUIRE (Selected.CallAddress == CompatibilityTestStart + 0x1000);
  TableChecksum (sizeof (*TestTable), TRUE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_SUCCESS && Selected.StaleChecksumAccepted);
  TestTable->PciExpressBase = 0;
  TableChecksum (sizeof (*TestTable), FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_SUCCESS && !Selected.StaleChecksumAccepted);
  TableChecksum (sizeof (*TestTable), TRUE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  TestTable->LastPciBus = 0;
  TableChecksum (sizeof (*TestTable), FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_SUCCESS);
  TableChecksum (sizeof (*TestTable), TRUE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  TestTable->PciExpressBase = 0;
  TestTable->LastPciBus = 0;
  TestTable->EfiSystemTable = 0;
  TableChecksum (sizeof (*TestTable), FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_SUCCESS);
  TableChecksum (sizeof (*TestTable), TRUE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  TestTable->EfiSystemTable ^= 0x1000;
  TableChecksum (sizeof (*TestTable), FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  TableChecksum (COMPAT_REQUIRED_LENGTH, FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_SUCCESS);
  REQUIRE (Selected.TableLength == COMPAT_REQUIRED_LENGTH && !Selected.StaleChecksumAccepted);
  TableChecksum (COMPAT_REQUIRED_LENGTH, TRUE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  TableChecksum (COMPAT_REQUIRED_LENGTH - 1, FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  SetMem ((UINT8 *)TestTable + sizeof (*TestTable), MAX_UINT8 - sizeof (*TestTable), 0xa5);
  TableChecksum (MAX_UINT8, FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_SUCCESS && Selected.TableLength == MAX_UINT8);
  // The checksum covers the entire extension, not just the copied prefix.
  ((UINT8 *)TestTable)[MAX_UINT8 - 1] ^= 1;
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  for (Length = OFFSET_OF (EFI_COMPATIBILITY16_TABLE, PciExpressBase) + 1;
       Length < COMPAT_PCIE_END; ++Length) {
    TableChecksum (Length, FALSE);
    REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  }
  TableChecksum (COMPAT_PCIE_END, FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_SUCCESS && !Selected.StaleChecksumAccepted);
  ConfigureTable ();
  TestTable->PciExpressBase = 1;
  TableChecksum (sizeof (*TestTable), FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  TestTable->TableMajorRevision = 0xff;
  TableChecksum (sizeof (*TestTable), FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  TestTable->TableMinorRevision = 0xff;
  TableChecksum (sizeof (*TestTable), FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  SetMem ((VOID *)(CompatibilityTestStart + 0x1000), COMPAT_CALL_SAMPLE_BYTES, 0);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  SetMem ((VOID *)(CompatibilityTestStart + 0x1200), 4, 0);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  TestTable->Compatibility16CallSegment = 0;
  TableChecksum (sizeof (*TestTable), FALSE);
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  ConfigureTable ();
  CopyMem ((VOID *)(CompatibilityTestStart + 0x400), TestTable, sizeof (*TestTable));
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NO_MAPPING);
  REQUIRE (Selected.Table == NULL);
  // A header at the very end must not authorize a read beyond the scan region.
  ZeroMem ((VOID *)CompatibilityTestStart, CompatibilityTestEnd - CompatibilityTestStart);
  CopyMem ((VOID *)(CompatibilityTestEnd - 16), "IFE$", 4);
  *(UINT8 *)(CompatibilityTestEnd - 16 + 5) = MAX_UINT8;
  REQUIRE (SelectCompatibility16 (&Logger, &Selected) == EFI_NOT_FOUND);
  REQUIRE (SelectCompatibility16 (NULL, &Selected) == EFI_INVALID_PARAMETER);
  LogPrint (&Logger, L"PASS real Compatibility16 scanner, bounded copies/checksums, ambiguity and stale-checksum limits\r\n");
  return EFI_SUCCESS;
}

STATIC EFI_STATUS CompatibilityTests (VOID)
{
  EFI_PHYSICAL_ADDRESS Address = 0x9ffff;
  EFI_STATUS Status;
  // The test build redirects only the scanner's start/end constants to this
  // owned low-memory buffer. No E/F-segment writes, CSM calls or GPU actions.
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 2, &Address);
  if (EFI_ERROR (Status)) { return Status; }
  CompatibilityTestStart = (UINTN)Address;
  CompatibilityTestEnd = (UINTN)Address + 2 * EFI_PAGE_SIZE;
  Status = CompatibilityAssertions ();
  gBS->FreePages (Address, 2);
  return Status;
}

STATIC VOID ConfigureGpus (UINT64 Attributes)
{
  UINTN Index;
  ZeroMem (&Plan, sizeof (Plan));
  ZeroMem (Records, sizeof (Records));
  ZeroMem (&Root, sizeof (Root));
  Root.ParentHandle = &Root;
  for (Index = 0; Index < ARRAY_SIZE (Records); ++Index) {
    Records[Index].Config[PCI_BASE_CLASS_OFFSET] = PCI_BASE_CLASS_DISPLAY;
    Records[Index].Config[0] = 0x34;
    Records[Index].Config[1] = 0x12;
    Records[Index].Bus = Index + 1;
    Records[Index].Handle = &Pci[Index];
    Records[Index].PciIo = &Pci[Index];
    Records[Index].RootBridgeHandle = &Root;
    Records[Index].RootBridgeIo = &Root;
  }
  Records[0].CurrentAttributes = Attributes;
  Plan.Inventory.Devices = Records;
  Plan.Inventory.Count = 2;
  Plan.Config.TargetPci.Bus = 2;
}

STATIC EFI_STATUS VideoTests (VOID)
{
  UINTN Index;
  CONST UINT64 Owners[] = { EFI_PCI_IO_ATTRIBUTE_VGA_IO, EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY,
                           EFI_PCI_IO_ATTRIBUTE_VGA_IO_16 };
  for (Index = 0; Index < ARRAY_SIZE (Owners); ++Index) {
    ConfigureGpus (Owners[Index]);
    REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_SUCCESS);
    REQUIRE (Plan.Active.Bus == 1 && Plan.Target.Bus == 2);
  }
  ConfigureGpus (EFI_PCI_IO_ATTRIBUTE_IO);
  REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_NO_MAPPING);
  ConfigureGpus (EFI_PCI_IO_ATTRIBUTE_VGA_PALETTE_IO);
  REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_NO_MAPPING);
  ConfigureGpus (EFI_PCI_IO_ATTRIBUTE_VGA_IO);
  Plan.Inventory.Count = 3;
  Records[2].CurrentAttributes = EFI_PCI_IO_ATTRIBUTE_VGA_IO_16;
  REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_NO_MAPPING);
  ConfigureGpus (EFI_PCI_IO_ATTRIBUTE_VGA_IO_16);
  Plan.Config.TargetPci.Bus = 1;
  REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_ALREADY_STARTED);
  ConfigureGpus (EFI_PCI_IO_ATTRIBUTE_VGA_IO_16);
  Records[0].CurrentAttributesStatus = EFI_DEVICE_ERROR;
  REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_NO_MAPPING);
  ConfigureGpus (EFI_PCI_IO_ATTRIBUTE_VGA_IO_16);
  Records[1].RootBridgeHandle = &Records[1];
  REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_NO_MAPPING);
  ConfigureGpus (EFI_PCI_IO_ATTRIBUTE_VGA_IO_16);
  Records[1].Segment = Plan.Config.TargetPci.Segment = 1;
  REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_UNSUPPORTED);
  ConfigureGpus (EFI_PCI_IO_ATTRIBUTE_VGA_IO_16);
  Plan.Config.HasExpectedVendor = TRUE;
  Plan.Config.ExpectedVendor = 0x5678;
  REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_SECURITY_VIOLATION);
  ConfigureGpus (EFI_PCI_IO_ATTRIBUTE_VGA_IO_16);
  Records[1].ConfigStatus = EFI_DEVICE_ERROR;
  REQUIRE (FindVideoEndpoints (&Logger, &Plan) == EFI_NOT_FOUND);
  LogPrint (&Logger, L"PASS VGA decode variants, identity and topology guard regressions\r\n");
  return EFI_SUCCESS;
}


#include "AuditBoundaryTests.inc"

EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  APP_FILE_CONTEXT Files;
  EFI_FILE_PROTOCOL *Result;
  EFI_STATUS Status;
  UINTN Size;
  CHAR8 Text[128];
  (VOID)SystemTable;
  Status = AppFileInitialize (ImageHandle, &Files);
  if (EFI_ERROR (Status)) { return Status; }
  Status = LogInitialize (&Logger, &Files, TRUE, L"PortabilityTest.log");
  if (EFI_ERROR (Status)) { AppFileClose (&Files); return Status; }
  Status = AuditBoundaryTests ();
  if (!EFI_ERROR (Status)) { Status = MemoryTests (); }
  if (!EFI_ERROR (Status)) { Status = CaptureTests (); }
  if (!EFI_ERROR (Status)) { Status = CompatibilityTests (); }
  if (!EFI_ERROR (Status)) { Status = VideoTests (); }
  AsciiSPrint (Text, sizeof (Text), "%a checks=%u failed-line=%u status=%r\n",
    EFI_ERROR (Status) ? "FAIL" : "PASS", (UINT32)Checks, (UINT32)FailedLine, Status);
  LogPrint (&Logger, L"%a", Text);
  LogClose (&Logger);
  if (!EFI_ERROR (AppFileOpenAdjacent (&Files, L"PortabilityTest.result",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0, &Result))) {
    Size = AsciiStrLen (Text);
    Result->Write (Result, &Size, Text);
    Result->Flush (Result);
    Result->Close (Result);
  }
  AppFileClose (&Files);
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  return Status;
}

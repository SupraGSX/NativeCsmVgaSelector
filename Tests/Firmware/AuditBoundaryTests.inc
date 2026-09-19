/* Actual production protocol boundaries with synthetic services.
   SPDX-License-Identifier: GPL-3.0-only */
#include "LegacyDispatch.h"
#include "BootReport.h"
#include "ConfigFile.h"
#include <Guid/FileInfo.h>
STATIC EFI_STATUS AllocationResult, FreeResult, WatchdogResult;
STATIC EFI_PHYSICAL_ADDRESS AllocatedAddress;
STATIC UINTN AllocationCalls, FreeCalls, WatchdogCalls;
STATIC BOOLEAN CorrectAllocation, CorrectWatchdog;
STATIC CHAR16 ReportText[4096];
STATIC EFI_STATUS EFIAPI AuditAllocate (EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE Memory,
  UINTN Pages, EFI_PHYSICAL_ADDRESS *Address)
{
  ++AllocationCalls;
  CorrectAllocation = Type == AllocateMaxAddress && Memory == EfiBootServicesData &&
    Pages == 1 && *Address == 0x9f000;
  *Address = AllocatedAddress;
  return AllocationResult;
}
STATIC EFI_STATUS EFIAPI AuditFree (EFI_PHYSICAL_ADDRESS Address, UINTN Pages)
{
  ++FreeCalls;
  if (Address != AllocatedAddress || Pages != 1) { return EFI_INVALID_PARAMETER; }
  return FreeResult;
}
STATIC EFI_STATUS EFIAPI AuditWatchdog (UINTN Timeout, UINT64 Code, UINTN Size, CHAR16 *Data)
{
  ++WatchdogCalls;
  CorrectWatchdog = Timeout == 0 && Code == 0 && Size == 0 && Data == NULL;
  return WatchdogResult;
}
STATIC EFI_STATUS EFIAPI AuditOutput (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Text)
{
  (VOID)This;
  StrCatS (ReportText, ARRAY_SIZE (ReportText), Text);
  return EFI_SUCCESS;
}
STATIC EFI_FILE_PROTOCOL AuditFile;
STATIC EFI_STATUS EFIAPI AuditOpen (EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **File,
  CHAR16 *Name, UINT64 Mode, UINT64 Attributes)
{
  (VOID)This; (VOID)Name; (VOID)Mode; (VOID)Attributes;
  *File = &AuditFile; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI AuditInfo (EFI_FILE_PROTOCOL *This, EFI_GUID *Guid, UINTN *Size, VOID *Buffer)
{
  (VOID)This; (VOID)Guid;
  if (*Size < sizeof (EFI_FILE_INFO)) { *Size = sizeof (EFI_FILE_INFO); return EFI_BUFFER_TOO_SMALL; }
  ZeroMem (Buffer, *Size);
  ((EFI_FILE_INFO *)Buffer)->FileSize = 4;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI AuditRead (EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buffer)
{
  (VOID)This;
  if (*Size != 4) { return EFI_BAD_BUFFER_SIZE; }
  CopyMem (Buffer, "test", 4); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI AuditClose (EFI_FILE_PROTOCOL *This)
{ (VOID)This; return EFI_DEVICE_ERROR; }

STATIC EFI_STATUS AuditBoundaryTests (VOID)
{
  EFI_BOOT_SERVICES Services, *OriginalBoot = gBS;
  EFI_SYSTEM_TABLE System, *OriginalSystem = gST;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Output;
  EFI_DISPATCH_OPROM_TABLE Table;
  APP_FILE_CONTEXT Files;
  EFI_FILE_PROTOCOL Directory;
  EFI_STATUS Status;
  UINTN Device, Function, Size;
  UINT8 *Bytes;

  ZeroMem (&Plan, sizeof (Plan));
  Plan.Target.Bus = 7;
  Plan.Compatibility16.PnpSegment = 0xf000;
  Plan.Compatibility16.PnpOffset = 0x123;
  Plan.BootTarget.BbsCount = 3;
  Plan.BootTarget.FirmwareBbsTable = (BBS_TABLE *)(UINTN)0x80000;
  for (Device = 0; Device < 32; ++Device) {
    for (Function = 0; Function < 8; ++Function) {
      Plan.Target.Device = (UINT8)Device; Plan.Target.Function = (UINT8)Function;
      SetMem (&Table, sizeof (Table), 0xa5);
      LegacyDispatchPrepare (&Plan, &Table);
      REQUIRE (Table.PciBus == 7 && Table.PciDeviceFunction == (Device << 3 | Function));
      REQUIRE (Table.PnPInstallationCheckSegment == 0xf000 && Table.PnPInstallationCheckOffset == 0x123 &&
        Table.OpromSegment == 0xc000 && Table.NumberBbsEntries == 3 && Table.BbsTablePointer == 0x80000);
    }
  }
  CopyMem (&Services, gBS, sizeof (Services));
  Services.AllocatePages = AuditAllocate; Services.FreePages = AuditFree;
  Services.SetWatchdogTimer = AuditWatchdog;
  gBS = &Services;
  AllocatedAddress = 0x80000; AllocationResult = EFI_OUT_OF_RESOURCES;
  Status = LegacyDispatchReserve ();
  gBS = OriginalBoot;
  REQUIRE (Status == EFI_OUT_OF_RESOURCES && FreeCalls == 0 && CorrectAllocation);
  gBS = &Services; AllocationResult = EFI_SUCCESS;
  Status = LegacyDispatchReserve ();
  gBS = OriginalBoot;
  REQUIRE (Status == EFI_SUCCESS && LegacyDispatchAddress () == AllocatedAddress);
  gBS = &Services;
  Status = LegacyDispatchReserve ();
  gBS = OriginalBoot;
  REQUIRE (Status == EFI_ALREADY_STARTED && AllocationCalls == 2);
  gBS = &Services; FreeResult = EFI_DEVICE_ERROR;
  Status = LegacyDispatchRelease ();
  gBS = OriginalBoot;
  REQUIRE (Status == EFI_DEVICE_ERROR && LegacyDispatchAddress () == AllocatedAddress);
  gBS = &Services; FreeResult = EFI_SUCCESS;
  Status = LegacyDispatchRelease ();
  gBS = OriginalBoot;
  REQUIRE (Status == EFI_SUCCESS && LegacyDispatchAddress () == 0);
  gBS = &Services; AllocatedAddress = 0;
  Status = LegacyDispatchReserve ();
  gBS = OriginalBoot;
  REQUIRE (Status == EFI_UNSUPPORTED && FreeCalls == 3);
  gBS = &Services; AllocatedAddress = 0xa0000;
  Status = LegacyDispatchReserve ();
  gBS = OriginalBoot;
  REQUIRE (Status == EFI_UNSUPPORTED && FreeCalls == 4);
  for (Device = 0; Device < 3; ++Device) {
    WatchdogResult = Device == 0 ? EFI_SUCCESS : (Device == 1 ? EFI_UNSUPPORTED : EFI_DEVICE_ERROR);
    gBS = &Services;
    Status = BootReportDisableWatchdog ();
    gBS = OriginalBoot;
    REQUIRE (Status == (Device == 2 ? EFI_DEVICE_ERROR : EFI_SUCCESS) && CorrectWatchdog);
  }
  REQUIRE (WatchdogCalls == 3);

  CopyMem (&System, gST, sizeof (System));
  ZeroMem (&Output, sizeof (Output)); Output.OutputString = AuditOutput;
  System.ConOut = System.StdErr = &Output;
  for (Device = 0; Device < 3; ++Device) {
    BootReportReset ();
    ReportText[0] = 0;
    if (Device != 2) {
      BootReportObserve (L"NCV_DISK_FIRMWARE_WARNING_BBS\r\n");
      BootReportObserve (L"NCV_DISK_FIRMWARE_WARNING_ORDER\r\n");
    }
    /* A durable BEGIN record alone is not an attempt: final log close can
       still fail. Only the typed boundary after that close marks an attempt. */
    BootReportObserve (L"NCV_BOOT_NATIVE_LEGACY_BOOT_BEGIN\r\n");
    if (Device == 1) { BootReportDiskBootAttempt (); }
    BootReportObserve (L"NCV_BOOT_FAIL_TEST_RETURN\r\n");
    gST = &System;
    BootReportShow (EFI_DEVICE_ERROR);
    gST = OriginalSystem;
    if (Device == 0) {
      REQUIRE (StrStr (ReportText, L"native disk boot was not reached") != NULL);
      REQUIRE (StrStr (ReportText, L"LegacyDevOrder marked this disk disabled") != NULL);
    } else if (Device == 1) {
      REQUIRE (StrStr (ReportText, L"Native boot was attempted at the user's request") != NULL);
      REQUIRE (StrStr (ReportText, L"Firmware BBS status/priority") != NULL);
    } else {
      REQUIRE (StrStr (ReportText, L"Firmware boot warning") == NULL);
    }
  }
  BootReportReset ();
  ZeroMem (&Files, sizeof (Files)); ZeroMem (&Directory, sizeof (Directory));
  ZeroMem (&AuditFile, sizeof (AuditFile));
  Files.ImageDirectory = &Directory; Directory.Open = AuditOpen;
  AuditFile.GetInfo = AuditInfo; AuditFile.Read = AuditRead; AuditFile.Close = AuditClose;
  Status = ReadConfigBytesNamed (&Files, L"Config.ini", &Bytes, &Size);
  REQUIRE (Status == EFI_DEVICE_ERROR && Bytes == NULL && Size == 0);
  LogPrint (&Logger, L"PASS actual dispatch arguments/allocation, watchdog, retained disk warnings and config close failures\r\n");
  return EFI_SUCCESS;
}

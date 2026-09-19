/* Synthetic legacy-disk firmware harness; never install on physical hardware.
   SPDX-License-Identifier: GPL-3.0-only */

/* Exercise the actual static controller validator without production hooks or
   changing the production runtime-plan ABI. Other modules link normally. */
#define NativeCsmVgaRuntimePlanBuild HarnessRuntimePlanBuild
#define NativeCsmVgaRuntimePlanFinalizeValidation HarnessRuntimePlanFinalizeValidation
#define NativeCsmVgaRuntimePlanRelease HarnessRuntimePlanRelease
#include "RuntimePlan.c"

#include <Guid/GlobalVariable.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>

STATIC APP_LOGGER Logger;
STATIC NATIVE_CSM_VGA_RUNTIME_PLAN Plan;
STATIC PCI_DEVICE_RECORD Records[2];
STATIC EFI_PCI_IO_PROTOCOL DummyPci;
STATIC EFI_LEGACY_BIOS_PROTOCOL Legacy;
STATIC EFI_RUNTIME_SERVICES TestRuntime;
STATIC BBS_TABLE *Entries;
STATIC BBS_TABLE OriginalEntries[3];
STATIC UINTN BbsCalls;
STATIC UINTN Checks;
STATIC UINTN FailedLine;
STATIC BOOLEAN ReturnDisabledOrder;
STATIC BOOLEAN UsbOptionPresent = TRUE;
STATIC BOOLEAN DuplicateUsbOption;
STATIC BOOLEAN UsbOptionActive = TRUE;
STATIC UINT16 UsbOptionType = BBS_TYPE_HARDDRIVE;
STATIC UINT16 DuplicateUsbOptionType = BBS_TYPE_HARDDRIVE;
STATIC CONST CHAR16 *UsbOptionName = L"USB";
STATIC CONST UINT8 PathBytes[] = { 5, 1, 9, 0, 2, 0, 0, 0, 0, 0x7f, 0xff, 4, 0 };
STATIC CONST UINT8 UsbData[] = { 0x55, 0x53, 0x42, 0x00, 0x91, 0x7e };
STATIC CONST UINT8 InternalData[] = { 0x48, 0x44, 0x00, 0x82 };

#define REQUIRE(Expression) do { \
  ++Checks; \
  if (!(Expression)) { FailedLine = __LINE__; \
    LogPrint (&Logger, L"FAIL line %u: %a\r\n", (UINT32)__LINE__, #Expression); \
    return EFI_ABORTED; } \
} while (0)

STATIC EFI_STATUS EFIAPI TestGetVariable (
  CHAR16 *Name, EFI_GUID *Guid, UINT32 *Attributes, UINTN *Size, VOID *Data
  )
{
  UINT8 Buffer[256];
  UINTN Bytes;
  UINTN DescriptionBytes;
  CONST CHAR16 *Description;
  CONST UINT8 *Optional;
  UINTN OptionalBytes;
  UINT32 Flags = LOAD_OPTION_ACTIVE;
  UINT16 PathSize = sizeof (PathBytes);
  CONST UINT16 Order[] = { 1, 2, 3 };

  if (ReturnDisabledOrder && StrCmp (Name, L"LegacyDevOrder") == 0) {
    CONST UINT8 OrderBytes[] = { 2, 8, 0, 0, 0, 1, 0, 2, 0xff };
    if (Attributes != NULL) { *Attributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS; }
    if (*Size < sizeof (OrderBytes)) { *Size = sizeof (OrderBytes); return EFI_BUFFER_TOO_SMALL; }
    *Size = sizeof (OrderBytes); CopyMem (Data, OrderBytes, sizeof (OrderBytes)); return EFI_SUCCESS;
  }
  if (!CompareGuid (Guid, &gEfiGlobalVariableGuid)) { return EFI_NOT_FOUND; }
  if (StrCmp (Name, L"BootOrder") == 0) {
    Bytes = DuplicateUsbOption ? sizeof (Order) : 2 * sizeof (UINT16);
    CopyMem (Buffer, Order, Bytes);
  } else {
    if (StrCmp (Name, L"Boot0001") == 0) {
      Description = L"Hard Drive";
      Optional = InternalData;
      OptionalBytes = sizeof (InternalData);
    } else if (UsbOptionPresent &&
               ((StrCmp (Name, L"Boot0002") == 0) ||
                (DuplicateUsbOption && (StrCmp (Name, L"Boot0003") == 0)))) {
      Description = UsbOptionName;
      Optional = UsbData;
      OptionalBytes = sizeof (UsbData);
    } else { return EFI_NOT_FOUND; }
    if ((StrCmp (Name, L"Boot0001") != 0) && !UsbOptionActive) { Flags = 0; }
    DescriptionBytes = StrSize (Description);
    CopyMem (Buffer, &Flags, sizeof (Flags));
    CopyMem (Buffer + 4, &PathSize, sizeof (PathSize));
    CopyMem (Buffer + 6, Description, DescriptionBytes);
    CopyMem (Buffer + 6 + DescriptionBytes, PathBytes, sizeof (PathBytes));
    if (StrCmp (Name, L"Boot0002") == 0) {
      CopyMem (Buffer + 6 + DescriptionBytes + 4, &UsbOptionType, sizeof (UsbOptionType));
    } else if (StrCmp (Name, L"Boot0003") == 0) {
      CopyMem (Buffer + 6 + DescriptionBytes + 4, &DuplicateUsbOptionType, sizeof (DuplicateUsbOptionType));
    }
    CopyMem (Buffer + 6 + DescriptionBytes + sizeof (PathBytes), Optional, OptionalBytes);
    Bytes = 6 + DescriptionBytes + sizeof (PathBytes) + OptionalBytes;
  }
  if (Attributes != NULL) {
    *Attributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS;
  }
  if (*Size < Bytes) { *Size = Bytes; return EFI_BUFFER_TOO_SMALL; }
  *Size = Bytes;
  CopyMem (Data, Buffer, Bytes);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI TestGetBbsInfo (
  EFI_LEGACY_BIOS_PROTOCOL *This, UINT16 *HddCount, HDD_INFO **HddInfo,
  UINT16 *BbsCount, BBS_TABLE **BbsTable
  )
{
  (VOID)This;
  ++BbsCalls;
  *HddCount = 0;
  *HddInfo = NULL;
  *BbsCount = ARRAY_SIZE (OriginalEntries);
  *BbsTable = Entries;
  return EFI_SUCCESS;
}

STATIC VOID ConfigureController (UINT8 BaseClass, UINT8 SubClass)
{
  ZeroMem (&Plan, sizeof (Plan));
  ZeroMem (Records, sizeof (Records));
  Records[0].PciIo = &DummyPci;
  Records[0].Bus = 5;
  Records[0].Config[PCI_BASE_CLASS_OFFSET] = BaseClass;
  Records[0].Config[PCI_SUBCLASS_OFFSET] = SubClass;
  Plan.Inventory.Devices = Records;
  Plan.Inventory.Count = 1;
  Plan.Config.TargetControllerPci.Bus = 5;
  Plan.Config.HasTargetBbsDescription = TRUE;
  StrCpyS (Plan.Config.TargetBbsDescription, PROBE_CONFIG_TEXT_CHARS, L"USB disk B ");
  // These are the parser's existing defaults. USB resolution must replace them.
  StrCpyS (Plan.Config.LegacyOptionDescription, PROBE_CONFIG_TEXT_CHARS, L"Hard Drive");
  StrCpyS (Plan.Config.ExcludeDescription, PROBE_CONFIG_TEXT_CHARS, L"USB");
}

STATIC EFI_STATUS ControllerTests (VOID)
{
  ConfigureController (0x0c, 0x03);
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  REQUIRE (StrCmp (Plan.Config.LegacyOptionDescription, L"USB") == 0);
  REQUIRE (StrCmp (Plan.Config.ExcludeDescription, L"Hard Drive") == 0);
  REQUIRE (Plan.StorageController.InventoryRecord == &Records[0]);
  // Target editing must also resolve defaults in the opposite direction.
  Records[0].Config[PCI_BASE_CLASS_OFFSET] = 1;
  Records[0].Config[PCI_SUBCLASS_OFFSET] = 8;
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  REQUIRE (StrCmp (Plan.Config.LegacyOptionDescription, L"Hard Drive") == 0);
  REQUIRE (StrCmp (Plan.Config.ExcludeDescription, L"USB") == 0);
  ConfigureController (1, 6);
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  ConfigureController (0x0c, 0x05); // SMBus is not a USB disk controller.
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_NO_MAPPING);
  ConfigureController (0x0c, 3);
  Records[0].ConfigStatus = EFI_DEVICE_ERROR;
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_NO_MAPPING);
  ConfigureController (0x0c, 3);
  Records[0].LocationStatus = EFI_DEVICE_ERROR;
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_NO_MAPPING);
  ConfigureController (0x0c, 3);
  Plan.Config.HasTargetBbsDescription = FALSE;
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_INVALID_PARAMETER);
  Plan.Config.HasTargetBbsDescription = TRUE;
  Plan.Config.TargetBbsDescription[0] = 0;
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_INVALID_PARAMETER);
  ConfigureController (0x0c, 3);
  CopyMem (&Records[1], &Records[0], sizeof (Records[0]));
  Plan.Inventory.Count = 2;
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_NO_MAPPING);
  Records[1].Segment = 1; // BBS has no segment field: still ambiguous.
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_NO_MAPPING);
  ConfigureController (0x0c, 3);
  Plan.Config.HasLegacyOptionDescription = TRUE;
  Plan.Config.HasExcludeDescription = TRUE;
  StrCpyS (Plan.Config.LegacyOptionDescription, PROBE_CONFIG_TEXT_CHARS, L"External Disks");
  StrCpyS (Plan.Config.ExcludeDescription, PROBE_CONFIG_TEXT_CHARS, L"Network");
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  REQUIRE (StrCmp (Plan.Config.LegacyOptionDescription, L"External Disks") == 0);
  REQUIRE (StrCmp (Plan.Config.ExcludeDescription, L"Network") == 0);
  LogPrint (&Logger, L"PASS controller selection and policy tests\r\n");
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Discover (VOID)
{
  BbsCalls = 0;
  return LegacyBootBootTargetDiscover (&Logger, &Legacy,
    Plan.StorageControllerAddress.Bus, Plan.StorageControllerAddress.Device,
    Plan.StorageControllerAddress.Function, Plan.Config.TargetBbsDescription,
    Plan.Config.LegacyOptionDescription, Plan.Config.ExcludeDescription, &Plan.BootTarget);
}

STATIC EFI_STATUS BootTargetTests (VOID)
{
  BBS_BBS_DEVICE_PATH *BootOption;
  UINT32 OptionsSize;
  VOID *Options;
  UINT16 Flags;
  EFI_STATUS Status;

  ConfigureController (0x0c, 3);
  Entries[0].BootPriority = 1;
  Flags = 0x0400; // Unselected disabled entry; must remain preserved.
  CopyMem (&Entries[0].StatusFlags, &Flags, sizeof (Flags));
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  REQUIRE (Discover () == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetJournalAndValidate (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  LogPrint (&Logger, L"REGRESSION planned priorities: %u %u %u; first row disabled\r\n",
    Plan.BootTarget.PlannedPriorities[0], Plan.BootTarget.PlannedPriorities[1], Plan.BootTarget.PlannedPriorities[2]);
  Status = LegacyBootBootPriorityApply (&Logger, &Plan.BootTarget);
  LogPrint (&Logger, L"REGRESSION priority collision: application status=%r (eligible row must not be masked); live=%u %u %u\r\n",
    Status, Entries[0].BootPriority, Entries[1].BootPriority, Entries[2].BootPriority);
  REQUIRE (Status == EFI_SUCCESS);
  REQUIRE (Entries[0].BootPriority == 1);
  REQUIRE (Entries[1].BootPriority == 1 && Entries[2].BootPriority == 0);
  REQUIRE (LegacyBootBootPriorityRollback (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (Entries[0].BootPriority == 1 && Entries[1].BootPriority == OriginalEntries[1].BootPriority);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));

  ConfigureController (0x0c, 3);
  // Explicit selection can attempt a structurally valid disk even when the
  // firmware's priority sentinel discouraged it. Original bytes roll back.
  {
    CONST UINT16 Priorities[] = { BBS_IGNORE_ENTRY, BBS_DO_NOT_BOOT_FROM, BBS_LOWEST_PRIORITY };
    UINTN Trial;
    for (Trial = 0; Trial < ARRAY_SIZE (Priorities); ++Trial) {
      ConfigureController (0x0c, 3);
      Entries[2].BootPriority = Priorities[Trial];
      REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
      REQUIRE (Discover () == EFI_SUCCESS);
      REQUIRE (LegacyBootBootTargetPrepare (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
      REQUIRE (LegacyBootBootTargetJournalAndValidate (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
      REQUIRE (LegacyBootBootPriorityApply (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
      REQUIRE (Entries[2].BootPriority == 0);
      REQUIRE (LegacyBootBootPriorityRollback (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
      REQUIRE (Entries[2].BootPriority == Priorities[Trial]);
      REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
      CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));
    }
  }
  ReturnDisabledOrder = TRUE;
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  REQUIRE (Discover () == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetPrepare (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (Plan.BootTarget.LegacyDevOrderUsable && Plan.BootTarget.LegacyDevOrderDisabled[2]);
  REQUIRE (CompareMem (Entries, OriginalEntries, sizeof (OriginalEntries)) == 0);
  Status = LegacyBootBootTargetJournalAndValidate (&Logger, &Plan.BootTarget);
  LogPrint (&Logger, L"REGRESSION explicitly disabled target: journal=%r usable-order=%u\r\n", Status, Plan.BootTarget.LegacyDevOrderUsable);
  REQUIRE (Status == EFI_SUCCESS && Plan.BootTarget.LegacyDevOrderUsable);
  REQUIRE (Plan.BootTarget.LegacyDevOrderDisabled[2]);
  Status = LegacyBootBootPriorityApply (&Logger, &Plan.BootTarget);
  LogPrint (&Logger, L"REGRESSION explicitly disabled target: apply=%r target-priority=%u\r\n", Status, Entries[2].BootPriority);
  REQUIRE (Status == EFI_SUCCESS && Entries[2].BootPriority == 0);
  REQUIRE (LegacyBootBootPriorityRollback (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  ReturnDisabledOrder = FALSE;
  CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));

  ConfigureController (0x0c, 3);
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  REQUIRE (Discover () == EFI_SUCCESS);
  REQUIRE (BbsCalls == 1);
  REQUIRE (Plan.BootTarget.BootOptionNumber == 2);
  REQUIRE (Plan.BootTarget.LiveBbsIndex == 2); // second disk, same USB controller
  REQUIRE (AsciiStrCmp (Plan.BootTarget.LiveDescription, "USB disk B ") == 0);
  REQUIRE (LegacyBootBootTargetJournalAndValidate (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (LegacyBootBootPriorityApply (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (Entries[2].BootPriority == 0 && Entries[0].BootPriority != 0 && Entries[1].BootPriority != 0);
  REQUIRE (LegacyBootBootTargetValidateApplied (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetGetLegacyBootArguments (&Plan.BootTarget,
             &BootOption, &OptionsSize, &Options) == EFI_SUCCESS);
  REQUIRE (CompareMem (BootOption, PathBytes, 4) == 0);
  REQUIRE (((UINT8 *)BootOption)[4] == UsbOptionType && ((UINT8 *)BootOption)[5] == 0);
  REQUIRE (CompareMem ((UINT8 *)BootOption + 6, PathBytes + 6, sizeof (PathBytes) - 6) == 0);
  REQUIRE (Plan.BootTarget.BbsDeviceType == UsbOptionType && Plan.BootTarget.LiveBbsEntry.DeviceType == BBS_HARDDISK);
  REQUIRE (OptionsSize == sizeof (UsbData) && CompareMem (Options, UsbData, sizeof (UsbData)) == 0);
  REQUIRE (BbsCalls == 1); // priority work does not rediscover firmware state
  REQUIRE (LegacyBootBootPriorityRollback (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (CompareMem (Entries, OriginalEntries, sizeof (OriginalEntries)) == 0);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);

  UsbOptionPresent = FALSE;
  REQUIRE (Discover () == EFI_NOT_FOUND); // must not silently use Hard Drive
  REQUIRE (BbsCalls == 0);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  UsbOptionPresent = TRUE;
  DuplicateUsbOption = TRUE;
  REQUIRE (EFI_ERROR (Discover ()));
  REQUIRE (BbsCalls == 0);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  // Two boot groups with the same requested name remain ambiguous even when
  // one uses BBS(HD) and the other BBS(USB).
  DuplicateUsbOptionType = UsbOptionType == BBS_TYPE_USB ? BBS_TYPE_HARDDRIVE : BBS_TYPE_USB;
  REQUIRE (Discover () == EFI_ABORTED);
  REQUIRE (BbsCalls == 0);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  DuplicateUsbOption = FALSE;
  DuplicateUsbOptionType = UsbOptionType;
  UsbOptionActive = FALSE;
  REQUIRE (Discover () == EFI_NOT_FOUND);
  REQUIRE (BbsCalls == 0);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  UsbOptionActive = TRUE;

  Entries[1].DescStringSegment = Entries[2].DescStringSegment;
  Entries[1].DescStringOffset = Entries[2].DescStringOffset;
  REQUIRE (Discover () == EFI_ABORTED); // two identical names on one controller
  REQUIRE (BbsCalls == 1);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  Flags = 0x0700; // A failed duplicate must not hide ambiguous disk identity.
  CopyMem (&Entries[1].StatusFlags, &Flags, sizeof (Flags));
  REQUIRE (Discover () == EFI_ABORTED);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));
  StrCpyS (Plan.Config.TargetBbsDescription, PROBE_CONFIG_TEXT_CHARS, L"USB disk B");
  REQUIRE (Discover () == EFI_NOT_FOUND); // trailing space is part of identity
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  StrCpyS (Plan.Config.TargetBbsDescription, PROBE_CONFIG_TEXT_CHARS, L"USB disk B ");
  Entries[2].Bus = 6;
  REQUIRE (Discover () == EFI_NOT_FOUND); // moved or unplugged selected disk
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));
  Flags = 0x0700; // device has failed
  CopyMem (&Entries[2].StatusFlags, &Flags, sizeof (Flags));
  REQUIRE (Discover () == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetPrepare (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (!Plan.BootTarget.JournalAttempted && !Plan.BootTarget.PrioritiesApplied);
  REQUIRE (LegacyBootBootTargetJournalAndValidate (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (LegacyBootBootPriorityApply (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (Entries[2].BootPriority == 0);
  REQUIRE (LegacyBootBootPriorityRollback (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));

  Flags = 0x0400; // present but disabled
  CopyMem (&Entries[2].StatusFlags, &Flags, sizeof (Flags));
  REQUIRE (Discover () == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetPrepare (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (!Plan.BootTarget.JournalAttempted && !Plan.BootTarget.PrioritiesApplied);
  REQUIRE (LegacyBootBootTargetJournalAndValidate (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (LegacyBootBootPriorityApply (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (Entries[2].BootPriority == 0);
  REQUIRE (LegacyBootBootPriorityRollback (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  Flags = 0x0100; // enabled but no media
  CopyMem (&Entries[2].StatusFlags, &Flags, sizeof (Flags));
  REQUIRE (Discover () == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetPrepare (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (!Plan.BootTarget.JournalAttempted && !Plan.BootTarget.PrioritiesApplied);
  REQUIRE (LegacyBootBootTargetJournalAndValidate (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (LegacyBootBootPriorityApply (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (Entries[2].BootPriority == 0);
  REQUIRE (LegacyBootBootPriorityRollback (&Logger, &Plan.BootTarget) == EFI_SUCCESS);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));
  Entries[2].DeviceType = BBS_CDROM;
  REQUIRE (Discover () == EFI_NOT_FOUND);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));

  // BBS(USB) is not permission to dispatch an internal or non-USB live disk.
  if (UsbOptionType == BBS_TYPE_USB) {
    Entries[2].Class = 1;
    Entries[2].SubClass = 8;
    REQUIRE (Discover () == EFI_COMPROMISED_DATA);
    REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
    CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));
    Entries[2].SubClass = 5; // Serial bus but not USB.
    REQUIRE (Discover () == EFI_COMPROMISED_DATA);
    REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
    CopyMem (Entries, OriginalEntries, sizeof (OriginalEntries));
  }

  // Firmware with a custom generic USB option can use the documented override.
  UsbOptionName = L"External Disks";
  Plan.Config.HasLegacyOptionDescription = TRUE;
  StrCpyS (Plan.Config.LegacyOptionDescription, PROBE_CONFIG_TEXT_CHARS, UsbOptionName);
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  REQUIRE (Discover () == EFI_SUCCESS);
  REQUIRE (Plan.BootTarget.BootOptionNumber == 2 && Plan.BootTarget.LiveBbsIndex == 2);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  UsbOptionName = L"USB";

  // Some firmware groups USB-HDD under Hard Drive; only an explicit override
  // may use that option. The live disk must still be the exact USB target.
  StrCpyS (Plan.Config.LegacyOptionDescription, PROBE_CONFIG_TEXT_CHARS, L"Hard Drive");
  Plan.Config.HasExcludeDescription = TRUE;
  StrCpyS (Plan.Config.ExcludeDescription, PROBE_CONFIG_TEXT_CHARS, L"USB");
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  REQUIRE (Discover () == EFI_SUCCESS);
  REQUIRE (Plan.BootTarget.BootOptionNumber == 1 && Plan.BootTarget.LiveBbsIndex == 2);
  REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);

  // Preserve the existing internal-disk path, including opaque handoff data.
  ConfigureController (1, 8);
  Records[0].Bus = 4;
  Plan.Config.TargetControllerPci.Bus = 4;
  StrCpyS (Plan.Config.TargetBbsDescription, PROBE_CONFIG_TEXT_CHARS, L"Internal disk");
  REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
  REQUIRE (Discover () == EFI_SUCCESS);
  REQUIRE (Plan.BootTarget.BootOptionNumber == 1 && Plan.BootTarget.LiveBbsIndex == 0);
  REQUIRE (Plan.BootTarget.LoadOptionsSize == sizeof (InternalData));
  REQUIRE (CompareMem (Plan.BootTarget.LoadOptions, InternalData, sizeof (InternalData)) == 0);
  Status = LegacyBootBootTargetRelease (&Plan.BootTarget);
  REQUIRE (Status == EFI_SUCCESS);
  REQUIRE (CompareMem (Entries, OriginalEntries, sizeof (OriginalEntries)) == 0);
  if (UsbOptionType == BBS_TYPE_USB) {
    // Even an explicit option-name override must not pair the USB group with
    // a verified internal controller/disk.
    Plan.Config.HasLegacyOptionDescription = TRUE;
    Plan.Config.HasExcludeDescription = TRUE;
    StrCpyS (Plan.Config.LegacyOptionDescription, PROBE_CONFIG_TEXT_CHARS, L"USB");
    StrCpyS (Plan.Config.ExcludeDescription, PROBE_CONFIG_TEXT_CHARS, L"Hard Drive");
    REQUIRE (FindStorageController (&Logger, &Plan) == EFI_SUCCESS);
    REQUIRE (Discover () == EFI_COMPROMISED_DATA);
    REQUIRE (LegacyBootBootTargetRelease (&Plan.BootTarget) == EFI_SUCCESS);
  }
  LogPrint (&Logger, L"PASS USB BBS discovery, handoff arguments, priority transaction and internal regression\r\n");
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  APP_FILE_CONTEXT Files;
  EFI_FILE_PROTOCOL *Result;
  EFI_RUNTIME_SERVICES *OriginalRuntime;
  EFI_PHYSICAL_ADDRESS Strings = 0x9ffff;
  EFI_STATUS Status;
  UINTN Index, Size;
  UINT16 Flags = 0x0500;
  CONST CHAR8 *Names[] = { "Internal disk", "USB disk A", "USB disk B " };
  CHAR8 Text[120];

  (VOID)SystemTable;
  Status = AppFileInitialize (ImageHandle, &Files);
  if (EFI_ERROR (Status)) { return Status; }
  Status = LogInitialize (&Logger, &Files, TRUE, L"BootTargetTest.log");
  if (EFI_ERROR (Status)) { AppFileClose (&Files); return Status; }
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1, &Strings);
  if (!EFI_ERROR (Status)) {
    ZeroMem ((VOID *)(UINTN)Strings, EFI_PAGE_SIZE);
    Entries = (BBS_TABLE *)(UINTN)(Strings + 1024);
    for (Index = 0; Index < ARRAY_SIZE (OriginalEntries); ++Index) {
      UINTN Address = (UINTN)Strings + 256 * Index;
      CopyMem ((VOID *)Address, Names[Index], AsciiStrSize (Names[Index]));
      Entries[Index].BootPriority = BBS_UNPRIORITIZED_ENTRY;
      Entries[Index].Bus = Index == 0 ? 4 : 5;
      Entries[Index].Class = Index == 0 ? 1 : 0x0c;
      Entries[Index].SubClass = Index == 0 ? 8 : 3;
      Entries[Index].DeviceType = BBS_HARDDISK;
      CopyMem (&Entries[Index].StatusFlags, &Flags, sizeof (Flags));
      Entries[Index].DescStringSegment = (UINT16)(Address >> 4);
      Entries[Index].DescStringOffset = (UINT16)(Address & 15);
    }
    CopyMem (OriginalEntries, Entries, sizeof (OriginalEntries));
    Legacy.GetBbsInfo = TestGetBbsInfo;
    OriginalRuntime = gRT;
    CopyMem (&TestRuntime, gRT, sizeof (TestRuntime));
    TestRuntime.GetVariable = TestGetVariable;
    gRT = &TestRuntime; // Only this application's runtime-library pointer changes.
    Status = ControllerTests ();
    if (!EFI_ERROR (Status)) { Status = BootTargetTests (); }
    if (!EFI_ERROR (Status)) {
      UsbOptionType = DuplicateUsbOptionType = BBS_TYPE_USB;
      Status = BootTargetTests ();
    }
    gRT = OriginalRuntime;
    gBS->FreePages (Strings, 1);
  }
  AsciiSPrint (Text, sizeof (Text), "%a checks=%u failed-line=%u status=%r\n",
    EFI_ERROR (Status) ? "FAIL" : "PASS", (UINT32)Checks, (UINT32)FailedLine, Status);
  LogPrint (&Logger, L"%a", Text);
  LogClose (&Logger);
  if (!EFI_ERROR (AppFileOpenAdjacent (&Files, L"BootTargetTest.result",
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

/* Synthetic AHCI firmware fixtures: never touch physical BIOS memory.
   SPDX-License-Identifier: GPL-3.0-only */
UINT8 FirmwareDiskTestMemory[NCV_DISK_FIRMWARE_BYTES];
STATIC UINT8 FirmwareDiskValid[NCV_DISK_FIRMWARE_BLOCKS];

STATIC VOID FirmwareDiskFixture (VOID)
{
  UINTN I;
  StorageFixture ();
  SetMem (FirmwareDiskTestMemory, sizeof (FirmwareDiskTestMemory), 0xff);
  SetMem (FirmwareDiskValid, sizeof (FirmwareDiskValid), 1);
  StorageDevices[0].Config[0x0a] = 6;
  StorageDevices[0].Config[9] = 1;
  StorageEntry.SubClass = 6;
  // Unaligned segment plus offset, rather than a ROM image base.
  StorageEntry.BootHandlerSegment = StorageEntry.DescStringSegment = 0xe713;
  StorageEntry.BootHandlerOffset = 0x3000;
  StorageEntry.DescStringOffset = 0x0908;
  StorageEntry.MfgStringSegment = StorageEntry.MfgStringOffset = 0;
  for (I = 0; I < 64; ++I) { FirmwareDiskTestMemory[0xa130 + I] = (UINT8)(I + 1); }
  CopyMem (FirmwareDiskTestMemory + 0x7a38, "Example AHCI disk", 18);
}

STATIC EFI_STATUS PrepareFirmwareDisk (VOID)
{
  return LegacyFirmwareDiskPrepare (FirmwareDiskTestMemory, FirmwareDiskValid,
    FirmwareDiskTestMemory, &StorageInventory);
}

STATIC EFI_STATUS FirmwareDiskTests (VOID)
{
  BBS_TABLE Saved, Rows[3];
  LEGACY_BOOT_BOOT_TARGET_CONTEXT Context;
  UINT16 Planned[3], Expected[3], Flags;
  BOOLEAN Disabled[3] = {TRUE, FALSE, FALSE};
  UINT16 Ranks[3] = {0, 1, 2};
  UINTN I;
  CONST UINT16 RejectFlags[] = {BIT9, BIT8, BIT8 | BIT9 | BIT10, BIT8 | BIT10 | BIT11, BIT0, BIT10};
  CONST UINT16 RejectPriorities[] = {BBS_IGNORE_ENTRY, BBS_DO_NOT_BOOT_FROM, BBS_LOWEST_PRIORITY};
  FirmwareDiskFixture ();
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  REQUIRE (!LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
  REQUIRE (!EFI_ERROR (PrepareFirmwareDisk ()));
  REQUIRE (LegacyFirmwareDiskMatch (&StorageEntry));
  REQUIRE (LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
  REQUIRE (StrCmp (LegacyBootBbsEligibilityReason (&StorageEntry), L"eligible-corroborated-firmware-AHCI-zero-status") == 0);
  Saved = StorageEntry;
  for (I = 0; I < ARRAY_SIZE (RejectFlags); ++I) {
    CopyMem (&StorageEntry.StatusFlags, &RejectFlags[I], 2);
    REQUIRE (!LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
  }
  StorageEntry = Saved;
  for (I = 0; I < ARRAY_SIZE (RejectPriorities); ++I) {
    StorageEntry.BootPriority = RejectPriorities[I];
    REQUIRE (!LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
  }
  StorageEntry = Saved;
  Rows[0] = Rows[1] = Rows[2] = StorageEntry;
  Flags = 0x500; Rows[1].Bus = 5; CopyMem (&Rows[1].StatusFlags, &Flags, 2);
  ZeroMem (&Context, sizeof (Context));
  Context.BbsJournal = Rows; Context.BbsCount = 3; Context.LiveBbsIndex = 0;
  Context.PlannedPriorities = Planned; Context.ExpectedPriorities = Expected;
  REQUIRE (!EFI_ERROR (LegacyBootBuildPriorityPlan (&Context)));
  REQUIRE (Planned[0] == 0 && Planned[1] == 1 && Planned[2] == Rows[2].BootPriority);
  REQUIRE (!LegacyBootBbsIndexIsTransactionEligible (&Context, 2));
  Context.LegacyDevOrderUsable = TRUE; Context.LegacyDevOrderDisabled = Disabled; Context.LegacyDevOrderRanks = Ranks;
  REQUIRE (LegacyBootBbsIndexIsTransactionEligible (&Context, 0));
  REQUIRE (!EFI_ERROR (LegacyBootBuildPriorityPlan (&Context)));
  // A change after VGA dispatch must invalidate the selected disk, too.
  FirmwareDiskTestMemory[0xa130] ^= 1;
  REQUIRE (!LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
  FirmwareDiskTestMemory[0xa130] ^= 1;
  REQUIRE (LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
  FirmwareDiskTestMemory[0x7a38] ^= 1;
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry));
  FirmwareDiskTestMemory[0x7a38] ^= 1;
  StorageEntry.BootHandlerSegment = 0xc000;
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry)); StorageEntry = Saved;
  StorageEntry.BootHandlerSegment = StorageEntry.BootHandlerOffset = 0xffff;
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry)); StorageEntry = Saved;
  StorageEntry.DescStringSegment = 0;
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry)); StorageEntry = Saved;
  StorageEntry.Bus++;
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry)); StorageEntry = Saved;
  StorageDevices[1] = StorageDevices[0]; StorageDevices[1].Segment = 1; StorageInventory.Count = 2;
  REQUIRE (!EFI_ERROR (PrepareFirmwareDisk ()));
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry));
  StorageInventory.Count = 1; StorageDevices[0].Config[9] = 0;
  REQUIRE (!EFI_ERROR (PrepareFirmwareDisk ()));
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry));
  StorageDevices[0].Config[9] = 1; FirmwareDiskValid[0xa130 / 512] = 0;
  REQUIRE (!EFI_ERROR (PrepareFirmwareDisk ()));
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry));
  FirmwareDiskValid[0xa130 / 512] = 1;
  SetMem (FirmwareDiskTestMemory + 0xa130, 64, 0xff);
  REQUIRE (!EFI_ERROR (PrepareFirmwareDisk ()));
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry));
  FirmwareDiskFixture ();
  // Exercise the actual snapshot wrapper with its physical reads redirected
  // by build_harness to this fixture allocation, never OVMF's legacy area.
  REQUIRE (!EFI_ERROR (LegacyFirmwareDiskCapture (&StorageInventory)));
  REQUIRE (LegacyFirmwareDiskMatch (&StorageEntry));
  LegacyRomGuardReset ();
  REQUIRE (!LegacyFirmwareDiskMatch (&StorageEntry));
  REQUIRE (!LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
  LogPrint (&Logger, L"PASS firmware AHCI zero-status path, explicit failure/disable rejection, mutation, bounds, uniqueness and selected-only priority policy\r\n");
  return EFI_SUCCESS;
}

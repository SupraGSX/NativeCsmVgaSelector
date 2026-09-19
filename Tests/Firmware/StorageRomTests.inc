/* Included only by the disposable firmware harness. SPDX-License-Identifier: GPL-3.0-only */
STATIC PCI_DEVICE_RECORD StorageDevices[2];
STATIC PCI_INVENTORY StorageInventory;
STATIC BBS_TABLE StorageEntry;
STATIC UINTN StorageAt = 0x10000;

STATIC VOID FixPnp (UINTN Pnp)
{
  UINTN I; UINT8 Sum = 0;
  Shadow[StorageAt + Pnp + 9] = 0;
  for (I = 0; I < 32; ++I) { Sum = (UINT8)(Sum + Shadow[StorageAt + Pnp + I]); }
  Shadow[StorageAt + Pnp + 9] = (UINT8)(0 - Sum);
}

STATIC VOID StorageFixture (VOID)
{
  Reset ();
  Rom (StorageAt, 0x2000, 0xabcd, 0x1001);
  Shadow[StorageAt + 0x2a] = 8; Shadow[StorageAt + 0x2b] = 1;
  W16 (StorageAt + 0x1a, 0x100);
  ZeroMem (Shadow + StorageAt + 0x100, 32);
  CopyMem (Shadow + StorageAt + 0x100, "$PnP", 4);
  Shadow[StorageAt + 0x104] = 1; Shadow[StorageAt + 0x105] = 2;
  W16 (StorageAt + 0x110, 0x180);
  Shadow[StorageAt + 0x112] = 1; Shadow[StorageAt + 0x113] = 8;
  W16 (StorageAt + 0x116, 0x200);
  CopyMem (Shadow + StorageAt + 0x180, "Example legacy disk", 20);
  FixPnp (0x100);
  ZeroMem (StorageDevices, sizeof (StorageDevices));
  StorageDevices[0].Bus = 4;
  StorageDevices[0].Config[0] = 0xcd; StorageDevices[0].Config[1] = 0xab;
  StorageDevices[0].Config[2] = 1; StorageDevices[0].Config[3] = 0x10;
  StorageDevices[0].Config[0x0b] = 1; StorageDevices[0].Config[0x0a] = 8;
  StorageInventory.Devices = StorageDevices; StorageInventory.Count = 1;
  ZeroMem (&StorageEntry, sizeof (StorageEntry));
  StorageEntry.BootPriority = BBS_UNPRIORITIZED_ENTRY;
  StorageEntry.DeviceType = BBS_HARDDISK;
  StorageEntry.Bus = 4; StorageEntry.Class = 1; StorageEntry.SubClass = 8;
  StorageEntry.BootHandlerSegment = StorageEntry.DescStringSegment = StorageEntry.MfgStringSegment = 0xd000;
  StorageEntry.BootHandlerOffset = 0x200; StorageEntry.DescStringOffset = 0x180;
}

STATIC EFI_STATUS PrepareStorage (VOID)
{ return LegacyStorageRomPrepare (Shadow, Valid, Shadow, &StorageInventory); }

STATIC EFI_STATUS StorageTests (VOID)
{
  BOOLEAN Missing;
  CHAR8 Manufacturer[129];
  BBS_TABLE Saved, Rows[3];
  LEGACY_BOOT_BOOT_TARGET_CONTEXT Context;
  UINT16 Planned[3], Expected[3], Flags;
  BOOLEAN Disabled[3] = {TRUE, FALSE, FALSE};
  UINT16 Ranks[3] = {0, 1, 2};
  UINTN I;
  CONST UINT16 RejectFlags[] = {BIT9, BIT8, BIT8 | BIT9 | BIT10, BIT8 | BIT10 | BIT11, BIT0};
  CONST UINT16 RejectPriorities[] = {BBS_IGNORE_ENTRY, BBS_DO_NOT_BOOT_FROM, BBS_LOWEST_PRIORITY};
  StorageFixture ();
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (LegacyStorageRomMatch (&StorageEntry, &Missing) && Missing);
  StorageEntry.MfgStringSegment = 0; // Other firmware represents absence as 0000:0000.
  REQUIRE (LegacyStorageRomMatch (&StorageEntry, &Missing) && Missing);
  StorageEntry.MfgStringSegment = StorageEntry.BootHandlerSegment;
  REQUIRE (LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
  REQUIRE (LegacyBootCopyManufacturer (NULL, &StorageEntry, Manufacturer, sizeof (Manufacturer)) == LegacyBootFarStringAbsent);
  REQUIRE (Manufacturer[0] == 0);
  REQUIRE (!EFI_ERROR (LegacyStorageRomVerify (&Logger)));
  // Firmware evidence: PCIR can misclassify a storage ROM. Actual PCI and
  // PnP class agreement remains mandatory; identifiers must still match.
  Shadow[StorageAt + 0x2b] = 2;
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (LegacyStorageRomMatch (&StorageEntry, NULL));
  StorageDevices[0].Config[0x0b] = 2;
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  StorageDevices[0].Config[0x0b] = 1;
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
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
  Rows[0] = StorageEntry; Rows[1] = StorageEntry; Rows[2] = StorageEntry;
  Rows[1].Bus = 5; Flags = 0x500; CopyMem (&Rows[1].StatusFlags, &Flags, 2);
  Rows[2].Bus = 6; // Uncorroborated zero flags must stay untouched.
  ZeroMem (&Context, sizeof (Context));
  Context.BbsJournal = Rows; Context.BbsCount = 3; Context.LiveBbsIndex = 0;
  Context.PlannedPriorities = Planned; Context.ExpectedPriorities = Expected;
  REQUIRE (!EFI_ERROR (LegacyBootBuildPriorityPlan (&Context)));
  REQUIRE (Planned[0] == 0 && Planned[1] == 1 && Planned[2] == Rows[2].BootPriority);
  REQUIRE (Rows[0].BootPriority == BBS_UNPRIORITIZED_ENTRY); // No real BBS write.
  Rows[2] = StorageEntry; // Even a corroborated unselected zero-status disk stays untouched.
  REQUIRE (!LegacyBootBbsIndexIsTransactionEligible (&Context, 2));
  REQUIRE (!EFI_ERROR (LegacyBootBuildPriorityPlan (&Context)));
  REQUIRE (Planned[2] == Rows[2].BootPriority);
  Context.LegacyDevOrderUsable = TRUE; Context.LegacyDevOrderDisabled = Disabled; Context.LegacyDevOrderRanks = Ranks;
  REQUIRE (LegacyBootBbsIndexIsTransactionEligible (&Context, 0));
  REQUIRE (!EFI_ERROR (LegacyBootBuildPriorityPlan (&Context)));
  StorageEntry.BootHandlerOffset++;
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL)); StorageEntry = Saved;
  StorageEntry.DescStringOffset++;
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL)); StorageEntry = Saved;
  StorageEntry.MfgStringSegment++;
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL)); StorageEntry = Saved;
  StorageEntry.Bus++;
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL)); StorageEntry = Saved;
  StorageEntry.BootHandlerSegment = 0;
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL)); StorageEntry = Saved;
  Shadow[StorageAt + 0x228] ^= 1; // Code, not a BBS string.
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  REQUIRE (LegacyStorageRomVerify (&Logger) == EFI_CRC_ERROR);
  Shadow[StorageAt + 0x228] ^= 1;
  REQUIRE (LegacyStorageRomMatch (&StorageEntry, NULL));
  StorageDevices[1] = StorageDevices[0]; StorageDevices[1].Segment = 1; StorageInventory.Count = 2;
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  StorageInventory.Count = 1; StorageDevices[0].Config[0] ^= 1;
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  StorageDevices[0].Config[0] ^= 1; StorageDevices[0].ConfigStatus = EFI_DEVICE_ERROR;
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  StorageDevices[0].ConfigStatus = EFI_SUCCESS;
  Valid[(StorageAt + 0x1000) / 512] = 0;
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  Valid[(StorageAt + 0x1000) / 512] = 1;
  Shadow[StorageAt + 0x109] ^= 1;
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  FixPnp (0x100); W16 (StorageAt + 0x106, 0x100); FixPnp (0x100);
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL)); // Cycle.
  W16 (StorageAt + 0x106, 0xfff0); FixPnp (0x100);
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL)); // Bounds.
  W16 (StorageAt + 0x106, 0x140); FixPnp (0x100);
  CopyMem (Shadow + StorageAt + 0x140, Shadow + StorageAt + 0x100, 32);
  W16 (StorageAt + 0x146, 0); FixPnp (0x140);
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL)); // Ambiguous linked PnP headers.
  W16 (StorageAt + 0x156, 0x240); FixPnp (0x140);
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (LegacyStorageRomMatch (&StorageEntry, NULL)); // Distinct second disk handler.
  W16 (StorageAt + 0x10e, 0x1c0); FixPnp (0x100);
  CopyMem (Shadow + StorageAt + 0x1c0, "Vendor", 7); StorageEntry.MfgStringOffset = 0x1c0;
  REQUIRE (!EFI_ERROR (PrepareStorage ()));
  REQUIRE (LegacyStorageRomMatch (&StorageEntry, &Missing) && !Missing);
  LegacyStorageRomReset ();
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  REQUIRE (!LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
  Flags = 0x500; CopyMem (&StorageEntry.StatusFlags, &Flags, 2);
  REQUIRE (LegacyBootBbsEntryIsTransactionEligible (&StorageEntry)); // Existing UEFI-backed path.
  REQUIRE (LegacyStorageRomPrepare (NULL, Valid, Shadow, &StorageInventory) == EFI_INVALID_PARAMETER);
  REQUIRE (!LegacyStorageRomMatch (&Saved, NULL));
  LogPrint (&Logger, L"PASS resident ROM provenance, status policy, priority plan, missing manufacturer, integrity and ambiguity cases\r\n");
  return EFI_SUCCESS;
}

STATIC EFI_STATUS CapturedStorage (APP_FILE_CONTEXT *Files)
{
  UINT32 Meta[5]; UINTN Index;
  EFI_STATUS Status;
  BOOLEAN Missing;
  CONST CHAR16 *Names[] = {L"capture-a.bin", L"capture-b.bin"};
  CONST CHAR16 *Entries[] = {L"capture-a.bbs", L"capture-b.bbs"};
  CONST CHAR16 *Configs[] = {L"capture-a.pci", L"capture-b.pci"};
  CONST CHAR16 *Metadata[] = {L"capture-a.storage", L"capture-b.storage"};
  for (Index = 0; Index < 2; ++Index) {
    Status = ReadExact (Files, Metadata[Index], Meta, sizeof (Meta));
    if (Status == EFI_NOT_FOUND && Index == 0) { return EFI_SUCCESS; }
    REQUIRE (!EFI_ERROR (Status));
    REQUIRE (!EFI_ERROR (ReadExact (Files, Names[Index], Shadow, sizeof (Shadow))));
    REQUIRE (!EFI_ERROR (ReadExact (Files, Entries[Index], &StorageEntry, sizeof (StorageEntry))));
    ZeroMem (StorageDevices, sizeof (StorageDevices));
    REQUIRE (!EFI_ERROR (ReadExact (Files, Configs[Index], StorageDevices[0].Config, 64)));
    StorageDevices[0].Segment = Meta[0]; StorageDevices[0].Bus = Meta[1];
    StorageDevices[0].Device = Meta[2]; StorageDevices[0].Function = Meta[3];
    StorageInventory.Count = 1; StorageInventory.Devices = StorageDevices;
    SetMem (Valid, sizeof (Valid), 1);
    REQUIRE (!EFI_ERROR (PrepareStorage ()));
    REQUIRE (LegacyStorageRomMatch (&StorageEntry, &Missing) == (BOOLEAN)Meta[4]);
    REQUIRE (LegacyBootBbsEntryIsTransactionEligible (&StorageEntry));
    if (Meta[4]) {
      REQUIRE (Missing);
      REQUIRE (LegacyBootBbsStatusFlagsRaw (&StorageEntry) == 0);
    }
    REQUIRE (!EFI_ERROR (LegacyStorageRomVerify (&Logger)));
    LogPrint (&Logger, L"PASS captured storage %u: ROM-proof=%u status=%04x eligible=yes; shadow layout guard remains independent\r\n",
      (UINT32)Index, Meta[4], LegacyBootBbsStatusFlagsRaw (&StorageEntry));
    LegacyStorageRomReset ();
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS GuardRefreshTests (VOID)
{
  STATIC NATIVE_CSM_VGA_RUNTIME_PLAN GuardPlan;
  UINT8 StorageCopy[0x2000];
  ZeroMem (&GuardPlan, sizeof (GuardPlan));
  StorageFixture ();
  CopyMem (StorageCopy, Shadow + StorageAt, sizeof (StorageCopy));
  SetMem (Shadow + StorageAt, sizeof (StorageCopy), 0xff);
  GuardPlan.Active.Vendor = 0x1234; GuardPlan.Active.DeviceId = 0x5678;
  GuardPlan.SelectedRomSize = 0xec00; GuardPlan.Inventory = StorageInventory;
  REQUIRE (!EFI_ERROR (LegacyRomGuardCheck (&Logger, &GuardPlan)));
  REQUIRE (!RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  // Model a storage ROM appearing during the one GetBbsInfo query.
  CopyMem (Shadow + StorageAt, StorageCopy, sizeof (StorageCopy));
  CopyMem (Original, Shadow, sizeof (Shadow));
  REQUIRE (!EFI_ERROR (LegacyRomGuardRecheckAfterBbs (&Logger)));
  REQUIRE (LegacyStorageRomMatch (&StorageEntry, NULL));
  REQUIRE (CompareMem (Shadow, Original, sizeof (Shadow)) == 0);
  CopyMem (Shadow + 0xe800, StorageCopy, sizeof (StorageCopy));
  CopyMem (Original, Shadow, sizeof (Shadow));
  REQUIRE (LegacyRomGuardRecheckAfterBbs (&Logger) == EFI_UNSUPPORTED);
  REQUIRE (RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
  REQUIRE (!LegacyStorageRomMatch (&StorageEntry, NULL));
  REQUIRE (CompareMem (Shadow, Original, sizeof (Shadow)) == 0);
  LegacyRomGuardReset ();
  REQUIRE (RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
  // A fresh successful guard must revoke the previous overlap-only menu.
  SetMem (Shadow + 0xe800, 0x5800, 0xff);
  REQUIRE (!EFI_ERROR (LegacyRomGuardCheck (&Logger, &GuardPlan)));
  REQUIRE (!RomRecoveryAvailable (EFI_UNSUPPORTED, EFI_SUCCESS));
  LegacyRomGuardReset ();
  LogPrint (&Logger, L"PASS full guard refresh after lazy ROM initialization: new provenance captured, new overlap blocked, fixture bytes untouched\r\n");
  return EFI_SUCCESS;
}

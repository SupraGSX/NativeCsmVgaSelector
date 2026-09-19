/** Private implementation included only by LegacyBootTarget.c.
    SPDX-License-Identifier: LGPL-2.1-or-later */
STATIC
BOOLEAN
LegacyBootBbsPriorityIsValid (
  IN UINT16  Priority
  )
{
  // Exact LegacyBmValidBbsEntry priority predicate. Transaction eligibility
  // adds StatusFlags checks in LegacyBootBbsEntryIsTransactionEligible().
  switch (Priority) {
    case BBS_IGNORE_ENTRY:
    case BBS_DO_NOT_BOOT_FROM:
    case BBS_LOWEST_PRIORITY:
      return FALSE;
    default:
      return TRUE;
  }
}

STATIC
UINT16
LegacyBootBbsStatusFlagsRaw (
  IN CONST BBS_TABLE  *Entry
  )
{
  return LegacyBootReadLe16 (
           (CONST UINT8 *)Entry + OFFSET_OF (BBS_TABLE, StatusFlags)
           );
}

STATIC
BOOLEAN EFIAPI __attribute__((noinline))
LegacyBootBbsEntryIsTransactionEligible (
  IN CONST BBS_TABLE  *Entry
  )
{
  UINT16  StatusFlags;
  UINT16  MediaPresent;

  if ((Entry == NULL) ||
      !LegacyBootBbsPriorityIsValid (Entry->BootPriority))
  {
    return FALSE;
  }

  StatusFlags  = LegacyBootBbsStatusFlagsRaw (Entry);
  MediaPresent = (UINT16)((StatusFlags >> 10) & 0x3U);
  /* TianoCore UpdateBevBcvTable produces all-zero flags for ROM-backed
     entries. Accept that encoding only with independently corroborated PCI,
     resident ROM/PnP or firmware AHCI handler evidence. This predicate preserves
     discouraged unselected rows; selected-disk policy is separate below. */
  if (StatusFlags == 0 && (LegacyStorageRomMatch (Entry, NULL) ||
                          LegacyFirmwareDiskMatch (Entry))) { return TRUE; }
  return (BOOLEAN)(
    ((StatusFlags & BIT8) != 0) &&
    ((StatusFlags & BIT9) == 0) &&
    ((MediaPresent == 1U) || (MediaPresent == 2U))
    );
}

/* Firmware policy is advisory for a uniquely user-selected disk. Structural
   validation and zero-status handler provenance remain mandatory. */
STATIC BOOLEAN EFIAPI __attribute__((noinline))
LegacyBootSelectedDiskCanAttempt (CONST BBS_TABLE *Entry)
{
  return Entry != NULL && Entry->DeviceType == BBS_HARDDISK &&
    (LegacyBootBbsStatusFlagsRaw (Entry) != 0 ||
     LegacyStorageRomMatch (Entry, NULL) || LegacyFirmwareDiskMatch (Entry));
}

STATIC
BOOLEAN
LegacyBootBbsIndexIsTransactionEligible (
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  IN UINTN                             Index
  )
{
  if ((Context == NULL) || (Context->BbsJournal == NULL) ||
      (Index >= Context->BbsCount))
  {
    return FALSE;
  }

  if (Index == Context->LiveBbsIndex) {
    return LegacyBootSelectedDiskCanAttempt (&Context->BbsJournal[Index]);
  }
  if (!LegacyBootBbsEntryIsTransactionEligible (&Context->BbsJournal[Index])) {
    return FALSE;
  }

  // Zero flags cannot express an independent enabled/disabled decision.
  // The provenance exception is limited to the explicitly selected disk;
  // never promote other zero-status devices as boot fallbacks.
  if (LegacyBootBbsStatusFlagsRaw (&Context->BbsJournal[Index]) == 0 &&
      Index != Context->LiveBbsIndex) { return FALSE; }

  // In the pinned LegacyDevOrder encoding, 0xFF in the high byte marks a
  // disabled device.  Such a row must remain byte-for-byte preserved even if
  // its live BBS StatusFlags happen to look enabled.
  return (BOOLEAN)(
    !Context->LegacyDevOrderUsable ||
    (Context->LegacyDevOrderDisabled != NULL &&
     !Context->LegacyDevOrderDisabled[Index])
    );
}

STATIC
CONST CHAR16 *
LegacyBootBbsEligibilityReason (
  IN CONST BBS_TABLE  *Entry
  )
{
  UINT16  StatusFlags;
  UINT16  MediaPresent;

  if (Entry == NULL) {
    return L"null-entry";
  }

  switch (Entry->BootPriority) {
    case BBS_IGNORE_ENTRY:
      return L"BBS_IGNORE_ENTRY";
    case BBS_DO_NOT_BOOT_FROM:
      return L"BBS_DO_NOT_BOOT_FROM";
    case BBS_LOWEST_PRIORITY:
      return L"BBS_LOWEST_PRIORITY";
    default:
      break;
  }

  StatusFlags  = LegacyBootBbsStatusFlagsRaw (Entry);
  MediaPresent = (UINT16)((StatusFlags >> 10) & 0x3U);
  if (StatusFlags == 0 && LegacyStorageRomMatch (Entry, NULL)) {
    return L"eligible-verified-resident-ROM-zero-status";
  }
  if (StatusFlags == 0) {
    return LegacyFirmwareDiskMatch (Entry) ?
      L"eligible-corroborated-firmware-AHCI-zero-status" :
      L"zero-status-without-supported-ROM-or-firmware-handler";
  }
  if ((StatusFlags & BIT8) == 0) {
    return L"StatusFlags.Enabled=0";
  }

  if ((StatusFlags & BIT9) != 0) {
    return L"StatusFlags.Failed=1";
  }

  if (MediaPresent == 0) {
    return L"StatusFlags.MediaPresent=no-media";
  }

  if (MediaPresent == 3U) {
    return L"StatusFlags.MediaPresent=reserved";
  }

  return L"eligible";
}

STATIC
CONST CHAR16 *
LegacyBootBbsIndexEligibilityReason (
  IN CONST LEGACY_BOOT_BOOT_TARGET_CONTEXT  *Context,
  IN UINTN                             Index
  )
{
  if ((Context == NULL) || (Context->BbsJournal == NULL) ||
      (Index >= Context->BbsCount))
  {
    return L"invalid-index";
  }

  if (Context->LegacyDevOrderUsable &&
      (Context->LegacyDevOrderDisabled != NULL) &&
      Context->LegacyDevOrderDisabled[Index])
  {
    return L"LegacyDevOrder-disabled-high-byte-0xFF";
  }

  if (LegacyBootBbsStatusFlagsRaw (&Context->BbsJournal[Index]) == 0 && Index != Context->LiveBbsIndex) {
    return L"unselected-zero-status-preserved";
  }
  return LegacyBootBbsEligibilityReason (&Context->BbsJournal[Index]);
}


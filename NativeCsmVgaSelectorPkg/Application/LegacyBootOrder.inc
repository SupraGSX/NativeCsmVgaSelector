/** Private implementation included only by LegacyBootTarget.c.
    SPDX-License-Identifier: LGPL-2.1-or-later */
STATIC
EFI_STATUS
LegacyBootLogLegacyDevOrderFallback (
  IN APP_LOGGER   *Logger,
  IN EFI_STATUS   Status,
  IN CONST CHAR16 *Reason
  )
{
  EFI_STATUS  LogStatus;

  if ((Logger == NULL) || (Reason == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  LogStatus = LogPrint (
                Logger,
                L"LegacyDevOrder %s: 0x%016lx (%s)\r\n"
                L"NCV_BOOT_LEGACY_DEV_ORDER_FALLBACK=LIVE_BBS_INDEX_ORDER\r\n"
                L"LegacyDevOrder remains read-only; remaining eligible "
                L"BBS_HARDDISK entries will be assigned in ascending live "
                L"BBS index order.\r\n",
                Reason,
                (UINT64)Status,
                EfiStatusName (Status)
                );
  if (EFI_ERROR (LogStatus)) {
    return LogStatus;
  }

  return LogFlush (Logger);
}

/* Keep this helper out of the stack-sensitive Boot frame under LTO. */
STATIC
__attribute__((noinline))
EFI_STATUS
LegacyBootCaptureLegacyDevOrder (
  IN     APP_LOGGER                    *Logger,
  IN OUT LEGACY_BOOT_BOOT_TARGET_CONTEXT    *Context
  )
{
  EFI_STATUS  Status;
  UINT8       *Variable;
  UINTN       VariableSize;
  UINT32      Attributes;
  UINT16      *Ranks;
  BOOLEAN     *Disabled;
  BOOLEAN     SeenType[256];
  BOOLEAN     SeenIndex[LEGACY_BOOT_BOOT_TARGET_MAX_BBS_ENTRIES];
  UINTN       Offset;
  UINTN       EntryBytes;
  UINTN       DataCount;
  UINTN       DataIndex;
  UINTN       Index;
  UINT16      Length;
  UINT16      EncodedIndex;
  UINT16      LiveIndex;
  UINT16      HighByte;
  UINT16      GlobalRank;
  UINT8       BbsType;

  if ((Logger == NULL) || (Context == NULL) ||
      (Context->BbsJournal == NULL) || (Context->BbsCount == 0) ||
      (Context->BbsCount > LEGACY_BOOT_BOOT_TARGET_MAX_BBS_ENTRIES) ||
      (Context->LegacyDevOrderData != NULL) ||
      (Context->LegacyDevOrderRanks != NULL) ||
      (Context->LegacyDevOrderDisabled != NULL))
  {
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
  if (Status == EFI_NOT_FOUND) {
    Context->LegacyDevOrderPresent = FALSE;
    Context->LegacyDevOrderUsable  = FALSE;
    return LegacyBootLogLegacyDevOrderFallback (Logger, Status, L"is absent");
  }

  if (EFI_ERROR (Status)) {
    Context->LegacyDevOrderPresent = FALSE;
    Context->LegacyDevOrderUsable  = FALSE;
    return LegacyBootLogLegacyDevOrderFallback (
             Logger,
             Status,
             L"cannot be safely read"
             );
  }

  if ((Attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0) {
    FreePool (Variable);
    Context->LegacyDevOrderPresent = FALSE;
    Context->LegacyDevOrderUsable  = FALSE;
    return LegacyBootLogLegacyDevOrderFallback (
             Logger,
             EFI_COMPROMISED_DATA,
             L"has unsupported attributes"
             );
  }

  Ranks = AllocatePool ((UINTN)Context->BbsCount * sizeof (UINT16));
  Disabled = AllocateZeroPool (
               (UINTN)Context->BbsCount * sizeof (BOOLEAN)
               );
  if ((Ranks == NULL) || (Disabled == NULL)) {
    if (Ranks != NULL) {
      FreePool (Ranks);
    }

    if (Disabled != NULL) {
      FreePool (Disabled);
    }

    FreePool (Variable);
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < Context->BbsCount; ++Index) {
    Ranks[Index] = MAX_UINT16;
  }

  ZeroMem (SeenType, sizeof (SeenType));
  ZeroMem (SeenIndex, sizeof (SeenIndex));
  Offset     = 0;
  GlobalRank = 0;
  Status = LogPrint (
             Logger,
             L"\r\n=== Read-only pinned LegacyDevOrder validation ===\r\n"
             L"name=LegacyDevOrder guid=a56074db-65fe-45f7-bd21-"
             L"2d2bdd8e9652 attributes=0x%08x size=0x%lx CRC32=0x%08x\r\n",
             Attributes,
             (UINT64)VariableSize,
             LegacyBootCrc32 (Variable, VariableSize)
             );
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  while (Offset < VariableSize) {
    if ((VariableSize - Offset) < 3U) {
      Status = EFI_COMPROMISED_DATA;
      LogPrint (
        Logger,
        L"LegacyDevOrder truncated entry header at offset 0x%lx\r\n",
        (UINT64)Offset
        );
      goto Failed;
    }

    BbsType = Variable[Offset];
    Length  = LegacyBootReadLe16 (Variable + Offset + 1U);
    if ((Length < sizeof (UINT16)) ||
        ((Length & 1U) != 0) ||
        ((UINTN)Length > (VariableSize - Offset - 1U)))
    {
      Status = EFI_COMPROMISED_DATA;
      LogPrint (
        Logger,
        L"LegacyDevOrder malformed entry at offset 0x%lx: type=0x%02x "
        L"length=0x%04x remaining=0x%lx\r\n",
        (UINT64)Offset,
        BbsType,
        Length,
        (UINT64)(VariableSize - Offset)
        );
      goto Failed;
    }

    if (SeenType[BbsType]) {
      Status = EFI_COMPROMISED_DATA;
      LogPrint (
        Logger,
        L"LegacyDevOrder contains duplicate BBS type 0x%02x\r\n",
        BbsType
        );
      goto Failed;
    }

    SeenType[BbsType] = TRUE;
    EntryBytes         = 1U + (UINTN)Length;
    DataCount          = ((UINTN)Length - sizeof (UINT16)) / sizeof (UINT16);
    Status = LogPrint (
               Logger,
               L"LegacyDevOrder entry offset=0x%lx type=0x%02x "
               L"length=0x%04x data-count=%u\r\n",
               (UINT64)Offset,
               BbsType,
               Length,
               DataCount
               );
    if (EFI_ERROR (Status)) {
      goto Failed;
    }

    for (DataIndex = 0; DataIndex < DataCount; ++DataIndex) {
      EncodedIndex = LegacyBootReadLe16 (
                       Variable + Offset + 3U +
                       (DataIndex * sizeof (UINT16))
                       );
      LiveIndex = (UINT16)(EncodedIndex & 0x00FFU);
      HighByte  = (UINT16)(EncodedIndex & 0xFF00U);
      if ((HighByte != 0) && (HighByte != 0xFF00U)) {
        Status = EFI_COMPROMISED_DATA;
        LogPrint (
          Logger,
          L"LegacyDevOrder type 0x%02x data[%u]=0x%04x uses an "
          L"unsupported high-byte encoding\r\n",
          BbsType,
          DataIndex,
          EncodedIndex
          );
        goto Failed;
      }

      if ((LiveIndex >= Context->BbsCount) || SeenIndex[LiveIndex]) {
        Status = EFI_COMPROMISED_DATA;
        LogPrint (
          Logger,
          L"LegacyDevOrder type 0x%02x data[%u]=0x%04x has an "
          L"out-of-range or duplicate live BBS index\r\n",
          BbsType,
          DataIndex,
          EncodedIndex
          );
        goto Failed;
      }

      if ((Context->BbsJournal[LiveIndex].DeviceType != (UINT16)BbsType) ||
          !LegacyBootBbsPriorityIsValid (
             Context->BbsJournal[LiveIndex].BootPriority
             ))
      {
        Status = EFI_COMPROMISED_DATA;
        LogPrint (
          Logger,
          L"LegacyDevOrder data 0x%04x conflicts with live BBS[%u]: "
          L"variable-type=0x%02x live-type=0x%04x live-priority=0x%04x\r\n",
          EncodedIndex,
          LiveIndex,
          BbsType,
          Context->BbsJournal[LiveIndex].DeviceType,
          Context->BbsJournal[LiveIndex].BootPriority
          );
        goto Failed;
      }

      SeenIndex[LiveIndex] = TRUE;
      Ranks[LiveIndex]     = GlobalRank++;
      Disabled[LiveIndex]  = (BOOLEAN)(HighByte == 0xFF00U);
      Status = LogPrint (
                 Logger,
                 L"  data[%u]=0x%04x -> BBS[%u] global-order-rank=%u "
                 L"disabled-high-byte=%s\r\n",
                 DataIndex,
                 EncodedIndex,
                 LiveIndex,
                 Ranks[LiveIndex],
                 Disabled[LiveIndex] ? L"yes" : L"no"
                 );
      if (EFI_ERROR (Status)) {
        goto Failed;
      }
    }

    Offset += EntryBytes;
  }

  // A present value is used only when it describes every reference-valid
  // live BBS row exactly once.  This intentionally fails closed instead of
  // silently inventing order for a stale or incomplete variable.
  for (Index = 0; Index < Context->BbsCount; ++Index) {
    if (LegacyBootBbsPriorityIsValid (
          Context->BbsJournal[Index].BootPriority
          ) != SeenIndex[Index])
    {
      Status = EFI_COMPROMISED_DATA;
      LogPrint (
        Logger,
        L"LegacyDevOrder complete-coverage failure at BBS[%u]: "
        L"reference-valid=%s represented=%s type=0x%04x priority=0x%04x\r\n",
        Index,
        LegacyBootBbsPriorityIsValid (
          Context->BbsJournal[Index].BootPriority
          ) ? L"yes" : L"no",
        SeenIndex[Index] ? L"yes" : L"no",
        Context->BbsJournal[Index].DeviceType,
        Context->BbsJournal[Index].BootPriority
        );
      goto Failed;
    }
  }

  if (Disabled[Context->LiveBbsIndex]) {
    Status = LogPrint (Logger,
      L"NCV_DISK_FIRMWARE_WARNING_ORDER\r\n"
      L"Firmware boot warning: LegacyDevOrder disables selected BBS[%u] (0xFF). "
      L"User selection overrides this policy if preflight passes; other disabled rows remain preserved.\r\n",
      Context->LiveBbsIndex);
    if (EFI_ERROR (Status)) { goto Failed; }
  }

  Context->LegacyDevOrderPresent    = TRUE;
  Context->LegacyDevOrderUsable     = TRUE;
  Context->LegacyDevOrderAttributes = Attributes;
  Context->LegacyDevOrderData       = Variable;
  Context->LegacyDevOrderSize       = VariableSize;
  Context->LegacyDevOrderCrc32      = LegacyBootCrc32 (
                                        Variable,
                                        VariableSize
                                        );
  Context->LegacyDevOrderRanks      = Ranks;
  Context->LegacyDevOrderDisabled   = Disabled;
  Status = LogPrint (
             Logger,
             L"LegacyDevOrder strict validation passed: covered=%u, "
             L"selected BBS[%u] follows user choice; this read-only order will rank "
             L"remaining entries and unselected 0xFF-high-byte entries will be "
             L"preserved exactly\r\n",
             GlobalRank,
             Context->LiveBbsIndex
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return LogFlush (Logger);

Failed:
  FreePool (Ranks);
  FreePool (Disabled);
  FreePool (Variable);
  Context->LegacyDevOrderPresent = FALSE;
  Context->LegacyDevOrderUsable  = FALSE;
  return LegacyBootLogLegacyDevOrderFallback (
           Logger,
           Status,
           L"is malformed or cannot be safely parsed"
           );
}


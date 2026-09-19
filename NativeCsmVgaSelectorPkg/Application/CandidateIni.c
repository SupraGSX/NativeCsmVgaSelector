#include "CandidateIni.h"

typedef struct {
  char     *Data;
  NCV_SIZE Capacity;
  NCV_SIZE Used;
  int      Failed;
} NCV_TEXT;

typedef enum {
  NcvSectionNone = 0,
  NcvSectionProfile,
  NcvSectionVideo,
  NcvSectionEndpoint,
  NcvSectionBoot,
  NcvSectionBehavior,
  NcvSectionMarker,
  NcvSectionMaximum
} NCV_CONFIG_SECTION;

static void
ZeroBytes (
  void     *Buffer,
  NCV_SIZE Size
  )
{
  NCV_U8   *Bytes;
  NCV_SIZE Index;

  Bytes = (NCV_U8 *)Buffer;
  for (Index = 0; Index < Size; ++Index) {
    Bytes[Index] = 0;
  }
}

static int
AsciiEqual (
  const char *Left,
  const char *Right
  )
{
  if ((Left == 0) || (Right == 0)) {
    return 0;
  }

  while ((*Left != '\0') && (*Right != '\0')) {
    if (*Left++ != *Right++) {
      return 0;
    }
  }

  return (*Left == '\0') && (*Right == '\0');
}

static int
CopyAscii (
  char        *Destination,
  NCV_SIZE    DestinationBytes,
  const char  *Source
  )
{
  NCV_SIZE Length;

  if ((Destination == 0) || (DestinationBytes == 0) || (Source == 0)) {
    return 0;
  }

  Length = 0;
  while ((Length < DestinationBytes) && (Source[Length] != '\0')) {
    ++Length;
  }
  if ((Length == 0) || (Length >= DestinationBytes)) {
    return 0;
  }

  for (Length = 0; Source[Length] != '\0'; ++Length) {
    unsigned char Value;

    Value = (unsigned char)Source[Length];
    if ((Value < 0x20U) || (Value > 0x7eU)) {
      return 0;
    }
    Destination[Length] = Source[Length];
  }
  Destination[Length] = '\0';
  return 1;
}

static void
PutChar (
  NCV_TEXT *Text,
  char     Character
  )
{
  if ((Text == 0) || (Text->Used + 1U >= Text->Capacity)) {
    if (Text != 0) {
      Text->Failed = 1;
    }
    return;
  }

  Text->Data[Text->Used++] = Character;
  Text->Data[Text->Used] = '\0';
}

static void
PutText (
  NCV_TEXT    *Text,
  const char  *Source
  )
{
  while ((Source != 0) && (*Source != '\0')) {
    PutChar (Text, *Source++);
  }
}

static void
PutCommentText (
  NCV_TEXT    *Text,
  const char  *Source,
  NCV_SIZE    Capacity
  )
{
  NCV_SIZE Index;

  if (Source == 0) {
    PutText (Text, "<unavailable>");
    return;
  }

  for (Index = 0; Index < Capacity; ++Index) {
    unsigned char Value;

    Value = (unsigned char)Source[Index];
    if (Value == 0) {
      return;
    }
    if ((Value >= 0x20U) && (Value <= 0x7eU)) {
      PutChar (Text, (char)Value);
    } else {
      PutChar (Text, '.');
    }
  }
  PutText (Text, "<unterminated>");
}

static void
PutHex (
  NCV_TEXT      *Text,
  unsigned long Value,
  unsigned      Digits
  )
{
  static const char Hex[] = "0123456789ABCDEF";
  unsigned          Index;

  for (Index = 0; Index < Digits; ++Index) {
    PutChar (
      Text,
      Hex[(Value >> ((Digits - Index - 1U) * 4U)) & 15U]
      );
  }
}

static void
PutDec (
  NCV_TEXT *Text,
  NCV_SIZE Value
  )
{
  char     Buffer[24];
  unsigned Count;

  if (Value == 0) {
    PutChar (Text, '0');
    return;
  }

  Count = 0;
  while ((Value != 0) && (Count < sizeof (Buffer))) {
    Buffer[Count++] = (char)('0' + (Value % 10U));
    Value /= 10U;
  }
  while (Count != 0) {
    PutChar (Text, Buffer[--Count]);
  }
}

static void
PutBdf (
  NCV_TEXT              *Text,
  const NCV_PCI_ADDRESS *Address
  )
{
  PutHex (Text, Address->Segment, 4);
  PutChar (Text, ':');
  PutHex (Text, Address->Bus, 2);
  PutChar (Text, ':');
  PutHex (Text, Address->Device, 2);
  PutChar (Text, '.');
  PutHex (Text, Address->Function, 1);
}

static int
AddressLess (
  const NCV_PCI_ADDRESS *Left,
  const NCV_PCI_ADDRESS *Right
  )
{
  if (Left->Segment != Right->Segment) {
    return Left->Segment < Right->Segment;
  }
  if (Left->Bus != Right->Bus) {
    return Left->Bus < Right->Bus;
  }
  if (Left->Device != Right->Device) {
    return Left->Device < Right->Device;
  }
  return Left->Function < Right->Function;
}

static int
AddressEqual (
  const NCV_PCI_ADDRESS *Left,
  const NCV_PCI_ADDRESS *Right
  )
{
  return (Left->Segment == Right->Segment) &&
         (Left->Bus == Right->Bus) &&
         (Left->Device == Right->Device) &&
         (Left->Function == Right->Function);
}

static unsigned
GopRank (
  const NCV_DISPLAY_CANDIDATE *Candidate
  )
{
  if (!Candidate->GopAssociationKnown) {
    return 1U;
  }
  return Candidate->GopAssociated ? 1U : 0U;
}

static unsigned
ActiveVgaRank (
  const NCV_DISPLAY_CANDIDATE *Candidate
  )
{
  if (!Candidate->ActiveLegacyVgaOwnerKnown) {
    return 1U;
  }
  return Candidate->ActiveLegacyVgaOwner ? 1U : 0U;
}

int
NcvDisplayCollectionAppend (
  NCV_DISPLAY_COLLECTION      *Collection,
  const NCV_DISPLAY_CANDIDATE *Candidate
  )
{
  if ((Collection == 0) || (Candidate == 0)) {
    return -1;
  }

  ++Collection->ObservedCount;
  if (Collection->Count >= NCV_MAX_DISPLAY_CANDIDATES) {
    Collection->Overflow = 1;
    return 1;
  }

  Collection->Items[Collection->Count++] = *Candidate;
  return 0;
}

int
NcvStorageCollectionAppend (
  NCV_STORAGE_COLLECTION      *Collection,
  const NCV_STORAGE_CANDIDATE *Candidate
  )
{
  if ((Collection == 0) || (Candidate == 0)) {
    return -1;
  }

  ++Collection->ObservedCount;
  if (Collection->Count >= NCV_MAX_STORAGE_CANDIDATES) {
    Collection->Overflow = 1;
    return 1;
  }

  Collection->Items[Collection->Count++] = *Candidate;
  return 0;
}

int
NcvIniValueIsExactlyRepresentable (
  const char *Value,
  NCV_SIZE   Capacity
  )
{
  NCV_SIZE Index;
  int      HasNonSpace;

  if ((Value == 0) || (Capacity == 0)) {
    return 0;
  }

  HasNonSpace = 0;
  for (Index = 0; Index < Capacity; ++Index) {
    unsigned char Character;

    Character = (unsigned char)Value[Index];
    if (Character == 0) {
      if ((Index == 0) || (Index >= NCV_CONFIG_TEXT_BYTES)) {
        return 0;
      }
      return HasNonSpace;
    }
    if ((Character < 0x20U) || (Character > 0x7eU) ||
        (Character == '='))
    {
      return 0;
    }
    HasNonSpace |= Character != ' ';
  }

  return 0;
}

int
NcvEncodeDiskDescription (
  const char *Value,
  char       *Output,
  NCV_SIZE   Capacity
  )
{
  NCV_SIZE Length;
  NCV_SIZE Index;
  int      Quoted;
  NCV_TEXT Text;

  if ((Output == 0) || (Capacity == 0) ||
      !NcvIniValueIsExactlyRepresentable (Value, NCV_CONFIG_TEXT_BYTES)) {
    return -1;
  }
  for (Length = 0; Value[Length] != '\0'; ++Length) { }
  Quoted = (Value[0] == ' ') || (Value[0] == '"') ||
           (Value[Length - 1U] == ' ');
  Text.Data = Output; Text.Capacity = Capacity; Text.Used = 0; Text.Failed = 0;
  Output[0] = '\0';
  if (Quoted) { PutChar (&Text, '"'); }
  for (Index = 0; Index < Length; ++Index) {
    if (Quoted && ((Value[Index] == '"') || (Value[Index] == '\\'))) {
      PutChar (&Text, '\\');
    }
    PutChar (&Text, Value[Index]);
  }
  if (Quoted) { PutChar (&Text, '"'); }
  return Text.Failed ? -2 : 0;
}

static int
CopyDiskDescription (
  char       *Destination,
  NCV_SIZE   Capacity,
  const char *Value
  )
{
  NCV_SIZE Read;
  NCV_SIZE Written;
  char     Character;

  if (Value[0] != '"') {
    return CopyAscii (Destination, Capacity, Value);
  }
  Written = 0;
  for (Read = 1; Value[Read] != '\0'; ++Read) {
    Character = Value[Read];
    if (Character == '"') {
      if ((Value[Read + 1U] != '\0') || (Written >= Capacity)) { return 0; }
      Destination[Written] = '\0';
      return NcvIniValueIsExactlyRepresentable (Destination, Capacity);
    }
    if (Character == '\\') {
      Character = Value[++Read];
      if ((Character != '\\') && (Character != '"')) { return 0; }
    }
    if ((Written + 1U >= Capacity) ||
        ((unsigned char)Character < 0x20U) ||
        ((unsigned char)Character > 0x7eU) || (Character == '=')) { return 0; }
    Destination[Written++] = Character;
  }
  return 0;
}

int
NcvRankDisplays (
  NCV_DISPLAY_CANDIDATE *Items,
  NCV_SIZE              Count,
  NCV_SIZE              *Selected
  )
{
  NCV_SIZE Index;
  NCV_SIZE Best;

  if ((Items == 0) || (Selected == 0) ||
      (Count > NCV_MAX_DISPLAY_CANDIDATES))
  {
    return -1;
  }

  Best = NCV_NO_SELECTION;
  for (Index = 0; Index < Count; ++Index) {
    Items[Index].Selected = 0;
    Items[Index].Eligible = (NCV_BOOL)(
      Items[Index].AddressValid && !(Items[Index].ActiveLegacyVgaOwnerKnown && Items[Index].ActiveLegacyVgaOwner) &&
      Items[Index].AcceptedLegacyRom &&
      Items[Index].PathDiscovered &&
      Items[Index].PathValidated
      );
    if (!Items[Index].Eligible) {
      continue;
    }

    if ((Best == NCV_NO_SELECTION) ||
        (ActiveVgaRank (&Items[Index]) <
         ActiveVgaRank (&Items[Best])) ||
        ((ActiveVgaRank (&Items[Index]) ==
          ActiveVgaRank (&Items[Best])) &&
         (GopRank (&Items[Index]) < GopRank (&Items[Best]))) ||
        ((ActiveVgaRank (&Items[Index]) ==
          ActiveVgaRank (&Items[Best])) &&
         (GopRank (&Items[Index]) == GopRank (&Items[Best])) &&
         AddressLess (&Items[Index].Address, &Items[Best].Address)))
    {
      Best = Index;
    }
  }

  if (Best != NCV_NO_SELECTION) {
    Items[Best].Selected = 1;
  }
  *Selected = Best;
  return 0;
}

int
NcvRankStorage (
  NCV_STORAGE_CANDIDATE *Items,
  NCV_SIZE              Count,
  NCV_SIZE              *Selected
  )
{
  NCV_SIZE Index;
  NCV_SIZE Best;

  if ((Items == 0) || (Selected == 0) ||
      (Count > NCV_MAX_STORAGE_CANDIDATES))
  {
    return -1;
  }

  /* Match the boot selector's case-insensitive controller/name identity.
     Ambiguous rows remain visible in discovery, but cannot produce a unique INI. */
  for (Index = 0; Index < Count; ++Index) {
    NCV_SIZE Other;
    Items[Index].IdentityAmbiguous = 0;
    if (!Items[Index].AddressValid) { continue; }
    for (Other = 0; Other < Count; ++Other) {
      NCV_SIZE Character = 0;
      if (Other == Index || !Items[Other].AddressValid ||
          !AddressEqual (&Items[Index].Address, &Items[Other].Address)) { continue; }
      for (;;) {
        unsigned char Left = (unsigned char)Items[Index].Description[Character];
        unsigned char Right = (unsigned char)Items[Other].Description[Character];
        if (Left >= 'A' && Left <= 'Z') { Left += 'a' - 'A'; }
        if (Right >= 'A' && Right <= 'Z') { Right += 'a' - 'A'; }
        if (Left != Right) { break; }
        if (Left == 0) { Items[Index].IdentityAmbiguous = 1; break; }
        if (++Character == NCV_CANDIDATE_STRING_BYTES) { break; }
      }
    }
    if (Items[Index].IdentityAmbiguous) { Items[Index].Eligible = 0; }
  }
  Best = NCV_NO_SELECTION;
  for (Index = 0; Index < Count; ++Index) {
    Items[Index].Selected = 0;
    if (!Items[Index].Eligible) {
      continue;
    }

    if ((Best == NCV_NO_SELECTION) ||
        (Items[Index].Usb < Items[Best].Usb) ||
        ((Items[Index].Usb == Items[Best].Usb) &&
         (Items[Index].BbsIndex < Items[Best].BbsIndex)))
    {
      Best = Index;
    }
  }

  if (Best != NCV_NO_SELECTION) {
    Items[Best].Selected = 1;
  }
  *Selected = Best;
  return 0;
}

static int
ValidateSelections (
  const NCV_DISPLAY_CANDIDATE *Displays,
  NCV_SIZE                    DisplayCount,
  NCV_SIZE                    SelectedDisplay,
  const NCV_STORAGE_CANDIDATE *Storage,
  NCV_SIZE                    StorageCount,
  NCV_SIZE                    SelectedStorage
  )
{
  NCV_SIZE Index;
  NCV_SIZE DisplaySelections;
  NCV_SIZE StorageSelections;

  if ((Displays == 0) || (Storage == 0) ||
      (DisplayCount > NCV_MAX_DISPLAY_CANDIDATES) ||
      (StorageCount > NCV_MAX_STORAGE_CANDIDATES) ||
      ((SelectedDisplay != NCV_NO_SELECTION) &&
       (SelectedDisplay >= DisplayCount)) ||
      ((SelectedStorage != NCV_NO_SELECTION) &&
       (SelectedStorage >= StorageCount)))
  {
    return 0;
  }

  DisplaySelections = 0;
  for (Index = 0; Index < DisplayCount; ++Index) {
    if (Displays[Index].Selected) {
      ++DisplaySelections;
    }
  }
  StorageSelections = 0;
  for (Index = 0; Index < StorageCount; ++Index) {
    if (Storage[Index].Selected) {
      ++StorageSelections;
    }
  }

  if (SelectedDisplay == NCV_NO_SELECTION) {
    if (DisplaySelections != 0) {
      return 0;
    }
  } else if ((DisplaySelections != 1) ||
             !Displays[SelectedDisplay].Selected ||
             !Displays[SelectedDisplay].Eligible ||
             !Displays[SelectedDisplay].AddressValid)
  {
    return 0;
  }

  if (SelectedStorage == NCV_NO_SELECTION) {
    if (StorageSelections != 0) {
      return 0;
    }
  } else if ((StorageSelections != 1) ||
             !Storage[SelectedStorage].Selected ||
             !Storage[SelectedStorage].Eligible ||
             !Storage[SelectedStorage].AddressValid ||
             !NcvIniValueIsExactlyRepresentable (
                Storage[SelectedStorage].Description,
                NCV_CANDIDATE_STRING_BYTES
                ))
  {
    return 0;
  }

  return 1;
}

int
NcvFormatCandidateIni (
  const NCV_DISPLAY_CANDIDATE *Displays,
  NCV_SIZE                    DisplayCount,
  NCV_SIZE                    SelectedDisplay,
  const NCV_STORAGE_CANDIDATE *Storage,
  NCV_SIZE                    StorageCount,
  NCV_SIZE                    SelectedStorage,
  char                        *Output,
  NCV_SIZE                    Capacity,
  NCV_SIZE                    *OutputSize
  )
{
  NCV_TEXT Text;
  NCV_SIZE Index;
  NCV_BOOL Complete;

  if ((Output == 0) || (OutputSize == 0) || (Capacity == 0) ||
      !ValidateSelections (
         Displays,
         DisplayCount,
         SelectedDisplay,
         Storage,
         StorageCount,
         SelectedStorage
         ))
  {
    return -1;
  }

  Text.Data = Output;
  Text.Capacity = Capacity;
  Text.Used = 0;
  Text.Failed = 0;
  Output[0] = '\0';
  Complete = (NCV_BOOL)(
    (SelectedDisplay != NCV_NO_SELECTION) &&
    (SelectedStorage != NCV_NO_SELECTION)
    );

  PutText (&Text, "# Generated by Native CSM VGA Selector 1.2 read-only discovery.\r\n");
  if (Complete) {
    PutText (
      &Text,
      "# One display and one storage candidate are selected below.\r\n"
      "# To select another candidate, comment the active setting and\r\n"
      "# uncomment the desired setting.\r\n"
      );
  } else {
    PutText (
      &Text,
      "# This Probe-only result is not boot-ready because at least one\r\n"
      "# candidate category has no eligible automatic selection.\r\n"
      );
  }
  PutText (&Text, "\r\n[Marker]\r\nEnabled=false\r\n\r\n[Behavior]\r\nProbe=false\r\n\r\n[Video]\r\n");

  for (Index = 0; Index < DisplayCount; ++Index) {
    PutText (&Text, "# Display candidate ");
    PutDec (&Text, Index + 1U);
    if (Complete && (Index == SelectedDisplay)) {
      PutText (&Text, " - selected");
    }
    PutText (&Text, "\r\n# PCI identity: ");
    PutHex (&Text, Displays[Index].Vendor, 4);
    PutChar (&Text, ':');
    PutHex (&Text, Displays[Index].DeviceId, 4);
    PutText (&Text, " subsystem ");
    PutHex (&Text, Displays[Index].SubsystemVendor, 4);
    PutChar (&Text, ':');
    PutHex (&Text, Displays[Index].SubsystemDevice, 4);
    PutText (&Text, "\r\n# GOP association: ");
    if (!Displays[Index].GopAssociationKnown) {
      PutText (&Text, "unknown (ranked conservatively)");
    } else {
      PutText (&Text, Displays[Index].GopAssociated ? "yes" : "no");
    }
    PutText (&Text, "\r\n# Active legacy VGA owner: ");
    if (!Displays[Index].ActiveLegacyVgaOwnerKnown) {
      PutText (&Text, "unknown (ranked conservatively as active)");
    } else {
      PutText (
        &Text,
        Displays[Index].ActiveLegacyVgaOwner ? "yes" : "no"
        );
    }
    PutText (&Text, "\r\n# Option ROM exposed: ");
    PutText (&Text, Displays[Index].OptionRomExposed ? "yes" : "no");
    PutText (&Text, "\r\n# Valid legacy x86 ROM: ");
    PutText (&Text, Displays[Index].AcceptedLegacyRom ? "yes" : "no");
    PutText (&Text, "\r\n# Upstream path discovered: ");
    PutText (&Text, Displays[Index].PathDiscovered ? "yes" : "no");
    PutText (&Text, "\r\n# Upstream path validation: ");
    PutText (&Text, Displays[Index].PathValidated ? "passed" : "failed");
    PutText (&Text, "\r\n");
    if (Displays[Index].AddressValid) {
      if (!Complete || (Index != SelectedDisplay)) {
        PutText (&Text, "# ");
      }
      PutText (&Text, "TargetPci=");
      PutBdf (&Text, &Displays[Index].Address);
      PutText (&Text, "\r\n\r\n");
    } else {
      PutText (&Text, "# PCI location unavailable: segment=0x");
      PutHex (&Text, Displays[Index].RawSegment, 16);
      PutText (&Text, " bus=0x");
      PutHex (&Text, Displays[Index].RawBus, 16);
      PutText (&Text, " device=0x");
      PutHex (&Text, Displays[Index].RawDevice, 16);
      PutText (&Text, " function=0x");
      PutHex (&Text, Displays[Index].RawFunction, 16);
      PutText (&Text, "; no TargetPci emitted\r\n\r\n");
    }
  }

  PutText (
    &Text,
    "[Endpoint]\r\n"
    "IoDecoding=On\r\n"
    "MemoryDecoding=On\r\n"
    "BusMastering=Ignore\r\n"
    "\r\n[Boot]\r\n"
    );
  for (Index = 0; Index < StorageCount; ++Index) {
    char Encoded[NCV_ENCODED_DESCRIPTION_BYTES];

    PutText (&Text, "# Storage candidate ");
    PutDec (&Text, Index + 1U);
    if (Complete && (Index == SelectedStorage)) {
      PutText (&Text, " - selected");
    }
    PutText (&Text, "\r\n# Manufacturer: ");
    PutCommentText (
      &Text,
      Storage[Index].Manufacturer,
      NCV_CANDIDATE_STRING_BYTES
      );
    PutText (&Text, "\r\n# Model: ");
    PutCommentText (
      &Text,
      Storage[Index].Description,
      NCV_CANDIDATE_STRING_BYTES
      );
    PutText (&Text, "\r\n# BBS index: ");
    PutDec (&Text, Storage[Index].BbsIndex);
    PutText (&Text, "\r\n# Controller type: ");
    PutText (&Text, Storage[Index].Usb ? "USB" : "non-USB");
    PutText (&Text, "\r\n");

    if (Storage[Index].AddressValid) {
      if (!Complete || (Index != SelectedStorage)) {
        PutText (&Text, "# ");
      }
      PutText (&Text, "TargetControllerPci=");
      PutBdf (&Text, &Storage[Index].Address);
      PutText (&Text, "\r\n");
    } else {
      PutText (&Text, "# Invalid BBS PCI location: bus=0x");
      PutHex (&Text, Storage[Index].RawBus, 8);
      PutText (&Text, " device=0x");
      PutHex (&Text, Storage[Index].RawDevice, 8);
      PutText (&Text, " function=0x");
      PutHex (&Text, Storage[Index].RawFunction, 8);
      PutText (&Text, "\r\n");
    }

    if (!Complete || (Index != SelectedStorage)) {
      PutText (&Text, "# ");
    }
    PutText (&Text, "TargetBbsDescription=");
    if (NcvEncodeDiskDescription (Storage[Index].Description, Encoded,
                                 sizeof (Encoded)) == 0) {
      PutText (&Text, Encoded);
    } else {
      /* Unrepresentable candidates remain diagnostic comments only. */
      PutCommentText (&Text, Storage[Index].Description, NCV_CANDIDATE_STRING_BYTES);
    }
    PutText (&Text, "\r\n\r\n");
  }

  if (Text.Failed) {
    return -2;
  }
  *OutputSize = Text.Used;
  return 0;
}

static int
IsSpace (
  char Character
  )
{
  return (Character == ' ') || (Character == '\t');
}

static void
Trim (
  char *Text
  )
{
  NCV_SIZE Start;
  NCV_SIZE End;
  NCV_SIZE Index;

  Start = 0;
  while (IsSpace (Text[Start])) {
    ++Start;
  }

  End = 0;
  while (Text[End] != '\0') {
    ++End;
  }
  while ((End > Start) && IsSpace (Text[End - 1U])) {
    --End;
  }

  if (Start != 0) {
    for (Index = 0; Index < End - Start; ++Index) {
      Text[Index] = Text[Start + Index];
    }
  }
  Text[End - Start] = '\0';
}

static int
HexDigit (
  char Character
  )
{
  if ((Character >= '0') && (Character <= '9')) {
    return Character - '0';
  }
  if ((Character >= 'a') && (Character <= 'f')) {
    return Character - 'a' + 10;
  }
  if ((Character >= 'A') && (Character <= 'F')) {
    return Character - 'A' + 10;
  }
  return -1;
}

static int
ParseHex16 (
  const char *Text,
  NCV_U16    *Value
  )
{
  NCV_SIZE Index;
  NCV_U16  Result;

  if ((Text == 0) || (Value == 0)) {
    return 0;
  }
  for (Index = 0; Index < 4; ++Index) {
    if (Text[Index] == '\0') {
      return 0;
    }
  }
  if (Text[4] != '\0') {
    return 0;
  }

  Result = 0;
  for (Index = 0; Index < 4; ++Index) {
    int Digit;

    Digit = HexDigit (Text[Index]);
    if (Digit < 0) {
      return 0;
    }
    Result = (NCV_U16)((Result << 4) | (NCV_U16)Digit);
  }
  *Value = Result;
  return 1;
}

static int
ParsePciAddress (
  const char      *Text,
  NCV_PCI_ADDRESS *Address
  )
{
  NCV_U16 Segment;
  NCV_U16 Bus;
  NCV_U16 Device;
  int     Function;
  int     Digit;
  NCV_SIZE Index;

  if ((Text == 0) || (Address == 0)) {
    return 0;
  }
  for (Index = 0; Index < 12; ++Index) {
    if (Text[Index] == '\0') {
      return 0;
    }
  }
  if ((Text[12] != '\0') || (Text[4] != ':') ||
      (Text[7] != ':') || (Text[10] != '.'))
  {
    return 0;
  }

  Segment = 0;
  for (Index = 0; Index < 4; ++Index) {
    Digit = HexDigit (Text[Index]);
    if (Digit < 0) {
      return 0;
    }
    Segment = (NCV_U16)((Segment << 4) | (NCV_U16)Digit);
  }
  Digit = HexDigit (Text[5]);
  if (Digit < 0) {
    return 0;
  }
  Bus = (NCV_U16)(Digit << 4);
  Digit = HexDigit (Text[6]);
  if (Digit < 0) {
    return 0;
  }
  Bus = (NCV_U16)(Bus | (NCV_U16)Digit);

  Digit = HexDigit (Text[8]);
  if (Digit < 0) {
    return 0;
  }
  Device = (NCV_U16)(Digit << 4);
  Digit = HexDigit (Text[9]);
  if (Digit < 0) {
    return 0;
  }
  Device = (NCV_U16)(Device | (NCV_U16)Digit);
  Function = HexDigit (Text[11]);
  if ((Function < 0) || (Device > 31U) || (Function > 7)) {
    return 0;
  }

  Address->Segment = Segment;
  Address->Bus = (NCV_U8)Bus;
  Address->Device = (NCV_U8)Device;
  Address->Function = (NCV_U8)Function;
  return 1;
}

static NCV_CONFIG_RESULT
SetSection (
  const char         *Text,
  NCV_CONFIG_SECTION *Section
  )
{
  if ((Text == 0) || (Section == 0)) {
    return NcvConfigInvalidParameter;
  }
  if (AsciiEqual (Text, "[Profile]")) {
    *Section = NcvSectionProfile;
  } else if (AsciiEqual (Text, "[Video]")) {
    *Section = NcvSectionVideo;
  } else if (AsciiEqual (Text, "[Endpoint]")) {
    *Section = NcvSectionEndpoint;
  } else if (AsciiEqual (Text, "[Boot]")) {
    *Section = NcvSectionBoot;
  } else if (AsciiEqual (Text, "[Marker]")) {
    *Section = NcvSectionMarker;
  } else if (AsciiEqual (Text, "[Behavior]")) {
    *Section = NcvSectionBehavior;
  } else {
    return NcvConfigUnsupported;
  }
  return NcvConfigSuccess;
}

static NCV_CONFIG_RESULT
SetKeyValue (
  NCV_CONFIG_CORE    *Config,
  NCV_CONFIG_SECTION Section,
  const char         *Key,
  const char         *Value
  )
{
  NCV_BOOL BooleanValue;

  if ((Config == 0) || (Key == 0) || (Value == 0)) {
    return NcvConfigInvalidParameter;
  }

  if (Section == NcvSectionMarker) {
    return MarkerConfigSetValue (&Config->Marker, Key, Value) ? NcvConfigSuccess : NcvConfigCompromisedData;
  }
  if (Section == NcvSectionProfile) {
    if (!AsciiEqual (Key, "Name") || Config->HasName ||
        !CopyAscii (Config->Name, NCV_CONFIG_TEXT_BYTES, Value))
    {
      return NcvConfigCompromisedData;
    }
    Config->HasName = 1;
    return NcvConfigSuccess;
  }

  if (Section == NcvSectionVideo) {
    if (AsciiEqual (Key, "TargetPci")) {
      if (Config->HasTargetPci ||
          !ParsePciAddress (Value, &Config->TargetPci))
      {
        return NcvConfigCompromisedData;
      }
      Config->HasTargetPci = 1;
      return NcvConfigSuccess;
    }
    if (AsciiEqual (Key, "ExpectedVendor")) {
      if (Config->HasExpectedVendor ||
          !ParseHex16 (Value, &Config->ExpectedVendor))
      {
        return NcvConfigCompromisedData;
      }
      Config->HasExpectedVendor = 1;
      return NcvConfigSuccess;
    }
    if (AsciiEqual (Key, "ExpectedDevice")) {
      if (Config->HasExpectedDevice ||
          !ParseHex16 (Value, &Config->ExpectedDevice))
      {
        return NcvConfigCompromisedData;
      }
      Config->HasExpectedDevice = 1;
      return NcvConfigSuccess;
    }
    if (AsciiEqual (Key, "ExpectedSubsystemVendor")) {
      if (Config->HasExpectedSubsystemVendor ||
          !ParseHex16 (Value, &Config->ExpectedSubsystemVendor))
      {
        return NcvConfigCompromisedData;
      }
      Config->HasExpectedSubsystemVendor = 1;
      return NcvConfigSuccess;
    }
    if (AsciiEqual (Key, "ExpectedSubsystemDevice")) {
      if (Config->HasExpectedSubsystemDevice ||
          !ParseHex16 (Value, &Config->ExpectedSubsystemDevice))
      {
        return NcvConfigCompromisedData;
      }
      Config->HasExpectedSubsystemDevice = 1;
      return NcvConfigSuccess;
    }
    return NcvConfigUnsupported;
  }

  if (Section == NcvSectionBoot) {
    if (AsciiEqual (Key, "TargetControllerPci")) {
      if (Config->HasTargetControllerPci ||
          !ParsePciAddress (Value, &Config->TargetControllerPci))
      {
        return NcvConfigCompromisedData;
      }
      Config->HasTargetControllerPci = 1;
      return NcvConfigSuccess;
    }
    if (AsciiEqual (Key, "LegacyOptionDescription")) {
      if (Config->HasLegacyOptionDescription ||
          !CopyAscii (
             Config->LegacyOptionDescription,
             NCV_CONFIG_TEXT_BYTES,
             Value
             ))
      {
        return NcvConfigCompromisedData;
      }
      Config->HasLegacyOptionDescription = 1;
      return NcvConfigSuccess;
    }
    if (AsciiEqual (Key, "ExcludeDescription")) {
      if (Config->HasExcludeDescription ||
          !CopyAscii (
             Config->ExcludeDescription,
             NCV_CONFIG_TEXT_BYTES,
             Value
             ))
      {
        return NcvConfigCompromisedData;
      }
      Config->HasExcludeDescription = 1;
      return NcvConfigSuccess;
    }
    if (AsciiEqual (Key, "TargetBbsDescription")) {
      if (Config->HasTargetBbsDescription ||
          !CopyDiskDescription (
             Config->TargetBbsDescription,
             NCV_CONFIG_TEXT_BYTES,
             Value
             ))
      {
        return NcvConfigCompromisedData;
      }
      Config->HasTargetBbsDescription = 1;
      return NcvConfigSuccess;
    }
    return NcvConfigUnsupported;
  }

  if (Section == NcvSectionEndpoint) {
    NCV_U8   *Policy;
    NCV_BOOL *Present;

    if (AsciiEqual (Key, "IoDecoding")) {
      Policy = &Config->IoDecoding;
      Present = &Config->HasIoDecoding;
    } else if (AsciiEqual (Key, "MemoryDecoding")) {
      Policy = &Config->MemoryDecoding;
      Present = &Config->HasMemoryDecoding;
    } else if (AsciiEqual (Key, "BusMastering")) {
      Policy = &Config->BusMastering;
      Present = &Config->HasBusMastering;
    } else {
      return NcvConfigUnsupported;
    }

    if (*Present) {
      return NcvConfigCompromisedData;
    }
    if (AsciiEqual (Value, "On")) {
      *Policy = NcvEndpointOn;
    } else if (AsciiEqual (Value, "Off")) {
      *Policy = NcvEndpointOff;
    } else if (AsciiEqual (Value, "Ignore")) {
      *Policy = NcvEndpointIgnore;
    } else {
      return NcvConfigCompromisedData;
    }
    *Present = 1;
    return NcvConfigSuccess;
  }

  if (Section == NcvSectionBehavior) {
    if (AsciiEqual (Value, "true")) {
      BooleanValue = 1;
    } else if (AsciiEqual (Value, "false")) {
      BooleanValue = 0;
    } else {
      return NcvConfigCompromisedData;
    }

    if (AsciiEqual (Key, "Probe") && !Config->HasProbe) {
      Config->Probe = BooleanValue;
      Config->HasProbe = 1;
      return NcvConfigSuccess;
    }
    if (AsciiEqual (Key, "VerboseLog") && !Config->HasVerboseLog) {
      Config->VerboseLog = BooleanValue;
      Config->HasVerboseLog = 1;
      return NcvConfigSuccess;
    }
    if (AsciiEqual (Key, "AutoBoot") && !Config->HasAutoBoot) {
      Config->AutoBoot = BooleanValue;
      Config->HasAutoBoot = 1;
      return NcvConfigSuccess;
    }

    return NcvConfigCompromisedData;
  }

  return NcvConfigCompromisedData;
}

void
NcvConfigDefaults (
  NCV_CONFIG_CORE *Config
  )
{
  if (Config == 0) {
    return;
  }

  ZeroBytes (Config, sizeof (*Config));
  MarkerStringCopy (Config->Marker.Header, "NATIVE CSM BOOT PROFILE 1");
  Config->Probe = 1;
  Config->VerboseLog = 1;
  Config->AutoBoot = 1;
  Config->RequireReferenceSnapshotMatch = 0;
  (void)CopyAscii (
          Config->LegacyOptionDescription,
          NCV_CONFIG_TEXT_BYTES,
          "Hard Drive"
          );
  (void)CopyAscii (
          Config->ExcludeDescription,
          NCV_CONFIG_TEXT_BYTES,
          "USB"
          );
}

NCV_CONFIG_RESULT
NcvParseConfigStrict (
  const NCV_U8    *Bytes,
  NCV_SIZE        ByteCount,
  NCV_CONFIG_CORE *Config
  )
{
  char               Line[NCV_CONFIG_MAX_LINE_BYTES];
  NCV_SIZE           LineLength;
  NCV_SIZE           Index;
  NCV_CONFIG_SECTION Section;
  NCV_BOOL           SectionSeen[NcvSectionMaximum];

  if ((Bytes == 0) || (ByteCount == 0) || (Config == 0)) {
    return NcvConfigInvalidParameter;
  }

  ZeroBytes (SectionSeen, sizeof (SectionSeen));
  Section = NcvSectionNone;
  LineLength = 0;
  for (Index = 0; Index <= ByteCount; ++Index) {
    NCV_U8 Byte;

    Byte = (Index == ByteCount) ? (NCV_U8)'\n' : Bytes[Index];
    if (Byte == '\r') {
      continue;
    }
    if (Byte != '\n') {
      if ((Byte < 0x20U) || (Byte > 0x7eU) ||
          (LineLength + 1U >= sizeof (Line)))
      {
        return NcvConfigCompromisedData;
      }
      Line[LineLength++] = (char)Byte;
      continue;
    }

    Line[LineLength] = '\0';
    Trim (Line);
    if ((Line[0] != '\0') && (Line[0] != '#') && (Line[0] != ';')) {
      if (Line[0] == '[') {
        NCV_CONFIG_RESULT Result;

        Result = SetSection (Line, &Section);
        if (Result != NcvConfigSuccess) {
          return Result;
        }
        if (SectionSeen[Section]) {
          return NcvConfigCompromisedData;
        }
        SectionSeen[Section] = 1;
      } else {
        char              *Equals;
        char              *Scan;
        NCV_CONFIG_RESULT Result;

        if (Section == NcvSectionNone) {
          return NcvConfigCompromisedData;
        }
        Equals = 0;
        for (Scan = Line; *Scan != '\0'; ++Scan) {
          if (*Scan == '=') {
            if (Equals != 0) {
              return NcvConfigCompromisedData;
            }
            Equals = Scan;
          }
        }
        if (Equals == 0) {
          return NcvConfigCompromisedData;
        }
        *Equals = '\0';
        Trim (Line);
        Trim (Equals + 1);
        if ((Line[0] == '\0') || ((Equals + 1)[0] == '\0')) {
          return NcvConfigCompromisedData;
        }
        Result = SetKeyValue (Config, Section, Line, Equals + 1);
        if (Result != NcvConfigSuccess) {
          return Result;
        }
      }
    }
    LineLength = 0;
  }

  if (!MarkerConfigIsValid (&Config->Marker)) { return NcvConfigCompromisedData; }
  if (!Config->Probe && Config->HasTargetPci &&
      Config->HasTargetControllerPci &&
      AddressEqual (&Config->TargetPci, &Config->TargetControllerPci))
  {
    return NcvConfigCompromisedData;
  }
  return NcvConfigSuccess;
}

void
NcvConfigCoreGetBootRequirements (
  const NCV_CONFIG_CORE *Config,
  NCV_BOOT_REQUIREMENTS *Requirements
  )
{
  if (Requirements == 0) {
    return;
  }
  ZeroBytes (Requirements, sizeof (*Requirements));
  if (Config == 0) {
    return;
  }

  Requirements->Probe = Config->Probe;
  Requirements->HasTargetPci = Config->HasTargetPci;
  Requirements->HasTargetControllerPci = Config->HasTargetControllerPci;
  Requirements->HasIoDecoding = Config->HasIoDecoding;
  Requirements->HasMemoryDecoding = Config->HasMemoryDecoding;
  Requirements->HasBusMastering = Config->HasBusMastering;
}

NCV_U8
NcvBootRequirementsValidate (
  const NCV_BOOT_REQUIREMENTS *Requirements
  )
{
  NCV_U8 Result;

  if (Requirements == 0) {
    return 0xffU;
  }

  Result = 0;
  if (Requirements->Probe) {
    Result |= NCV_BOOT_INVALID_PROBE_MODE;
  }
  if (!Requirements->HasTargetPci) {
    Result |= NCV_BOOT_MISSING_TARGET_PCI;
  }
  if (!Requirements->HasTargetControllerPci) {
    Result |= NCV_BOOT_MISSING_CONTROLLER_PCI;
  }
  if (!Requirements->HasIoDecoding) {
    Result |= NCV_BOOT_MISSING_IO_POLICY;
  }
  if (!Requirements->HasMemoryDecoding) {
    Result |= NCV_BOOT_MISSING_MEMORY_POLICY;
  }
  if (!Requirements->HasBusMastering) {
    Result |= NCV_BOOT_MISSING_BUS_MASTER_POLICY;
  }
  return Result;
}

NCV_OUTPUT_KIND
NcvChooseOutputKind (
  NCV_BOOL FirstRun,
  NCV_SIZE SelectedDisplay,
  NCV_SIZE SelectedStorage
  )
{
  if (FirstRun &&
      (SelectedDisplay != NCV_NO_SELECTION) &&
      (SelectedStorage != NCV_NO_SELECTION))
  {
    return NcvOutputActiveIni;
  }
  return NcvOutputProbeIni;
}

const char *
NcvOutputFileNameAscii (
  NCV_OUTPUT_KIND Kind
  )
{
  return (Kind == NcvOutputActiveIni) ?
         "Config.ini" : "Probe.ini";
}

/* Recovery accepts only an explicit diagnostic mode or a complete boot config. */
NCV_CONFIG_RESULT
NcvValidateConfigMode (const NCV_CONFIG_CORE *Config, NCV_U8 *Missing)
{
  NCV_BOOT_REQUIREMENTS Requirements;
  if (Config == 0 || Missing == 0) { return NcvConfigInvalidParameter; }
  *Missing = 0;
  if (!Config->HasProbe) { return NcvConfigCompromisedData; }
  if (Config->Probe) { return NcvConfigSuccess; }
  NcvConfigCoreGetBootRequirements (Config, &Requirements);
  *Missing = NcvBootRequirementsValidate (&Requirements);
  return *Missing ? NcvConfigCompromisedData : NcvConfigSuccess;
}

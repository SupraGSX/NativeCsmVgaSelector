#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CandidateIni.h"

static NCV_PCI_ADDRESS
Address (
  NCV_U16 Segment,
  NCV_U8  Bus,
  NCV_U8  Device,
  NCV_U8  Function
  )
{
  NCV_PCI_ADDRESS Result;

  Result.Segment = Segment;
  Result.Bus = Bus;
  Result.Device = Device;
  Result.Function = Function;
  return Result;
}

static NCV_DISPLAY_CANDIDATE
Display (
  NCV_PCI_ADDRESS AddressValue,
  NCV_U16         Vendor,
  NCV_U16         DeviceId,
  NCV_U16         SubsystemVendor,
  NCV_U16         SubsystemDevice,
  int             Gop,
  int             Active,
  int             Rom,
  int             Path
  )
{
  NCV_DISPLAY_CANDIDATE Result;

  memset (&Result, 0, sizeof (Result));
  Result.Address = AddressValue;
  Result.RawSegment = AddressValue.Segment;
  Result.RawBus = AddressValue.Bus;
  Result.RawDevice = AddressValue.Device;
  Result.RawFunction = AddressValue.Function;
  Result.Vendor = Vendor;
  Result.DeviceId = DeviceId;
  Result.SubsystemVendor = SubsystemVendor;
  Result.SubsystemDevice = SubsystemDevice;
  Result.AddressValid = 1;
  Result.GopAssociated = (NCV_BOOL)Gop;
  Result.GopAssociationKnown = 1;
  Result.ActiveLegacyVgaOwner = (NCV_BOOL)Active;
  Result.ActiveLegacyVgaOwnerKnown = 1;
  Result.OptionRomExposed = (NCV_BOOL)Rom;
  Result.AcceptedLegacyRom = (NCV_BOOL)Rom;
  Result.PathDiscovered = (NCV_BOOL)Path;
  Result.PathValidated = (NCV_BOOL)Path;
  return Result;
}

static NCV_STORAGE_CANDIDATE
Storage (
  NCV_SIZE        BbsIndex,
  NCV_PCI_ADDRESS AddressValue,
  NCV_U8          BaseClass,
  NCV_U8          SubClass,
  const char      *Manufacturer,
  const char      *Description,
  int             Usb
  )
{
  NCV_STORAGE_CANDIDATE Result;

  memset (&Result, 0, sizeof (Result));
  Result.BbsIndex = BbsIndex;
  Result.Address = AddressValue;
  Result.RawBus = AddressValue.Bus;
  Result.RawDevice = AddressValue.Device;
  Result.RawFunction = AddressValue.Function;
  Result.BaseClass = BaseClass;
  Result.SubClass = SubClass;
  Result.AddressValid = 1;
  Result.Usb = (NCV_BOOL)Usb;
  assert (snprintf (
            Result.Manufacturer,
            sizeof (Result.Manufacturer),
            "%s",
            Manufacturer
            ) < (int)sizeof (Result.Manufacturer));
  assert (snprintf (
            Result.Description,
            sizeof (Result.Description),
            "%s",
            Description
            ) < (int)sizeof (Result.Description));
  Result.Eligible = (NCV_BOOL)NcvIniValueIsExactlyRepresentable (
                                Result.Description,
                                sizeof (Result.Description)
                                );
  return Result;
}

static size_t
CountActiveKey (
  const char *Text,
  const char *Key
  )
{
  size_t Count;
  size_t KeyLength;
  const char *Line;

  Count = 0;
  KeyLength = strlen (Key);
  Line = Text;
  while (*Line != '\0') {
    if (strncmp (Line, Key, KeyLength) == 0) {
      ++Count;
    }
    Line = strchr (Line, '\n');
    if (Line == NULL) {
      break;
    }
    ++Line;
  }
  return Count;
}

static size_t
CountSubstring (
  const char *Text,
  const char *Needle
  )
{
  size_t Count;
  size_t Length;

  Count = 0;
  Length = strlen (Needle);
  while ((Text = strstr (Text, Needle)) != NULL) {
    ++Count;
    Text += Length;
  }
  return Count;
}

static int
AddressEquals (
  const NCV_PCI_ADDRESS *Left,
  const NCV_PCI_ADDRESS *Right
  )
{
  return (Left->Segment == Right->Segment) &&
         (Left->Bus == Right->Bus) &&
         (Left->Device == Right->Device) &&
         (Left->Function == Right->Function);
}

static void
ParseAndRequireBoot (
  const char      *Text,
  NCV_SIZE        TextSize,
  NCV_CONFIG_CORE *Parsed
  )
{
  NCV_BOOT_REQUIREMENTS Requirements;

  NcvConfigDefaults (Parsed);
  assert (
    NcvParseConfigStrict (
      (const NCV_U8 *)Text,
      TextSize,
      Parsed
      ) == NcvConfigSuccess
    );
  NcvConfigCoreGetBootRequirements (Parsed, &Requirements);
  assert (NcvBootRequirementsValidate (&Requirements) == 0);
}

static void
BuildCapturedFixture (
  NCV_DISPLAY_COLLECTION *Displays,
  NCV_STORAGE_COLLECTION *StorageItems
  )
{
  NCV_DISPLAY_CANDIDATE Video;
  NCV_STORAGE_CANDIDATE Disk;

  memset (Displays, 0, sizeof (*Displays));
  memset (StorageItems, 0, sizeof (*StorageItems));

  Video = Display (
            Address (0, 1, 0, 0),
            0x1234,
            0x1000,
            0x1462,
            0x5104,
            1,
            1,
            1,
            1
            );
  assert (NcvDisplayCollectionAppend (Displays, &Video) == 0);
  Video = Display (
            Address (0, 9, 0, 0),
            0x5678,
            0x0005,
            0x5678,
            0x0032,
            0,
            0,
            1,
            1
            );
  assert (NcvDisplayCollectionAppend (Displays, &Video) == 0);

  Disk = Storage (
           25,
           Address (0, 2, 0, 0),
           0x01,
           0x08,
           "NVME Storage",
           "EXAMPLE STORAGE DEVICE",
           0
           );
  assert (NcvStorageCollectionAppend (StorageItems, &Disk) == 0);
  Disk = Storage (
           26,
           Address (0, 5, 0, 0),
           0x01,
           0x08,
           "NVME Storage",
           "WDC WDS512G1X0C-00ENX0",
           0
           );
  assert (NcvStorageCollectionAppend (StorageItems, &Disk) == 0);
  Disk = Storage (
           27,
           Address (0, 0x12, 0, 0),
           0x0c,
           0x03,
           "USB Storage",
           "General UDisk 5.00",
           1
           );
  assert (NcvStorageCollectionAppend (StorageItems, &Disk) == 0);
  Disk = Storage (
           28,
           Address (0, 0x12, 0, 0),
           0x0c,
           0x03,
           "USB Storage",
           "SanDisk Extreme Pro 0",
           1
           );
  assert (NcvStorageCollectionAppend (StorageItems, &Disk) == 0);
}

static void
TestCapturedFixture (
  const char *OutputPath
  )
{
  NCV_DISPLAY_COLLECTION Displays;
  NCV_STORAGE_COLLECTION StorageItems;
  NCV_SIZE SelectedDisplay;
  NCV_SIZE SelectedStorage;
  NCV_SIZE TextSize;
  char *Text;
  NCV_CONFIG_CORE Parsed;
  FILE *Output;

  BuildCapturedFixture (&Displays, &StorageItems);
  assert (Displays.Count == 2);
  assert (Displays.ObservedCount == 2);
  assert (StorageItems.Count == 4);
  assert (StorageItems.ObservedCount == 4);
  assert (
    NcvRankDisplays (
      Displays.Items,
      Displays.Count,
      &SelectedDisplay
      ) == 0
    );
  assert (
    NcvRankStorage (
      StorageItems.Items,
      StorageItems.Count,
      &SelectedStorage
      ) == 0
    );
  assert (SelectedDisplay == 1);
  assert (
    AddressEquals (
      &Displays.Items[SelectedDisplay].Address,
      &(NCV_PCI_ADDRESS){ 0, 9, 0, 0 }
      )
    );
  assert (SelectedStorage == 0);
  assert (
    AddressEquals (
      &StorageItems.Items[SelectedStorage].Address,
      &(NCV_PCI_ADDRESS){ 0, 2, 0, 0 }
      )
    );
  assert (
    strcmp (
      StorageItems.Items[SelectedStorage].Description,
      "EXAMPLE STORAGE DEVICE"
      ) == 0
    );

  Text = calloc (1, NCV_CANDIDATE_INI_BYTES);
  assert (Text != NULL);
  TextSize = 0;
  assert (
    NcvFormatCandidateIni (
      Displays.Items,
      Displays.Count,
      SelectedDisplay,
      StorageItems.Items,
      StorageItems.Count,
      SelectedStorage,
      Text,
      NCV_CANDIDATE_INI_BYTES,
      &TextSize
      ) == 0
    );
  assert (CountSubstring (Text, "# Display candidate ") == 2);
  assert (CountSubstring (Text, "# Storage candidate ") == 4);
  assert (CountActiveKey (Text, "TargetPci=") == 1);
  assert (CountActiveKey (Text, "TargetControllerPci=") == 1);
  assert (CountActiveKey (Text, "TargetBbsDescription=") == 1);
  assert (strstr (Text, "TargetPci=0000:09:00.0\r\n") != NULL);
  assert (strstr (Text, "# TargetPci=0000:01:00.0\r\n") != NULL);
  assert (
    strstr (Text, "TargetControllerPci=0000:02:00.0\r\n") != NULL
    );
  assert (
    strstr (Text, "TargetBbsDescription=EXAMPLE STORAGE DEVICE\r\n") != NULL
    );
  assert (
    strstr (Text, "# TargetControllerPci=0000:05:00.0\r\n") != NULL
    );
  assert (
    strstr (
      Text,
      "# TargetBbsDescription=WDC WDS512G1X0C-00ENX0\r\n"
      ) != NULL
    );
  assert (
    CountSubstring (Text, "# TargetControllerPci=0000:12:00.0\r\n") == 2
    );
  assert (
    strstr (
      Text,
      "# TargetBbsDescription=General UDisk 5.00\r\n"
      ) != NULL
    );
  assert (
    strstr (
      Text,
      "# TargetBbsDescription=SanDisk Extreme Pro 0\r\n"
      ) != NULL
    );

  ParseAndRequireBoot (Text, TextSize, &Parsed);
  assert (!Parsed.Probe);
  assert (
    AddressEquals (
      &Parsed.TargetPci,
      &Displays.Items[SelectedDisplay].Address
      )
    );
  assert (
    AddressEquals (
      &Parsed.TargetControllerPci,
      &StorageItems.Items[SelectedStorage].Address
      )
    );
  assert (Parsed.IoDecoding == NcvEndpointOn);
  assert (Parsed.MemoryDecoding == NcvEndpointOn);
  assert (Parsed.BusMastering == NcvEndpointIgnore);
  assert (Parsed.HasTargetBbsDescription);
  assert (
    strcmp (Parsed.TargetBbsDescription, "EXAMPLE STORAGE DEVICE") == 0
    );
  printf (
    "captured fixture: displays=%llu storage=%llu selected-display="
    "%04x:%02x:%02x.%x selected-storage=%04x:%02x:%02x.%x\n",
    (unsigned long long)Displays.Count,
    (unsigned long long)StorageItems.Count,
    (unsigned)Parsed.TargetPci.Segment,
    (unsigned)Parsed.TargetPci.Bus,
    (unsigned)Parsed.TargetPci.Device,
    (unsigned)Parsed.TargetPci.Function,
    (unsigned)Parsed.TargetControllerPci.Segment,
    (unsigned)Parsed.TargetControllerPci.Bus,
    (unsigned)Parsed.TargetControllerPci.Device,
    (unsigned)Parsed.TargetControllerPci.Function
    );
  printf (
    "production strict reparse: Probe=%s IoDecoding=%u MemoryDecoding=%u "
    "BusMastering=%u TargetBbsDescription=%s BootValidation=PASS\n",
    Parsed.Probe ? "true" : "false",
    (unsigned)Parsed.IoDecoding,
    (unsigned)Parsed.MemoryDecoding,
    (unsigned)Parsed.BusMastering,
    Parsed.TargetBbsDescription
    );

  Output = fopen (OutputPath, "wb");
  assert (Output != NULL);
  assert (fwrite (Text, 1, (size_t)TextSize, Output) == (size_t)TextSize);
  assert (fclose (Output) == 0);
  free (Text);
}

static void
TestDisplayRankingCases (void)
{
  NCV_DISPLAY_CANDIDATE Items[4];
  NCV_SIZE Selected;
  size_t Index;

  Items[0] = Display (
               Address (0, 7, 0, 0),
               1,
               1,
               1,
               1,
               0,
               0,
               1,
               1
               );
  assert (NcvRankDisplays (Items, 1, &Selected) == 0);
  assert (Selected == 0);

  Items[0] = Display (
               Address (0, 1, 0, 0),
               1,
               1,
               1,
               1,
               1,
               1,
               1,
               1
               );
  Items[1] = Display (
               Address (0, 9, 0, 0),
               1,
               1,
               1,
               1,
               0,
               0,
               1,
               1
               );
  Items[2] = Display (
               Address (0, 8, 0, 0),
               1,
               1,
               1,
               1,
               1,
               0,
               1,
               1
               );
  Items[3] = Display (
               Address (0, 4, 0, 0),
               1,
               1,
               1,
               1,
               0,
               0,
               1,
               1
               );
  assert (NcvRankDisplays (Items, 4, &Selected) == 0);
  assert (Selected == 3);
  assert (!Items[0].Eligible); /* The current active VGA cannot be a switch target. */
  for (Index = 0; Index < 4; ++Index) {
    assert (!!Items[Index].Selected == (Index == 3));
  }

  for (Index = 0; Index < 4; ++Index) {
    Items[Index].AcceptedLegacyRom = 0;
  }
  assert (NcvRankDisplays (Items, 4, &Selected) == 0);
  assert (Selected == NCV_NO_SELECTION);

  Items[0] = Display (
               Address (0, 1, 0, 0),
               1,
               1,
               1,
               1,
               0,
               0,
               1,
               1
               );
  Items[1] = Display (
               Address (0, 0, 0, 0),
               1,
               1,
               1,
               1,
               0,
               0,
               1,
               1
               );
  Items[1].ActiveLegacyVgaOwnerKnown = 0;
  assert (NcvRankDisplays (Items, 2, &Selected) == 0);
  assert (Selected == 0);

  Items[1].ActiveLegacyVgaOwnerKnown = 1;
  Items[1].GopAssociationKnown = 0;
  Items[0].GopAssociated = 0;
  assert (NcvRankDisplays (Items, 2, &Selected) == 0);
  assert (Selected == 0);

  Items[0].PathDiscovered = 1;
  Items[0].PathValidated = 0;
  assert (NcvRankDisplays (Items, 1, &Selected) == 0);
  assert (Selected == NCV_NO_SELECTION);
  Items[0].PathDiscovered = 0;
  Items[0].PathValidated = 1;
  assert (NcvRankDisplays (Items, 1, &Selected) == 0);
  assert (Selected == NCV_NO_SELECTION);
}

static void
TestStorageRankingCases (void)
{
  NCV_STORAGE_CANDIDATE Items[4];
  NCV_SIZE Selected;

  Items[0] = Storage (
               9,
               Address (0, 5, 0, 0),
               1,
               8,
               "NVME",
               "Only disk",
               0
               );
  assert (NcvRankStorage (Items, 1, &Selected) == 0);
  assert (Selected == 0);

  Items[0] = Storage (
               30,
               Address (0, 5, 0, 0),
               1,
               8,
               "NVME",
               "Later non USB",
               0
               );
  Items[1] = Storage (
               20,
               Address (0, 2, 0, 0),
               1,
               8,
               "NVME",
               "Earlier non USB",
               0
               );
  Items[2] = Storage (
               5,
               Address (0, 0x12, 0, 0),
               0x0c,
               3,
               "USB",
               "USB one",
               1
               );
  Items[3] = Storage (
               6,
               Address (0, 0x12, 0, 0),
               0x0c,
               3,
               "USB",
               "USB two",
               1
               );
  assert (NcvRankStorage (Items, 4, &Selected) == 0);
  assert (Selected == 1);
  Items[0].Eligible = 0;
  Items[1].Eligible = 0;
  assert (NcvRankStorage (Items, 4, &Selected) == 0);
  assert (Selected == 2);
  Items[2].Eligible = 0;
  Items[3].Eligible = 0;
  assert (NcvRankStorage (Items, 4, &Selected) == 0);
  assert (Selected == NCV_NO_SELECTION);
  Items[0] = Items[1];
  Items[0].Eligible = Items[1].Eligible = 1;
  Items[0].AddressValid = Items[1].AddressValid = 1;
  strcpy (Items[0].Description, "Same Disk");
  strcpy (Items[1].Description, "same disk");
  assert (NcvRankStorage (Items, 2, &Selected) == 0);
  assert (Selected == NCV_NO_SELECTION && Items[0].IdentityAmbiguous && Items[1].IdentityAmbiguous);

}

static void
TestCollectionBoundaryAndOverflow (void)
{
  NCV_DISPLAY_COLLECTION Displays;
  NCV_STORAGE_COLLECTION StorageItems;
  NCV_DISPLAY_CANDIDATE Video;
  NCV_STORAGE_CANDIDATE Disk;
  size_t Index;

  memset (&Displays, 0, sizeof (Displays));
  memset (&StorageItems, 0, sizeof (StorageItems));
  Video = Display (
            Address (0, 1, 0, 0),
            1,
            1,
            1,
            1,
            0,
            0,
            1,
            1
            );
  for (Index = 0; Index < NCV_MAX_DISPLAY_CANDIDATES; ++Index) {
    assert (NcvDisplayCollectionAppend (&Displays, &Video) == 0);
  }
  assert (Displays.Count == NCV_MAX_DISPLAY_CANDIDATES);
  assert (!Displays.Overflow);
  assert (NcvDisplayCollectionAppend (&Displays, &Video) == 1);
  assert (Displays.Count == NCV_MAX_DISPLAY_CANDIDATES);
  assert (Displays.ObservedCount == NCV_MAX_DISPLAY_CANDIDATES + 1U);
  assert (Displays.Overflow);

  Disk = Storage (
           1,
           Address (0, 2, 0, 0),
           1,
           8,
           "Disk",
           "Disk",
           0
           );
  for (Index = 0; Index < NCV_MAX_STORAGE_CANDIDATES; ++Index) {
    assert (NcvStorageCollectionAppend (&StorageItems, &Disk) == 0);
  }
  assert (StorageItems.Count == NCV_MAX_STORAGE_CANDIDATES);
  assert (!StorageItems.Overflow);
  assert (NcvStorageCollectionAppend (&StorageItems, &Disk) == 1);
  assert (StorageItems.Count == NCV_MAX_STORAGE_CANDIDATES);
  assert (StorageItems.ObservedCount == NCV_MAX_STORAGE_CANDIDATES + 1U);
  assert (StorageItems.Overflow);
}

static void
TestInvalidDescriptionAndZeroSelections (void)
{
  NCV_DISPLAY_CANDIDATE Displays[1];
  NCV_STORAGE_CANDIDATE StorageItems[1];
  NCV_SIZE SelectedDisplay;
  NCV_SIZE SelectedStorage;
  NCV_SIZE TextSize;
  char *Text;
  NCV_CONFIG_CORE Parsed;
  NCV_BOOT_REQUIREMENTS Requirements;

  Displays[0] = Display (
                  Address (0, 9, 0, 0),
                  1,
                  1,
                  1,
                  1,
                  0,
                  0,
                  0,
                  1
                  );
  StorageItems[0] = Storage (
                      1,
                      Address (0, 2, 0, 0),
                      1,
                      8,
                      "NVME",
                      "Disk with\nnewline",
                      0
                      );
  assert (
    strcmp (StorageItems[0].Description, "Disk with\nnewline") == 0
    );
  assert (!StorageItems[0].Eligible);
  assert (NcvRankDisplays (Displays, 1, &SelectedDisplay) == 0);
  assert (SelectedDisplay == NCV_NO_SELECTION);
  assert (NcvRankStorage (StorageItems, 1, &SelectedStorage) == 0);
  assert (SelectedStorage == NCV_NO_SELECTION);

  Text = calloc (1, NCV_CANDIDATE_INI_BYTES);
  assert (Text != NULL);
  TextSize = 0;
  assert (
    NcvFormatCandidateIni (
      Displays,
      1,
      SelectedDisplay,
      StorageItems,
      1,
      SelectedStorage,
      Text,
      NCV_CANDIDATE_INI_BYTES,
      &TextSize
      ) == 0
    );
  assert (CountActiveKey (Text, "TargetPci=") == 0);
  assert (CountActiveKey (Text, "TargetControllerPci=") == 0);
  assert (CountActiveKey (Text, "TargetBbsDescription=") == 0);
  NcvConfigDefaults (&Parsed);
  assert (
    NcvParseConfigStrict (
      (const NCV_U8 *)Text,
      TextSize,
      &Parsed
      ) == NcvConfigSuccess
    );
  NcvConfigCoreGetBootRequirements (&Parsed, &Requirements);
  assert (NcvBootRequirementsValidate (&Requirements) != 0);
  free (Text);
}

static void
TestPartialSelectionsStayProbeOnly (void)
{
  NCV_DISPLAY_CANDIDATE Displays[1];
  NCV_STORAGE_CANDIDATE StorageItems[1];
  NCV_SIZE SelectedDisplay;
  NCV_SIZE SelectedStorage;
  NCV_SIZE TextSize;
  char *Text;

  Displays[0] = Display (
                  Address (0, 9, 0, 0),
                  0x5678,
                  0x0005,
                  0x5678,
                  0x0032,
                  0,
                  0,
                  1,
                  1
                  );
  StorageItems[0] = Storage (
                      25,
                      Address (0, 2, 0, 0),
                      1,
                      8,
                      "NVME Storage",
                      "EXAMPLE STORAGE DEVICE",
                      0
                      );
  assert (NcvRankDisplays (Displays, 1, &SelectedDisplay) == 0);
  assert (NcvRankStorage (StorageItems, 1, &SelectedStorage) == 0);
  assert (SelectedDisplay == 0);
  assert (SelectedStorage == 0);

  Text = calloc (1, NCV_CANDIDATE_INI_BYTES);
  assert (Text != NULL);
  StorageItems[0].Eligible = 0;
  StorageItems[0].Selected = 0;
  SelectedStorage = NCV_NO_SELECTION;
  TextSize = 0;
  assert (
    NcvFormatCandidateIni (
      Displays,
      1,
      SelectedDisplay,
      StorageItems,
      1,
      SelectedStorage,
      Text,
      NCV_CANDIDATE_INI_BYTES,
      &TextSize
      ) == 0
    );
  assert (CountActiveKey (Text, "TargetPci=") == 0);
  assert (CountActiveKey (Text, "TargetControllerPci=") == 0);
  assert (CountActiveKey (Text, "TargetBbsDescription=") == 0);

  Displays[0].AddressValid = 0;
  assert (NcvRankDisplays (Displays, 1, &SelectedDisplay) == 0);
  assert (SelectedDisplay == NCV_NO_SELECTION);
  StorageItems[0].Eligible = 1;
  StorageItems[0].Selected = 1;
  SelectedStorage = 0;
  memset (Text, 0, NCV_CANDIDATE_INI_BYTES);
  TextSize = 0;
  assert (
    NcvFormatCandidateIni (
      Displays,
      1,
      SelectedDisplay,
      StorageItems,
      1,
      SelectedStorage,
      Text,
      NCV_CANDIDATE_INI_BYTES,
      &TextSize
      ) == 0
    );
  assert (CountActiveKey (Text, "TargetPci=") == 0);
  assert (CountActiveKey (Text, "TargetControllerPci=") == 0);
  assert (CountActiveKey (Text, "TargetBbsDescription=") == 0);
  assert (strstr (Text, "PCI location unavailable") != NULL);
  free (Text);
}

static void
TestStrictParserCommentsAndFilenames (void)
{
  static const char Text[] =
    "# TargetPci=0000:01:00.0\r\n"
    "; TargetControllerPci=0000:05:00.0\r\n"
    "[Behavior]\r\n"
    "Probe=false\r\n"
    "[Video]\r\n"
    "TargetPci=0000:09:00.0\r\n"
    "[Endpoint]\r\n"
    "IoDecoding=On\r\n"
    "MemoryDecoding=On\r\n"
    "BusMastering=Ignore\r\n"
    "[Boot]\r\n"
    "TargetControllerPci=0000:02:00.0\r\n"
    "TargetBbsDescription=EXAMPLE STORAGE DEVICE\r\n";
  NCV_CONFIG_CORE Parsed;
  NCV_BOOT_REQUIREMENTS Requirements;

  NcvConfigDefaults (&Parsed);
  assert (
    NcvParseConfigStrict (
      (const NCV_U8 *)Text,
      sizeof (Text) - 1U,
      &Parsed
      ) == NcvConfigSuccess
    );
  NcvConfigCoreGetBootRequirements (&Parsed, &Requirements);
  assert (NcvBootRequirementsValidate (&Requirements) == 0);
  assert (
    AddressEquals (
      &Parsed.TargetPci,
      &(NCV_PCI_ADDRESS){ 0, 9, 0, 0 }
      )
    );
  assert (
    strcmp (
      NcvOutputFileNameAscii (
        NcvChooseOutputKind (1, 0, 0)
        ),
      "Config.ini"
      ) == 0
    );
  assert (
    strcmp (
      NcvOutputFileNameAscii (
        NcvChooseOutputKind (0, 0, 0)
        ),
      "Probe.ini"
      ) == 0
    );
  assert (
    strcmp (
      NcvOutputFileNameAscii (
        NcvChooseOutputKind (1, NCV_NO_SELECTION, 0)
        ),
      "Probe.ini"
      ) == 0
    );
}

static void
TestPaddedDiskDescriptionRoundTrips (void)
{
  const char *Names[] = {
    "SanDisk Extreme Pro 0 ", "SanDisk Extreme Pro 0", "  USB disk  ",
    " \"USB\\disk\" ", "\"Quoted disk\"", "USB\\disk", "Disk ; # comments"
  };
  NCV_DISPLAY_CANDIDATE Gpu = Display (Address (0, 9, 0, 0), 1, 1, 1, 1, 0, 0, 1, 1);
  NCV_STORAGE_CANDIDATE Disk;
  NCV_CONFIG_CORE Parsed;
  NCV_SIZE SelectedGpu, SelectedDisk, Used;
  char Text[4096], Encoded[NCV_ENCODED_DESCRIPTION_BYTES], Maximum[65];
  size_t Index, Capacity;

  assert (NcvRankDisplays (&Gpu, 1, &SelectedGpu) == 0 && SelectedGpu == 0);
  for (Index = 0; Index < sizeof (Names) / sizeof (Names[0]); ++Index) {
    Disk = Storage (30, Address (0, 19, 0, 0), 0x0c, 3, "USB Storage", Names[Index], 1);
    assert (Disk.Eligible);
    assert (NcvRankStorage (&Disk, 1, &SelectedDisk) == 0 && SelectedDisk == 0);
    assert (NcvFormatCandidateIni (&Gpu, 1, SelectedGpu, &Disk, 1, SelectedDisk,
                                  Text, sizeof (Text), &Used) == 0);
    ParseAndRequireBoot (Text, Used, &Parsed);
    assert (strcmp (Parsed.TargetBbsDescription, Names[Index]) == 0);
    /* The matching identity is not normalized into another disk's name. */
    if (Index == 0) {
      assert (strstr (Text, "TargetBbsDescription=\"SanDisk Extreme Pro 0 \"\r\n"));
      assert (strcmp (Parsed.TargetBbsDescription, Names[1]) != 0);
    }
    assert (NcvEncodeDiskDescription (Names[Index], Encoded, sizeof (Encoded)) == 0);
    Used = strlen (Encoded);
    for (Capacity = 0; Capacity <= Used; ++Capacity) {
      assert (NcvEncodeDiskDescription (Names[Index], Encoded, Capacity) != 0);
    }
    assert (NcvEncodeDiskDescription (Names[Index], Encoded, Used + 1) == 0);
  }
  memset (Maximum, '"', 64); Maximum[64] = 0;
  assert (NcvEncodeDiskDescription (Maximum, Encoded, sizeof (Encoded)) == 0);
  assert (strlen (Encoded) == sizeof (Encoded) - 1);
  snprintf (Text, sizeof (Text), "[Boot]\nTargetBbsDescription=%s\n", Encoded);
  NcvConfigDefaults (&Parsed);
  assert (NcvParseConfigStrict ((const NCV_U8 *)Text, strlen (Text), &Parsed) == NcvConfigSuccess);
  assert (strcmp (Parsed.TargetBbsDescription, Maximum) == 0);
  assert (!NcvIniValueIsExactlyRepresentable ("   ", 4));
  assert (!NcvIniValueIsExactlyRepresentable ("Disk\t", 6));
  assert (!NcvIniValueIsExactlyRepresentable ("Disk=x", 7));
  memset (Maximum, 'x', sizeof (Maximum));
  assert (!NcvIniValueIsExactlyRepresentable (Maximum, sizeof (Maximum)));
}

static void
TestMalformedQuotedDescriptions (void)
{
  const char *Values[] = {"\"unterminated", "\"\"", "\"   \"", "\"disk\"junk",
                         "\"disk\\q\"", "\"disk\\", "\"disk=name\"", "\"disk\t\""};
  char Text[512];
  NCV_CONFIG_CORE Parsed;
  size_t Index;
  for (Index = 0; Index < sizeof (Values) / sizeof (Values[0]); ++Index) {
    snprintf (Text, sizeof (Text), "[Boot]\nTargetBbsDescription=%s\n", Values[Index]);
    NcvConfigDefaults (&Parsed);
    assert (NcvParseConfigStrict ((const NCV_U8 *)Text, strlen (Text), &Parsed) == NcvConfigCompromisedData);
  }
  /* Blank padded lines must not underflow the shared trimming routine. */
  strcpy (Text, "   \n  \r\n[Boot]\nTargetBbsDescription=  Ordinary disk  \n");
  NcvConfigDefaults (&Parsed);
  assert (NcvParseConfigStrict ((const NCV_U8 *)Text, strlen (Text), &Parsed) == NcvConfigSuccess);
  assert (strcmp (Parsed.TargetBbsDescription, "Ordinary disk") == 0);
}

int
main (
  int   ArgumentCount,
  char  **Arguments
  )
{
  if (ArgumentCount != 2) {
    fprintf (
      stderr,
      "usage: %s OUTPUT_FIXTURE_INI\n",
      Arguments[0]
      );
    return EXIT_FAILURE;
  }

  TestCapturedFixture (Arguments[1]);
  TestDisplayRankingCases ();
  TestStorageRankingCases ();
  TestCollectionBoundaryAndOverflow ();
  TestInvalidDescriptionAndZeroSelections ();
  TestPartialSelectionsStayProbeOnly ();
  TestStrictParserCommentsAndFilenames ();
  TestPaddedDiskDescriptionRoundTrips ();
  TestMalformedQuotedDescriptions ();
  puts ("candidate collection/ranking/format/strict-reparse tests: PASS");
  return EXIT_SUCCESS;
}

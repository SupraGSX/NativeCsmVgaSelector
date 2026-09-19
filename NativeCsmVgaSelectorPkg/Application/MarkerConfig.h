/* Portable optional menu-marker configuration. SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_MARKER_CONFIG_H
#define NCV_MARKER_CONFIG_H

typedef struct {
  unsigned Seen, Enabled, Partition;
  unsigned Signature;
  unsigned long long Start, Sectors;
  char Profile[33], Path[128], Header[65];
} NCV_MARKER_CONFIG;

static inline unsigned MarkerStringLength (const char *Text)
{
  unsigned Size = 0;
  while (Text[Size]) { ++Size; }
  return Size;
}

static inline int MarkerStringEqual (const char *Left, const char *Right)
{
  while (*Left && *Left == *Right) { ++Left; ++Right; }
  return *Left == *Right;
}

/* Caller verifies the destination's capacity before copying. */
static inline void MarkerStringCopy (char *Destination, const char *Source)
{
  while ((*Destination++ = *Source++)) { }
}

static inline int MarkerParseNumber (
  const char *Text, unsigned long long Maximum, unsigned long long *Result
  )
{
  unsigned long long Number = 0;
  unsigned Base = 10, Digit;
  if (!*Text) { return 0; }
  if (Text[0] == '0' && (Text[1] == 'x' || Text[1] == 'X')) {
    Base = 16; Text += 2;
    if (!*Text) { return 0; }
  }
  for (; *Text; ++Text) {
    if (*Text >= '0' && *Text <= '9') { Digit = *Text - '0'; }
    else if (*Text >= 'a' && *Text <= 'f') { Digit = *Text - 'a' + 10; }
    else if (*Text >= 'A' && *Text <= 'F') { Digit = *Text - 'A' + 10; }
    else { return 0; }
    if (Digit >= Base || Digit > Maximum || Number > (Maximum - Digit) / Base) { return 0; }
    Number = Number * Base + Digit;
  }
  *Result = Number;
  return 1;
}

static inline int MarkerConfigSetValue (NCV_MARKER_CONFIG *Config, const char *Key, const char *Value)
{
  static const char *Keys[] = {
    "Enabled", "PartitionNumber", "DiskSignature", "PartitionStart",
    "PartitionSectors", "Profile", "Path", "Header"
  };
  unsigned Index, Character;
  unsigned long long Number, Maximum;
  for (Index = 0; Index < 8; ++Index) {
    if (MarkerStringEqual (Key, Keys[Index])) { break; }
  }
  if (Index == 8 || (Config->Seen & (1U << Index))) { return 0; }
  Config->Seen |= 1U << Index;
  if (Index < 5) {
    if (Index == 0) {
      if (MarkerStringEqual (Value, "true")) { Number = 1; }
      else if (MarkerStringEqual (Value, "false")) { Number = 0; }
      else { return 0; }
    } else {
      Maximum = Index < 3 ? 0xffffffffULL : ~0ULL;
      if (!MarkerParseNumber (Value, Maximum, &Number)) { return 0; }
    }
    switch (Index) {
      case 0: Config->Enabled = (unsigned)Number; break;
      case 1: Config->Partition = (unsigned)Number; break;
      case 2: Config->Signature = (unsigned)Number; break;
      case 3: Config->Start = Number; break;
      case 4: Config->Sectors = Number; break;
    }
  } else if (Index == 5) {
    if (!*Value || MarkerStringLength (Value) > 32) { return 0; }
    for (Character = 0; Value[Character]; ++Character) {
      char Ch = Value[Character];
      if (!((Ch >= 'A' && Ch <= 'Z') || (Ch >= '0' && Ch <= '9') || Ch == '_')) { return 0; }
    }
    MarkerStringCopy (Config->Profile, Value);
  } else if (Index == 6) {
    if (Value[0] != '\\' || MarkerStringLength (Value) < 2 || MarkerStringLength (Value) > 127) { return 0; }
    for (Character = 0; Value[Character]; ++Character) {
      unsigned char Ch = (unsigned char)Value[Character];
      if (Ch < 33 || Ch > 126 || Ch == '/' || Ch == ':' || (Ch == '.' && Value[Character+1] == '.')) { return 0; }
    }
    MarkerStringCopy (Config->Path, Value);
  } else {
    if (!*Value || MarkerStringLength (Value) > 64) { return 0; }
    for (Character = 0; Value[Character]; ++Character) {
      if ((unsigned char)Value[Character] < 32 || (unsigned char)Value[Character] > 126) { return 0; }
    }
    MarkerStringCopy (Config->Header, Value);
  }
  return 1;
}

static inline int MarkerConfigIsValid (const NCV_MARKER_CONFIG *Config)
{
  if (!Config->Seen) { return 1; }
  if (!(Config->Seen & 1U)) { return 0; }
  if (!Config->Enabled) { return 1; }
  return (Config->Seen & 127U) == 127U && Config->Partition && Config->Signature &&
         Config->Start && Config->Sectors && Config->Start <= ~0ULL - Config->Sectors;
}
#endif

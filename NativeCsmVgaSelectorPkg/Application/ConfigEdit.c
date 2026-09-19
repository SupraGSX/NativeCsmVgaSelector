/** Bounded target-only rewrite; unrelated INI bytes are preserved.
    SPDX-License-Identifier: GPL-3.0-only */
#include "ConfigEdit.h"

static NCV_SIZE StringLength (const char *Text)
{
  NCV_SIZE Size = 0;
  while (Text[Size]) { ++Size; }
  return Size;
}

static void CopyBytes (char *Destination, const char *Source, NCV_SIZE Size)
{
  while (Size--) { *Destination++ = *Source++; }
}

static int EqualIgnoringCase (const char *Text, NCV_SIZE Size, const char *Expected)
{
  NCV_SIZE Index;
  if (Size != StringLength (Expected)) { return 0; }
  for (Index = 0; Index < Size; ++Index) {
    char Left = Text[Index], Right = Expected[Index];
    if (Left >= 'A' && Left <= 'Z') { Left += 32; }
    if (Right >= 'A' && Right <= 'Z') { Right += 32; }
    if (Left != Right) { return 0; }
  }
  return 1;
}

static int AppendDescription (char *Output, NCV_SIZE Capacity, NCV_SIZE *Written,
                              const char *Description)
{
  const char *Key = "TargetBbsDescription=";
  NCV_SIZE KeySize = StringLength (Key), ValueSize = StringLength (Description);
  NCV_SIZE Prefix = *Written != 0 && Output[*Written - 1] != '\n' ? 2 : 0;
  if (Prefix + KeySize + ValueSize + 2 >= Capacity - *Written) { return NCV_EDIT_CAPACITY; }
  if (Prefix) { Output[(*Written)++] = '\r'; Output[(*Written)++] = '\n'; }
  CopyBytes (Output + *Written, Key, KeySize); *Written += KeySize;
  CopyBytes (Output + *Written, Description, ValueSize); *Written += ValueSize;
  Output[(*Written)++] = '\r'; Output[(*Written)++] = '\n';
  return 0;
}

int NcvEditTargets (
  const char *Input, NCV_SIZE Size, const char *Gpu, const char *Controller,
  const char *Disk, char *Output, NCV_SIZE Capacity, NCV_SIZE *Used
  )
{
  NCV_SIZE Position = 0, Written = 0;
  int Section = 0, Seen = 0;
  char EncodedDisk[NCV_ENCODED_DESCRIPTION_BYTES];
  if (!Input || !Output || !Used || !Gpu || !Controller || !Disk) { return NCV_EDIT_INVALID; }
  *Used = 0;
  if (!Capacity) { return NCV_EDIT_CAPACITY; }
  if (NcvEncodeDiskDescription (Disk, EncodedDisk, sizeof (EncodedDisk)) != 0) { return NCV_EDIT_INVALID; }
  while (Position < Size) {
    NCV_SIZE End = Position, Next, Start, TrimmedEnd, Equals, KeyEnd;
    const char *Key = 0, *Value = 0;
    int Bit = 0;
    while (End < Size && Input[End] != '\n') { ++End; }
    Next = End < Size ? End + 1 : End;
    Start = Position;
    TrimmedEnd = End;
    while (Start < TrimmedEnd && (Input[Start] == ' ' || Input[Start] == '\t')) { ++Start; }
    while (TrimmedEnd > Start && (Input[TrimmedEnd-1] == '\r' || Input[TrimmedEnd-1] == ' ' || Input[TrimmedEnd-1] == '\t')) { --TrimmedEnd; }
    if (Start < TrimmedEnd && Input[Start] == '[' && Input[TrimmedEnd-1] == ']') {
      if (Section == 2 && !(Seen & 4)) {
        if (AppendDescription (Output, Capacity, &Written, EncodedDisk)) { return NCV_EDIT_CAPACITY; }
        Seen |= 4;
      }
      Section = EqualIgnoringCase (Input + Start + 1, TrimmedEnd - Start - 2, "Video") ? 1 :
                EqualIgnoringCase (Input + Start + 1, TrimmedEnd - Start - 2, "Boot") ? 2 : 0;
    } else if (Start < TrimmedEnd && Input[Start] != '#' && Input[Start] != ';') {
      Equals = Start;
      while (Equals < TrimmedEnd && Input[Equals] != '=') { ++Equals; }
      KeyEnd = Equals;
      while (KeyEnd > Start && (Input[KeyEnd-1] == ' ' || Input[KeyEnd-1] == '\t')) { --KeyEnd; }
      if (Equals < TrimmedEnd) {
        if (Section == 1 && EqualIgnoringCase (Input + Start, KeyEnd - Start, "TargetPci")) {
          Key = "TargetPci"; Value = Gpu; Bit = 1;
        }
        if (Section == 2 && EqualIgnoringCase (Input + Start, KeyEnd - Start, "TargetControllerPci")) {
          Key = "TargetControllerPci"; Value = Controller; Bit = 2;
        }
        if (Section == 2 && EqualIgnoringCase (Input + Start, KeyEnd - Start, "TargetBbsDescription")) {
          Key = "TargetBbsDescription"; Value = EncodedDisk; Bit = 4;
        }
      }
    }
    if (Key != 0) {
      NCV_SIZE KeySize = StringLength (Key), ValueSize = StringLength (Value);
      if (Seen & Bit) { return NCV_EDIT_INVALID; }
      Seen |= Bit;
      if (KeySize + ValueSize + 3 >= Capacity - Written) { return -1; }
      CopyBytes (Output + Written, Key, KeySize); Written += KeySize;
      Output[Written++] = '=';
      CopyBytes (Output + Written, Value, ValueSize); Written += ValueSize;
      Output[Written++] = '\r'; Output[Written++] = '\n';
    } else {
      if (Next - Position >= Capacity - Written) { return -1; }
      CopyBytes (Output + Written, Input + Position, Next - Position);
      Written += Next - Position;
    }
    Position = Next;
  }
  if (Section == 2 && !(Seen & 4)) {
    if (AppendDescription (Output, Capacity, &Written, EncodedDisk)) { return NCV_EDIT_CAPACITY; }
    Seen |= 4;
  }
  if (Seen != 7) { return NCV_EDIT_INVALID; }
  Output[Written] = 0;
  *Used = Written;
  return 0;
}

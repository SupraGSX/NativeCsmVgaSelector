/* SPDX-License-Identifier: GPL-3.0-only */
#include "CandidateIni.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void Check(const char *Text, int Usable, unsigned Flags)
{
  NCV_CONFIG_CORE Config;
  NCV_U8 Missing = 0;
  NcvConfigDefaults(&Config);
  assert(NcvParseConfigStrict((const NCV_U8 *)Text, strlen(Text), &Config) == NcvConfigSuccess);
  assert((NcvValidateConfigMode(&Config, &Missing) == NcvConfigSuccess) == Usable);
  assert(Missing == Flags);
}
int main(void)
{
  NCV_CONFIG_CORE Config;
  const char *Removed = "[Behavior]\nProbe=false\nRequireReferenceSnapshotMatch=false\n";
  Check("# comment only\n", 0, 0);

  Check("[Behavior]\nProbe=true\n", 1, 0);
  Check("[Behavior]\nProbe=false\n", 0, 0x3e);
  Check("[Behavior]\nProbe=false\n[Video]\nTargetPci=0000:03:00.0\n[Boot]\nTargetControllerPci=0000:05:00.0\n[Endpoint]\nIoDecoding=On\nMemoryDecoding=On\nBusMastering=Ignore\n", 1, 0);
  NcvConfigDefaults(&Config);
  assert(NcvParseConfigStrict((const NCV_U8 *)Removed, strlen(Removed), &Config) != NcvConfigSuccess);
  puts("configuration recovery validation: PASS");
  return 0;
}

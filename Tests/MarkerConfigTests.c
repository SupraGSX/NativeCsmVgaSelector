#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "CandidateIni.h"
static int parse(const char*s){NCV_CONFIG_CORE c;NcvConfigDefaults(&c);return NcvParseConfigStrict((const NCV_U8*)s,strlen(s),&c)==NcvConfigSuccess;}
int main(void){
 assert(parse("[Marker]\nEnabled=false\n"));
 assert(parse("[Marker]\nEnabled=true\nPartitionNumber=1\nDiskSignature=0x12345678\nPartitionStart=2048\nPartitionSectors=131072\nProfile=SECONDARY\nPath=\\BOOTSEL.DAT\n"));
 assert(!parse("[Marker]\nEnabled=true\n"));
 assert(!parse("[Marker]\nEnabled=false\nEnabled=false\n"));
 assert(!parse("[Marker]\nUnknown=1\n"));
 assert(!parse("[Marker]\nEnabled=false\nPartitionStart=18446744073709551616\n"));
 assert(!parse("[Marker]\nEnabled=false\nProfile=bad-value\n"));
 assert(!parse("[Marker]\nEnabled=false\nPath=\\..\\file\n"));
 assert(!parse("[Marker]\nEnabled=false\nHeader=\n"));
 assert(!parse("[Marker]\nEnabled=true\nPartitionNumber=1\nDiskSignature=1\nPartitionStart=18446744073709551615\nPartitionSectors=2\nProfile=SECONDARY\nPath=\\BOOTSEL.DAT\n"));
 assert(!parse("[Marker]\nEnabled=false\n[Marker]\nEnabled=false\n"));
 assert(parse("[Behavior]\nProbe=true\n"));
 puts("Unified marker INI: 12 parser cases passed");return 0;
}

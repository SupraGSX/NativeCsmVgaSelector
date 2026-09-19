#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ConfigEdit.h"
int main(void){
 const char *s="# retained\n[Marker]\nEnabled=false\n[Behavior]\nProbe=false\nAutoBoot=true\n[Video]\n# TargetPci=0000:09:00.0\nTargetPci=0000:03:00.0\n[Endpoint]\nIoDecoding=On\nMemoryDecoding=On\nBusMastering=Ignore\n[Boot]\nTargetControllerPci=0000:02:00.0\nTargetBbsDescription=Original Disk\n";
 char out[4096];NCV_SIZE n;NCV_CONFIG_CORE c;
 assert(!NcvEditTargets(s,strlen(s),"0000:04:00.0","0000:06:00.0","Example disk B",out,sizeof(out),&n));
 assert(strstr(out,"# retained\n[Marker]\nEnabled=false\n[Behavior]\nProbe=false\nAutoBoot=true\n"));assert(strstr(out,"# TargetPci=0000:09:00.0\n"));
 NcvConfigDefaults(&c);assert(NcvParseConfigStrict((const NCV_U8*)out,n,&c)==NcvConfigSuccess);assert(c.TargetPci.Bus==4&&c.TargetControllerPci.Bus==6);assert(strcmp(c.TargetBbsDescription,"Example disk B")==0);
 NCV_SIZE original_size=n;
 for(NCV_SIZE z=0;z<=original_size;z++)assert(NcvEditTargets(s,strlen(s),"0000:04:00.0","0000:06:00.0","Example disk B",out,z,&n)!=0);
 assert(NcvEditTargets(s,strlen(s),"x","y","bad\n[Marker]",out,sizeof(out),&n)!=0);
 assert(NcvEditTargets("[Video]\nTargetPci=x\nTargetPci=y\n",strlen("[Video]\nTargetPci=x\nTargetPci=y\n"),"x","y","z",out,sizeof(out),&n)!=0);
 assert(NcvEditTargets("",0,"x","y","z",out,sizeof(out),&n)!=0);
 const char *names[]={"SanDisk Extreme Pro 0 "," USB "," \"USB\\disk\" "};
 for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++){
  assert(!NcvEditTargets(s,strlen(s),"0000:04:00.0","0000:06:00.0",names[i],out,sizeof(out),&n));
  NcvConfigDefaults(&c);assert(NcvParseConfigStrict((const NCV_U8*)out,n,&c)==NcvConfigSuccess);
  assert(strcmp(c.TargetBbsDescription,names[i])==0);
  assert(strstr(out,"# retained\n[Marker]\nEnabled=false\n"));
  NCV_SIZE full=n;
  for(NCV_SIZE z=0;z<=full;z++)assert(NcvEditTargets(s,strlen(s),"0000:04:00.0","0000:06:00.0",names[i],out,z,&n)!=0);
 }
 assert(NcvEditTargets(s,strlen(s),"x","y","   ",out,sizeof(out),&n)!=0);
 const char *base="[Behavior]\nProbe=false\n[Video]\nTargetPci=0000:03:00.0\n[Endpoint]\nIoDecoding=On\nMemoryDecoding=On\nBusMastering=Ignore\n[Boot]\nTargetControllerPci=0000:05:00.0";
 const char *tails[]={"", "\n", "\n# retained tail\n", "\n[Marker]\nEnabled=false\n"};
 for (size_t i=0;i<sizeof(tails)/sizeof(tails[0]);i++) {
  char input[1024]; unsigned char missing=0;
  snprintf(input,sizeof(input),"%s%s",base,tails[i]);
  NcvConfigDefaults(&c);
  assert(NcvParseConfigStrict((const NCV_U8*)input,strlen(input),&c)==NcvConfigSuccess);
  assert(NcvValidateConfigMode(&c,&missing)==NcvConfigSuccess);
  assert(!NcvEditTargets(input,strlen(input),"0000:04:00.0","0000:05:00.0","new disk",out,sizeof(out),&n));
  NcvConfigDefaults(&c);
  assert(NcvParseConfigStrict((const NCV_U8*)out,n,&c)==NcvConfigSuccess);
  assert(!strcmp(c.TargetBbsDescription,"new disk"));
  assert(!strstr(strstr(out,"TargetBbsDescription=")+1,"TargetBbsDescription="));
  NCV_SIZE full=n;
  for(NCV_SIZE z=0;z<=full;z++) assert(NcvEditTargets(input,strlen(input),"0000:04:00.0","0000:05:00.0","new disk",out,z,&n)==NCV_EDIT_CAPACITY);
 }
 puts("Target rewrite preserves unrelated settings; strict parse and bounded output tests PASS");
}

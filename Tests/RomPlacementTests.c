/* Necessary-condition checks, malformed inputs and optional real ROM fixture.
   SPDX-License-Identifier: GPL-3.0-only */
#include "RomPlacement.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned char Rom[1024], Before[1024];
static NCV_ROM_PLACEMENT_FACTS Facts;
static void W16(unsigned at, unsigned value) { Rom[at]=(unsigned char)value; Rom[at+1]=(unsigned char)(value>>8); }
static void Checksum(void) {
  unsigned sum=0;
  Rom[1023]=0;
  for (unsigned i=0;i<sizeof Rom;i++) sum+=Rom[i];
  Rom[1023]=(unsigned char)(0-sum);
}
static void Fixture(unsigned revision, unsigned runtime) {
  memset(Rom,0,sizeof Rom);
  Rom[0]=0x55; Rom[1]=0xaa; Rom[2]=2;
  W16(0x18,0x1c); memcpy(Rom+0x1c,"PCIR",4);
  W16(0x26,0x1c); Rom[0x28]=(unsigned char)revision;
  W16(0x2c,2); W16(0x32,runtime); Checksum();
}
static NCV_ROM_PLACEMENT_RESULT Assess(unsigned limit) {
  NcvRomPlacementReadFacts(Rom,sizeof Rom,&Facts);
  return NcvRomPlacementAssess(&Facts,limit);
}
int main(int argc,char **argv) {
  Fixture(0,1);
  assert(Assess(512)==NcvPlacementInPlaceRom);
  assert(Facts.Valid && Facts.Revision==0 && Facts.MaxRuntimeBytes==0);
  Fixture(3,1);
  assert(Assess(512)==NcvPlacementNeedsFirmwareAndBackend);
  assert(Assess(511)==NcvPlacementRuntimeOverlap);
  assert(Assess(1024)==NcvPlacementNeedsFirmwareAndBackend);
  assert(Assess(0)==NcvPlacementInvalidRom);
  assert(Assess(65537)==NcvPlacementInvalidRom);
  Fixture(3,0); assert(Assess(512)==NcvPlacementUnknownRuntime);
  Fixture(3,3); assert(Assess(512)==NcvPlacementInvalidRom);
  Fixture(3,1); W16(0x26,0x18); Checksum(); assert(Assess(512)==NcvPlacementInvalidRom);
  Fixture(3,1); W16(0x18,0xffff); Checksum(); assert(Assess(512)==NcvPlacementInvalidRom);
  Fixture(3,1); Rom[0x30]=3; Checksum(); assert(Assess(512)==NcvPlacementInvalidRom);
  Fixture(3,1); Rom[0x200]=1; assert(Assess(512)==NcvPlacementInvalidRom);
  Fixture(3,1); W16(0x2c,1); Checksum(); assert(Assess(512)==NcvPlacementInvalidRom);
  Fixture(3,1); Rom[2]=0; Checksum(); assert(Assess(512)==NcvPlacementInvalidRom);
  Fixture(3,1); Rom[2]=3; Checksum(); assert(Assess(512)==NcvPlacementInvalidRom);
  Fixture(3,1); Rom[0]=0; Checksum(); assert(Assess(512)==NcvPlacementInvalidRom);
  Fixture(3,1); NcvRomPlacementReadFacts(Rom,~0ULL,&Facts); assert(!Facts.Valid);
  NcvRomPlacementReadFacts(NULL,1024,&Facts); assert(!Facts.Valid);
  NcvRomPlacementReadFacts(Rom,1024,NULL);
  assert(NcvRomPlacementAssess(NULL,1024)==NcvPlacementInvalidRom);
  for (unsigned size=0;size<1024;size++) {
    unsigned char *exact=malloc(size?size:1);
    assert(exact); Fixture(3,1); memcpy(exact,Rom,size);
    NcvRomPlacementReadFacts(exact,size,&Facts); assert(!Facts.Valid); free(exact);
  }
  uint32_t rng=0x74381ab2U;
  for(unsigned test=0;test<10000;test++) {
    Fixture(3,1);
    for(unsigned changes=0;changes<1+test%9;changes++) {
      rng=rng*1664525U+1013904223U; unsigned at=rng%sizeof Rom;
      rng=rng*1664525U+1013904223U; Rom[at]^=(unsigned char)(rng>>24);
    }
    if(test%2) Checksum();
    memcpy(Before,Rom,sizeof Rom);
    NcvRomPlacementReadFacts(Rom,sizeof Rom,&Facts);
    NCV_ROM_PLACEMENT_RESULT result=NcvRomPlacementAssess(&Facts,512);
    assert(result>=NcvPlacementInvalidRom && result<=NcvPlacementNeedsFirmwareAndBackend);
    assert(!memcmp(Before,Rom,sizeof Rom));
    if(result==NcvPlacementNeedsFirmwareAndBackend) assert(Facts.Valid && Facts.Revision>=3 && Facts.MaxRuntimeBytes && Facts.MaxRuntimeBytes<=512);
  }
  if(argc==2) {
    FILE *f=fopen(argv[1],"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); long size=ftell(f); assert(size>0 && size<=65536);
    rewind(f); unsigned char *data=malloc((size_t)size); assert(data);
    assert(fread(data,1,(size_t)size,f)==(size_t)size); fclose(f);
    NcvRomPlacementReadFacts(data,(unsigned long long)size,&Facts);
    assert(Facts.Valid && NcvRomPlacementAssess(&Facts,0xe800)==NcvPlacementInPlaceRom);
    printf("Captured GPU: revision=%u bytes=%u; split-placement contract absent, execution remains unavailable.\n",Facts.Revision,Facts.ImageBytes);
    free(data);
  }
  puts("ROM placement checks: PASS (10000 mutations, 1024 exact-sized truncations, bounds/checksums/contracts; no execution authorization)");
  return 0;
}

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "PciIdLookup.h"
int main(void) {
 char Name[128];const char*Text="# test\n1234  Vendor\n\t0001  First GPU\n\t\t1234 0002  Wrong subsystem\n\t0002  Second GPU\r\n5678  Other\n\t0002  Other GPU\nC 03  Display\n\t0002  Not a device\n";
 assert(PciIdLookup(Text,strlen(Text),0x1234,2,Name,sizeof(Name))&&strcmp(Name,"Second GPU")==0);
 assert(PciIdLookup(Text,strlen(Text),0x5678,2,Name,sizeof(Name))&&strcmp(Name,"Other GPU")==0);
 assert(!PciIdLookup(Text,strlen(Text),0x1234,3,Name,sizeof(Name))&&Name[0]==0);
 assert(!PciIdLookup(NULL,0,0,0,Name,sizeof(Name)));
 assert(!PciIdLookup(Text,strlen(Text),0,0,NULL,0));
 assert(PciIdLookup(Text,strlen(Text),0x1234,1,Name,5)&&strcmp(Name,"Firs")==0);
 assert(!PciIdLookup("1234  V\n\t0001  bad\001name",sizeof("1234  V\n\t0001  bad\001name")-1,0x1234,1,Name,sizeof(Name)));
 for(size_t N=0;N<strlen(Text);N++)PciIdLookup(Text,N,0x1234,2,Name,sizeof(Name));
 puts("PCI ID parser and bounded-input tests passed");return 0;
}

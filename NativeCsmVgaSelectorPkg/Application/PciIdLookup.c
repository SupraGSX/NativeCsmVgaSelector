#include "PciIdLookup.h"
static int Hex4(const char *Text, unsigned *Value) {
  unsigned I, V=0, D;
  for(I=0;I<4;I++) {
    if(Text[I]>='0'&&Text[I]<='9')D=(unsigned)(Text[I]-'0');
    else if(Text[I]>='a'&&Text[I]<='f')D=(unsigned)(Text[I]-'a'+10);
    else if(Text[I]>='A'&&Text[I]<='F')D=(unsigned)(Text[I]-'A'+10);
    else return 0;
    V=V*16+D;
  }
  *Value=V;return 1;
}
int PciIdLookup(const char *Data, unsigned long long Size, unsigned Vendor,
                unsigned Device, char *Name, unsigned Capacity) {
  unsigned long long Start=0,End,N,Offset;
  unsigned Id,I;
  int Match=0;
  if(Name==0||Capacity==0)return 0;
  Name[0]=0;
  if(Data==0)return 0;
  while(Start<Size) {
    End=Start;while(End<Size&&Data[End]!='\n')End++;
    N=End-Start;
    if(N>0&&Data[Start+N-1]=='\r')N--;
    if(N>=6&&Data[Start]!='\t'&&Hex4(Data+Start,&Id)&&Data[Start+4]==' '&&Data[Start+5]==' ') {
      Match=Id==Vendor;
    } else if(N>0&&Data[Start]!='#'&&Data[Start]!='\t'&&Data[Start]!=' ') {
      Match=0;
    } else if(Match&&N>=8&&Data[Start]=='\t'&&Hex4(Data+Start+1,&Id)&&
              Id==Device&&Data[Start+5]==' '&&Data[Start+6]==' ') {
      Offset=7;while(Offset<N&&Data[Start+Offset]==' ')Offset++;
      while(N>Offset&&(Data[Start+N-1]==' '||Data[Start+N-1]=='\t'))N--;
      if(N==Offset)return 0;
      /* Reject control characters; database text is never used as a format string. */
      for(I=0;Offset+I<N;I++)if((unsigned char)Data[Start+Offset+I]<32)return 0;
      for(I=0;Offset+I<N&&I+1<Capacity;I++) {
        unsigned char C=(unsigned char)Data[Start+Offset+I];Name[I]=C<127?(char)C:'?';
      }
      Name[I]=0;return I!=0;
    }
    Start=End<Size?End+1:Size;
  }
  return 0;
}

#ifndef NCV_PCI_NAMES_H
#define NCV_PCI_NAMES_H
#include <Uefi.h>
#include "ProbeConfig.h"
VOID PciNamesLoad(EFI_HANDLE ImageHandle);
VOID PciNamesRelease(VOID);
VOID PciNamesLookup(UINT16 Vendor,UINT16 Device,CHAR8 *Name,UINTN Capacity);
VOID PciNamesForTarget(CONST PROBE_PCI_ADDRESS *Target,CHAR8 *Name,UINTN Capacity);
#endif

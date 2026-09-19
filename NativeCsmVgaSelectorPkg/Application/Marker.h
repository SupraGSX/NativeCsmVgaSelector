#ifndef NCV_MARKER_H
#define NCV_MARKER_H
#include <Uefi.h>
#include "MarkerConfig.h"
VOID MarkerConfigure (CONST NCV_MARKER_CONFIG *Config);
EFI_STATUS MarkerApply (VOID);
EFI_STATUS MarkerRestore (VOID);
#endif

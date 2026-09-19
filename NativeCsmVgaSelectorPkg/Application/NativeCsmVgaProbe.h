/** @file Selector Probe-mode entry point.
  BootReady is true only after interactive setup saves and closes successfully.
  The caller must reload Config.ini before entering the normal boot path.
**/
#ifndef NATIVE_CSM_VGA_PROBE_RUN_H_
#define NATIVE_CSM_VGA_PROBE_RUN_H_

#include <Uefi.h>
#include "ProbeConfig.h"

EFI_STATUS
EFIAPI
NativeCsmVgaProbeRun (
  IN EFI_HANDLE          ImageHandle,
  IN CONST PROBE_CONFIG  *Config,
  OUT BOOLEAN            *BootReady
  );

#endif

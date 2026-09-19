/** @file Selector Boot-mode entry point. */
#ifndef NATIVE_CSM_VGA_BOOT_RUN_H_
#define NATIVE_CSM_VGA_BOOT_RUN_H_

#include <Uefi.h>
#include "ProbeConfig.h"

EFI_STATUS
EFIAPI
NativeCsmVgaBootRun (
  IN EFI_HANDLE          ImageHandle,
  IN CONST PROBE_CONFIG  *Config
  );

#endif

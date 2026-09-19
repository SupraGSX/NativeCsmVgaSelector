/** Final pre-switch input, outside the sensitive Boot stack.
    SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_BOOT_COUNTDOWN_H_
#define NCV_BOOT_COUNTDOWN_H_
#include "Log.h"
#include "ProbeConfig.h"

/* Early five-second F2/Esc window, before runtime preflight owns resources. */
EFI_STATUS EFIAPI BootSetupWindow (BOOLEAN *EditRequested);
/* Resolve the display name before entering Boot and releasing the PCI database. */
VOID EFIAPI BootCountdownPrepare (CONST PROBE_PCI_ADDRESS *Target);
EFI_STATUS EFIAPI BootCountdownWait (APP_LOGGER *Logger);
/* Close a returning attempt before any setup request may be consumed. */
EFI_STATUS EFIAPI BootCountdownFinish (APP_LOGGER *Logger, APP_FILE_CONTEXT *Files,
                                     EFI_STATUS Status, EFI_STATUS PlanCleanupStatus);
BOOLEAN EFIAPI BootCountdownCanEdit (VOID);
BOOLEAN EFIAPI BootCountdownRequestEdit (VOID);
BOOLEAN EFIAPI BootCountdownTakeEdit (EFI_STATUS Status, EFI_STATUS MarkerStatus);
/* Call before TakeEdit/error UI; failed marker restoration revokes editability. */
BOOLEAN EFIAPI BootCountdownTakeCancel (EFI_STATUS Status, EFI_STATUS MarkerStatus);
#endif

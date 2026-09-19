/** Boot diagnostics independent of file logging and the frozen Boot stack.
    SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_BOOT_REPORT_H_
#define NCV_BOOT_REPORT_H_

#include <Uefi.h>
#include "Log.h"

EFI_STATUS EFIAPI BootReportDisableWatchdog (VOID);
/* Typed boundary after the final log commit, immediately before LegacyBoot. */
VOID EFIAPI BootReportDiskBootAttempt (VOID);
VOID EFIAPI BootReportReset (VOID);
VOID EFIAPI BootReportSetStage (CONST CHAR16 *Stage);
/* Observe formatted NCV_* records for diagnostics only, never for boot policy. */
VOID EFIAPI BootReportObserve (CONST CHAR16 *Text);
VOID EFIAPI BootReportLogFailure (EFI_STATUS Status);
/* Render a returning error without choosing or waiting for a recovery action. */
VOID EFIAPI BootReportShow (EFI_STATUS Status);
/* Only call on a path that already permits returning to firmware. */
VOID EFIAPI BootReportHold (EFI_STATUS Status);
EFI_STATUS EFIAPI BootReportFinish (APP_LOGGER *Logger, APP_FILE_CONTEXT *Files, EFI_STATUS Status);
/* Cleanup result is independent of a pre-existing primary error. */
EFI_STATUS EFIAPI BootReportFinishDetailed (APP_LOGGER *Logger, APP_FILE_CONTEXT *Files,
                                          EFI_STATUS Status, BOOLEAN *Closed);
/* Remain halted after an existing irreversible-failure decision. */
VOID EFIAPI BootReportHalt (APP_LOGGER *Logger, CONST CHAR16 *Code, EFI_STATUS Status);

#endif

/** SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_CONFIG_RECOVERY_H
#define NCV_CONFIG_RECOVERY_H
#include "AppFile.h"
EFI_STATUS ConfigReadUsable (APP_FILE_CONTEXT *Files, CONST CHAR16 *Name, UINT8 **Bytes, UINTN *Size);
EFI_STATUS ConfigLoadRecoverable (APP_FILE_CONTEXT *Files, UINT8 **Bytes, UINTN *Size);
/* EFI_SUCCESS means the user confirmed and existing files were preserved.
   Every other status means return to firmware without starting setup. */
EFI_STATUS ConfigOfferFreshSetup (APP_FILE_CONTEXT *Files, EFI_STATUS Failure);
VOID ConfigWaitForReturn (EFI_STATUS Failure);
#endif

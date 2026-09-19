/** @file
  Symbolic EFI status formatting.

  SPDX-License-Identifier: LGPL-2.1-or-later
**/

#ifndef NATIVE_CSM_VGA_SELECTOR_STATUS_PRINT_H_
#define NATIVE_CSM_VGA_SELECTOR_STATUS_PRINT_H_

#include <Uefi.h>

#include "Log.h"

/** Returns a stable symbolic name for common EFI status values. **/
CONST CHAR16 *
EfiStatusName (
  IN EFI_STATUS  Status
  );

/** Logs Operation, its symbolic status name, and the native-width hex value. **/
EFI_STATUS
StatusPrint (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Operation,
  IN EFI_STATUS    Status
  );

#endif

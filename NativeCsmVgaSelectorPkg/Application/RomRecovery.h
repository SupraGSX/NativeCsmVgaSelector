/** Overlap-only recovery choices after a returning preflight rejection.
    SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_ROM_RECOVERY_H_
#define NCV_ROM_RECOVERY_H_
#include "LegacyRomGuard.h"

VOID EFIAPI RomRecoveryReset (VOID);
/* Captures values, never borrows the soon-to-be-released plan or ROM pointer. */
VOID EFIAPI RomRecoveryRecord (CONST NATIVE_CSM_VGA_RUNTIME_PLAN *Plan,
                              CONST LEGACY_ROM_GUARD_RESULT *Conflict);
BOOLEAN EFIAPI RomRecoveryAvailable (EFI_STATUS BootStatus, EFI_STATUS CleanupStatus);
/* TRUE: handled and user acknowledged. FALSE: use ordinary error handling.
   No boot retries, firmware variable writes, CSM calls or ROM writes exist here. */
BOOLEAN EFIAPI RomRecoveryOffer (EFI_HANDLE ImageHandle, EFI_STATUS BootStatus,
                               EFI_STATUS CleanupStatus);
#endif

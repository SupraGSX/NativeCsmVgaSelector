/** Compatibility16 dispatch preparation and owned low-memory reservation.
    SPDX-License-Identifier: GPL-3.0-only */
#ifndef NCV_LEGACY_DISPATCH_H_
#define NCV_LEGACY_DISPATCH_H_
#include "RuntimePlan.h"
EFI_STATUS EFIAPI LegacyDispatchReserve (VOID);
EFI_STATUS EFIAPI LegacyDispatchRelease (VOID);
EFI_PHYSICAL_ADDRESS EFIAPI LegacyDispatchAddress (VOID);
VOID EFIAPI LegacyDispatchPrepare (CONST NATIVE_CSM_VGA_RUNTIME_PLAN *Plan, EFI_DISPATCH_OPROM_TABLE *Table);
#endif

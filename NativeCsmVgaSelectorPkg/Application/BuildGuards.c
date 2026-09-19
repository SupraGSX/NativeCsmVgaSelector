/** Compile-time guards for layouts used by the sensitive boot routine.
    SPDX-License-Identifier: GPL-3.0-only */
#include "ProbeConfig.h"
#include "RuntimePlan.h"
#include "LegacyBootTarget.h"
STATIC_ASSERT (sizeof (PROBE_CONFIG) == 576, "Boot configuration layout changed");
STATIC_ASSERT (sizeof (NATIVE_CSM_VGA_RUNTIME_PLAN) == 24824, "Runtime plan layout changed");
STATIC_ASSERT (sizeof (LEGACY_BOOT_BOOT_TARGET_CONTEXT) == 3816, "Legacy target layout changed");

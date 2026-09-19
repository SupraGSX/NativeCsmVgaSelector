#ifndef NCV_TARGET_SELECTION_H
#define NCV_TARGET_SELECTION_H
#include <Uefi.h>
#include "CandidateIni.h"
EFI_STATUS SelectBootTargets (
  NCV_DISPLAY_CANDIDATE *Displays, UINTN DisplayCount, UINTN *SelectedDisplay,
  NCV_STORAGE_CANDIDATE *Storage, UINTN StorageCount, UINTN *SelectedStorage,
  CONST NCV_CONFIG_CORE *RetainedSettings, BOOLEAN SavedTargetMissing
  );
#endif

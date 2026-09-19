/** Probe-only keyboard target selection. Does not write files or touch PCI state. */
#include "TargetSelection.h"
#include "PciNames.h"
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC UINTN NextDisplay (NCV_DISPLAY_CANDIDATE *Items, UINTN Count, UINTN Current, BOOLEAN Backward) {
  UINTN Step, Index;
  if (Count == 0) { return (UINTN)NCV_NO_SELECTION; }
  Index = Current < Count ? Current : (Backward ? 0 : Count - 1);
  for (Step = 0; Step < Count; ++Step) {
    Index = Backward ? (Index + Count - 1) % Count : (Index + 1) % Count;
    if (Items[Index].Eligible) { return Index; }
  }
  return (UINTN)NCV_NO_SELECTION;
}
STATIC UINTN NextStorage (NCV_STORAGE_CANDIDATE *Items, UINTN Count, UINTN Current, BOOLEAN Backward) {
  UINTN Step, Index;
  if (Count == 0) { return (UINTN)NCV_NO_SELECTION; }
  Index = Current < Count ? Current : (Backward ? 0 : Count - 1);
  for (Step = 0; Step < Count; ++Step) {
    Index = Backward ? (Index + Count - 1) % Count : (Index + 1) % Count;
    if (Items[Index].Eligible) { return Index; }
  }
  return (UINTN)NCV_NO_SELECTION;
}

/* Review retained policies without silently weakening them for a new GPU. */
STATIC BOOLEAN ShowRetainedSettings (
  CONST NCV_CONFIG_CORE *Settings, CONST NCV_DISPLAY_CANDIDATE *Gpu,
  CONST NCV_STORAGE_CANDIDATE *Disk
  )
{
  BOOLEAN Conflict = FALSE;
  CONST NCV_MARKER_CONFIG *Marker;
  if (Settings == NULL) { return FALSE; }
  Print (L"\r\nRetained GPU restrictions: ");
  if (!Settings->HasExpectedVendor && !Settings->HasExpectedDevice &&
      !Settings->HasExpectedSubsystemVendor && !Settings->HasExpectedSubsystemDevice) {
    Print (L"none");
  }
  if (Settings->HasExpectedVendor) {
    Print (L"vendor=%04x ", Settings->ExpectedVendor);
    Conflict |= Gpu != NULL && Gpu->Vendor != Settings->ExpectedVendor;
  }
  if (Settings->HasExpectedDevice) {
    Print (L"device=%04x ", Settings->ExpectedDevice);
    Conflict |= Gpu != NULL && Gpu->DeviceId != Settings->ExpectedDevice;
  }
  if (Settings->HasExpectedSubsystemVendor) {
    Print (L"subvendor=%04x ", Settings->ExpectedSubsystemVendor);
    Conflict |= Gpu != NULL && Gpu->SubsystemVendor != Settings->ExpectedSubsystemVendor;
  }
  if (Settings->HasExpectedSubsystemDevice) {
    Print (L"subdevice=%04x", Settings->ExpectedSubsystemDevice);
    Conflict |= Gpu != NULL && Gpu->SubsystemDevice != Settings->ExpectedSubsystemDevice;
  }
  if (Conflict) {
    Print (L"\r\nWARNING: selected GPU conflicts with retained restrictions.\r\nEdit Expected* values in Config.ini before booting.");
  }
  Marker = &Settings->Marker;
  if (!Marker->Enabled) {
    Print (L"\r\nMenu marker: disabled\r\n");
  } else {
    Print (L"\r\nMenu marker: profile %a, disk %08x, partition %u\r\n",
           Marker->Profile, Marker->Signature, Marker->Partition);
    Print (L"  start=%lu sectors=%lu path=%a\r\n", Marker->Start, Marker->Sectors, Marker->Path);
    Print (L"Marker destination stays fixed; its disk is not inferred from this choice.\r\n");
    if (Disk != NULL &&
        (Disk->Address.Segment != Settings->TargetControllerPci.Segment ||
         Disk->Address.Bus != Settings->TargetControllerPci.Bus ||
         Disk->Address.Device != Settings->TargetControllerPci.Device ||
         Disk->Address.Function != Settings->TargetControllerPci.Function ||
         AsciiStrCmp (Disk->Description, Settings->TargetBbsDescription) != 0)) {
      Print (L"WARNING: boot disk changed; check that marker routing is still intended.\r\n");
      Conflict = TRUE;
    }
  }
  return Conflict;
}

EFI_STATUS SelectBootTargets (
  NCV_DISPLAY_CANDIDATE *Displays, UINTN DisplayCount, UINTN *SelectedDisplay,
  NCV_STORAGE_CANDIDATE *Storage, UINTN StorageCount, UINTN *SelectedStorage,
  CONST NCV_CONFIG_CORE *RetainedSettings, BOOLEAN SavedTargetMissing
  ) {
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  UINTN Event, Focus, Gpu, Disk, Index;
  BOOLEAN Review, Ready, Conflict, Confirmed;
  CHAR8 Name[128];
  if (DisplayCount > NCV_MAX_DISPLAY_CANDIDATES || StorageCount > NCV_MAX_STORAGE_CANDIDATES ||
      Displays == NULL || Storage == NULL || SelectedDisplay == NULL || SelectedStorage == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (gST->ConIn == NULL || gST->ConIn->WaitForKey == NULL) { return EFI_UNSUPPORTED; }
  Gpu = *SelectedDisplay; Disk = *SelectedStorage; Focus = 0; Review = FALSE; Confirmed = FALSE;
  gST->ConIn->Reset (gST->ConIn, FALSE);
  for (;;) {
    if (gST->ConOut != NULL) { gST->ConOut->ClearScreen (gST->ConOut); }
    Print (L"Native CSM VGA Selector - target setup\r\n\r\n");
    if (SavedTargetMissing) { Print (L"Saved target unavailable. Review replacement choices carefully.\r\n"); }
    Print (Review ? L"Review the targets to save:\r\n\r\n" : L"Choose the GPU and disk you intend to boot.\r\n\r\n");
    Print (Focus == 0 ? L"> GPU:  " : L"  GPU:  ");
    if (Gpu < DisplayCount) {
      PciNamesLookup (Displays[Gpu].Vendor, Displays[Gpu].DeviceId, Name, sizeof (Name));
      Print (L"%a\r\n        ", Name);
      Print (L"candidate %u  %04x:%02x:%02x.%x  PCI %04x:%04x\r\n",
        (UINT32)Gpu + 1, Displays[Gpu].Address.Segment, Displays[Gpu].Address.Bus,
        Displays[Gpu].Address.Device, Displays[Gpu].Address.Function,
        Displays[Gpu].Vendor, Displays[Gpu].DeviceId);
      Print (L"        %s\r\n", Displays[Gpu].GopAssociated ? L"Current GOP display" : L"Secondary display");
    } else { Print (L"No eligible legacy VGA adapter found\r\n"); }
    Print (Focus == 1 ? L"> Disk: " : L"  Disk: ");
    if (Disk < StorageCount) {
      Print (L"candidate %u  %a\r\n", (UINT32)Disk + 1, Storage[Disk].Description);
      Print (L"        controller %04x:%02x:%02x.%x\r\n",
        Storage[Disk].Address.Segment, Storage[Disk].Address.Bus,
        Storage[Disk].Address.Device, Storage[Disk].Address.Function);
    } else { Print (L"No eligible legacy boot disk found\r\n"); }
    if (Disk < StorageCount && Storage[Disk].FirmwareStatusKnown) {
      Print (L"        Firmware status=%04x priority=%04x; preflight checks still apply.\r\n",
        Storage[Disk].FirmwareStatus, Storage[Disk].FirmwarePriority);
      if (Storage[Disk].FirmwareStatus == 0) {
        Print (L"        Firmware left status unset; handler validation runs before boot.\r\n");
      } else if (!(Storage[Disk].FirmwareStatus & 0x100) ||
                 (Storage[Disk].FirmwareStatus & 0x200) ||
                 ((Storage[Disk].FirmwareStatus >> 10) & 3) == 0 ||
                 ((Storage[Disk].FirmwareStatus >> 10) & 3) == 3 ||
                 Storage[Disk].FirmwarePriority >= 0xfffc) {
        Print (L"        Firmware boot warning: your selection requests an attempt anyway.\r\n");
      }
    }
    for (Index = 0; Index < StorageCount; ++Index) {
      if (Storage[Index].IdentityAmbiguous) {
        Print (L"Some disk names are ambiguous on the same controller; preflight needs a unique identity.\r\n");
        break;
      }
    }
    Conflict = FALSE;
    if (Review) {
      Conflict = ShowRetainedSettings (RetainedSettings,
        Gpu < DisplayCount ? &Displays[Gpu] : NULL,
        Disk < StorageCount ? &Storage[Disk] : NULL);
    }
    Ready = Gpu < DisplayCount && Disk < StorageCount && Displays[Gpu].Eligible && Storage[Disk].Eligible;
    if (!Ready) { Print (L"\r\nCannot save a boot-ready INI without both eligible targets.\r\n"); }
    if (Review && Conflict && !Confirmed) {
      Print (L"Press A to acknowledge retained-policy warnings before saving.\r\n");
    }
    Print (Review ? L"\r\nEnter: save and boot   Esc: return to choices\r\n" :
      L"\r\nUp/Down or Tab: choose field\r\nLeft/Right: change candidate\r\nEnter: review   Esc: cancel without saving\r\n");
    do {
      Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Event);
      if (EFI_ERROR (Status)) { return Status; }
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    } while (Status == EFI_NOT_READY);
    if (EFI_ERROR (Status)) { return Status; }
    if (Key.ScanCode == SCAN_ESC || Key.UnicodeChar == 0x1b) {
      if (Review) { Review = FALSE; Confirmed = FALSE; continue; }
      return EFI_ABORTED;
    }
    if (Review && (Key.UnicodeChar == L'a' || Key.UnicodeChar == L'A')) {
      Confirmed = TRUE;
      continue;
    }
    if (Key.UnicodeChar == '\r') {
      if (!Ready) { continue; }
      if (!Review) { Review = TRUE; continue; }
      if (Conflict && !Confirmed) { continue; }
      *SelectedDisplay = Gpu; *SelectedStorage = Disk;
      for (Index = 0; Index < DisplayCount; ++Index) { Displays[Index].Selected = Index == Gpu; }
      for (Index = 0; Index < StorageCount; ++Index) { Storage[Index].Selected = Index == Disk; }
      return EFI_SUCCESS;
    }
    if (Review) { continue; }
    if (Key.ScanCode == SCAN_UP || Key.ScanCode == SCAN_DOWN || Key.UnicodeChar == '\t') { Focus ^= 1; }
    if (Key.ScanCode == SCAN_LEFT || Key.ScanCode == SCAN_RIGHT) {
      if (Focus == 0) { Gpu = NextDisplay (Displays, DisplayCount, Gpu, Key.ScanCode == SCAN_LEFT); }
      else { Disk = NextStorage (Storage, StorageCount, Disk, Key.ScanCode == SCAN_LEFT); }
    }
  }
}

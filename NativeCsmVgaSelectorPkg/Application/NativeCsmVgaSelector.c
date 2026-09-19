/** @file Shared strict configuration dispatcher for Native CSM VGA Selector 1.2. */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "AppFile.h"
#include "ProbeConfig.h"
#include "ConfigRecovery.h"
#include "NativeCsmVgaBoot.h"
#include "Marker.h"
#include "PciNames.h"
#include "NativeCsmVgaProbe.h"
#include "BootReport.h"
#include "BootCountdown.h"
#include "RomRecovery.h"

/* Return from discovery and release all of its resources before entering Boot.
   Reload the verified on-disk file, including its retained policies and marker. */
STATIC
__attribute__((noinline))
EFI_STATUS
RunSetupAndReload (EFI_HANDLE ImageHandle, PROBE_CONFIG *Config)
{
  APP_FILE_CONTEXT Files;
  EFI_STATUS Status, CloseStatus;
  BOOLEAN BootReady = FALSE;

  Status = NativeCsmVgaProbeRun (ImageHandle, Config, &BootReady);
  ProbeConfigEndEdit ();
  if (EFI_ERROR (Status) || !BootReady) { return Status; }

  ZeroMem (&Files, sizeof (Files));
  Status = AppFileInitialize (ImageHandle, &Files);
  if (!EFI_ERROR (Status)) {
    Status = ProbeConfigLoad (&Files, NULL, Config);
    CloseStatus = AppFileClose (&Files);
    if (!EFI_ERROR (Status)) { Status = CloseStatus; }
  }
  if (!EFI_ERROR (Status) && (Config->Probe || Config->FirstRun)) {
    Status = EFI_COMPROMISED_DATA;
  }
  if (EFI_ERROR (Status)) {
    Print (L"Saved Config.ini could not be reloaded for boot: %r\r\n", Status);
    ConfigWaitForReturn (Status);
  }
  return Status;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  APP_FILE_CONTEXT  Files;
  PROBE_CONFIG      Config;
  EFI_STATUS        Status;
  EFI_STATUS        MarkerStatus;
  BOOLEAN           EditRequested;

  (VOID)SystemTable;
  BootReportReset ();
  BootCountdownPrepare (NULL);
  RomRecoveryReset ();
  BootReportSetStage (L"Opening configuration volume");
  ZeroMem (&Files, sizeof (Files));
  ZeroMem (&Config, sizeof (Config));
  Status = BootReportDisableWatchdog ();
  if (EFI_ERROR (Status)) {
    BootReportSetStage (L"Disabling inherited firmware watchdog");
    BootReportHold (Status);
    return Status;
  }
  Status = AppFileInitialize (ImageHandle, &Files);
  if (EFI_ERROR (Status)) {
    Print (L"Config.ini parsing/opening failed: %r\r\n", Status);
    ConfigWaitForReturn (Status);
    return Status;
  }
  Status = ProbeConfigLoad (&Files, NULL, &Config);
  if (EFI_ERROR (Status) && Status != EFI_ABORTED) {
    Status = ConfigOfferFreshSetup (&Files, Status);
    if (!EFI_ERROR (Status)) { Status = ProbeConfigLoad (&Files, NULL, &Config); }
  }
  MarkerStatus = AppFileClose (&Files);
  if (EFI_ERROR (MarkerStatus)) {
    BootReportSetStage (L"Closing configuration volume");
    BootReportHold (MarkerStatus);
    return EFI_ERROR (Status) ? Status : MarkerStatus;
  }
  if (EFI_ERROR (Status)) { return Status; }
  PciNamesLoad (ImageHandle);
  for (;;) {
    if (Config.Probe) {
      Status = RunSetupAndReload (ImageHandle, &Config);
      if (EFI_ERROR (Status) || Config.Probe) { break; }
    }
    BootCountdownPrepare (&Config.TargetPci);
    if (Config.AutoBoot) {
      Status = BootSetupWindow (&EditRequested);
      if (EFI_ERROR (Status)) {
        if (Status != EFI_ABORTED) { BootReportHold (Status); }
        break;
      }
      if (EditRequested) {
        ProbeConfigBeginEdit ();
        Config.Probe = TRUE;
        Config.DiscoveryMode = TRUE;
        Config.FirstRun = TRUE;
        continue;
      }
    }
    /* Resolve the display name and release the database before the sensitive
       Boot frame. A separate five-second switch countdown follows preflight. */
    PciNamesRelease ();
    BootReportSetStage (L"Opening boot volume");
    Status = NativeCsmVgaBootRun (ImageHandle, &Config);
    MarkerStatus = MarkerRestore ();
    if (EFI_ERROR (MarkerStatus)) {
      Print (L"Menu marker restoration failed: %r; inspect it before retrying.\r\n", MarkerStatus);
      if (!EFI_ERROR (Status)) {
        BootReportSetStage (L"Restoring boot-menu marker");
        Status = MarkerStatus;
      }
      BootReportObserve (L"NCV_BOOT_FAIL_MARKER_RESTORE");
    }
    if (BootCountdownTakeCancel (Status, MarkerStatus)) { break; }
    if (!BootCountdownTakeEdit (Status, MarkerStatus)) {
      if (!RomRecoveryOffer (ImageHandle, Status, MarkerStatus)) { BootReportHold (Status); }
      if (!BootCountdownTakeEdit (Status, MarkerStatus)) { break; }
    }
    /* Every owning boot frame and its resources have returned before editing.
       Saving reloads the configuration and runs a completely fresh preflight. */
    BootReportReset ();
    RomRecoveryReset ();
    ProbeConfigBeginEdit ();
    Config.Probe = TRUE;
    Config.DiscoveryMode = TRUE;
    Config.FirstRun = TRUE;
    PciNamesLoad (ImageHandle);
  }
  PciNamesRelease ();
  return Status;
}

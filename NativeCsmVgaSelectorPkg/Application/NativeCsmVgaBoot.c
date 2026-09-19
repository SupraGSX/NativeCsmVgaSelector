/** @file
  Configurable native CSM boot execution operation driven only by a validated runtime plan.
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "AppFile.h"
#include "Log.h"
#include "RuntimePlan.h"
#include "NativeCsmVgaBoot.h"
#include "BootReport.h"
#include "MemoryMap.h"
#include "LegacyDispatch.h"
#include "Marker.h"
#include "BootCountdown.h"

#define NATIVECsmVGA_AUTORUN                    1
#define NATIVECsmVGA_SINGLE_LEGACY_BOOT_CALL    1
#define NATIVECsmVGA_NO_SEABIOS                 1
#define NATIVECsmVGA_NO_INSTALL_PCI_ROM         1

#define LEGACY_VGA_SHADOW_BASE          0xC0000U
#define LEGACY_VGA_SHADOW_SIZE          0x10000U
#define LEGACY_FIRMWARE_BASE            0xE0000U
#define LEGACY_FIRMWARE_SIZE            0x20000U

#define SHA256_DIGEST_SIZE              32U
#define PCI_COMMAND_OFFSET              0x04U
#define PCI_BRIDGE_CONTROL_OFFSET       0x3EU
#define PCI_BRIDGE_VGA_ENABLE           BIT3
#define PCI_COMMAND_IO                  BIT0
#define PCI_COMMAND_MEMORY              BIT1
#define PCI_COMMAND_BUS_MASTER          BIT2

STATIC_ASSERT (
  sizeof (EFI_DISPATCH_OPROM_TABLE) == 15,
  "Framework dispatch table ABI"
  );
STATIC_ASSERT (
  OFFSET_OF (
    EFI_COMPATIBILITY16_TABLE,
    Compatibility16CallSegment
    ) == 0x0C,
  "Compatibility16 ABI"
  );
STATIC_ASSERT (
  OFFSET_OF (EFI_COMPATIBILITY16_TABLE, LastPciBus) == 0x52,
  "Compatibility16 ABI"
  );

STATIC
UINT32
RotateRight32 (
  IN UINT32  Value,
  IN UINT32  Count
  )
{
  return (Value >> Count) | (Value << (32U - Count));
}

STATIC
VOID
Sha256Block (
  IN OUT UINT32       State[8],
  IN CONST UINT8      Block[64]
  )
{
  STATIC CONST UINT32  Constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  };
  UINT32  Words[64];
  UINT32  A;
  UINT32  B;
  UINT32  C;
  UINT32  D;
  UINT32  E;
  UINT32  F;
  UINT32  G;
  UINT32  H;
  UINT32  Temporary1;
  UINT32  Temporary2;
  UINTN   Index;

  for (Index = 0; Index < 16U; ++Index) {
    Words[Index] =
      ((UINT32)Block[Index * 4U] << 24) |
      ((UINT32)Block[Index * 4U + 1U] << 16) |
      ((UINT32)Block[Index * 4U + 2U] << 8) |
      Block[Index * 4U + 3U];
  }

  for (; Index < 64U; ++Index) {
    Words[Index] =
      (RotateRight32 (Words[Index - 2U], 17U) ^
       RotateRight32 (Words[Index - 2U], 19U) ^
       (Words[Index - 2U] >> 10)) +
      Words[Index - 7U] +
      (RotateRight32 (Words[Index - 15U], 7U) ^
       RotateRight32 (Words[Index - 15U], 18U) ^
       (Words[Index - 15U] >> 3)) +
      Words[Index - 16U];
  }

  A = State[0];
  B = State[1];
  C = State[2];
  D = State[3];
  E = State[4];
  F = State[5];
  G = State[6];
  H = State[7];
  for (Index = 0; Index < 64U; ++Index) {
    Temporary1 =
      H +
      (RotateRight32 (E, 6U) ^
       RotateRight32 (E, 11U) ^
       RotateRight32 (E, 25U)) +
      ((E & F) ^ ((~E) & G)) +
      Constants[Index] +
      Words[Index];
    Temporary2 =
      (RotateRight32 (A, 2U) ^
       RotateRight32 (A, 13U) ^
       RotateRight32 (A, 22U)) +
      ((A & B) ^ (A & C) ^ (B & C));
    H = G;
    G = F;
    F = E;
    E = D + Temporary1;
    D = C;
    C = B;
    B = A;
    A = Temporary1 + Temporary2;
  }

  State[0] += A;
  State[1] += B;
  State[2] += C;
  State[3] += D;
  State[4] += E;
  State[5] += F;
  State[6] += G;
  State[7] += H;
}

STATIC
BOOLEAN
HashRange (
  IN  CONST VOID  *Data,
  IN  UINTN       Size,
  OUT UINT8       Digest[SHA256_DIGEST_SIZE]
  )
{
  UINT32       State[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
  };
  CONST UINT8  *Walker;
  UINT8        Tail[128];
  UINTN        Remaining;
  UINTN        TailBytes;
  UINTN        Index;
  UINT64       BitCount;

  //
  // Physical address 0 is the start of the IVT, not a missing buffer.
  //
  if (Digest == NULL) {
    return FALSE;
  }

  Walker    = Data;
  Remaining = Size;
  BitCount  = (UINT64)Size * 8U;
  while (Remaining >= 64U) {
    Sha256Block (State, Walker);
    Walker    += 64U;
    Remaining -= 64U;
  }

  ZeroMem (Tail, sizeof (Tail));
  CopyMem (Tail, Walker, Remaining);
  Tail[Remaining] = 0x80U;
  TailBytes = (Remaining < 56U) ? 64U : 128U;
  Tail[TailBytes - 8U] = (UINT8)(BitCount >> 56);
  Tail[TailBytes - 7U] = (UINT8)(BitCount >> 48);
  Tail[TailBytes - 6U] = (UINT8)(BitCount >> 40);
  Tail[TailBytes - 5U] = (UINT8)(BitCount >> 32);
  Tail[TailBytes - 4U] = (UINT8)(BitCount >> 24);
  Tail[TailBytes - 3U] = (UINT8)(BitCount >> 16);
  Tail[TailBytes - 2U] = (UINT8)(BitCount >> 8);
  Tail[TailBytes - 1U] = (UINT8)BitCount;
  Sha256Block (State, Tail);
  if (TailBytes == 128U) {
    Sha256Block (State, Tail + 64U);
  }

  for (Index = 0; Index < 8U; ++Index) {
    Digest[Index * 4U]     = (UINT8)(State[Index] >> 24);
    Digest[Index * 4U + 1] = (UINT8)(State[Index] >> 16);
    Digest[Index * 4U + 2] = (UINT8)(State[Index] >> 8);
    Digest[Index * 4U + 3] = (UINT8)State[Index];
  }

  return TRUE;
}

STATIC
VOID
Phase (
  IN APP_LOGGER    *Logger,
  IN CONST CHAR16  *Text
  )
{
  LogPrint (Logger, L"%s\r\n", Text);
  BootReportLogFailure (LogCommitAndReopen (Logger));
}

STATIC VOID EFIAPI __attribute__((noinline))
Capture (APP_LOGGER *Logger, CONST CHAR16 *Name, UINTN Base, UINTN Size)
{
  UINT8 Digest[SHA256_DIGEST_SIZE];
  CHAR16 Hex[SHA256_DIGEST_SIZE * 2 + 1];
  CONST CHAR16 *Digits = L"0123456789abcdef";
  UINTN Index;
  HashRange ((CONST VOID *)Base, Size, Digest);
  for (Index = 0; Index < sizeof (Digest); ++Index) {
    Hex[Index * 2] = Digits[Digest[Index] >> 4];
    Hex[Index * 2 + 1] = Digits[Digest[Index] & 15];
  }
  Hex[sizeof (Digest) * 2] = 0;
  LogPrint (Logger, L"%s base=0x%05lx bytes=0x%lx SHA256=%s\r\n",
    Name, (UINT64)Base, (UINT64)Size, Hex);
}

STATIC
EFI_STATUS
ReadBridgeControl (
  IN  NATIVE_CSM_VGA_PLAN_BRIDGE  *Bridge,
  OUT UINT16                      *Value
  )
{
  if ((Bridge == NULL) || (Value == NULL) ||
      (Bridge->Record.PciIo == NULL) ||
      (Bridge->Record.PciIo->Pci.Read == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  return Bridge->Record.PciIo->Pci.Read (
                                    Bridge->Record.PciIo,
                                    EfiPciIoWidthUint16,
                                    PCI_BRIDGE_CONTROL_OFFSET,
                                    1,
                                    Value
                                    );
}

STATIC
BOOLEAN
RouteBridgeStep (
  IN     APP_LOGGER                    *Logger,
  IN     UINTN                         Step,
  IN OUT NATIVE_CSM_VGA_PLAN_BRIDGE    *Bridge,
  IN     UINT16                        Requested
  )
{
  EFI_STATUS  WriteStatus;
  EFI_STATUS  ReadStatus;
  UINT16      Actual;
  CHAR16      Marker[128];
  BOOLEAN     Verified;

  UnicodeSPrint (
    Marker,
    sizeof (Marker),
    L"NCV_BOOT_ROUTE_STEP_%02u_%02x_%02x_%u_BEGIN",
    (UINT32)Step,
    (UINT32)Bridge->Record.Bus,
    (UINT32)Bridge->Record.Device,
    (UINT32)Bridge->Record.Function
  );
  Phase (Logger, Marker);
  WriteStatus = Bridge->Record.PciIo->Pci.Write (
                                              Bridge->Record.PciIo,
                                              EfiPciIoWidthUint16,
                                              PCI_BRIDGE_CONTROL_OFFSET,
                                              1,
                                              &Requested
                                              );
  Actual = 0;
  ReadStatus = ReadBridgeControl (Bridge, &Actual);
  Verified = (BOOLEAN)(
    !EFI_ERROR (WriteStatus) &&
    !EFI_ERROR (ReadStatus) &&
    (Actual == Requested)
    );
  LogPrint (
    Logger,
    L"route step=%u BDF=%04x:%02x:%02x.%x offset=0x%02x "
    L"original=0x%04x mask=0x%04x requested=0x%04x "
    L"write=0x%016lx read=0x%016lx actual=0x%04x expected=0x%04x "
    L"pass=%a\r\n",
    (UINT32)Step,
    (UINT32)Bridge->Record.Segment,
    (UINT32)Bridge->Record.Bus,
    (UINT32)Bridge->Record.Device,
    (UINT32)Bridge->Record.Function,
    PCI_BRIDGE_CONTROL_OFFSET,
    Bridge->OriginalBridgeControl,
    PCI_BRIDGE_VGA_ENABLE,
    Requested,
    (UINT64)WriteStatus,
    (UINT64)ReadStatus,
    Actual,
    Requested,
    Verified ? "yes" : "no"
    );
  LogCommitAndReopen (Logger);
  if (!Verified) {
    LogPrint (
      Logger,
      L"NCV_BOOT_ROUTE_FAILURE_STEP=%u\r\n"
      L"NCV_BOOT_ROUTE_FAILURE_BDF=%04x:%02x:%02x.%x\r\n"
      L"NCV_BOOT_ROUTE_FAILURE_OFFSET=0x%02x\r\n"
      L"NCV_BOOT_ROUTE_FAILURE_WRITE_STATUS=0x%016lx\r\n"
      L"NCV_BOOT_ROUTE_FAILURE_READ_STATUS=0x%016lx\r\n"
      L"NCV_BOOT_ROUTE_FAILURE_EXPECTED=0x%04x\r\n"
      L"NCV_BOOT_ROUTE_FAILURE_ACTUAL=0x%04x\r\n",
      (UINT32)Step,
      (UINT32)Bridge->Record.Segment,
      (UINT32)Bridge->Record.Bus,
      (UINT32)Bridge->Record.Device,
      (UINT32)Bridge->Record.Function,
      PCI_BRIDGE_CONTROL_OFFSET,
      (UINT64)WriteStatus,
      (UINT64)ReadStatus,
      Requested,
      Actual
    );
    LogCommitAndReopen (Logger);
    return FALSE;
  }

  UnicodeSPrint (
    Marker,
    sizeof (Marker),
    L"NCV_BOOT_ROUTE_STEP_%02u_%02x_%02x_%u_PASS",
    (UINT32)Step,
    (UINT32)Bridge->Record.Bus,
    (UINT32)Bridge->Record.Device,
    (UINT32)Bridge->Record.Function
  );
  Phase (Logger, Marker);
  return TRUE;
}

STATIC
UINT16
RequiredEndpointCommand (
  IN CONST NATIVE_CSM_VGA_RUNTIME_PLAN  *Plan
  )
{
  UINT16  Command;

  Command = Plan->Target.OriginalCommand;
  if (Plan->Config.IoDecoding == NativeCsmVgaEndpointOn) { Command |= PCI_COMMAND_IO; }
  if (Plan->Config.IoDecoding == NativeCsmVgaEndpointOff) { Command &= ~PCI_COMMAND_IO; }
  if (Plan->Config.MemoryDecoding == NativeCsmVgaEndpointOn) { Command |= PCI_COMMAND_MEMORY; }
  if (Plan->Config.MemoryDecoding == NativeCsmVgaEndpointOff) { Command &= ~PCI_COMMAND_MEMORY; }
  if (Plan->Config.BusMastering == NativeCsmVgaEndpointOn) { Command |= PCI_COMMAND_BUS_MASTER; }
  if (Plan->Config.BusMastering == NativeCsmVgaEndpointOff) { Command &= ~PCI_COMMAND_BUS_MASTER; }

  return Command;
}

STATIC
__attribute__((noinline))
EFI_STATUS
ValidateBootReadiness (
  IN APP_LOGGER *Logger,
  IN NATIVE_CSM_VGA_RUNTIME_PLAN *Plan
  )
{
  if ((Logger == NULL) || (Plan == NULL) || !Plan->Complete ||
      !Plan->ValidationCompleted || !Plan->ValidationPassed) {
    return EFI_INVALID_PARAMETER;
  }
  if (Plan->Config.RequireReferenceSnapshotMatch) { return EFI_UNSUPPORTED; }
  return LegacyBootBootTargetPrepare (Logger, &Plan->BootTarget);
}

STATIC
BOOLEAN
RouteApply (
  IN     APP_LOGGER                    *Logger,
  IN OUT NATIVE_CSM_VGA_RUNTIME_PLAN   *Plan
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  UINTN       Step;
  UINT16      Requested;
  UINT16      ActualCommand;
  UINT64      RequiredAttributes;

  Step = 1;
  for (Index = 0; Index < Plan->ActiveExclusiveCount; ++Index) {
    Requested = (UINT16)(
      Plan->ActiveExclusivePath[Index].OriginalBridgeControl &
      ~PCI_BRIDGE_VGA_ENABLE
      );
    Plan->ActiveExclusivePath[Index].WriteAttempted = TRUE;
    Plan->RouteDirty = TRUE;
    if (!RouteBridgeStep (
           Logger,
           Step++,
           &Plan->ActiveExclusivePath[Index],
           Requested
           ))
    {
      return FALSE;
    }

    Plan->ActiveExclusivePath[Index].WriteAttempted = TRUE;
    Plan->ActiveExclusivePath[Index].WriteApplied   = TRUE;
    Plan->RouteDirty                                = TRUE;
  }

  //
  // Shared bridges are never disabled.  If firmware left one clear, only its
  // VGA-forwarding bit is enabled for the target path.
  //
  for (Index = 0; Index < Plan->SharedPathCount; ++Index) {
    Requested = (UINT16)(
      Plan->SharedPath[Index].OriginalBridgeControl |
      PCI_BRIDGE_VGA_ENABLE
      );
    if (Requested == Plan->SharedPath[Index].OriginalBridgeControl) {
      ++Step;
      continue;
    }

    Plan->SharedPath[Index].WriteAttempted = TRUE;
    Plan->RouteDirty = TRUE;
    if (!RouteBridgeStep (
           Logger,
           Step++,
           &Plan->SharedPath[Index],
           Requested
           ))
    {
      return FALSE;
    }

    Plan->SharedPath[Index].WriteAttempted = TRUE;
    Plan->SharedPath[Index].WriteApplied   = TRUE;
    Plan->RouteDirty                       = TRUE;
  }

  for (Index = 0; Index < Plan->TargetExclusiveCount; ++Index) {
    Requested = (UINT16)(
      Plan->TargetExclusivePath[Index].OriginalBridgeControl |
      PCI_BRIDGE_VGA_ENABLE
      );
    Plan->TargetExclusivePath[Index].WriteAttempted = TRUE;
    Plan->RouteDirty = TRUE;
    if (!RouteBridgeStep (
           Logger,
           Step++,
           &Plan->TargetExclusivePath[Index],
           Requested
           ))
    {
      return FALSE;
    }

    Plan->TargetExclusivePath[Index].WriteAttempted = TRUE;
    Plan->TargetExclusivePath[Index].WriteApplied   = TRUE;
    Plan->RouteDirty                                = TRUE;
  }

  Phase (Logger, L"NCV_BOOT_ENDPOINT_TARGET_LEGACY_VGA_IO_MEMORY_BEGIN");
  Requested = RequiredEndpointCommand (Plan);
  RequiredAttributes = 0;
  if (Plan->Config.IoDecoding != NativeCsmVgaEndpointIgnore) { RequiredAttributes |= PCI_COMMAND_IO; }
  if (Plan->Config.MemoryDecoding != NativeCsmVgaEndpointIgnore) { RequiredAttributes |= PCI_COMMAND_MEMORY; }
  if (Plan->Config.BusMastering != NativeCsmVgaEndpointIgnore) { RequiredAttributes |= PCI_COMMAND_BUS_MASTER; }
  Plan->EndpointWriteAttempted = TRUE;
  Plan->RouteDirty = TRUE;
  Status = Plan->Target.PciIo->Pci.Write (
                                     Plan->Target.PciIo,
                                     EfiPciIoWidthUint16,
                                     PCI_COMMAND_OFFSET,
                                     1,
                                     &Requested
                                     );
  ActualCommand = 0;
  if (EFI_ERROR (Plan->Target.PciIo->Pci.Read (Plan->Target.PciIo, EfiPciIoWidthUint16, PCI_COMMAND_OFFSET, 1, &ActualCommand))) { Status = EFI_DEVICE_ERROR; }
  LogPrint (
    Logger,
    L"endpoint-policy io=%u memory=%u busmaster=%u original=0x%04x write-mask=0x%04x requested=0x%04x actual=0x%04x\r\n",
    (UINT32)Plan->Config.IoDecoding, (UINT32)Plan->Config.MemoryDecoding,
    (UINT32)Plan->Config.BusMastering, Plan->Target.OriginalCommand,
    (UINT16)RequiredAttributes, Requested, ActualCommand
    );
  LogPrint (
    Logger,
    L"endpoint-enforcement io=%s memory=%s busmaster=%s\r\n",
    ((Plan->Config.IoDecoding == NativeCsmVgaEndpointIgnore) ||
     (((ActualCommand & PCI_COMMAND_IO) != 0) == (Plan->Config.IoDecoding == NativeCsmVgaEndpointOn))) ? L"pass" : L"fail",
    ((Plan->Config.MemoryDecoding == NativeCsmVgaEndpointIgnore) ||
     (((ActualCommand & PCI_COMMAND_MEMORY) != 0) == (Plan->Config.MemoryDecoding == NativeCsmVgaEndpointOn))) ? L"pass" : L"fail",
    ((Plan->Config.BusMastering == NativeCsmVgaEndpointIgnore) ||
     (((ActualCommand & PCI_COMMAND_BUS_MASTER) != 0) == (Plan->Config.BusMastering == NativeCsmVgaEndpointOn))) ? L"pass" : L"fail"
    );
  Plan->EndpointWriteApplied = (BOOLEAN)(
    ActualCommand != Plan->Target.OriginalCommand
    );
  /* Preserve the attempt even if read-back fails. */
  if (Plan->EndpointWriteApplied) {
    Plan->RouteDirty = TRUE;
  }

  LogCommitAndReopen (Logger);
  if (EFI_ERROR (Status) || ((ActualCommand & ~((UINT16)RequiredAttributes)) != (Plan->Target.OriginalCommand & ~((UINT16)RequiredAttributes))) ||
      ((Plan->Config.IoDecoding == NativeCsmVgaEndpointOn) && ((ActualCommand & PCI_COMMAND_IO) == 0)) ||
      ((Plan->Config.IoDecoding == NativeCsmVgaEndpointOff) && ((ActualCommand & PCI_COMMAND_IO) != 0)) ||
      ((Plan->Config.MemoryDecoding == NativeCsmVgaEndpointOn) && ((ActualCommand & PCI_COMMAND_MEMORY) == 0)) ||
      ((Plan->Config.MemoryDecoding == NativeCsmVgaEndpointOff) && ((ActualCommand & PCI_COMMAND_MEMORY) != 0)) ||
      ((Plan->Config.BusMastering == NativeCsmVgaEndpointOn) && ((ActualCommand & PCI_COMMAND_BUS_MASTER) == 0)) ||
      ((Plan->Config.BusMastering == NativeCsmVgaEndpointOff) && ((ActualCommand & PCI_COMMAND_BUS_MASTER) != 0)))
  {
    LogPrint (Logger, L"NCV_BOOT_FAIL_TARGET_LEGACY_VGA_IO_MEMORY_ENABLE\r\n");
    LogCommitAndReopen (Logger);
    return FALSE;
  }

  Phase (Logger, L"NCV_BOOT_ENDPOINT_TARGET_LEGACY_VGA_IO_MEMORY_PASS");
  return TRUE;
}

STATIC
BOOLEAN
WriteBridgeControl (
  IN OUT NATIVE_CSM_VGA_PLAN_BRIDGE   *Bridge,
  IN     UINT16                        Value
  )
{
  EFI_STATUS  Status;
  UINT16      Actual;

  if ((Bridge == NULL) || (Bridge->Record.PciIo == NULL) ||
      (Bridge->Record.PciIo->Pci.Write == NULL))
  {
    return FALSE;
  }

  Status = Bridge->Record.PciIo->Pci.Write (
                                         Bridge->Record.PciIo,
                                         EfiPciIoWidthUint16,
                                         PCI_BRIDGE_CONTROL_OFFSET,
                                         1,
                                         &Value
                                         );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  return (BOOLEAN)(
    !EFI_ERROR (ReadBridgeControl (Bridge, &Actual)) &&
    (Actual == Value)
    );
}

STATIC
VOID
RouteRestore (
  IN     APP_LOGGER                    *Logger,
  IN OUT NATIVE_CSM_VGA_RUNTIME_PLAN   *Plan
  )
{
  EFI_STATUS  StepStatus;
  INTN        Index;
  UINTN       Step;
    UINT16      Command;
  UINT16      Actual;

  if (Plan->EndpointWriteAttempted) {
    Phase (Logger, L"NCV_BOOT_ENDPOINT_ROLLBACK_BEGIN");
    StepStatus = Plan->Target.PciIo->Pci.Write (
                                           Plan->Target.PciIo,
                                           EfiPciIoWidthUint16,
                                           PCI_COMMAND_OFFSET,
                                           1,
                                           &Plan->Target.OriginalCommand
                                           );
    if (EFI_ERROR (StepStatus) ||
        EFI_ERROR (Plan->Target.PciIo->Pci.Read (
                                             Plan->Target.PciIo,
                                             EfiPciIoWidthUint16,
                                             PCI_COMMAND_OFFSET,
                                             1,
                                             &Command
                                             )) ||
        (Command != Plan->Target.OriginalCommand))
    {
      LogPrint (Logger, L"NCV_BOOT_ENDPOINT_ROLLBACK_FAILED\r\n");
      LogCommitAndReopen (Logger);
      return;
    }

    Plan->EndpointWriteApplied   = FALSE;
    Plan->EndpointWriteAttempted = FALSE;
    Phase (Logger, L"NCV_BOOT_ENDPOINT_ROLLBACK_PASS");
  }

  Phase (Logger, L"NCV_BOOT_ROUTE_ROLLBACK_BEGIN");
  for (Index = (INTN)Plan->TargetExclusiveCount - 1;
       Index >= 0;
       --Index)
  {
    if (!Plan->TargetExclusivePath[Index].WriteAttempted) {
      continue;
    }

    Step =
      Plan->ActiveExclusiveCount +
      Plan->SharedPathCount +
      (UINTN)Index +
      1U;
    Actual = 0;
    if (!WriteBridgeControl (
           &Plan->TargetExclusivePath[Index],
           Plan->TargetExclusivePath[Index].OriginalBridgeControl
           ) ||
        EFI_ERROR (ReadBridgeControl (
                     &Plan->TargetExclusivePath[Index],
                     &Actual
                     )) ||
        (Actual !=
         Plan->TargetExclusivePath[Index].OriginalBridgeControl))
    {
      LogPrint (Logger, L"NCV_BOOT_ROUTE_ROLLBACK_FAILED\r\n");
      LogCommitAndReopen (Logger);
      return;
    }

    Plan->TargetExclusivePath[Index].WriteApplied   = FALSE;
    Plan->TargetExclusivePath[Index].WriteAttempted = FALSE;
    LogPrint (
      Logger,
      L"NCV_BOOT_ROUTE_ROLLBACK_STEP_%u_PASS\r\n",
      (UINT32)Step
      );
    LogCommitAndReopen (Logger);
  }

  for (Index = (INTN)Plan->SharedPathCount - 1; Index >= 0; --Index) {
    if (!Plan->SharedPath[Index].WriteAttempted) {
      continue;
    }

    Step = Plan->ActiveExclusiveCount + (UINTN)Index + 1U;
    Actual = 0;
    if (!WriteBridgeControl (
           &Plan->SharedPath[Index],
           Plan->SharedPath[Index].OriginalBridgeControl
           ) ||
        EFI_ERROR (ReadBridgeControl (
                     &Plan->SharedPath[Index],
                     &Actual
                     )) ||
        (Actual != Plan->SharedPath[Index].OriginalBridgeControl))
    {
      LogPrint (Logger, L"NCV_BOOT_ROUTE_ROLLBACK_FAILED\r\n");
      LogCommitAndReopen (Logger);
      return;
    }

    Plan->SharedPath[Index].WriteApplied   = FALSE;
    Plan->SharedPath[Index].WriteAttempted = FALSE;
    LogPrint (
      Logger,
      L"NCV_BOOT_ROUTE_ROLLBACK_STEP_%u_PASS\r\n",
      (UINT32)Step
      );
    LogCommitAndReopen (Logger);
  }

  for (Index = (INTN)Plan->ActiveExclusiveCount - 1;
       Index >= 0;
       --Index)
  {
    if (!Plan->ActiveExclusivePath[Index].WriteAttempted) {
      continue;
    }

    Step   = (UINTN)Index + 1U;
    Actual = 0;
    if (!WriteBridgeControl (
           &Plan->ActiveExclusivePath[Index],
           Plan->ActiveExclusivePath[Index].OriginalBridgeControl
           ) ||
        EFI_ERROR (ReadBridgeControl (
                     &Plan->ActiveExclusivePath[Index],
                     &Actual
                     )) ||
        (Actual !=
         Plan->ActiveExclusivePath[Index].OriginalBridgeControl))
    {
      LogPrint (Logger, L"NCV_BOOT_ROUTE_ROLLBACK_FAILED\r\n");
      LogCommitAndReopen (Logger);
      return;
    }

    Plan->ActiveExclusivePath[Index].WriteApplied   = FALSE;
    Plan->ActiveExclusivePath[Index].WriteAttempted = FALSE;
    LogPrint (
      Logger,
      L"NCV_BOOT_ROUTE_ROLLBACK_STEP_%u_PASS\r\n",
      (UINT32)Step
      );
    LogCommitAndReopen (Logger);
  }

  Plan->RouteDirty = FALSE;
  Phase (Logger, L"NCV_BOOT_ROUTE_ROLLBACK_COMPLETE");
}

EFI_STATUS
EFIAPI
NativeCsmVgaBootRun (
  IN EFI_HANDLE          ImageHandle,
  IN CONST PROBE_CONFIG  *Config
  )
{
  APP_FILE_CONTEXT                 Files;
  APP_LOGGER                       Logger;
  NATIVE_CSM_VGA_RUNTIME_PLAN      Plan;
  EFI_STATUS                       Status;
  EFI_STATUS                       CleanupStatus;
  UINT32                           Granularity;
  EFI_PHYSICAL_ADDRESS             DispatchAddress;
  EFI_DISPATCH_OPROM_TABLE         *DispatchTable;
  EFI_IA32_REGISTER_SET            Registers;
  BOOLEAN                          FarCallResult;
  UINT8                            FirmwareBefore[SHA256_DIGEST_SIZE];
  UINT8                            FirmwareAfter[SHA256_DIGEST_SIZE];
  UINT16                           Int10Offset;
  UINT16                           Int10Segment;

  ZeroMem (&Files, sizeof (Files));
  ZeroMem (&Logger, sizeof (Logger));
  ZeroMem (&Plan, sizeof (Plan));

  Status = AppFileInitialize (ImageHandle, &Files);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LogInitialize (
             &Logger,
             &Files,
             TRUE,
             L"NativeCsmVgaBoot.log"
             );
  if (EFI_ERROR (Status)) {
    BootReportLogFailure (Status);
    AppFileClose (&Files);
    return Status;
  }

  Status = LogBeginDurableFileFirst (&Logger);
  if (EFI_ERROR (Status)) {
    BootReportLogFailure (Status);
    LogClose (&Logger);
    AppFileClose (&Files);
    return Status;
  }

  Phase (&Logger, L"NCV_BOOT_START");
  LogPrint (&Logger, L"NCV_BUILD=1.2.2-audit-fixes.1 base=9104ae943c53\r\n");
  LogPrint (&Logger, L"NCV_BOOT_AUTORUN_ENABLED\r\n");
  LogPrint (&Logger, L"NCV_MODE=BOOT\r\n");
  Status = NativeCsmVgaRuntimePlanBuild (&Files, &Logger, Config, &Plan);
  if (EFI_ERROR (Status)) {
    goto PreflightFailure;
  }

  Status = ValidateBootReadiness (&Logger, &Plan);
  if (EFI_ERROR (Status)) {
    goto PreflightFailure;
  }

  Capture (&Logger, L"IVT", 0, 0x400U);
  Capture (&Logger, L"BDA", 0x400U, 0x100U);
  Capture (
    &Logger,
    L"C0000-CFFFF",
    LEGACY_VGA_SHADOW_BASE,
    LEGACY_VGA_SHADOW_SIZE
    );
  Capture (
    &Logger,
    L"E0000-FFFFF",
    LEGACY_FIRMWARE_BASE,
    LEGACY_FIRMWARE_SIZE
    );
  if ((Plan.InitialInt10Segment < 0xC000U) ||
      (Plan.InitialInt10Segment > 0xF000U))
  {
    Status = EFI_COMPROMISED_DATA;
    LogPrint (
      &Logger,
      L"Initial INT10 vector is outside legacy video/firmware memory\r\n"
      );
    goto PreflightFailure;
  }

  Phase (&Logger, L"NCV_BOOT_PREFLIGHT_COMPLETE");
  if (!Plan.Config.AutoBoot) {
    LogPrint (
      &Logger,
      L"AutoBoot=false; validated runtime plan returned read-only without "
      L"hardware mutation\r\n"
      );
    Status = EFI_SUCCESS;
    goto NormalReturn;
  }

  Status = LegacyDispatchReserve ();
  if (EFI_ERROR (Status)) { goto PreflightFailure; }

  LogPrint (
    &Logger,
    L"NCV_BOOT_PREFLIGHT_PASSED_AUTOMATICALLY_PROCEEDING\r\n"
    );
  Phase (&Logger, L"NCV_BOOT_AUTOMATIC_EXECUTION_BEGIN");
  Status = LogFileBarrier (&Logger);
  if (EFI_ERROR (Status)) {
    goto PreflightFailure;
  }
  Status = BootCountdownWait (&Logger);
  if (EFI_ERROR (Status)) { goto NormalReturn; }
  Status = MarkerApply ();
  if (EFI_ERROR (Status)) {
    LogPrint (&Logger, L"Menu marker handoff failed: %r\r\n", Status);
    goto PreflightFailure;
  }
  Phase (&Logger, L"NCV_BOOT_ACTIVE_GOP_DISCONNECT_BEGIN");
  Status = LogFileBarrier (&Logger);
  if (EFI_ERROR (Status)) { goto PreflightFailure; }
  Status = gBS->DisconnectController (Plan.Active.Handle, NULL, NULL);
  if (EFI_ERROR (Status)) {
    LogPrint (&Logger, L"NCV_BOOT_FAIL_ACTIVE_GOP_DISCONNECT\r\n");
    goto NormalReturn;
  }

  Phase (&Logger, L"NCV_BOOT_ACTIVE_GOP_DISCONNECT_COMPLETE");
  Phase (&Logger, L"NCV_BOOT_VGA_ROUTE_TRANSFER_BEGIN");
  if (!RouteApply (&Logger, &Plan)) {
    RouteRestore (&Logger, &Plan);
    BootReportHalt (&Logger, L"NCV_BOOT_FAIL_ROUTE_TRANSFER", EFI_DEVICE_ERROR);
  }

  Phase (&Logger, L"NCV_BOOT_VGA_ROUTE_TRANSFER_COMPLETE");
  Phase (&Logger, L"NCV_BOOT_LEGACY_REGION_UNLOCK_BEGIN");
  Granularity = 0;
  Status = Plan.LegacyRegion2->UnLock (
                                Plan.LegacyRegion2,
                                LEGACY_VGA_SHADOW_BASE,
                                LEGACY_VGA_SHADOW_SIZE,
                                &Granularity
                                );
  if (EFI_ERROR (Status)) {
    RouteRestore (&Logger, &Plan);
    BootReportHalt (&Logger, L"NCV_BOOT_FAIL_LEGACY_REGION_UNLOCK", Status);
  }

  Phase (&Logger, L"NCV_BOOT_LEGACY_REGION_UNLOCK_COMPLETE");
  Status = MemoryMapValidateBootRanges ();
  if (EFI_ERROR (Status)) { BootReportHalt (&Logger, L"NCV_BOOT_FAIL_LOW_MEMORY_RANGE", Status); }
  HashRange (
    (CONST VOID *)(UINTN)LEGACY_FIRMWARE_BASE,
    LEGACY_FIRMWARE_SIZE,
    FirmwareBefore
    );
  Phase (&Logger, L"NCV_BOOT_TARGET_LEGACY_VGA_ROM_COPY_BEGIN");
  CopyMem (
    (VOID *)(UINTN)LEGACY_VGA_SHADOW_BASE,
    Plan.SelectedRom,
    Plan.SelectedRomSize
    );
  Phase (&Logger, L"NCV_BOOT_TARGET_LEGACY_VGA_ROM_COPY_COMPLETE");
  if ((CompareMem (
         (CONST VOID *)(UINTN)LEGACY_VGA_SHADOW_BASE,
         Plan.SelectedRom,
         Plan.SelectedRomSize
         ) != 0) ||
      !HashRange (
         (CONST VOID *)(UINTN)LEGACY_FIRMWARE_BASE,
         LEGACY_FIRMWARE_SIZE,
         FirmwareAfter
         ) ||
      (CompareMem (
         FirmwareBefore,
         FirmwareAfter,
         SHA256_DIGEST_SIZE
         ) != 0))
  {
    BootReportHalt (&Logger, L"NCV_BOOT_FAIL_TARGET_LEGACY_VGA_ROM_READBACK", EFI_COMPROMISED_DATA);
  }

  Phase (&Logger, L"NCV_BOOT_TARGET_LEGACY_VGA_ROM_READBACK_VERIFIED");
  DispatchAddress = LegacyDispatchAddress ();
  DispatchTable = (EFI_DISPATCH_OPROM_TABLE *)(UINTN)DispatchAddress;
  LegacyDispatchPrepare (&Plan, DispatchTable);
  Phase (&Logger, L"NCV_BOOT_COMPATIBILITY16_DISPATCH_TABLE_READY");

  ZeroMem (&Registers, sizeof (Registers));
  Registers.X.AX = 5;
  Registers.X.ES = (UINT16)(DispatchAddress >> 4);
  Registers.X.BX = (UINT16)(DispatchAddress & 0x0FU);
  Phase (&Logger, L"NCV_BOOT_COMPATIBILITY16_FARCALL_BEGIN");
  FarCallResult = Plan.LegacyBios->FarCall86 (
                                     Plan.LegacyBios,
                                     Plan.Compatibility16.CallSegment,
                                     Plan.Compatibility16.CallOffset,
                                     &Registers,
                                     NULL,
                                     0
                                     );
  Phase (&Logger, L"NCV_BOOT_COMPATIBILITY16_FARCALL_RETURNED");
  Status = MemoryMapValidateBootRanges ();
  if (EFI_ERROR (Status)) { BootReportHalt (&Logger, L"NCV_BOOT_FAIL_LOW_MEMORY_RANGE", Status); }
  HashRange (
    (CONST VOID *)(UINTN)LEGACY_FIRMWARE_BASE,
    LEGACY_FIRMWARE_SIZE,
    FirmwareAfter
    );
  Int10Offset  = ((UINT16 *)(UINTN)(0x10U * 4U))[0];
  Int10Segment = ((UINT16 *)(UINTN)(0x10U * 4U))[1];
  if (FarCallResult ||
      (Registers.X.AX != 0) ||
      (Int10Segment != (LEGACY_VGA_SHADOW_BASE >> 4)) ||
      (Int10Offset >= Plan.SelectedRomSize) ||
      (CompareMem (
         FirmwareBefore,
         FirmwareAfter,
         SHA256_DIGEST_SIZE
         ) != 0))
  {
    BootReportHalt (&Logger, L"NCV_BOOT_FAIL_TARGET_LEGACY_VGA_INT10_OWNERSHIP", EFI_DEVICE_ERROR);
  }

  Phase (&Logger, L"NCV_BOOT_TARGET_LEGACY_VGA_INT10_VERIFIED");
  Phase (&Logger, L"NCV_BOOT_TARGET_BBS_TRANSACTION_BEGIN");
  Status = LegacyBootBootTargetJournalAndValidate (
             &Logger,
             &Plan.BootTarget
             );
  if (!EFI_ERROR (Status)) {
    Status = LegacyBootBootPriorityApply (&Logger, &Plan.BootTarget);
  }

  if (!EFI_ERROR (Status)) {
    Status = LegacyBootBootTargetValidateApplied (
               &Logger,
               &Plan.BootTarget
               );
  }

  if (EFI_ERROR (Status)) {
    LegacyBootBootPriorityRollback (&Logger, &Plan.BootTarget);
    BootReportHalt (&Logger, L"NCV_BOOT_FAIL_BBS_TRANSACTION", Status);
  }

  Phase (&Logger, L"NCV_BOOT_TARGET_BBS_TRANSACTION_COMPLETE");
  Status = LegacyBootBootTargetGetLegacyBootArguments (
             &Plan.BootTarget,
             &Plan.BootOption,
             &Plan.OpaqueLoadOptionsSize,
             &Plan.OpaqueLoadOptions
             );
  if (EFI_ERROR (Status)) {
    BootReportHalt (&Logger, L"NCV_BOOT_FAIL_LEGACY_BOOT_OPTION", Status);
  }

  Phase (&Logger, L"NCV_BOOT_LEGACY_BOOT_OPTION_READY");
  Phase (&Logger, L"NCV_BOOT_FINAL_PREBOOT_VALIDATION_COMPLETE");
  Phase (&Logger, L"NCV_BOOT_NATIVE_LEGACY_BOOT_BEGIN");
  LogPrint (
    &Logger,
    L"Selected disk native boot attempt: user choice overrides any firmware boot warnings recorded above.\r\n"
    L"NCV_BOOT_PENDING_IF_NO_RETURN=NATIVE_LEGACY_BOOT_DID_NOT_RETURN\r\n"
    L"NCV_BOOT_IRREVERSIBLE_NATIVE_CSM_LEGACY_BOOT_BOUNDARY\r\n"
    );
  Status = LogCommitAndClose (&Logger);
  if (EFI_ERROR (Status)) {
    /* ROM dispatch has happened: do not return to firmware or boot onward. */
    BootReportHalt (&Logger, L"NCV_BOOT_FAIL_FINAL_LOG_COMMIT", Status);
  }
  BootReportDiskBootAttempt ();
  Status = Plan.LegacyBios->LegacyBoot (
                              Plan.LegacyBios,
                              Plan.BootOption,
                              Plan.OpaqueLoadOptionsSize,
                              Plan.OpaqueLoadOptions
                              );
  LogReopenAppend (&Logger);
  LogPrintBestEffort (
    &Logger,
    L"NCV_BOOT_NATIVE_LEGACY_BOOT_RETURNED\r\n"
    L"NCV_BOOT_FAIL_LEGACY_BOOT_RETURNED status=0x%016lx\r\n",
    (UINT64)Status
    );
  BootReportHalt (&Logger, L"NCV_BOOT_FAIL_LEGACY_BOOT_RETURNED", Status);

PreflightFailure:
  LogPrint (
    &Logger,
    L"NCV_BOOT_FAIL_PREFLIGHT status=0x%016lx\r\n",
    (UINT64)Status
    );

NormalReturn:
  CleanupStatus = NativeCsmVgaRuntimePlanRelease (&Plan);
  return BootCountdownFinish (&Logger, &Files, Status, CleanupStatus);
}

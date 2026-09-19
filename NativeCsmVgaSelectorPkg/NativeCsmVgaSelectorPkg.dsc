[Defines]
  DEFINE SELECTOR_EDITION = Release
  PLATFORM_NAME = NativeCsmVgaSelector$(SELECTOR_EDITION)
  PLATFORM_GUID = A4E1F2D5-8F93-4177-A1A6-62A8D40228C0
  PLATFORM_VERSION = 1.2
  DSC_SPECIFICATION = 0x00010006
  OUTPUT_DIRECTORY = Build/NativeCsmVgaSelector$(SELECTOR_EDITION)
  SUPPORTED_ARCHITECTURES = X64
  BUILD_TARGETS = RELEASE
  SKUID_IDENTIFIER = DEFAULT
!include MdePkg/MdeLibs.dsc.inc
[LibraryClasses.common]
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLibOptionalDevicePathProtocol.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
[Components]
  NativeCsmVgaSelectorPkg/Application/NativeCsmVgaSelector.inf

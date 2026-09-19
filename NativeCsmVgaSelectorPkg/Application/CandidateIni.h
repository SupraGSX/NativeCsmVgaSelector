/** Pure candidate ownership, ranking, formatting, and strict INI parsing. */
#ifndef NATIVE_CSM_VGA_CANDIDATE_INI_H_
#define NATIVE_CSM_VGA_CANDIDATE_INI_H_

#include "MarkerConfig.h"

#define NCV_MAX_DISPLAY_CANDIDATES  32U
#define NCV_MAX_STORAGE_CANDIDATES  64U
#define NCV_CANDIDATE_STRING_BYTES  129U
#define NCV_CONFIG_TEXT_BYTES       65U
/* Quotes, worst-case escaped content, and the terminating NUL. */
#define NCV_ENCODED_DESCRIPTION_BYTES (2U * (NCV_CONFIG_TEXT_BYTES - 1U) + 3U)
#define NCV_CONFIG_MAX_LINE_BYTES   256U
#define NCV_CANDIDATE_INI_BYTES     65536U
#define NCV_NO_SELECTION            ((NCV_SIZE)-1)

#define NCV_BOOT_INVALID_PROBE_MODE         0x01U
#define NCV_BOOT_MISSING_TARGET_PCI         0x02U
#define NCV_BOOT_MISSING_CONTROLLER_PCI     0x04U
#define NCV_BOOT_MISSING_IO_POLICY          0x08U
#define NCV_BOOT_MISSING_MEMORY_POLICY      0x10U
#define NCV_BOOT_MISSING_BUS_MASTER_POLICY  0x20U

typedef unsigned char  NCV_U8;
typedef unsigned short NCV_U16;
typedef unsigned int   NCV_U32;
typedef unsigned long long NCV_SIZE;
typedef unsigned char  NCV_BOOL;

typedef struct {
  NCV_U16 Segment;
  NCV_U8  Bus;
  NCV_U8  Device;
  NCV_U8  Function;
} NCV_PCI_ADDRESS;

typedef struct {
  NCV_PCI_ADDRESS Address;
  NCV_SIZE RawSegment;
  NCV_SIZE RawBus;
  NCV_SIZE RawDevice;
  NCV_SIZE RawFunction;
  NCV_U16 Vendor;
  NCV_U16 DeviceId;
  NCV_U16 SubsystemVendor;
  NCV_U16 SubsystemDevice;
  NCV_BOOL AddressValid;
  NCV_BOOL GopAssociated;
  NCV_BOOL GopAssociationKnown;
  NCV_BOOL ActiveLegacyVgaOwner;
  NCV_BOOL ActiveLegacyVgaOwnerKnown;
  NCV_BOOL OptionRomExposed;
  NCV_BOOL AcceptedLegacyRom;
  NCV_BOOL PathDiscovered;
  NCV_BOOL PathValidated;
  NCV_BOOL Eligible;
  NCV_BOOL Selected;
} NCV_DISPLAY_CANDIDATE;

typedef struct {
  NCV_DISPLAY_CANDIDATE Items[NCV_MAX_DISPLAY_CANDIDATES];
  NCV_SIZE Count;
  NCV_SIZE ObservedCount;
  NCV_BOOL Overflow;
} NCV_DISPLAY_COLLECTION;

typedef struct {
  NCV_SIZE BbsIndex;
  NCV_PCI_ADDRESS Address;
  NCV_U32 RawBus;
  NCV_U32 RawDevice;
  NCV_U32 RawFunction;
  NCV_U8 BaseClass;
  NCV_U8 SubClass;
  char Manufacturer[NCV_CANDIDATE_STRING_BYTES];
  char Description[NCV_CANDIDATE_STRING_BYTES];
  NCV_BOOL AddressValid;
  NCV_BOOL Usb;
  NCV_BOOL FirmwareStatusKnown;
  NCV_U16 FirmwareStatus, FirmwarePriority;
  NCV_BOOL IdentityAmbiguous;
  NCV_BOOL Eligible;
  NCV_BOOL Selected;
} NCV_STORAGE_CANDIDATE;

typedef struct {
  NCV_STORAGE_CANDIDATE Items[NCV_MAX_STORAGE_CANDIDATES];
  NCV_SIZE Count;
  NCV_SIZE ObservedCount;
  NCV_BOOL Overflow;
} NCV_STORAGE_COLLECTION;

typedef enum {
  NcvEndpointIgnore = 0,
  NcvEndpointOn,
  NcvEndpointOff
} NCV_ENDPOINT_POLICY;

typedef enum {
  NcvConfigSuccess = 0,
  NcvConfigInvalidParameter,
  NcvConfigUnsupported,
  NcvConfigCompromisedData
} NCV_CONFIG_RESULT;

typedef struct {
  NCV_MARKER_CONFIG Marker;
  NCV_BOOL HasProbe;
  NCV_BOOL Probe;
  NCV_BOOL HasName;
  NCV_BOOL HasTargetPci;
  NCV_BOOL HasTargetControllerPci;
  NCV_BOOL HasExpectedVendor;
  NCV_BOOL HasExpectedDevice;
  NCV_BOOL HasExpectedSubsystemVendor;
  NCV_BOOL HasExpectedSubsystemDevice;
  NCV_BOOL HasLegacyOptionDescription;
  NCV_BOOL HasExcludeDescription;
  NCV_BOOL HasTargetBbsDescription;
  NCV_BOOL HasVerboseLog;
  NCV_BOOL HasAutoBoot;
  /* Reserved for ABI stability; not a supported INI option. */
  NCV_BOOL HasRequireReferenceSnapshotMatch;
  NCV_BOOL HasIoDecoding;
  NCV_BOOL HasMemoryDecoding;
  NCV_BOOL HasBusMastering;
  NCV_BOOL VerboseLog;
  NCV_BOOL AutoBoot;
  NCV_BOOL RequireReferenceSnapshotMatch;
  NCV_U8 IoDecoding;
  NCV_U8 MemoryDecoding;
  NCV_U8 BusMastering;
  NCV_PCI_ADDRESS TargetPci;
  NCV_PCI_ADDRESS TargetControllerPci;
  NCV_U16 ExpectedVendor;
  NCV_U16 ExpectedDevice;
  NCV_U16 ExpectedSubsystemVendor;
  NCV_U16 ExpectedSubsystemDevice;
  char Name[NCV_CONFIG_TEXT_BYTES];
  char LegacyOptionDescription[NCV_CONFIG_TEXT_BYTES];
  char ExcludeDescription[NCV_CONFIG_TEXT_BYTES];
  char TargetBbsDescription[NCV_CONFIG_TEXT_BYTES];
} NCV_CONFIG_CORE;

typedef struct {
  NCV_BOOL Probe;
  NCV_BOOL HasTargetPci;
  NCV_BOOL HasTargetControllerPci;
  NCV_BOOL HasIoDecoding;
  NCV_BOOL HasMemoryDecoding;
  NCV_BOOL HasBusMastering;
} NCV_BOOT_REQUIREMENTS;

typedef enum {
  NcvOutputActiveIni = 0,
  NcvOutputProbeIni
} NCV_OUTPUT_KIND;

int
NcvDisplayCollectionAppend (
  NCV_DISPLAY_COLLECTION         *Collection,
  const NCV_DISPLAY_CANDIDATE    *Candidate
  );

int
NcvStorageCollectionAppend (
  NCV_STORAGE_COLLECTION         *Collection,
  const NCV_STORAGE_CANDIDATE    *Candidate
  );

int
NcvIniValueIsExactlyRepresentable (
  const char *Value,
  NCV_SIZE   Capacity
  );

/* Preserve firmware padding rather than changing the disk's matching key. */
int
NcvEncodeDiskDescription (
  const char *Value,
  char       *Output,
  NCV_SIZE   Capacity
  );

int
NcvRankDisplays (
  NCV_DISPLAY_CANDIDATE *Items,
  NCV_SIZE              Count,
  NCV_SIZE              *Selected
  );

int
NcvRankStorage (
  NCV_STORAGE_CANDIDATE *Items,
  NCV_SIZE              Count,
  NCV_SIZE              *Selected
  );

int
NcvFormatCandidateIni (
  const NCV_DISPLAY_CANDIDATE *Displays,
  NCV_SIZE                    DisplayCount,
  NCV_SIZE                    SelectedDisplay,
  const NCV_STORAGE_CANDIDATE *Storage,
  NCV_SIZE                    StorageCount,
  NCV_SIZE                    SelectedStorage,
  char                        *Output,
  NCV_SIZE                    Capacity,
  NCV_SIZE                    *OutputSize
  );

void
NcvConfigDefaults (
  NCV_CONFIG_CORE *Config
  );

NCV_CONFIG_RESULT
NcvParseConfigStrict (
  const NCV_U8    *Bytes,
  NCV_SIZE        ByteCount,
  NCV_CONFIG_CORE *Config
  );

void
NcvConfigCoreGetBootRequirements (
  const NCV_CONFIG_CORE  *Config,
  NCV_BOOT_REQUIREMENTS  *Requirements
  );

NCV_U8
NcvBootRequirementsValidate (
  const NCV_BOOT_REQUIREMENTS *Requirements
  );

NCV_OUTPUT_KIND
NcvChooseOutputKind (
  NCV_BOOL FirstRun,
  NCV_SIZE SelectedDisplay,
  NCV_SIZE SelectedStorage
  );

const char *
NcvOutputFileNameAscii (
  NCV_OUTPUT_KIND Kind
  );

NCV_CONFIG_RESULT NcvValidateConfigMode (const NCV_CONFIG_CORE *Config, NCV_U8 *Missing);

#endif

CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic
SANITIZERS ?= -fsanitize=address,undefined
APP := ../NativeCsmVgaSelectorPkg/Application
BUILD := build
TESTS := CandidateIniTests MarkerConfigTests PciIdLookupTests ConfigEditTests ConfigRecoveryTests RomPlacementTests
COMMON := $(BUILD)/CandidateIni.o
OBJECTS := $(COMMON) $(BUILD)/PciIdLookup.o $(BUILD)/ConfigEdit.o $(BUILD)/RomPlacement.o $(TESTS:%=$(BUILD)/%.o)

.PHONY: all test clean firmware firmware-boot-target firmware-diagnostics firmware-portability firmware-rom-guard firmware-rom-recovery firmware-countdown firmware-disk-host launcher audit-host
all: $(TESTS:%=$(BUILD)/%)
$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(SANITIZERS) -MMD -MP -I$(APP) -c $< -o $@
$(BUILD)/%.o: $(APP)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(SANITIZERS) -MMD -MP -I$(APP) -c $< -o $@
$(BUILD)/CandidateIniTests: $(BUILD)/CandidateIniTests.o $(COMMON)
$(BUILD)/MarkerConfigTests: $(BUILD)/MarkerConfigTests.o $(COMMON)
$(BUILD)/ConfigRecoveryTests: $(BUILD)/ConfigRecoveryTests.o $(COMMON)
$(BUILD)/PciIdLookupTests: $(BUILD)/PciIdLookupTests.o $(BUILD)/PciIdLookup.o
$(BUILD)/ConfigEditTests: $(BUILD)/ConfigEditTests.o $(COMMON) $(BUILD)/ConfigEdit.o
$(BUILD)/RomPlacementTests: $(BUILD)/RomPlacementTests.o $(BUILD)/RomPlacement.o
$(TESTS:%=$(BUILD)/%):
	$(CC) $(CFLAGS) $(SANITIZERS) $^ -o $@
test: all launcher
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/CandidateIniTests $(BUILD)/generated.ini
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/MarkerConfigTests
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/PciIdLookupTests
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/ConfigEditTests
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/ConfigRecoveryTests
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/RomPlacementTests
launcher:
	python3 LinuxLauncherTests.py
audit-host:
	python3 PackagingTests.py
	python3 LinuxIdentityTests.py
firmware:
	python3 Firmware/run.py
firmware-boot-target:
	python3 Firmware/run_boot_target.py
firmware-portability:
	python3 Firmware/run_portability.py
firmware-rom-guard:
	python3 Firmware/run_rom_guard.py
firmware-disk-host:
	python3 run_firmware_disk_host.py
firmware-countdown:
	python3 Firmware/run_countdown.py
firmware-rom-recovery:
	python3 Firmware/run_recovery.py
firmware-diagnostics:
	python3 Firmware/run_diagnostics.py
clean:
	rm -rf $(BUILD)
-include $(OBJECTS:.o=.d)

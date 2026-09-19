#!/bin/bash
set -eo pipefail
PROJECT_ROOT=$(cd "$(dirname "$0")" && pwd)
EDK2_DIR=${EDK2_DIR:-"$PROJECT_ROOT/Toolchain/edk2-stable202605"}
BUILD_OUTPUT_DIR=${BUILD_OUTPUT_DIR:-"$PROJECT_ROOT/Build"}
EDITION=${1:-Release}
case "$EDITION" in Release|Debug) ;; *) echo 'Use Release or Debug'; exit 1;; esac
[ "$(git -C "$EDK2_DIR" rev-parse HEAD)" = b03a21a63e3bd001f52c527e5a57feddb53a690b ] || { echo 'Wrong EDK2 commit'; exit 1; }
python3 "$PROJECT_ROOT/Scripts/check_handoff.py"
cd "$EDK2_DIR"
export WORKSPACE="$EDK2_DIR" PYTHON_COMMAND=python3 EDK_TOOLS_PATH="$EDK2_DIR/BaseTools"
. edksetup.sh BaseTools >/dev/null
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"
export PACKAGES_PATH="$WORKSPACE:$PROJECT_ROOT"
build cleanall -D SELECTOR_EDITION="$EDITION" -a X64 -b RELEASE -t GCC -p NativeCsmVgaSelectorPkg/NativeCsmVgaSelectorPkg.dsc
build -n 4 -D SELECTOR_EDITION="$EDITION" -a X64 -b RELEASE -t GCC -p NativeCsmVgaSelectorPkg/NativeCsmVgaSelectorPkg.dsc
python3 "$PROJECT_ROOT/Scripts/check_boot_layout.py" "$EDK2_DIR/Build/NativeCsmVgaSelector$EDITION/RELEASE_GCC/X64/NativeCsmVgaSelector.debug"
mkdir -p "$BUILD_OUTPUT_DIR/$EDITION"
# Remove timestamps and CodeView records containing local build paths.
# Unstripped symbols remain in the private EDK II build directory.
GenFw -z -o "$BUILD_OUTPUT_DIR/$EDITION/BOOTX64.EFI" "Build/NativeCsmVgaSelector$EDITION/RELEASE_GCC/X64/NativeCsmVgaSelector.efi"

cp "$PROJECT_ROOT/Data/pci.ids" "$BUILD_OUTPUT_DIR/$EDITION/pci.ids"

python3 "$PROJECT_ROOT/Scripts/build_manifest.py" --source "$PROJECT_ROOT" --edk2 "$EDK2_DIR" --output "$BUILD_OUTPUT_DIR/$EDITION/BUILD.json"

#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"
[[ $# == 0 || ( $# == 1 && $1 == --firmware ) ]] || { echo 'Usage: bash Scripts/validate.sh [--firmware]'; exit 2; }
bash build.sh Release
bash build.sh Debug
make -C Tests test audit-host firmware-disk-host
if [[ ${1:-} == --firmware ]]; then
    make -C Tests firmware-portability firmware-boot-target firmware-rom-guard firmware-countdown
fi
if command -v pwsh >/dev/null 2>&1; then
    pwsh -NoLogo -NoProfile -NonInteractive -File Tests/WindowsLoaderTests.ps1
else
    echo 'PowerShell file tests not run here; execute Tests/WindowsLoaderTests.ps1 with PowerShell.'
fi
python3 Scripts/package.py
python3 Scripts/verify_packages.py

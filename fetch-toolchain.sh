#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
path=Toolchain/edk2-stable202605
commit=b03a21a63e3bd001f52c527e5a57feddb53a690b
if [[ ! -e "$path" ]]; then
 mkdir -p Toolchain
 git clone --branch edk2-stable202605 --single-branch https://github.com/tianocore/edk2.git "$path"
fi
[[ $(git -C "$path" rev-parse HEAD) == "$commit" ]] || { echo 'Unexpected EDK II revision'; exit 1; }
git -C "$path" submodule update --init BaseTools/Source/C/BrotliCompress/brotli MdePkg/Library/BaseFdtLib/libfdt MdePkg/Library/MipiSysTLib/mipisyst
make -C "$path/BaseTools" -j4

# Build and package

Use Linux with GCC/binutils, make, Python 3, NASM and Git. `fetch-toolchain.sh`
fetches EDK II commit `b03a21a63e3bd001f52c527e5a57feddb53a690b`
(`edk2-stable202605`) and its dependencies into `Toolchain/`.
An existing source-local checkout can be reused; `EDK2_DIR` overrides its path.
The script refuses another revision. Follow any BaseTools dependency guidance
reported by the fetch script.

`bash build.sh Release` and `bash build.sh Debug` compile the shared
`NativeCsmVgaSelectorPkg` using the RELEASE compiler profile. Outputs are
`Build/Release/BOOTX64.EFI` and `Build/Debug/BOOTX64.EFI`, each with `pci.ids`.
Build output strips PE debug-path metadata and timestamps using EDK II GenFw;
local unstripped symbols remain available in the EDK II build directory.
The two labels are retained for installer packaging; logging is controlled by
`VerboseLog`, not a second copy of the implementation. Build sequentially when
sharing one EDK II checkout; its generated configuration is shared.

`make -C Tests test` builds sanitizer-backed native tests in `Tests/build`, with
compiler-generated header dependencies. Use `make -C Tests clean` to remove
these outputs. `SANITIZERS=` permits environments without sanitizer runtimes.

`make -C Tests launcher` runs the Linux desktop launcher regression tests
using Python 3, Bash, GNU env -S and temporary pseudo-terminals. It exercises
direct Run as a Program behavior, including paths with spaces and error
holding, using an installer stub that never accesses disks. It is included
in `make -C Tests test`.

`make -C Tests firmware` additionally requires QEMU x86, OVMF, dosfstools and
mtools. It builds a separate test harness and generates fresh synthetic disks.
OVMF defaults to `/usr/share/OVMF`; `python3 Tests/Firmware/run.py --help`
shows overrides and individual scenarios. Evidence stays under
`Build/FirmwareTests/`. Do not install the test harness on physical hardware.

`python3 Scripts/package.py` produces two ZIPs under `Build/Packages`:

- `NativeCsmVgaSelector-1.2-source.zip`: source, build scripts, tests, docs,
  and notices, excluding generated files, runtime settings and the toolchain.
- `NativeCsmVgaSelector-1.2.zip`: both EFI build names, fallback BOOTX64.EFI,
  installers, pci.ids, docs, notices, and SHA256SUMS.

The ZIP metadata and file order are deterministic for identical inputs.
`SOURCE_DATE_EPOCH` can set the archive timestamp; by default it is 1980-01-01.
This is deterministic packaging, not a claim that arbitrary compiler versions
produce identical binaries. Publish nothing automatically. Distribute the
matching source if you distribute the binary package under GPLv3.

The packager also creates the ready-to-run folder
`Build/Packages/NativeCsmVgaSelector-1.2`. The source `Installers/Linux`
launcher uses that folder; repackage after editing installer files. Editor
lock files and temporary swap files are excluded from release packages.

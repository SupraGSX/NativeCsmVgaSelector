# Licensing and provenance

The Native CSM VGA Selector project is distributed under **GNU General Public
License version 3 only (GPL-3.0-only)**. The full terms are in `LICENSE`.
This choice applies to the combined release and new project contributions.
Existing files marked `LGPL-2.1-or-later` retain those notices and their separate
upstream licensing; `LICENSES/LGPL-2.1.txt` supplies those terms. Combining them
in this GPLv3 release does not erase the upstream grants.

`Data/pci.ids` comes from the PCI ID Repository and retains the copyright and
license text in its header. This distribution uses its three-clause BSD option.
See `Data/README.md` for its version, checksum, and update instructions.

The optional source-local EDK II checkout is upstream build tooling. Its own
`License.txt`, SPDX notices, and submodule licenses remain authoritative for
those components. Fetching it does not relicense it under this project's GPL.
EDK II is not included in the source or installer ZIP; the pinned fetch script
and build instructions are included. Firmware links EDK II libraries under
their respective upstream licenses. The package retains the applicable EDK II
license notice in `LICENSES/EDK-II.txt`.

The source archive contains the corresponding project source, host tests,
firmware test harness, and build/install scripts for the release. If distributing
binary packages, also provide the matching source and comply with GPLv3's
source and notice requirements. This packaging step itself publishes nothing.

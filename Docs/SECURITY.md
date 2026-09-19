# Security and publication hygiene

For user-facing preparation and error recovery, see
[installation](INSTALLATION.md) and [troubleshooting](TROUBLESHOOTING.md).

Native CSM boot execution crosses an irreversible current-boot boundary and
is intended only for explicitly authorized, personally controlled hardware.
Probe performs no hardware mutation, although logs and generated configuration
files are filesystem writes beside the application. Source preparation alone
does not authorize deployment, NVRAM changes, firmware modification, or a
hardware test.

Do not commit or publish:

- firmware blobs, option-ROM dumps, or BIOS dumps;
- USB payload backups or disk images;
- private keys or private-key headers;
- credentials, tokens, `.env` files, or local configuration secrets;
- local usernames, home-directory paths, device serial numbers, or USB UUIDs;
- agent-session directories or handoff captures;
- generated `.efi`, `.debug`, `.map`, object, archive, log, or temporary files.

Do not add persistent firmware/NVRAM behavior, network access, credential
handling, firmware flashing, Secure Boot bypasses, or effects intended to
survive a cold reset.

## USB-installer safety

Installer support is intentionally limited to USB disks; internal disks and
host ESPs are never valid targets. Existing-FAT mode does not format or
repartition. Dedicated-USB mode destroys all content and proceeds only after
the displayed exact confirmation phrase is typed and the physical device is
revalidated. Back up any existing USB content first, and disconnect unrelated
removable disks to reduce selection mistakes.

The installers neither modify firmware variables nor change `BootOrder`.
They preserve existing Selector INI files and require an existing
`BOOTX64.EFI` to be backed up and verified before replacement.

The combined project release is licensed under **GPL-3.0-only**. The complete
terms are in [LICENSE](../LICENSE); inherited LGPL and upstream component/data
notices are retained as described in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
Preparing source or packages does not publish them.

Source packaging now uses the reviewed `SOURCE_MANIFEST.txt` allowlist. Update
it deliberately when source files are added. Unlisted source files and forbidden
capture/key/binary types stop packaging; symbolic-link files or parents are
rejected. Runtime docs/data are drawn only from that same reviewed set. Package
outputs are staged, and an edited/unexpected existing runtime directory is
preserved. `Scripts/verify_packages.py` checks the final archives and manifest.


## Publication review

Use a GitHub noreply identity for every author, committer and tagger. Inspect
commit/tag messages and all reachable history as well as the checked-out files.
Review every final archive member and its contents, including ASCII and UTF-16
strings inside binaries. Scan for credentials, private keys, local paths and
private contact/device information; filenames alone are not a sufficient check.
Keep build provenance and raw test evidence local. Publish only reviewed source,
license notices, documentation, the intended runtime files and their checksums.

Do not merge or push old repository history into a fresh publication. A source
allowlist prevents unexpected files from being packaged; it does not establish
that the contents of an allowed file are safe to publish.

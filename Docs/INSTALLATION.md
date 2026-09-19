# Installation and first boot

Native CSM VGA Selector starts as an x64 UEFI application, switches legacy VGA
output to your selected secondary GPU, then asks the firmware to boot your
selected legacy disk. The USB contains the selector; your operating system
stays on its existing disk. The selector does not install Windows, create an
OS bootloader, add storage drivers, or provide its own CSM.

## 1. Check that your hardware fits

You need all of the following:

- **x64 UEFI firmware with a working native CSM** (Compatibility Support
  Module) and legacy disk boot support. AMI/Aptio is the intended firmware
  family, but the brand alone does not establish compatibility. Traditional
  BIOS-only machines cannot launch this EFI application, and UEFI-only
  firmware without CSM cannot perform its legacy handoff.
- **A primary GPU and a different target GPU.** Firmware must expose exactly
  one active legacy-VGA owner. Version 1.2 does not support selecting that
  already-active GPU as a passthrough target.
- **A usable legacy VGA option ROM on the target GPU.** Having an EFI/GOP
  display output alone is insufficient. The selector uses the ROM exposed by
  firmware; you do not supply or flash a ROM file during setup.
- **A supported PCI arrangement.** The active and target GPUs must share the
  same PCI root bridge and parent host, and the target must be in PCI segment
  0. These are checked by the application; not every slot arrangement works.
- **An existing legacy-bootable OS disk listed by firmware.** UEFI-only OS
  installations are not targets for this handoff. NVMe requires the firmware's
  legacy storage support as well as a suitable driver in the OS. Do not
  convert or repartition an OS disk just to prepare this USB.
- **Permission to run this unsigned EFI application.** Normal Secure Boot
  policy may reject it; Secure Boot will generally need to be disabled by the
  machine owner. The program and installers never change that setting.

Check your motherboard manual for CSM, legacy storage boot, primary-display,
and Secure Boot options; labels vary. There is no claim of support for every
AMI system. See [validation limits](VALIDATION.md).

## 2. Extract the runtime package

Use **`NativeCsmVgaSelector-1.2.zip`**. Extract the entire ZIP to a folder on
your normal local OS drive. Do not run the installer inside the ZIP, from a
network share, or from the same USB disk you intend to prepare. The installers
exclude the disk containing their own files.

The runtime package includes `BOOTX64.EFI`, two named EFI build copies,
`pci.ids`, `SHA256SUMS`, the installers, and these guides. Keep them together.
Choose the default **Release Edition**. The Debug label currently uses the
same optimized implementation; `VerboseLog` controls diagnostic detail.

If you only have `NativeCsmVgaSelector-1.2-source.zip`, follow the separate
[build instructions](BUILD.md) first. End users with the runtime ZIP do not
need a compiler or the EDK II toolchain.

The installers compare their input files with `SHA256SUMS`. This detects
package damage or inconsistent files, but a checksum shipped in the same ZIP
does not independently authenticate the publisher. Obtain the package from a
source you trust.

## 3. Choose one USB installation method

A spare USB stick of **512 MB or larger** is a convenient choice for the
dedicated-USB method. It needs at least 257 MiB of capacity and 512-byte
logical sectors for the fixed 256 MiB FAT32 layout. Both installers support:

| Choice | What happens |
| --- | --- |
| **Existing FAT/FAT32 partition** | Copies the selector onto a selected USB partition without formatting or repartitioning it. An existing `EFI/BOOT/BOOTX64.EFI` requires a verified backup before replacement. |
| **Erase and prepare a dedicated USB disk** | Erases the entire selected USB disk, creates an MBR partition table with one active 256 MiB FAT32 partition starting at 1 MiB labeled `NCVSELECTOR`, then installs the files. Remaining capacity stays unallocated. |

Back up files on the selected USB before using erase mode. Check its displayed
model, capacity and identifying details. Type the displayed **ERASE ...**
confirmation phrase only for the intended
device; Enter, n, or any other answer cancels without changing it. These installers reject internal disks and system EFI partitions.
They do not create firmware boot entries or install an OS boot menu.

For an existing multiboot USB, such as one already using a fallback EFI loader,
prefer a separate stick. Replacing `BOOTX64.EFI` changes what its UEFI fallback
path launches, even when the other files are preserved.

### Windows installer

Use a modern x64 Windows installation with Windows PowerShell and the Storage
cmdlets, such as Windows 10 or 11. These preparation scripts are not intended
to run inside Windows 98 or XP.

1. Extract the runtime ZIP to a local folder, then open that folder.
2. Run **`Install-NativeCsmVgaSelector.cmd`** and accept its administrator
   elevation prompt. Keep `Installer.ps1` beside it. Results and errors remain
   visible until you press Enter. If PowerShell cannot start, open Command
   Prompt in the extracted folder and run the CMD launcher there.
3. Choose **Release Edition**, then your installation method.
4. Select the USB by model and capacity and follow the displayed prompts.
5. Wait for the installation verification to finish, then safely eject the
   USB. A drive letter may disappear after dedicated preparation; the
   installer removes its temporary drive-letter assignment when finished.

The launcher uses a process-scoped PowerShell execution-policy override; it
does not permanently change the system execution policy. The CMD launcher and Windows installer have been exercised in Windows 11
with virtual USB disks, including elevation, both installation methods,
backup verification and cancellation. Physical USB behavior and other Windows
versions remain untested. See [validation](VALIDATION.md).

### Linux installer

The script needs Bash, sudo when elevation is required, util-linux tools
(`lsblk`, `findmnt`, `blkid`, `mount`, `umount`, and `sfdisk`/`wipefs` for erase
mode), coreutils, `file`, grep, sed, and dosfstools (`mkfs.fat` for erase mode).
An error names a missing command; install its package with your distribution's
package manager and retry.

1. Extract the runtime ZIP to a local folder on your normal OS drive.
2. Right-click **`install.sh`** and choose **Run as a Program** (allow execution
   in file properties first if needed). It opens a terminal and keeps the
   result visible until you press Enter. The optional
   `NativeCsmVgaSelector-Installer.desktop` launcher does the same; your file
   manager may require **Allow Launching** or a trust setting for that launcher.
   **Run as a Program** on the `.desktop` file is also supported on Linux with
   GNU `env -S` (coreutils 8.30 or newer). Both paths use `launch.sh`. Keep the
   launcher beside `launch.sh` and `install.sh`; a copied shortcut by itself
   is not supported. Alternatively, open a
   terminal **in that extracted folder** and run:

   ```sh
   bash ./install.sh
   ```

3. Choose **Release Edition**, then an installation method.
4. Select the USB by model and capacity. Enter your sudo password when asked.
5. Wait for verification and script completion. Safely eject the USB before
   removing it. Existing volumes that you mounted yourself may remain mounted.

Graphical launching needs GNOME Terminal, Konsole, Xfce Terminal,
`x-terminal-emulator`, or xterm. On systems without one, use an existing
terminal. Keep `launch.sh` beside `install.sh`. From a source checkout, first
build and run `python3 Scripts/package.py`; the source-folder launcher then
uses the complete runtime folder in `Build/Packages/NativeCsmVgaSelector-1.2`.
Repackage after changing source installer files. A missing package produces
an explanation instead of proceeding with incomplete inputs.

If the file manager opens the `.desktop` file as text, use its **Run as a
Program** action or trust the shortcut as required by your desktop. On older
systems without `env -S`, run `launch.sh` instead. Both launch paths keep
installer completion and errors visible until Enter.

Do not source `install.sh`. Its erase confirmation shows the exact device
path; do not copy a device path from someone else's example.

### Manual installation

Use this method if you already know how to prepare a USB in your partitioning
tool. Formatting erases the partition, and changing its partition table can
erase the entire USB: identify the USB carefully before applying changes.

1. On a spare USB, create an **MBR/DOS partition table** and one **256 MiB FAT32
   primary partition**, beginning at the usual 1 MiB alignment. Mark it
   active/bootable to match the installer layout. The label is optional.
   This is the project's documented USB layout, not a requirement that every
   UEFI application use MBR or an active flag. Other layouts depend on firmware.
2. Open the FAT32 partition. Create an `EFI` folder, then a `BOOT` folder
   inside it.
3. Copy the runtime package's **`BOOTX64.EFI`** and **`pci.ids`** into `EFI/BOOT`.
   If that folder already contains a loader, save its original files elsewhere
   before replacing anything.
4. Safely eject the USB after all copying completes.

The final layout is:

```text
USB FAT32 partition root/
└── EFI/
    └── BOOT/
        ├── BOOTX64.EFI
        └── pci.ids
```

Do not place the EFI file loose in the USB root, leave it inside the ZIP, or
create an extra enclosing package folder above `EFI`. Use FAT32 for this guide,
not exFAT or NTFS. You do not need DOS system files, GRUB, an OS installer ISO,
or any prewritten `Config.ini` on this fresh selector USB.

`pci.ids` supplies friendly GPU names. It is optional for manual runtime use;
missing data falls back to numeric IDs. The packaged installers expect it.
Config and log files are created next to the executable, so this partition
must remain writable and have free space.

## 4. Boot and select your targets

1. Insert the USB and open your motherboard's one-time boot menu using the
   key described in its manual.
2. Select **`UEFI: <your USB device>`**, not its legacy USB entry. The selector
   itself starts through UEFI even though it later boots a legacy OS.
3. Initially keep the monitor on the GPU that shows the firmware screen.
4. On a fresh USB, hardware discovery opens target setup when suitable GPU
   and disk candidates exist. Use **Up/Down or Tab** to choose a field and
   **Left/Right** to change its selection. Select the intended secondary GPU
   and the existing OS boot disk. Disk selection is by disk/controller, not
   a Windows partition or drive letter.
5. Press **Enter** to review. Press **Enter again to save and boot**. Esc in
   review goes back to choices; Esc in choices cancels without saving.
6. `Config.ini` and its recovery copy are saved beside the EFI application.
   A successful save reloads those settings and continues directly into the
   **5-second F2 setup window, preflight checks, then a 5-second monitor-switch countdown**. Switch to
   the named GPU's monitor input during that countdown. No second firmware launch or external INI
   editing is needed for ordinary setup.
7. The normal hardware checks run before GPU handoff and legacy disk boot.
   Your disk's existing bootloader then chooses the OS. The selector does not
   automatically create separate Windows menu entries.

On subsequent boots, saved settings are used. Press **F2 during the initial five-second window**
to edit targets before preflight runs. It also remains available during the final
five-second monitor-switch countdown. Saving restarts this sequence with fresh
preflight. The two waits total ten seconds; validation takes additional time. If discovery cannot find suitable targets, or a save/check fails, it
stops rather than booting an arbitrary replacement. See [troubleshooting](TROUBLESHOOTING.md).

## Updating, recovery, and removal

**Update:** Back up the existing `EFI/BOOT` folder, including `Config.ini` and
`Config.ini.previous`. Run a new package's installer using **existing FAT/FAT32
mode**, or manually replace only `BOOTX64.EFI` and `pci.ids`. Preserve your
configuration. The installers back up an existing EFI loader after you confirm
replacement; keep your own copy of the whole folder for a complete rollback.

**Restore a previous version:** With the USB opened in your normal OS, restore
the saved `BOOTX64.EFI` and its matching saved configuration/database. An
installer-created `BOOTX64-backup-<timestamp>.efi` can be copied back under the
name `BOOTX64.EFI`. It is not automatically selected by firmware.

**Recover settings:** If the active INI is invalid, the application offers
**R** to restore a usable previous/temporary copy. With no usable recovery copy,
**F2 then Y** starts fresh setup after preserving the failed files. Fresh setup
disables optional markers and resets advanced policies. Simply deleting
`Config.ini` while leaving `Config.ini.previous` causes a recovery offer,
not a guaranteed fresh start.

**Remove:** For a dedicated selector USB, unplugging it stops using the
selector; you can later reformat that USB for another purpose. On a shared
USB, restore its previous fallback loader and remove only files you added for
the selector. Do not delete another bootloader's `EFI` directory. The USB
installers do not create persistent firmware entries, so there normally is no
entry to remove; manually created internal-disk entries require their own
cleanup.

Advanced internal-disk installation and optional menu markers are separate
from this USB walkthrough. Start with the USB method and see
[menu marker configuration](MARKER.md) only if you already maintain a compatible
boot menu.

### Audit-fix installer behavior

Linux records serial/WWN where available, sysfs identity and the kernel's disk
connection sequence. Destructive preparation requires that sequence, so even
a same-model USB reusing the same device number after reconnection is rejected
at revalidation. Identity is checked before each destructive phase. Device
operations are not an atomic transaction; do not unplug a USB while it is being
prepared. Existing-FAT mode and dedicated-USB mode retain their original scope.

Windows stages and verifies the complete new EFI before replacing the active
loader. A verified backup remains beside it. Returning commit/verification
failures restore the previous loader automatically; a failed restoration names
the retained backup and `.previous` file. If power is lost between FAT directory
updates, restore the verified backup to `EFI/BOOT/BOOTX64.EFI` from another OS.
No rename sequence promises arbitrary power-loss atomicity on FAT.

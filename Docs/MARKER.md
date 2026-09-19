# Optional menu marker in Config.ini

Omitting this section, or using the following, disables marker writes:

```ini
[Marker]
Enabled=false
```

Example enabled configuration (replace all target values for your installation):

```ini
[Marker]
Enabled=true
DiskSignature=0x12345678
PartitionNumber=1
PartitionStart=2048
PartitionSectors=131072
Path=\BOOTSEL.DAT
Profile=SECONDARY
Header=NATIVE CSM BOOT PROFILE 1
```

The exact MBR disk signature, partition number, start and size identify the
marker filesystem. Numbers accept decimal or `0x` hexadecimal. No label-only
matching or GPT marker targets are supported. Exactly one filesystem handle
must match. The boot-disk controller in `[Boot]` remains an independent target.

`Path` is absolute within that partition. `Profile` is 1–32 uppercase letters,
digits or underscores. Optional `Header` is 1–64 printable ASCII characters;
it defaults to the example above and allows compatibility with an existing
marker format. Unknown/duplicate settings, malformed numbers, overflow and
incomplete enabled configurations are rejected.

The existing marker must be exactly 512 bytes, start with the header plus a
newline followed by `PROFILE=`, and be writable. The core never creates it.
A typical initial file contains the header, newline, `PROFILE=DEFAULT`, space
padding, and a final newline. Your boot menu must read the profile and reset it
after consumption. The value offset is the ASCII header length plus 9 bytes.

After successful boot preflight and before GPU controller disconnection, the
core writes the selected profile, flushes it, reads it back, and closes handles.
A failure prevents GPU handoff. If the core returns before the irreversible CSM
boundary, it attempts to restore the original 512 bytes. Power loss or a fatal
halt after writing can leave a pending profile; the menu must handle that case.

First-run discovery and AutoBoot=false do not write a marker. Keep marker
settings disabled until you have prepared and verified your own boot menu.

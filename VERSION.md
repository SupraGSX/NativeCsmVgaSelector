# Native CSM VGA Selector 1.2.3

Prerelease based on the v1.2.2 implementation, including the selected-disk
warning policy, audit fixes, installer safeguards and reviewed source packaging.
See [audit fixes](Docs/AUDIT_FIXES.md) for changes and completed validation.

Physical native-CSM boot testing of this changed EFI is pending. Native Windows
storage operations, actual FAT power-loss behavior and physical USB hot-plug
timing also need platform testing. Firmware banners retain the 1.2 family;
boot logs record the full release build identifier.

Both packaged EFI editions are 245,760 bytes with SHA256:
`55173b45f6fd85d4a9c59ab82025c54d0c979fda647c35cc202ed2bf904eddda`.

This publication starts with a fresh Git history and a GitHub noreply author
identity. Source, license notices, final archives and commit metadata are
reviewed before publication. No old Git history or private build evidence is
included in the repository or release packages.

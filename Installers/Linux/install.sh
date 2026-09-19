#!/usr/bin/env bash

[[ ${BASH_SOURCE[0]} == "$0" ]] || { printf 'Run install.sh as a program; do not source it.\n' >&2; return 1; }

set -Eeuo pipefail

command -v dirname >/dev/null 2>&1 || { printf 'ERROR: Required command is unavailable: dirname\n' >&2; exit 1; }
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
# File managers can execute scripts without attaching a terminal.
if [[ ! -t 0 || ! -t 1 ]]; then
    [[ -f $SCRIPT_DIR/launch.sh ]] || { printf 'ERROR: Open a terminal in this folder and run: bash ./install.sh\n' >&2; exit 1; }
    exec bash "$SCRIPT_DIR/launch.sh"
fi

# Source checkout convenience: package.py prepares a complete runtime directory.
# Never install bare build output without the package's payload/manifest checks.
if [[ ! -e $SCRIPT_DIR/NativeCsmVgaSelector-1.2.efi && $SCRIPT_DIR == */Installers/Linux ]]; then
    RUNTIME_DIR="$SCRIPT_DIR/../../Build/Packages/NativeCsmVgaSelector-1.2"
    if [[ -f $RUNTIME_DIR/install.sh && -f $RUNTIME_DIR/SHA256SUMS ]]; then
        printf 'Using the built runtime package: %s\n\n' "$(cd -- "$RUNTIME_DIR" && pwd -P)"
        exec bash "$RUNTIME_DIR/install.sh"
    fi
    printf 'ERROR: This source folder has no built runtime package.\nBuild the project, then run python3 Scripts/package.py from the project root,\nor extract the runtime ZIP and run its install.sh.\n' >&2
    exit 1
fi
PRODUCT='Native CSM VGA Selector 1.2 USB Installer'
MOUNT_DIR=''; MOUNT_CREATED=0; TEMP_FILE=''; ACTIVE_MOUNT=''

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

as_root() {
    if (( EUID == 0 )); then "$@"; return; fi
    command -v sudo >/dev/null 2>&1 || die 'sudo is required for this operation.'
    sudo -- "$@"
}

run_on_volume() {
    if [[ -n $ACTIVE_MOUNT && -w $ACTIVE_MOUNT ]]; then "$@"; return; fi
    as_root "$@"
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n $TEMP_FILE ]]; then
        run_on_volume rm -f -- "$TEMP_FILE" >/dev/null 2>&1 || status=1
        TEMP_FILE=''
    fi
    if [[ -n $MOUNT_DIR ]]; then
        if (( MOUNT_CREATED )); then
            as_root umount -- "$MOUNT_DIR" >/dev/null 2>&1 || status=1
        fi
        rmdir -- "$MOUNT_DIR" >/dev/null 2>&1 || status=1
        MOUNT_CREATED=0; MOUNT_DIR=''
    fi
    exit "$status"
}

trap cleanup EXIT; trap 'exit 130' INT TERM

require_commands() { local command_name; for command_name in "$@"; do
    command -v "$command_name" >/dev/null 2>&1 || die "Required command is unavailable: $command_name"
done; }

trim_text() { local value=$1; value=${value#"${value%%[![:space:]]*}"}; value=${value%"${value##*[![:space:]]}"}; printf '%s' "$value"; }

require_commands bash lsblk findmnt blkid sha256sum file grep sed readlink mktemp date cp mv mkdir rmdir rm mount umount sync

source_uses_disk() {
    local source=$1 disk=$2 canonical ancestry
    source=${source%%\[*}
    canonical=$(readlink -f -- "$source" 2>/dev/null) || return 2
    [[ -b $canonical ]] || return 2
    ancestry=$(lsblk -snrpo PATH -- "$canonical" 2>/dev/null) || return 2
    if grep -Fxq -- "$disk" <<< "$ancestry"; then
        return 0
    fi
    return 1
}

path_uses_disk() {
    local path=$1 disk=$2 source
    source=$(findmnt -nro SOURCE -T "$path" 2>/dev/null) || return 2
    [[ -n $source ]] || return 2
    source_uses_disk "$source" "$disk"
}

disk_contains_protected_path() {
    local disk=$1 path result
    for path in / /boot /boot/efi "$SCRIPT_DIR"; do
        [[ -e $path ]] || continue
        if path_uses_disk "$path" "$disk"; then
            return 0
        else
            result=$?
            (( result == 1 )) || return 0
        fi
    done
    return 1
}

disk_contains_active_swap() {
    local disk=$1 swap_path remainder source result
    [[ -r /proc/swaps ]] || return 0
    while read -r swap_path remainder; do
        [[ $swap_path == Filename ]] && continue
        [[ -n $swap_path ]] || continue
        if [[ $swap_path == /dev/* ]]; then
            source=$swap_path
        else
            source=$(findmnt -nro SOURCE -T "$swap_path" 2>/dev/null) || return 0
        fi
        if source_uses_disk "$source" "$disk"; then
            return 0
        else
            result=$?
            (( result == 1 )) || return 0
        fi
    done < /proc/swaps
    return 1
}

disk_contains_active_mapping() {
    local disk=$1 type types
    types=$(lsblk -nrpo TYPE -- "$disk" 2>/dev/null) || return 0
    while read -r type; do
        case "$type" in
            crypt|dm|lvm|md|mpath|raid*) return 0 ;;
        esac
    done <<< "$types"
    return 1
}

read_disk_identity() {
    local requested=$1 extra line
    line=$(lsblk -dnrb -o PATH,TYPE,TRAN,RO,SIZE,MAJ:MIN -- "$requested" 2>/dev/null) || return 1
    IFS=' ' read -r ID_PATH ID_TYPE ID_TRAN ID_RO ID_SIZE ID_MAJMIN extra <<< "$line"
    [[ -z ${extra:-} && -n ${ID_MAJMIN:-} ]] || return 1
    ID_MODEL=$(lsblk -dno MODEL -- "$requested" 2>/dev/null) || return 1
    ID_MODEL=$(trim_text "$ID_MODEL")
    [[ -n $ID_MODEL && $ID_SIZE =~ ^[0-9]+$ && $ID_SIZE -gt 0 &&
       $ID_MAJMIN =~ ^[0-9]+:[0-9]+$ ]] || return 1
    ID_SERIAL=$(lsblk -dnro SERIAL -- "$requested" 2>/dev/null) || return 1
    ID_WWN=$(lsblk -dnro WWN -- "$requested" 2>/dev/null) || return 1
    ID_SYSFS=$(readlink -f -- "/sys/dev/block/$ID_MAJMIN") || return 1
    ID_DISKSEQ=''
    if [[ -r $ID_SYSFS/diskseq ]]; then read -r ID_DISKSEQ < "$ID_SYSFS/diskseq" || return 1; fi
    return 0
}

disk_is_safe_usb() {
    local disk=$1
    [[ -b $disk ]] || return 1
    read_disk_identity "$disk" || return 1
    [[ $ID_PATH == "$disk" && $ID_TYPE == disk && $ID_TRAN == usb && $ID_RO == 0 ]] || return 1
    disk_contains_protected_path "$disk" && return 1
    disk_contains_active_swap "$disk" && return 1
    disk_contains_active_mapping "$disk" && return 1
    return 0
}

disk_mount_count() {
    local disk=$1 count=0 mountpoint mounts
    mounts=$(lsblk -nrpo MOUNTPOINTS -- "$disk" 2>/dev/null) || return 1
    while IFS= read -r mountpoint; do
        [[ -n $mountpoint ]] && ((count += 1))
    done <<< "$mounts"
    printf '%u' "$count"
}

get_node_mounts() {
    local node=$1 output status=0 mount_state target
    NODE_MOUNTS=()
    output=$(findmnt -rn -S "$node" -o TARGET 2>/dev/null) || status=$?
    (( status == 0 || status == 1 )) || return 1
    if [[ -z $output ]]; then
        mount_state=$(lsblk -dnro MOUNTPOINTS -- "$node" 2>/dev/null) || return 1
        [[ -z $mount_state ]] || return 1
        return 0
    fi
    while IFS= read -r target; do [[ -n $target ]] && NODE_MOUNTS+=("$target"); done <<< "$output"
}

show_disk() {
    local disk=$1 partitions=0 type human_size mounts types
    read_disk_identity "$disk" || die "Could not read disk identity: $disk"
    types=$(lsblk -nrpo TYPE -- "$disk" 2>/dev/null) || die "Could not enumerate partitions on $disk"
    while read -r type; do
        [[ $type == part ]] && ((partitions += 1))
    done <<< "$types"
    human_size=$(lsblk -dnro SIZE -- "$disk") || die "Could not read the capacity of $disk"
    mounts=$(disk_mount_count "$disk") || die "Could not inspect mounts on $disk"
    printf '%s: %s; %s; partitions=%u; mounted filesystems=%s\n' \
        "$disk" "${ID_MODEL:-<unknown model>}" "$human_size" "$partitions" "$mounts"
    lsblk -nrpo PATH,SIZE,FSTYPE,MOUNTPOINTS -- "$disk" | sed 's/^/    /'
}

enumerate_usb_disks() {
    local path type transport ro size majmin extra rows
    USB_PATHS=()
    USB_MAJMIN=()
    USB_MODELS=()
    USB_SIZES=()
    USB_SERIALS=(); USB_WWNS=(); USB_SYSFS=(); USB_DISKSEQS=()
    rows=$(lsblk -dnrb -o PATH,TYPE,TRAN,RO,SIZE,MAJ:MIN 2>/dev/null) || die 'Could not enumerate physical disks.'
    while read -r path type transport ro size majmin extra; do
        [[ -z ${extra:-} && $type == disk && $transport == usb ]] || continue
        if disk_is_safe_usb "$path"; then
            USB_PATHS+=("$path")
            USB_MAJMIN+=("$ID_MAJMIN")
            USB_MODELS+=("$ID_MODEL")
            USB_SIZES+=("$ID_SIZE")
            USB_SERIALS+=("$ID_SERIAL"); USB_WWNS+=("$ID_WWN")
            USB_SYSFS+=("$ID_SYSFS"); USB_DISKSEQS+=("$ID_DISKSEQ")
        fi
    done <<< "$rows"
}

select_usb_disk() {
    local prompt=$1 answer index i
    enumerate_usb_disks
    ((${#USB_PATHS[@]} > 0)) || die 'No eligible USB physical disk was found.'
    printf '\n%s\n' "$prompt"
    for i in "${!USB_PATHS[@]}"; do
        printf '[%u] ' "$((i + 1))"
        show_disk "${USB_PATHS[$i]}"
    done
    printf '[C] Cancel\n'
    read -r -p 'Selection: ' answer
    [[ $answer != [Cc] ]] || exit 0
    [[ $answer =~ ^[0-9]+$ ]] || die 'Invalid USB-disk selection.'
    index=$((answer - 1))
    (( index >= 0 && index < ${#USB_PATHS[@]} )) || die 'Invalid USB-disk selection.'
    SELECTED_DISK_INDEX=$index
    return 0
}

revalidate_selected_disk() {
    local index=$1 disk=${USB_PATHS[$1]}
    disk_is_safe_usb "$disk" || die 'The selected disk no longer passes USB safety checks.'
    [[ $ID_PATH == "$disk" && $ID_MAJMIN == "${USB_MAJMIN[$index]}" &&
       $ID_MODEL == "${USB_MODELS[$index]}" && $ID_SIZE == "${USB_SIZES[$index]}" &&
       $ID_SERIAL == "${USB_SERIALS[$index]}" && $ID_WWN == "${USB_WWNS[$index]}" &&
       $ID_SYSFS == "${USB_SYSFS[$index]}" && $ID_DISKSEQ == "${USB_DISKSEQS[$index]}" ]] ||
        die 'The selected physical disk identity changed.'
}

verify_payload() {
    local name=$1 hash entry_hash entry_name count=0 expected=''
    SOURCE_PATH="$SCRIPT_DIR/$name"
    [[ -f $SOURCE_PATH && ! -L $SOURCE_PATH ]] || die "Package file is missing or is a symbolic link: $name"
    SOURCE_HASH=$(sha256sum -- "$SOURCE_PATH")
    SOURCE_HASH=${SOURCE_HASH%% *}
    [[ ! -L $SCRIPT_DIR/SHA256SUMS ]] || die 'SHA256SUMS is a symbolic link.'
    [[ ! -e $SCRIPT_DIR/SHA256SUMS || -f $SCRIPT_DIR/SHA256SUMS ]] || die 'SHA256SUMS is not a regular sibling file.'
    if [[ -f $SCRIPT_DIR/SHA256SUMS ]]; then
        while read -r entry_hash entry_name; do
            [[ $entry_hash =~ ^[0-9A-Fa-f]{64}$ ]] || continue
            entry_name=${entry_name#\*}
            entry_name=${entry_name#./}
            if [[ ${entry_name##*/} == "$name" ]]; then
                expected=${entry_hash,,}
                ((count += 1))
            fi
        done < "$SCRIPT_DIR/SHA256SUMS"
        (( count == 1 )) || die "SHA256SUMS must contain exactly one entry for $name."
        [[ ${SOURCE_HASH,,} == "$expected" ]] || die "SHA-256 verification failed for $name."
        printf 'Source SHA-256 verified through SHA256SUMS: %s\n' "${SOURCE_HASH,,}"
    else
        printf 'WARNING: SHA256SUMS is absent; file integrity cannot be checked against a package manifest.\n' >&2
        read -r -p 'Type CONTINUE WITHOUT SHA256SUMS to continue: ' confirmation
        [[ $confirmation == 'CONTINUE WITHOUT SHA256SUMS' ]] ||
            die 'Installation cancelled because SHA256SUMS is absent.'
    fi
}

verify_source() {
    verify_payload "$1"
    local efi_path=$SOURCE_PATH efi_hash=$SOURCE_HASH description
    description=$(file -b -- "$efi_path")
    [[ $description == *PE32+* && $description == *EFI\ application* && $description == *x86-64* ]] ||
        die 'Selected source is not an x86-64 EFI application.'
    verify_payload 'pci.ids'
    DATABASE_PATH=$SOURCE_PATH
    DATABASE_HASH=$SOURCE_HASH
    SOURCE_PATH=$efi_path
    SOURCE_HASH=$efi_hash
}

install_pci_database() {
    local boot_dir=$1 destination=$1/pci.ids temp installed_hash
    [[ ! -L $destination && ( ! -e $destination || -f $destination ) ]] ||
        die 'Existing pci.ids is not a regular file.'
    temp=$boot_dir/.pci.ids.ncv-tmp-$$
    [[ ! -e $temp && ! -L $temp ]] || die 'Temporary database destination already exists.'
    TEMP_FILE=$temp
    run_on_volume cp -- "$DATABASE_PATH" "$temp"
    run_on_volume sync -f "$temp"
    installed_hash=$(sha256sum -- "$temp")
    [[ ${installed_hash%% *} == "$DATABASE_HASH" ]] || die 'Temporary pci.ids hash mismatch.'
    run_on_volume mv -f -- "$temp" "$destination"
    TEMP_FILE=''
    run_on_volume sync -f "$destination"
    installed_hash=$(sha256sum -- "$destination")
    [[ ${installed_hash%% *} == "$DATABASE_HASH" ]] || die 'Installed pci.ids hash mismatch.'
    printf 'Installed and verified GPU names: %s\n' "$destination"
}

install_efi() {
    local mountpoint=$1 selected_majmin=$2 efi_dir boot_dir destination choice stamp backup old_hash backup_hash installed_hash temp
    ACTIVE_MOUNT=$mountpoint
    efi_dir=$mountpoint/EFI
    boot_dir=$efi_dir/BOOT
    destination=$boot_dir/BOOTX64.EFI
    run_on_volume mkdir -p -- "$efi_dir"
    [[ $(findmnt -nro MAJ:MIN -T "$efi_dir") == "$selected_majmin" ]] || die 'EFI directory crosses onto another filesystem.'
    run_on_volume mkdir -p -- "$boot_dir"
    [[ $(findmnt -nro MAJ:MIN -T "$boot_dir") == "$selected_majmin" ]] || die 'EFI/BOOT crosses onto another filesystem.'
    [[ ! -L $destination ]] || die 'Existing BOOTX64.EFI is a symbolic link; refusing replacement.'
    [[ ! -e $destination || -f $destination ]] || die 'Existing BOOTX64.EFI is not a regular file.'
    if [[ -f $destination ]]; then
        printf 'WARNING: Existing loader found: %s\n' "$destination" >&2
        read -r -p 'Type B to back it up and replace it, or press Enter to cancel: ' choice
        [[ $choice == B ]] || die 'Installation cancelled; existing loader was preserved.'
        stamp=$(date -u +%Y%m%d-%H%M%S)
        backup=$boot_dir/BOOTX64-backup-$stamp.efi
        [[ ! -e $backup && ! -L $backup ]] || die "Backup already exists: $backup"
        old_hash=$(sha256sum -- "$destination")
        old_hash=${old_hash%% *}
        run_on_volume cp -- "$destination" "$backup"
        run_on_volume sync -f "$backup"
        [[ -f $backup ]] || die 'Existing-loader backup was not created.'
        backup_hash=$(sha256sum -- "$backup")
        backup_hash=${backup_hash%% *}
        [[ $backup_hash == "$old_hash" ]] || die 'Existing-loader backup verification failed.'
        printf 'Existing loader backed up as %s\n' "${backup##*/}"
    fi
    temp=$boot_dir/.BOOTX64.EFI.ncv-tmp-$$
    [[ ! -e $temp && ! -L $temp ]] || die 'Temporary destination already exists.'
    TEMP_FILE=$temp
    run_on_volume cp -- "$SOURCE_PATH" "$temp"
    run_on_volume sync -f "$temp"
    installed_hash=$(sha256sum -- "$temp")
    installed_hash=${installed_hash%% *}
    [[ ${installed_hash,,} == ${SOURCE_HASH,,} ]] || die 'Temporary EFI copy hash verification failed.'
    run_on_volume mv -f -- "$temp" "$destination"
    TEMP_FILE=''
    run_on_volume sync -f "$destination"
    installed_hash=$(sha256sum -- "$destination")
    installed_hash=${installed_hash%% *}
    [[ ${installed_hash,,} == ${SOURCE_HASH,,} ]] || die 'Installed BOOTX64.EFI hash verification failed.'
    install_pci_database "$boot_dir"
    printf 'Installed and verified: %s\nInstalled SHA-256: %s\n' "$destination" "${installed_hash,,}"
}

existing_fat_mode() {
    local disk_index disk path type filesystem answer index current_major current_fs mountpoint options rows
    PART_PATHS=()
    PART_DISK_INDEX=()
    PART_FILESYSTEMS=()
    enumerate_usb_disks
    for disk_index in "${!USB_PATHS[@]}"; do
        disk=${USB_PATHS[$disk_index]}
        rows=$(lsblk -nrpo PATH,TYPE,FSTYPE -- "$disk" 2>/dev/null) ||
            die "Could not enumerate partitions on $disk"
        while read -r path type filesystem; do
            [[ $type == part ]] || continue
            filesystem=${filesystem,,}
            case "$filesystem" in fat|fat32|vfat) ;; *) continue ;; esac
            PART_PATHS+=("$path")
            PART_DISK_INDEX+=("$disk_index")
            PART_FILESYSTEMS+=("$filesystem")
        done <<< "$rows"
    done
    ((${#PART_PATHS[@]} > 0)) || die 'No eligible FAT/FAT32 partition on a USB disk was found.'
    printf '\nEligible existing FAT/FAT32 USB partitions:\n'
    for index in "${!PART_PATHS[@]}"; do
        get_node_mounts "${PART_PATHS[$index]}" || die 'Could not inspect candidate mount state safely.'
        mountpoint=${NODE_MOUNTS[*]:-<not mounted>}
        printf '[%u] %s; filesystem=%s; mount=%s\n' "$((index + 1))" "${PART_PATHS[$index]}" \
            "${PART_FILESYSTEMS[$index]}" "${mountpoint:-<not mounted>}"
    done
    printf '[C] Cancel\n'
    read -r -p 'Selection: ' answer
    [[ $answer != [Cc] ]] || exit 0
    [[ $answer =~ ^[0-9]+$ ]] || die 'Invalid FAT-partition selection.'
    index=$((answer - 1))
    (( index >= 0 && index < ${#PART_PATHS[@]} )) || die 'Invalid FAT-partition selection.'
    disk_index=${PART_DISK_INDEX[$index]}
    disk=${USB_PATHS[$disk_index]}
    revalidate_selected_disk "$disk_index"
    path=${PART_PATHS[$index]}
    [[ -b $path ]] || die 'The selected partition disappeared.'
    lsblk -snrpo PATH -- "$path" | grep -Fxq -- "$disk" || die 'The selected partition changed parent disks.'
    current_major=$(lsblk -dnro MAJ:MIN -- "$path")
    current_fs=$(lsblk -dnro FSTYPE -- "$path" 2>/dev/null) || die 'Could not revalidate the selected filesystem.'
    current_fs=$(trim_text "$current_fs")
    current_fs=${current_fs,,}
    [[ -n $current_major ]] || die 'The selected partition identity is unavailable.'
    case "$current_fs" in fat|fat32|vfat) ;; *) die 'The selected partition is no longer FAT/FAT32.' ;; esac
    get_node_mounts "$path" || die 'Could not revalidate the selected partition mount state.'
    CURRENT_MOUNTS=("${NODE_MOUNTS[@]}")
    if ((${#CURRENT_MOUNTS[@]} == 0)); then
        MOUNT_DIR=$(mktemp -d -t ncvselector.XXXXXXXX)
        as_root mount -o rw,nosuid,nodev -- "$path" "$MOUNT_DIR"
        MOUNT_CREATED=1
        mountpoint=$MOUNT_DIR
    elif ((${#CURRENT_MOUNTS[@]} == 1)); then
        mountpoint=${CURRENT_MOUNTS[0]}
        [[ $mountpoint != *'\x'* ]] || die 'Escaped mount-point paths are not supported safely.'
    else
        die 'The selected partition has multiple mounts; refusing an ambiguous target.'
    fi
    [[ $(findmnt -nro MAJ:MIN -T "$mountpoint") == "$current_major" ]] || die 'Mounted partition identity verification failed.'
    options=$(findmnt -nro OPTIONS -T "$mountpoint")
    [[ ,$options, == *,rw,* ]] || die 'The selected FAT filesystem is not mounted read-write.'
    install_efi "$mountpoint" "$current_major"
}

unmount_selected_disk() {
    local disk=$1 node target i j nodes mounts
    nodes=$(lsblk -nrpo PATH -- "$disk" 2>/dev/null) || die 'Could not enumerate selected-disk nodes for unmounting.'
    mapfile -t DISK_NODES <<< "$nodes"
    for ((i=${#DISK_NODES[@]}-1; i>=0; i--)); do
        node=${DISK_NODES[$i]}
        [[ -n $node ]] || continue
        get_node_mounts "$node" || die "Could not inspect mount state safely: $node"
        for ((j=${#NODE_MOUNTS[@]}-1; j>=0; j--)); do
            target=${NODE_MOUNTS[$j]}
            [[ $target != *'\x'* ]] || die 'Escaped mount-point paths cannot be unmounted safely.'
            as_root umount -- "$target"
        done
    done
    mounts=$(disk_mount_count "$disk") || die 'Could not verify selected-disk mount cleanup.'
    [[ $mounts == 0 ]] || die 'A selected-disk filesystem remains mounted.'
}

erase_mode() {
    local index disk confirmation logical start_sectors partition_sectors format_type format_label partition_rows new_path new_type target_major
    require_commands wipefs sfdisk mkfs.fat
    select_usb_disk 'Eligible USB disks for destructive preparation:'
    index=$SELECTED_DISK_INDEX
    disk=${USB_PATHS[$index]}
    [[ ${USB_DISKSEQS[$index]} =~ ^[1-9][0-9]*$ ]] ||
        die 'Destructive mode needs a kernel disk sequence number to detect USB reconnection.'
    printf '\nSelected USB disk:\n'
    show_disk "$disk"
    printf 'Serial: %s; WWN: %s; connection: %s\n' "${USB_SERIALS[$index]:-<none>}" "${USB_WWNS[$index]:-<none>}" "${USB_DISKSEQS[$index]}"
    # Validate geometry before confirmation, elevation or unmounting anything.
    logical=$(lsblk -dnrbo LOG-SEC -- "$disk") || die 'Could not query logical sector size.'
    logical=$(trim_text "$logical")
    [[ $logical =~ ^[0-9]+$ && $logical -gt 0 ]] || die 'Logical sector size is unavailable.'
    (( logical == 512 )) || die 'A 256 MiB FAT32 USB requires 512-byte logical sectors.'
    start_sectors=$((1048576 / logical))
    partition_sectors=$((268435456 / logical))
    (( USB_SIZES[index] >= 1048576 + 268435456 )) || die 'Selected USB disk needs at least 257 MiB for this layout.'
    printf 'WARNING: ALL DATA ON %s WILL BE DESTROYED.\n'  "$disk" >&2
    printf 'Its filesystems will be unmounted after confirmation.\n'
    printf 'The disk will use DOS/MBR with one active 256 MiB FAT32-LBA partition labeled NCVSELECTOR.\n'
    printf 'All remaining capacity will stay unallocated.\n'
    read -r -p "Type ERASE $disk ${USB_DISKSEQS[$index]} to erase this USB and install: " confirmation
    [[ $confirmation == "ERASE $disk ${USB_DISKSEQS[$index]}" ]] || { printf 'Cancelled; the USB was not changed.\n'; exit 0; }
    if (( EUID != 0 )); then
        command -v sudo >/dev/null 2>&1 || die 'sudo is required for destructive mode.'
        sudo -v
    fi
    revalidate_selected_disk "$index"
    unmount_selected_disk "$disk"
    revalidate_selected_disk "$index"
    [[ $(lsblk -dnrbo LOG-SEC -- "$disk") == "$logical" ]] || die 'Logical sector size changed.'
    revalidate_selected_disk "$index"
    as_root wipefs --lock=yes -a -- "$disk"
    revalidate_selected_disk "$index"
    printf 'label: dos\nunit: sectors\n\nstart=%u, size=%u, type=c, bootable\n' \
        "$start_sectors" "$partition_sectors" | as_root sfdisk --lock=yes --wipe always --wipe-partitions always "$disk"
    if command -v udevadm >/dev/null 2>&1; then as_root udevadm settle; fi
    partition_rows=$(lsblk -nrpo PATH,TYPE -- "$disk" 2>/dev/null) || die 'Could not discover the new partition.'
    NEW_PARTITIONS=()
    while read -r new_path new_type; do
        if [[ $new_type == part ]]; then NEW_PARTITIONS+=("$new_path"); fi
    done <<< "$partition_rows"
    ((${#NEW_PARTITIONS[@]} == 1)) || die 'Exactly one new partition was not discovered from lsblk.'
    TARGET_PARTITION=${NEW_PARTITIONS[0]}
    lsblk -snrpo PATH -- "$TARGET_PARTITION" | grep -Fxq -- "$disk" || die 'New partition ancestry verification failed.'
    revalidate_selected_disk "$index"
    as_root mkfs.fat -F 32 -n NCVSELECTOR "$TARGET_PARTITION"
    revalidate_selected_disk "$index"
    format_type=$(as_root blkid -s TYPE -o value -- "$TARGET_PARTITION") || die 'FAT32 format verification query failed.'
    format_label=$(as_root blkid -s LABEL -o value -- "$TARGET_PARTITION") || die 'Volume-label verification query failed.'
    [[ $format_type == vfat ]] || die 'FAT32 format verification failed.'
    [[ $format_label == NCVSELECTOR ]] || die 'Volume-label verification failed.'
    MOUNT_DIR=$(mktemp -d -t ncvselector.XXXXXXXX)
    revalidate_selected_disk "$index"
    as_root mount -o rw,nosuid,nodev -- "$TARGET_PARTITION" "$MOUNT_DIR"
    MOUNT_CREATED=1
    ACTIVE_MOUNT=$MOUNT_DIR
    target_major=$(lsblk -dnro MAJ:MIN -- "$TARGET_PARTITION") || die 'Prepared partition identity query failed.'
    [[ $(findmnt -nro MAJ:MIN -T "$MOUNT_DIR") == "$target_major" ]] || die 'Prepared partition mount verification failed.'
    install_efi "$MOUNT_DIR" "$target_major"
    run_on_volume sync -f "$MOUNT_DIR/EFI/BOOT/BOOTX64.EFI"
}

show_next_steps() {
    printf '\nNext steps:\n%s\n%s\n%s\n%s\n' \
        '1. Reboot and select the USB UEFI boot option.' \
        '2. Keep the monitor on the firmware display while selecting the secondary GPU and boot disk.' \
        '3. Press Enter to review, then Enter to save Config.ini and continue into boot.' \
        '4. Switch to the selected GPU input during the final five-second switch countdown; the earlier five-second window opens setup.'
    printf 'No second launch is needed after a successful save. Failed checks stop boot.\n'
    printf 'Guides in the extracted package: Docs/INSTALLATION.md and Docs/TROUBLESHOOTING.md\n'
    printf 'WARNING: Secure Boot may reject this unsigned EFI application; this installer never disables it.\n' >&2
}

printf '%s\n' "$PRODUCT"
printf 'Installs an x86-64 EFI application on USB disks only. Internal disks and system ESPs are excluded.\n'
printf '\n[1] Install Release Edition (default)\n[2] Install Debug Edition\n[3] Exit\n'
read -r -p 'Selection [1]: ' edition
case "$edition" in
    ''|1) verify_source 'NativeCsmVgaSelector-1.2.efi' ;;
    2) verify_source 'NativeCsmVgaSelector-1.2-Debug.efi' ;;
    3) exit 0 ;;
    *) die 'Invalid edition selection.' ;;
esac

printf '\n[1] Install onto an existing FAT/FAT32 partition on a USB disk\n'
printf '[2] Erase and prepare a dedicated USB disk\n[3] Exit\n'
read -r -p 'Selection: ' mode
case "$mode" in
    1) existing_fat_mode ;;
    2) erase_mode ;;
    3) exit 0 ;;
    *) die 'Invalid installation-mode selection.' ;;
esac

show_next_steps

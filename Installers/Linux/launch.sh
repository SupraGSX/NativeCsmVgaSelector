#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# File-manager entry point. Keep terminal ownership separate from disk operations.
[[ ${BASH_SOURCE[0]} == "$0" ]] || { printf 'Run launch.sh; do not source it.\n' >&2; return 1; }
set -u
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || exit 1

if [[ ! -t 0 || ! -t 1 ]]; then
    # A terminal that fails to attach stdin/stdout must not spawn us repeatedly.
    if [[ ${1-} == --terminal ]]; then
        printf 'ERROR: The terminal did not provide interactive input and output.\n' >&2
        exit 1
    fi
    if [[ -n ${DISPLAY-}${WAYLAND_DISPLAY-} ]]; then
        if command -v gnome-terminal >/dev/null 2>&1; then
            exec gnome-terminal --wait -- bash "$SCRIPT_DIR/launch.sh" --terminal
        elif command -v konsole >/dev/null 2>&1; then
            exec konsole --separate -e bash "$SCRIPT_DIR/launch.sh" --terminal
        elif command -v xfce4-terminal >/dev/null 2>&1; then
            exec xfce4-terminal --disable-server --execute bash "$SCRIPT_DIR/launch.sh" --terminal
        elif command -v x-terminal-emulator >/dev/null 2>&1; then
            exec x-terminal-emulator -e bash "$SCRIPT_DIR/launch.sh" --terminal
        elif command -v xterm >/dev/null 2>&1; then
            exec xterm -e bash "$SCRIPT_DIR/launch.sh" --terminal
        fi
    fi
    message="Could not run the installer in a terminal. Open a terminal in this folder and run: bash ./install.sh"
    printf 'ERROR: %s\nFolder: %s\n' "$message" "$SCRIPT_DIR" >&2
    if command -v zenity >/dev/null 2>&1; then
        zenity --error --title='USB installer' --text="$message" 2>/dev/null || :
    elif command -v notify-send >/dev/null 2>&1; then
        notify-send 'USB installer' "$message" 2>/dev/null || :
    fi
    exit 1
fi

finish() {
    local status=$?
    trap - EXIT INT TERM
    if (( status != 0 )); then
        printf '\nInstaller stopped (exit %s). Read the messages above.\n' "$status"
    fi
    printf '\nPress Enter to close this installer window. '
    read -r _ || :
    exit "$status"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
bash "$SCRIPT_DIR/install.sh"
exit "$?"

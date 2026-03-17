#!/bin/sh
# toggle.sh — Toggle CLUI-CC visibility from Hyprland keybind
#
# Usage in hyprland.conf:
#   bind = ALT, SPACE, exec, ~/.local/bin/clui-toggle
#
# Sends a toggle command to the GTK shell over its Unix socket.

SOCKET="${CLUI_SOCKET:-/tmp/clui-shell.sock}"

if [ ! -S "$SOCKET" ]; then
  echo "CLUI socket not found at $SOCKET — is clui-cc running?" >&2
  exit 1
fi

# Send toggle command as JSON (newline-delimited)
printf '{"cmd":"toggle"}\n' | socat - UNIX-CONNECT:"$SOCKET"

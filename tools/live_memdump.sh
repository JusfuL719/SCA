#!/usr/bin/env bash
# SCAhost --live-memdump — Phase-4 PEB CR3 latch + VIRT_READ bulk.
# Never touches HOOK_INSTALL_DRAW (.text stays canonical; no stale-RVA CTD).
#
# Default: SSH PC1 → C:\SCA\SCAhost.exe -v --live-memdump C:\SCA
# with auto-PEB from get_target_peb.ps1 (override with --peb 0x...).
set -uo pipefail

PC1="${PC1:-pc1@10.0.0.1}"
EXE='C:\SCA\SCAhost.exe'
PEB_PS='C:\SCA\get_target_peb.ps1'

ARGS=("$@")
if [ ${#ARGS[@]} -eq 0 ]; then
  # Forward-slash: PS→cmd hop strips the backslash before non-special chars
  # in single-token argv, so "C:\SCA" arrives as "C:SCA". Windows fopen
  # accepts mixed separators, so C:/SCA + "\r5apex_live_*.bin" works.
  ARGS=(-v --live-memdump C:/SCA)
fi

if ! printf '%s\n' "${ARGS[@]}" | grep -q -- '--peb'; then
  PEB=$(ssh "$PC1" "powershell -NoProfile -ExecutionPolicy Bypass -File $PEB_PS" 2>/dev/null |
    awk '/^PEB *=/ {print $3; exit}')
  if [[ -z "$PEB" || "$PEB" == "0x0" ]]; then
    echo "WARN: PEB auto-grab failed — run with target alive or pass --peb" >&2
  else
    ARGS+=(--peb "$PEB")
    echo "auto-PEB=$PEB"
  fi
fi

ssh "$PC1" "cmd /c \"cd /d C:\\SCA && ${EXE} ${ARGS[*]}\""
RC=$?
echo "==EXIT $RC=="
exit "$RC"

#!/usr/bin/env bash
# SSH-run sca-svc on PC1. Kills any stale instance first.
# Default --rva 0x26B87D = cmd-list wrapper Close call site (per-frame draw hook).
# PEB is auto-grabbed via get_target_peb.ps1 on PC1 to drive Phase 4 scanner;
# pass --peb explicitly to override.
#
# Auto timing: WAIT_TARGET_SEC>0 runs wait_target_peb.ps1 first (poll + settle + PEB read).
# Phase-4 CR3 capture needs a live r5apex_dx12 user PEB; blind Phase-1 is a fallback only.
# Example:  WAIT_TARGET_SEC=300 ./tools/run_installer.sh
set -uo pipefail

PC1=pc1
EXE='C:\SCA\sca-svc.exe'
PEB_PS='C:\SCA\get_target_peb.ps1'
WAIT_PEB_PS='C:\SCA\wait_target_peb.ps1'
WAIT_TARGET_SEC="${WAIT_TARGET_SEC:-0}"
WAIT_TARGET_SETTLE="${WAIT_TARGET_SETTLE:-5}"
WAIT_TARGET_POLL="${WAIT_TARGET_POLL:-2}"

ARGS=("$@")
if [ ${#ARGS[@]} -eq 0 ]; then
    ARGS=(-v --rva 0x26B87D)
fi

# Kill stale instance
ssh "$PC1" 'powershell -NoProfile -Command "Get-Process sca-svc,apphost,pexsvc -ErrorAction SilentlyContinue | Stop-Process -Force"' >/dev/null 2>&1 || true

# Auto-grab PEB unless caller supplied --peb
if ! printf '%s\n' "${ARGS[@]}" | grep -q -- '--peb'; then
    if [[ "${WAIT_TARGET_SEC}" =~ ^[0-9]+$ ]] && [ "${WAIT_TARGET_SEC}" -gt 0 ]; then
        echo "[*] wait for target PEB (timeout=${WAIT_TARGET_SEC}s settle=${WAIT_TARGET_SETTLE}s)..."
        PEB_OUT=$(ssh "$PC1" "powershell -NoProfile -ExecutionPolicy Bypass -File $WAIT_PEB_PS -TimeoutSec $WAIT_TARGET_SEC -PollSec $WAIT_TARGET_POLL -SettleSec $WAIT_TARGET_SETTLE" 2>/dev/null) \
            || PEB_OUT=""
        PEB=$(printf '%s\n' "$PEB_OUT" | awk '/^PEB *=/ {print $3; exit}')
        if [[ -z "$PEB" || "$PEB" == "0x0" ]]; then
            echo "[FAIL] wait_target_peb timed out or PEB read failed — fix: launch target or raise WAIT_TARGET_SEC" >&2
            exit 2
        fi
        ARGS+=(--peb "$PEB")
        echo "auto-PEB=$PEB"
    else
        PEB=$(ssh "$PC1" "powershell -NoProfile -ExecutionPolicy Bypass -File $PEB_PS" 2>/dev/null \
                | awk '/^PEB *=/ {print $3; exit}')
        if [[ -z "$PEB" || "$PEB" == "0x0" ]]; then
            echo "WARN: PEB auto-grab failed (target not running? $PEB_PS missing?) — running blind Phase 1" >&2
        else
            ARGS+=(--peb "$PEB")
            echo "auto-PEB=$PEB"
        fi
    fi
fi

ssh "$PC1" "cmd /c \"${EXE} ${ARGS[*]}\""
RC=$?
echo "==EXIT $RC=="
exit $RC

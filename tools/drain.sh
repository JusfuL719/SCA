#!/usr/bin/env bash
# Drain HV log.
# Default: live via SCAhost.exe --drain (HV stays up). Writes drain to C:\SCA\.
# --reboot: post-mortem path via D:\drain.flag + reboot (use after BSOD/wedge only).
#
# WAIT_TARGET_SEC>0: poll for r5apex_dx12 + PEB before drain (same as run_installer.sh).
set -uo pipefail

PC1=pc1
EXE='C:\SCA\SCAhost.exe'
PEB_PS='C:\SCA\get_target_peb.ps1'
WAIT_PEB_PS='C:\SCA\wait_target_peb.ps1'
WAIT_TARGET_SEC="${WAIT_TARGET_SEC:-0}"
WAIT_TARGET_SETTLE="${WAIT_TARGET_SETTLE:-5}"
WAIT_TARGET_POLL="${WAIT_TARGET_POLL:-2}"
LOCAL_DRAINS="$(dirname "$(readlink -f "$0")")/../drains"
DECODER="$(dirname "$(readlink -f "$0")")/../LOG_DECODER.txt"

MODE=live
for a in "$@"; do
    [[ "$a" == "--reboot" ]] && MODE=reboot
done

mkdir -p "$LOCAL_DRAINS"

if [[ "$MODE" == reboot ]]; then
    echo "[*] reboot drain: touch D:\\drain.flag + shutdown /r"
    ssh "$PC1" 'cmd /c "echo. > D:\drain.flag"'
    ssh "$PC1" 'shutdown /r /t 0 /f'
    echo "[*] waiting for PC1..."
    until ssh -o ConnectTimeout=5 "$PC1" 'echo up' 2>/dev/null; do sleep 5; done
    REMOTE_GLOB='D:\hypedbg-*.bin'
    REMOTE_PFX='D:'
else
    if [[ "${WAIT_TARGET_SEC}" =~ ^[0-9]+$ ]] && [ "${WAIT_TARGET_SEC}" -gt 0 ]; then
        echo "[*] wait for target PEB (timeout=${WAIT_TARGET_SEC}s)..."
        PEB_OUT=$(ssh "$PC1" "powershell -NoProfile -ExecutionPolicy Bypass -File $WAIT_PEB_PS -TimeoutSec $WAIT_TARGET_SEC -PollSec $WAIT_TARGET_POLL -SettleSec $WAIT_TARGET_SETTLE" 2>/dev/null) \
            || PEB_OUT=""
        PEB=$(printf '%s\n' "$PEB_OUT" | awk '/^PEB *=/ {print $3; exit}')
    else
        PEB=$(ssh "$PC1" "powershell -NoProfile -ExecutionPolicy Bypass -File $PEB_PS" 2>/dev/null \
                | awk '/^PEB *=/ {print $3; exit}')
    fi
    if [[ -z "$PEB" || "$PEB" == "0x0" ]]; then
        echo "[FAIL] PEB auto-grab failed — target running? or set WAIT_TARGET_SEC=300" >&2
        exit 2
    fi
    echo "[*] live drain: $EXE --peb $PEB --drain"
    ssh "$PC1" "cmd /c \"cd /d C:\\SCA && ${EXE} -v --peb ${PEB} --drain\""
    RC=$?
    [[ $RC -eq 0 ]] || { echo "[FAIL] SCAhost rc=$RC" >&2; exit $RC; }
    REMOTE_GLOB='C:\SCA\hypedbg-live-*.bin'
    REMOTE_PFX='C:/SCA'
fi

LATEST=$(ssh "$PC1" "powershell -NoProfile -Command \"(Get-ChildItem '$REMOTE_GLOB' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).Name\"" 2>/dev/null | tr -d '\r\n ')
[[ -n "$LATEST" ]] || { echo "[FAIL] no hypedbg-*.bin on PC1" >&2; exit 3; }

DEST="$LOCAL_DRAINS/$LATEST"
echo "[*] scp ${PC1}:${REMOTE_PFX}/${LATEST} -> $DEST"
scp -q "${PC1}:${REMOTE_PFX}/${LATEST}" "$DEST"

echo ""
echo "[*] decode (all HvLog markers):"
# -n 3: catches 3-char codes (E47, V9A, VB0, VC0, etc.) that -n 4 drops
CODES=$(strings -n 3 "$DEST" | grep -oE '^[A-Z][A-Z0-9]{1,5}' | sort -u)

if [[ -f "$DECODER" ]]; then
    echo "$CODES" | while read -r code; do
        desc=$(awk -v c="$code" '$0 ~ "^[ \t]*"c" *=" { sub(/^[^=]+=[ \t]*/,""); print; exit }' "$DECODER")
        if [[ -n "$desc" ]]; then
            printf "  %-8s %s\n" "$code" "$desc"
        fi
    done
else
    echo "$CODES" | awk '{ printf "  %s\n", $0 }'
fi

echo ""
echo "[OK] $DEST"

# Rotation: keep newest 20 hypedbg-live-*.bin; delete the rest.
ls -1t "$LOCAL_DRAINS"/hypedbg-live-*.bin 2>/dev/null | tail -n +21 | xargs -r rm --

#!/usr/bin/env bash
# Push last SCA HV build to PC1 HYPEBOOT USB. Override drive: DRIVE=X.

set -euo pipefail

EDK2=/srv/nfs/shared/Shared/Tools/EDK2
EFI_OUT="$EDK2/Build/SCAPkg/RELEASE_GCC/X64/PlatformInit.efi"
DRAIN_OUT="$EDK2/Build/SCAPkg/RELEASE_GCC/X64/HypeDrain.efi"
PC1=pc1@10.0.0.1

c_red()  { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
c_step() { printf '\033[36m== %s ==\033[0m\n' "$*"; }
die()    { c_red "$*"; exit 1; }

[[ -f "$EFI_OUT" ]]   || die "no PlatformInit.efi build artifact at $EFI_OUT — run build_sca_hv.sh first"
[[ -f "$DRAIN_OUT" ]] || die "no HypeDrain.efi build artifact at $DRAIN_OUT — run build_sca_hv.sh first"

c_step "PC1 reachability"
ssh -o ConnectTimeout=4 -o BatchMode=yes "$PC1" 'hostname' >/dev/null 2>&1 \
    || die "pc1 SSH unreachable — check sshd / firewall / lab-connectivity.md"
c_grn "pc1 reachable"

c_step "Detect HYPEBOOT USB drive letter"
if [[ -n "${DRIVE:-}" ]]; then
    LET="$DRIVE"
else
    LET=$(ssh -o ConnectTimeout=4 "$PC1" \
        'powershell -NoProfile -Command "Get-Volume -FileSystemLabel HYPEBOOT -ErrorAction SilentlyContinue | Select-Object -ExpandProperty DriveLetter"' \
        2>/dev/null | tr -d '\r\n ' | head -c1)
fi
[[ -n "$LET" && "$LET" =~ ^[A-Za-z]$ ]] \
    || die "no HYPEBOOT USB found on PC1 — plug in or set DRIVE=X"
c_grn "USB at ${LET}:"

REMOTE_DIR="${LET}:\\EFI\\Boot"
REMOTE_PI="${LET}:/EFI/Boot/PlatformInit.efi"
REMOTE_DR="${LET}:/HypeDrain.efi"

c_step "SCP PlatformInit.efi → ${PC1}:/${REMOTE_PI}"
scp -q "$EFI_OUT" "${PC1}:/${REMOTE_PI}"
c_grn "SCP PlatformInit.efi OK ($(stat -c%s "$EFI_OUT") bytes)"

c_step "SCP HypeDrain.efi → ${PC1}:/${REMOTE_DR}"
scp -q "$DRAIN_OUT" "${PC1}:/${REMOTE_DR}"
c_grn "SCP HypeDrain.efi OK ($(stat -c%s "$DRAIN_OUT") bytes)"

c_step "SHA256 verify"
LOCAL_PI=$(sha256sum "$EFI_OUT" | awk '{print toupper($1)}')
LOCAL_DR=$(sha256sum "$DRAIN_OUT" | awk '{print toupper($1)}')
REMOTE_PI_HASH=$(ssh "$PC1" "powershell -NoProfile -Command \"(Get-FileHash '${LET}:\\EFI\\Boot\\PlatformInit.efi' -Algorithm SHA256).Hash\"" 2>&1 | tr -d '\r\n ')
REMOTE_DR_HASH=$(ssh "$PC1" "powershell -NoProfile -Command \"(Get-FileHash '${LET}:\\HypeDrain.efi' -Algorithm SHA256).Hash\"" 2>&1 | tr -d '\r\n ')
[[ "$LOCAL_PI" == "$REMOTE_PI_HASH" ]] || die "PlatformInit.efi hash mismatch — local=$LOCAL_PI remote=$REMOTE_PI_HASH"
[[ "$LOCAL_DR" == "$REMOTE_DR_HASH" ]] || die "HypeDrain.efi hash mismatch — local=$LOCAL_DR remote=$REMOTE_DR_HASH"
c_grn "hashes match"

c_step "${LET}:\\ + ${REMOTE_DIR} final state"
ssh "$PC1" "cmd /c \"dir ${LET}:\\HypeDrain.efi & dir ${REMOTE_DIR} /od /b\""

c_grn ""
c_grn "DONE."
c_grn "  Boot HV: reboot PC1 + F11 -> HYPEBOOT USB (startup.nsh auto-loads PlatformInit + chains bootmgfw)"
c_grn "  Post-reboot drain: tools/drain.sh --reboot (drops drain.flag; HypeDrain runs on next boot)"

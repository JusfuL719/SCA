#!/usr/bin/env bash
# build_deploy_installer.sh — build SCAhost.exe on PC1, push back.
# Unsigned: SCA decoupled from PEX cert tree. PC1 is admin-controlled lab box;
# no WDAC/SmartScreen blocking on hand-launched binaries. Re-add signing only
# if WDAC starts rejecting the exe.

set -euo pipefail

SCA_ROOT=/srv/nfs/shared/Shared/SCA
INSTALLER_DIR=$SCA_ROOT/installer
PC1=pc1@10.0.0.1
TS=$(date +%Y%m%d_%H%M%S)

c_red()  { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
c_step() { printf '\033[36m== %s ==\033[0m\n' "$*"; }
die()    { c_red "$*"; exit 1; }

c_step "PC1 reachability"
ssh -o ConnectTimeout=4 -o BatchMode=yes "$PC1" 'hostname' >/dev/null 2>&1 \
    || die "pc1 SSH unreachable"
c_grn "pc1 reachable"

c_step "Sync build_installer_local.bat to PC1"
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
LOCAL_BAT="$SCRIPT_DIR/build_installer_local.bat"
[[ -f "$LOCAL_BAT" ]] || die "missing $LOCAL_BAT"
ssh "$PC1" 'cmd /c "if not exist C:\\Tools mkdir C:\\Tools"' 2>/dev/null
scp -q "$LOCAL_BAT" "${PC1}:C:/Tools/build_installer_local.bat"
c_grn "synced"

c_step "Tarball installer source"
TGZ="/tmp/sca_installer_${TS}.tgz"
tar -C "$INSTALLER_DIR" --exclude=./build -czf "$TGZ" .
TAR_SIZE=$(stat -c%s "$TGZ")
c_grn "tarball: ${TAR_SIZE} bytes"

c_step "Push tarball → PC1 C:\\Tmp\\sca_installer.tgz"
ssh "$PC1" 'cmd /c "if not exist C:\\Tmp mkdir C:\\Tmp"' 2>/dev/null
scp -q "$TGZ" "${PC1}:C:/Tmp/sca_installer.tgz"
rm -f "$TGZ"
c_grn "scp OK"

c_step "Extract on PC1 → C:\\SCA\\installer\\"
ssh "$PC1" 'powershell -NoProfile -Command "if (Test-Path C:\\SCA\\installer) { Get-ChildItem C:\\SCA\\installer -Exclude build | Remove-Item -Recurse -Force }; if (-not (Test-Path C:\\SCA\\installer)) { New-Item -ItemType Directory -Path C:\\SCA\\installer -Force | Out-Null }"' 2>&1 >/dev/null
# tar -m sets mtimes to now so MSBuild rebuilds.
ssh "$PC1" 'cmd /c "cd /d C:\\SCA\\installer && tar -xmzf C:\\Tmp\\sca_installer.tgz"'
c_grn "extracted"

c_step "Build SCAhost.exe on PC1"
BUILD_LOG="/tmp/sca_build_${TS}.log"
ssh "$PC1" 'cmd /c "C:\\Tools\\build_installer_local.bat"' > "$BUILD_LOG" 2>&1 || {
    c_red "BUILD FAILED — last 30 lines:"
    tail -30 "$BUILD_LOG"
    die "Full log: $BUILD_LOG"
}
grep -q "BUILD_OK" "$BUILD_LOG" || {
    c_red "build returned 0 but no BUILD_OK marker. Last 20 lines:"
    tail -20 "$BUILD_LOG"
    die "Full log: $BUILD_LOG"
}
c_grn "built"

c_step "Pull binary → ${INSTALLER_DIR}/build/Release/"
mkdir -p "${INSTALLER_DIR}/build/Release"
scp -q "${PC1}:C:/SCA/installer/build/Release/SCAhost.exe" \
    "${INSTALLER_DIR}/build/Release/SCAhost.exe"
SIZE_INST=$(stat -c%s "${INSTALLER_DIR}/build/Release/SCAhost.exe")
c_grn "pulled: SCAhost=${SIZE_INST}B (unsigned)"

c_step "Push binary + helper scripts to PC1"
ssh "$PC1" 'cmd /c "if not exist C:\\SCA mkdir C:\\SCA"' 2>/dev/null
scp -q "${INSTALLER_DIR}/build/Release/SCAhost.exe" \
    "${PC1}:C:/SCA/SCAhost.exe"
scp -q "$SCRIPT_DIR/get_target_peb.ps1" \
    "${PC1}:C:/SCA/get_target_peb.ps1"
scp -q "$SCRIPT_DIR/wait_target_peb.ps1" \
    "${PC1}:C:/SCA/wait_target_peb.ps1"
c_grn "pushed → C:\\SCA\\SCAhost.exe + get_target_peb.ps1 + wait_target_peb.ps1"

c_step "Hash verify"
verify_hash() {
    local local_path=$1 remote_path=$2
    local lh rh
    lh=$(sha256sum "$local_path" | awk '{print toupper($1)}')
    rh=$(ssh "$PC1" "powershell -NoProfile -Command \"(Get-FileHash '${remote_path}' -Algorithm SHA256).Hash\"" 2>&1 | tr -d '\r\n ')
    [[ "$lh" == "$rh" ]] || die "hash mismatch on $remote_path: local=$lh remote=$rh"
}
verify_hash "${INSTALLER_DIR}/build/Release/SCAhost.exe"  'C:\SCA\SCAhost.exe'
verify_hash "$SCRIPT_DIR/get_target_peb.ps1"               'C:\SCA\get_target_peb.ps1'
verify_hash "$SCRIPT_DIR/wait_target_peb.ps1"              'C:\SCA\wait_target_peb.ps1'
c_grn "hashes match"

c_grn ""
c_grn "DONE."
c_grn "  SCAhost.exe  ${SIZE_INST} B   PC1: C:\\SCA\\SCAhost.exe"
c_grn ""
c_grn "Run installer on PC1 (post-HV-boot, post-game-launch):"
c_grn "    C:\\SCA\\SCAhost.exe -v --rva 0xDEADBEEF --scratch 0x0 --pso 0"

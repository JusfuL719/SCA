#!/usr/bin/env bash
# SCA HV build only — in-tree, no rsync. PACKAGES_PATH points at SCA root.

set -euo pipefail

SCA_ROOT=/srv/nfs/shared/Shared/SCA
EDK2=/srv/nfs/shared/Shared/Tools/EDK2
EFI_OUT="$EDK2/Build/SCAPkg/RELEASE_GCC/X64/PlatformInit.efi"
DRAIN_OUT="$EDK2/Build/SCAPkg/RELEASE_GCC/X64/HypeDrain.efi"
TS=$(date +%Y%m%d_%H%M%S)

c_red()   { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn()   { printf '\033[32m%s\033[0m\n' "$*"; }
c_step()  { printf '\033[36m== %s ==\033[0m\n' "$*"; }
die()     { c_red "$*"; exit 1; }

[[ -d "$SCA_ROOT" ]] || die "missing $SCA_ROOT (canonical SCA HV source)"
[[ -f "$SCA_ROOT/SCAPkg.dsc" ]] || die "missing $SCA_ROOT/SCAPkg.dsc"

c_step "LOG_DECODER drift check"
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
if ! "$SCRIPT_DIR/check_log_decoder.py"; then
    die "LOG_DECODER.txt out of sync with HvLog/HvLogHex emit sites — fix above and rerun"
fi

c_step "EDK2 build (GCC, RELEASE, X64) — PlatformInit.efi + HypeDrain.efi"
cd "$EDK2"
set +eu
# shellcheck disable=SC1091
PYTHON_COMMAND=python3 . ./edksetup.sh BaseTools >/dev/null 2>&1
export PATH="$PWD/BaseTools/BinWrappers/PosixLike:$PATH"
export PACKAGES_PATH="$EDK2:$SCA_ROOT"
export GCC_BIN="$(dirname "$(which gcc)")/"
set -eu

rm -rf "$EDK2/Build/SCAPkg"  # clean build

BUILD_LOG="/tmp/sca_hv_build_${TS}.log"
if ! build -p SCAPkg.dsc -a X64 -t GCC -b RELEASE > "$BUILD_LOG" 2>&1; then
    c_red "BUILD FAILED — last 40 lines:"
    tail -40 "$BUILD_LOG"
    die "Full log: $BUILD_LOG"
fi
if ! grep -qE '^- Done -$' "$BUILD_LOG"; then
    c_red "build returned 0 but no '- Done -' marker. Last 20 lines:"
    tail -20 "$BUILD_LOG"
    die "Full log: $BUILD_LOG"
fi
[[ -f "$EFI_OUT" ]]   || die "missing $EFI_OUT after build"
[[ -f "$DRAIN_OUT" ]] || die "missing $DRAIN_OUT after build"
c_grn "build OK"
c_grn "  PlatformInit.efi: $(stat -c%s "$EFI_OUT") bytes @ $(date -r "$EFI_OUT" '+%H:%M:%S')"
c_grn "  HypeDrain.efi:    $(stat -c%s "$DRAIN_OUT") bytes @ $(date -r "$DRAIN_OUT" '+%H:%M:%S')"

c_grn ""
c_grn "DONE.  Next: tools/deploy_sca_hv.sh"

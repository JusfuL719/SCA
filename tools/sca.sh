#!/usr/bin/env bash
# Full SCA iteration. Modes: hv | installer | build | all (default).

set -euo pipefail

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
MODE="${1:-all}"

c_red()  { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
c_ylw()  { printf '\033[33m%s\033[0m\n' "$*"; }
c_step() { printf '\033[35m== [sca.sh:%s] %s ==\033[0m\n' "$MODE" "$*"; }
die()    { c_red "$*"; exit 1; }

case "$MODE" in
    hv)
        c_step "HV build"
        "$SCRIPT_DIR/build_sca_hv.sh"
        c_step "HV deploy"
        "$SCRIPT_DIR/deploy_sca_hv.sh"
        ;;
    installer)
        c_step "installer build + sign + deploy"
        "$SCRIPT_DIR/build_deploy_installer.sh"
        ;;
    build)
        c_step "HV build (no deploy)"
        "$SCRIPT_DIR/build_sca_hv.sh"
        ;;
    all|"")
        c_step "HV build"
        "$SCRIPT_DIR/build_sca_hv.sh"
        c_step "HV deploy"
        "$SCRIPT_DIR/deploy_sca_hv.sh"
        c_step "installer build + sign + deploy"
        "$SCRIPT_DIR/build_deploy_installer.sh"
        ;;
    *)
        die "unknown mode '$MODE' — use: hv | installer | build | all"
        ;;
esac

c_grn ""
c_grn "DONE.  Next:"
c_grn "  1. PC1: reboot, F11 → boot HYPEBOOT USB (startup.nsh auto-loads HV + chains Windows)"
c_grn "  2. Win boots → SSH to PC1 → run installer:"
c_grn "       ./tools/run_installer.sh                 # default args"
c_grn "       ./tools/run_installer.sh -v --rva 0xNNNN # custom RVA"
c_grn "  3. Drain log live (HV stays up):"
c_grn "       ./tools/drain.sh                # live drain default"
c_grn "       ./tools/drain.sh --reboot       # post-BSOD/wedge only"

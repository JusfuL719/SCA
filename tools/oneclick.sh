#!/usr/bin/env bash
# oneclick.sh — fire-and-forget operator side:
#
#   1. Wait up to 10 min for r5apex_dx12 to appear on PC1.
#   2. Auto-grab target PEB.
#   3. Fire SCAhost.exe install — internally arms Cr3PassiveSample
#      and polls until in-game CR3 is captured (V72/V73 in drain).
#   4. Exits when install completes (or installer's own poll times out).
#
# Pass extra SCAhost args through:
#
#   ./tools/oneclick.sh                                   # default install
#   ./tools/oneclick.sh -v --rva 0x26B87D                 # custom RVA
#   ./tools/oneclick.sh --render-sink-indirect 0xE045E70 0x40 32 \
#                       --sentinel "ZZZZZZZZ"             # sentinel-test
#
# Tuning knobs (env):
#   WAIT_TARGET_SEC      = 600    (10-min outer timeout)
#   WAIT_TARGET_SETTLE   = 5      (delay between proc-up and PEB read)
#   WAIT_TARGET_POLL     = 2      (poll interval inside wait_target_peb)

set -uo pipefail

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
exec env \
    WAIT_TARGET_SEC="${WAIT_TARGET_SEC:-600}" \
    WAIT_TARGET_SETTLE="${WAIT_TARGET_SETTLE:-5}" \
    WAIT_TARGET_POLL="${WAIT_TARGET_POLL:-2}" \
    "$SCRIPT_DIR/run_installer.sh" "$@"

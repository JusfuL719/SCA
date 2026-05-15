#!/usr/bin/env bash
# sentinel_test_rui.sh — operator-paced sentinel-test loop for the
# RUI/convar string-sink candidates from
# Tools/UNICORN_DUMPER/output/canon/rui_sink_*.jsonl.
#
# Workflow (operator-driven):
#   1. Boot PC1 into the fresh HV (reboot + F11 → HYPEBOOT).
#   2. Launch Apex, get into a match.
#   3. ssh pc1@10.0.0.1 'C:\SCA\wait_target_peb.ps1' to capture target PEB.
#   4. Run this script with the captured PEB. It paces through the
#      top-N candidates printed by probe_rui_sink.py, sentinel-testing
#      each. After each one the operator hits ENTER to advance, or 'p'
#      to mark the current candidate as PROMOTED (renders on screen).
#
# Per-candidate command shape (matches promote_winner.md):
#   SCAhost.exe -v --peb <peb> --render-sink-indirect <RVA> <PSTRING_OFF> <LEN> \
#               --sentinel "<SENTINEL>"
#
# HV deref reads (ImageBase + RVA + PSTRING_OFF) as a 64-bit pointer
# cell and writes the sentinel into the heap buffer at the resolved
# VA. Restore on slot expiry (default 300 ticks) flips it back.

set -euo pipefail

PC1=pc1@10.0.0.1
JSONL_DEFAULT=/srv/nfs/shared/Shared/Tools/UNICORN_DUMPER/output/canon/rui_sink_$(date +%Y-%m-%d).jsonl

PEB=""
JSONL=""
SENTINEL="ZZZZZZZZ"
TOP_N=10
HOLD_SECS=12

usage() {
    cat <<EOF
usage: $0 --peb <hex> [--jsonl PATH] [--sentinel STR] [--top N] [--hold SECS]

  --peb       target PEB VA (hex). Capture via wait_target_peb.ps1.
  --jsonl     probe output JSONL (default: today's rui_sink_<date>.jsonl).
  --sentinel  string to splat into each candidate (default: "ZZZZZZZZ").
  --top       max candidates to try, ordered by probe rank (default: 10).
  --hold      seconds to leave each sentinel in place before advancing
              (default: 12). Slot expiry default is 300 payload ticks
              (~75 ms each at the cycle-9k+ rate), so any value here >5
              gives the engine many frames to paint.

Inside the loop: ENTER advances, 'p' marks current as PROMOTED, 'q' quits.
EOF
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --peb)      PEB="$2"; shift 2;;
        --jsonl)    JSONL="$2"; shift 2;;
        --sentinel) SENTINEL="$2"; shift 2;;
        --top)      TOP_N="$2"; shift 2;;
        --hold)     HOLD_SECS="$2"; shift 2;;
        -h|--help)  usage;;
        *) printf 'unknown arg: %s\n' "$1" >&2; usage;;
    esac
done

[[ -n "$PEB" ]] || { printf 'need --peb\n' >&2; usage; }
[[ -n "$JSONL" ]] || JSONL="$JSONL_DEFAULT"
[[ -f "$JSONL" ]] || { printf 'no jsonl at %s\n' "$JSONL" >&2; exit 1; }

c_red()  { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
c_yel()  { printf '\033[33m%s\033[0m\n' "$*"; }
c_step() { printf '\033[36m== %s ==\033[0m\n' "$*"; }

c_step "PC1 reachability"
ssh -o ConnectTimeout=4 -o BatchMode=yes "$PC1" 'hostname' >/dev/null 2>&1 \
    || { c_red "pc1 SSH unreachable"; exit 1; }
c_grn "pc1 reachable"

# Sort actionable records by rank (lower = better), take top-N. Emit
# space-separated: name rank convar_rva pstring_off len flags default
mapfile -t ROWS < <(python3 - "$JSONL" "$TOP_N" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    recs = [json.loads(l) for l in f if l.strip()]
ok = [r for r in recs if r.get('status') in ('ok', 'ok_bad_flags')]
ok.sort(key=lambda r: (r.get('rank', 999), r['name']))
for r in ok[:int(sys.argv[2])]:
    print('%s\t%d\t0x%X\t0x%X\t32\t0x%X\t%s' % (
        r['name'], r.get('rank', 999),
        r['convar_global_rva'], r['pstring_slot_off'],
        r.get('flags_value') or 0,
        (r.get('default_preview') or '')[:24].replace('\t', ' ')))
PY
)

c_grn "loaded ${#ROWS[@]} candidate(s) from $JSONL"
echo

PROMOTED=()
for ROW in "${ROWS[@]}"; do
    IFS=$'\t' read -r NAME RANK CONVAR_RVA PSTRING_OFF LEN FLAGS DEFAULT <<<"$ROW"
    c_step "candidate rank=$RANK  $NAME"
    printf '  convar_global_rva = %s\n' "$CONVAR_RVA"
    printf '  pstring_off       = %s  (Source m_pszString default)\n' "$PSTRING_OFF"
    printf '  len               = %s\n' "$LEN"
    printf '  flags (probed)    = %s\n' "$FLAGS"
    printf '  default (probed)  = %q\n' "$DEFAULT"
    printf '  command:\n'
    printf '    SCAhost.exe -v --peb %s --render-sink-indirect %s %s %s --sentinel "%s"\n' \
           "$PEB" "$CONVAR_RVA" "$PSTRING_OFF" "$LEN" "$SENTINEL"
    echo

    # Fire on PC1.
    LOG=$(mktemp)
    ssh "$PC1" "C:\\SCA\\SCAhost.exe -v --peb ${PEB} --render-sink-indirect ${CONVAR_RVA} ${PSTRING_OFF} ${LEN} --sentinel \"${SENTINEL}\"" \
        > "$LOG" 2>&1 || true
    tail -20 "$LOG" | sed 's/^/    /'
    rm -f "$LOG"

    c_yel "  *** look at the Apex HUD for '$SENTINEL' (~${HOLD_SECS}s) ***"
    sleep "$HOLD_SECS"

    # Operator dispatch.
    printf '  [ENTER]=next  p=promote  q=quit  > '
    read -r KEY
    case "$KEY" in
        p|P)
            PROMOTED+=("$NAME ($CONVAR_RVA +$PSTRING_OFF len=$LEN flags=$FLAGS)")
            c_grn "  marked PROMOTED: $NAME"
            ;;
        q|Q)
            c_yel "  quitting at operator request"
            break
            ;;
        *) ;;
    esac
    echo
done

# Reset sink to canon before exit so we don't leave a stale override.
c_step "reset sink override to canon"
ssh "$PC1" "C:\\SCA\\SCAhost.exe -v --peb ${PEB} --render-sink 0 0" 2>&1 \
    | tail -5 | sed 's/^/  /'

echo
if [[ ${#PROMOTED[@]} -eq 0 ]]; then
    c_red "no candidates promoted. Run promote_winner.md fallback (Tier 2)."
else
    c_grn "promoted ${#PROMOTED[@]} candidate(s):"
    for p in "${PROMOTED[@]}"; do printf '  %s\n' "$p"; done
    c_grn ""
    c_grn "Next: edit SCA/HypeApexCanon.h per promote_winner.md step 1,"
    c_grn "      flip HypeRender.c canon path to derefed heap VA (step 2),"
    c_grn "      rebuild + redeploy."
fi

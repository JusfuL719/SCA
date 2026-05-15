#!/usr/bin/env bash
# sentinel_probe.sh — operator-facing helper for Track 1.4 (rev-4 netvar pivot).
#
# Five modes (rev-4):
#
#   $0 ents
#       Wrapper for SCAhost.exe --dump-teams. Lists ENT_SNAPSHOT[64]
#       mirror — 64 entity slots with VA + team + alive/downed flags.
#       Use to find the team / deathbox / scriptprop entity VAs you
#       want to sentinel-test.
#
#   $0 dump  <va> [count_qwords]
#       Generic: --peek-va at <va> for count*8 bytes (default 16 qwords).
#       Use against image+convar_rva (Tier-1) OR entity_va+0 (Tier-0
#       netvar dump of a candidate entity's full struct).
#
#   $0 confirm <buf_va>
#       Reads back the first 4 qwords at <buf_va>, decodes as ASCII.
#       Use against entity_va+netvar_offset to confirm the slot
#       currently holds text (e.g. team name, deathbox owner).
#
#   $0 sentinel <buf_va> <buf_capacity> [sentinel_text]
#       Atomic SET_SINK + RENDER_TEXT push. Default sentinel is the
#       distinctive "ZZZZZZZZ" so screen-presence is unambiguous.
#       Look at the HUD in-match within the next ~30 sec. If the
#       sentinel appears anywhere on screen → that's the sink, promote
#       it to canon. If nothing visible after 30 sec → drop the
#       candidate. The sink reverts automatically via the 300-tick
#       expire path (RDV in drain).
#
#   $0 reset
#       Fires SET_SINK with rva=0,len=0 — reverts to canon defaults.
#       Use between candidate iterations to clear any partial state.
#
# Convention: all RVAs in hex (0x prefix). VAs (full 64-bit) also in hex.
# PEB is auto-grabbed via wait_target_peb.ps1.
#
# === Quick-reference: rev-4 candidate offsets ===
#   m_szTeamname       (DT_Team)         offset 0x0990   buf_capacity=32-48
#   m_customOwnerName  (DT_DeathBoxProp) offset 0x1660   buf_capacity=32-64
#   m_title            (DT_ScriptProp)   offset 0x1680   buf_capacity=32-64
#
# Rev-4 procedure:
#   1. $0 ents                          # find entity VAs
#   2. $0 dump <ent_va>                 # confirm entity class (peek class name)
#   3. $0 confirm <ent_va + offset>     # peek netvar slot, expect text bytes
#   4. $0 sentinel <ent_va + offset> 32 # write sentinel, look at HUD

set -uo pipefail

PC1=pc1@10.0.0.1
SCAHOST='C:\SCA\SCAhost.exe'
WAIT_PEB_PS1='C:\SCA\wait_target_peb.ps1'

c_red()  { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
c_step() { printf '\033[36m== %s ==\033[0m\n' "$*"; }
die()    { c_red "$*"; exit 1; }

usage() {
    cat >&2 <<EOF
Usage:
  $0 ents                                          # list ENT_SNAPSHOT mirror
  $0 dump <va> [count_qwords=16]                   # generic peek
  $0 confirm <buf_va>                              # peek + ASCII decode
  $0 sentinel <buf_va> <buf_capacity> [text]       # SET_SINK + RENDER_TEXT
  $0 reset                                         # revert sink to canon

All VAs in hex (0x prefix). For entity-resident netvar sinks (rev-4):
  $0 ents                                          # find entity VAs
  $0 sentinel \$((ent_va + 0x990)) 32              # m_szTeamname
  $0 sentinel \$((ent_va + 0x1660)) 32             # m_customOwnerName
  $0 sentinel \$((ent_va + 0x1680)) 32             # m_title
EOF
    exit 2
}

get_peb() {
    PEB=$(ssh "$PC1" "powershell -NoProfile -ExecutionPolicy Bypass -File $WAIT_PEB_PS1 -TimeoutSec 30 -SettleSec 2" 2>/dev/null \
        | awk '/^PEB *=/ {print $3; exit}')
    [[ -n "$PEB" && "$PEB" != "0x0" ]] || die "PEB grab failed — Apex running in-match?"
    c_grn "PEB = $PEB"
}

get_image_base() {
    # Recover image base via a one-shot install probe. Cheaper: do a tiny
    # install with --no-install-confirm and parse the line that prints
    # "imagebase=0xXXXX". Reuses standard install path; HV CR3 recovery
    # gives ImageBase as a side effect.
    OUT=$(ssh "$PC1" "cmd /c \"$SCAHOST -v --peb $PEB --no-install-confirm 2>nul\"" 2>/dev/null) || true
    IMGBASE=$(printf '%s\n' "$OUT" | awk '/imagebase=/{ for(i=1;i<=NF;i++) if($i ~ /^imagebase=/) { sub(/^imagebase=/,"",$i); print $i; exit } }')
    [[ -n "$IMGBASE" ]] || die "ImageBase recovery failed — install path broken?"
    c_grn "ImageBase = $IMGBASE"
}

decode_qwords_ascii() {
    # Stdin: SCAhost --peek-va output. Stdout: ASCII decode per qword (LE).
    awk '/\+0x..:/ {
        gsub("0x", "", $2); v=$2
        out=""
        for(i=length(v); i>=2; i-=2) {
            c=strtonum("0x" substr(v, i-1, 2))
            if(c >= 32 && c < 127) out = out sprintf("%c", c)
            else                    out = out "."
        }
        printf "  %s  %s\n", $1, out
    }'
}

cmd_ents() {
    get_peb
    c_step "ENT_SNAPSHOT[64] mirror dump (rev-4: find entity VAs for netvar sinks)"
    ssh "$PC1" "cmd /c \"$SCAHOST --peb $PEB --dump-teams 2>nul\""
}

cmd_dump() {
    local arg="${1:-}"; local n="${2:-16}"
    [[ -n "$arg" ]] || usage
    get_peb
    local va
    # Heuristic: if the arg looks like an RVA (< 0x100000000 i.e. < 4 GB) treat
    # it as image-relative. If it's a full VA (>= 0x7FF0_0000_0000) take it as-is.
    # In between (heap range 0x100..0x800 in high dword) -> full VA.
    if (( arg < 0x100000000 )); then
        get_image_base
        va=$(printf '0x%x' $((IMGBASE + arg)))
        c_step "dump @ image+$arg (= $va), $n qwords"
    else
        va=$(printf '0x%x' "$arg")
        c_step "dump @ $va, $n qwords"
    fi
    # Installer's --peek-va prints 4 qwords. Loop in groups of 4.
    local i=0
    while (( i < n )); do
        local cur
        cur=$(printf '0x%x' $((va + i*8)))
        ssh "$PC1" "cmd /c \"$SCAHOST --peb $PEB --peek-va $cur 2>nul\"" 2>/dev/null \
            | tee >(decode_qwords_ascii >&2) >/dev/null \
            | grep '+0x'
        i=$((i + 4))
    done
    cat <<EOF

[*] Look for:
    - vtable pointer (high-dword 0x00007FF6_xxxx — Apex .text range)
    - heap pointers (high-dword 0x00000100..0x00000800 — heap range)
    - The pszString slot is typically a heap pointer in the +0x40..+0x70
      range. Decode column shows ASCII — heap pointers decode as garbage
      because they point AT the actual string buffer, they aren't the
      buffer themselves. Test each heap-pointer candidate with:
          $0 confirm <heap_va>
EOF
}

cmd_confirm() {
    local va="${1:-}"
    [[ -n "$va" ]] || usage
    get_peb
    c_step "buffer @ $va (4 qwords + ASCII)"
    ssh "$PC1" "cmd /c \"$SCAHOST --peb $PEB --peek-va $va 2>nul\"" 2>/dev/null \
        | tee >(decode_qwords_ascii >&2) >/dev/null \
        | grep '+0x'
}

cmd_sentinel() {
    local va="${1:-}"; local cap="${2:-}"; local text="${3:-ZZZZZZZZ}"
    [[ -n "$va" && -n "$cap" ]] || usage
    get_peb
    c_step "SENTINEL: --render-sink $va $cap --sentinel \"$text\""
    cat <<EOF
[!] Look at the in-match HUD in the next ~30 sec. The sink expires after
    300 payload ticks (~5 min real time at observed Apex hook rate, or
    ~5s at 60fps if hook rate is high). Sentinel restores via RDV path.
EOF
    ssh "$PC1" "cmd /c \"$SCAHOST -v --peb $PEB --render-sink $va $cap --sentinel \\\"$text\\\" 2>nul\""
}

cmd_reset() {
    get_peb
    c_step "RESET: --render-sink 0 0 (revert to canon defaults)"
    ssh "$PC1" "cmd /c \"$SCAHOST -v --peb $PEB --render-sink 0 0 --no-install-confirm 2>nul\""
}

case "${1:-}" in
    ents)     shift; cmd_ents             ;;
    dump)     shift; cmd_dump     "$@"    ;;
    confirm)  shift; cmd_confirm  "$@"    ;;
    sentinel) shift; cmd_sentinel "$@"    ;;
    reset)    shift; cmd_reset            ;;
    *)        usage                       ;;
esac

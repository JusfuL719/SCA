# SCA installer

User-mode driver for the HV-side draw track. Speaks VMMCALL bootstrap +
NPT-fault covert channel. No DLL load. No overlay window. No persistence.

One binary, several modes. Default = install. Everything else is RE/diag.

## Source

```
installer/
├── main.cpp           argv dispatch + lifecycle
├── auth.h             MixKey / BuildSecret (must match HV HYPE_BUILD_SECRET)
├── vmmcall.h          HANDSHAKE / NPT_CHANNEL_INIT stubs
├── covert.h           mailbox client (Fire / BulkVirtRead8 / BulkHvLogRead)
├── scratch_layout.h   64 KB scratch buffer layout
└── CMakeLists.txt     MSVC build
```

## Modes

≤1s = honors the "SCAhost.exe exits ≤1 sec after HV ack" rule. Stays-up = intentionally violates it (RE / live drain).

| Mode | argv | ≤1s? | What it does |
|---|---|---|---|
| `--live-memdump <dir>` | `-v [--peb 0x…]` | minutes | HANDSHAKE→SET_EPROCESS→NPT→Phase-4 CR3→SET_CR3 → stream **`.text` / `.rdata` / `.data`** into `dir\r5apex_live_*.bin` via `BulkVirtRead8` only (**never** runs `PMC_CMD_HOOK_INSTALL_DRAW`). Section RVAs mirror `UNICORN_DUMPER/redump.sh`; adjust `kSec*` constants if a patch reshapes PE. Use this when hook RVA drift would CTD Apex. |
| install (default) | `--rva 0x… [--module-base …] [--target-cr3 …] [--peb …]` + optional `--glow-*` / `--vgui-*` | yes | HANDSHAKE → SET_EPROCESS → NPT_CHANNEL_INIT → CR3 recovery (skip with `--no-recover`) → batched **SET_CR3 + HOOK_INSTALL_DRAW + HOOK_DRAW_PEEK + optional SET_GLOW_PARAMS + optional SET_VGUI_PARAMS in one mailbox round trip** → exit |
| `--reconfig` | `--glow-*` and/or `--vgui-*` knobs | yes | Push runtime knobs on already-armed HV (no install): gGlowParams and optional VGUI probe/backend params. |
| `--drain` | `[--drain-out path]` | stays up ~seconds | Pull 4 MB HV ring via `PMC_CMD_HV_LOG_READ`. Writes `hypedbg-live-<tsc>.bin`. HV keeps running. |
| `--diag-m0f` | — | yes | Read `PMC_CMD_GET_LAPIC_DISARMED_NPF_COUNT` (cycle 15 disarmed-LAPIC-NPF counter) and print. |
| `--drift-check` | `--module-base 0x…` | yes | Sanity-check the canonical `UNICORN_DUMPER/OFFSETS.md` pins (BucketTable_Ptr, BucketCount, SetHighlightId prologue) still resolve against the live Apex binary. One scatter VIRT_READ8 batch. Replaces the retired exhaustive RE probes. |
| `--dump-teams` | requires prior install this boot | yes | PEEK `ENT_SNAPSHOT[64]` mirror + LocalSnapshot.Team (bulkified scatter PEEK). |
| `--peek-ent <ENT_VA>` | optional `--module-base …` | yes | Dump `+0x290..+0x2B0` (HID / STACK[0] / MASK gate bytes). |
| `--peek-va <VA>` | — | yes | Generic 4-qword VIRT_READ8 dump. |
| `--bucket-write <SLOT> <OFF> <VAL>` | `--module-base 0x…` | yes | 4-byte write into `HIGHLIGHT_SETTINGS[slot]+off`. |
| `--hid-set <ENT_VA> <HID>` | `--module-base 0x…` | yes | Invoke `SetHighlightId` via `PMC_CMD_VIRT_CALL` (live RVA `0x818500`, was `0x817600` pre 2026-05-13 +0xF00 re-pin). |

`-v` enables verbose [OK]/[..]/[WARN]/[FAIL] trace on all modes.

## Install lifecycle (default mode)

1. HANDSHAKE — VMMCALL `0x60` (build secret + boot auth key). Probes every
   logical CPU (RDTSC-randomized order) until one accepts.
2. SET_EPROCESS — `0x0A`.
3. NPT_CHANNEL_INIT — VMMCALL `0x50` (trigger VA + mailbox VA). Pinned to
   the virtualized CPU. VMMCALL stub freed immediately after.
4. CR3 recovery — `Cr3PassiveSample` (Phase 1 EPROCESS scan, or Phase 4
   with `--peb`). 30s cap. Skipped if `--no-recover` + `--target-cr3`.
5. **Install-tail batch** — one mailbox round trip packs SET_CR3 (`0x02`)
   + HOOK_INSTALL_DRAW (`0x1B`, Arg1=`module_base+rva`, Arg2=scratch GVA) +
   HOOK_DRAW_PEEK (`0x1C`, Arg1=0 for cloak-copy sentinel verify) +
   optional SET_GLOW_PARAMS (`0x1D`) + optional SET_VGUI_PARAMS (`0x23`).
   The HV mailbox loop dispatches in
   order; `SET_CR3` calls `InvalidateSoftTlb` inside its own handler so
   `HOOK_INSTALL_DRAW` sees the refreshed TargetCr3 within the same batch.
6. Exit.

Install path is ≤1s end-to-end after the HV's first NPF. CR3 recovery
adds polling time, but Phase 4 (PEB-hinted) typically resolves in <50 ms.

## Glow knobs

`--glow-slot` (default 78), `--gate-mask` (0x01), `--filter`
(enemy|all|teammates|off), `--enabled`, `--vis-type`, `--glow-fix`,
`--write-vistype`, `--write-glowfix`. Any one of these flags flips
`glow_set=true` and pushes `SET_GLOW_PARAMS` at end of install (or as
the sole action under `--reconfig`).

## VGUI knobs

`--vgui-enable` (probe_only_arm default), `--vgui-disable`,
`--vgui-probe-only`, `--vgui-dry-arm`, `--vgui-rollback`,
`--vgui-fail-closed`, `--vgui-stable-need`, `--vgui-max-faults`,
`--vgui-global-rva`, `--vgui-slot-draw`, `--vgui-slot-pos`,
`--vgui-slot-color`, `--vgui-slot-font`, `--vgui-text-x`,
`--vgui-text-y`, `--vgui-text-rgba`.

Any `--vgui-*` flag flips `vgui_set=true` and emits `SET_VGUI_PARAMS`
(`0x23`) either at install-tail or as part of `--reconfig`.
With `--vgui-enable` alone, defaults arm read-only probe mode:
`Enabled=1 ProbeOnly=1 Rollback=0 FailClosed=1 StableNeed=8 MaxFaults=4`
using canon `GlobalRva=0x03D4C5A0` (drawtext_vgui_centered iface) and
slots `draw/pos/color/font = 26/36/92/25`. Source:
`output/canon/vgui_dryrun_verdict_2026-05-14.md` (v2 provenance-tracked).

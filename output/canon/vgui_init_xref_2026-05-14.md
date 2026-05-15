# VGUI static vtable disambiguation (no boot)

- Date: `2026-05-14`
- Iface RVA: `0x03D4C5A0`
- Stage 0 survivors: `12`
- Stage A hits: `0`
- Stage A' hits: `0`
- Stage A'' (drains): scanned `11` -> `0` matches (VG1=0 VGV=0 VG2=0 VGQ=0)
- Stage B scored candidates: `12`
- Operator decision: **`hold_fail_closed`**
- Confidence: `low`
- Reason: `stage_b_not_decisive`
- Selected vtable: `0x017F6298`

## Stage 0

- all_real input: `14`
- dropped purecall: `2`
- drop `0x01827068` reason=`purecall_same_fn_in_slots` shared_fn=`0x14144E0`
- drop `0x01827860` reason=`purecall_same_fn_in_slots` shared_fn=`0x14144E0`

## Stage A hits

- none

## Stage A' hits

- none

## Stage A'' (drain markers)

- Drains dir: `/srv/nfs/shared/Shared/SCA/drains`
- Drains scanned: `11`
- Drains with VGUI markers: `0`
- Drains targeting our iface: `0`
- Total markers: VG1=`0` VGV=`0` VG2=`0` VGQ=`0`
- Runtime base used for normalization: `0x7FF659A67000`

## Stage B ranking

| Rank | Vtable RVA | Score |
|---|---:|---:|
| 1 | `0x017F6298` | 86.95 |
| 2 | `0x01816038` | 56.75 |
| 3 | `0x01817308` | 56.75 |
| 4 | `0x0183DB28` | 47.65 |
| 5 | `0x01826990` | 34.05 |
| 6 | `0x01826318` | 25.65 |
| 7 | `0x01815860` | 21.00 |
| 8 | `0x018169E0` | 21.00 |
| 9 | `0x0183E4B8` | 21.00 |
| 10 | `0x01823508` | -38.00 |

### Top candidate `0x017F6298` slot breakdown (top 8 by hits)

| slot | hits | fn_rva | score | real_method | xmm_early | color_imm | rdata_arg | mloads | calls_in_text | steps |
|---:|---:|---|---:|:-:|:-:|:-:|:-:|---:|---:|---:|
| 26 | 26 | `0x23A900` | 6.50 | N | N | N | N | 1 | 0 | 3 |
| 22 | 16 | `0x23A530` | 20.00 | N | Y | N | N | 0 | 0 | 27 |
| 24 | 13 | `0x23A810` | 13.10 | Y | N | N | N | 0 | 0 | 6 |
| 16 | 13 | `0x23A270` | 13.20 | Y | N | N | N | 2 | 0 | 23 |
| 35 | 11 | `0x23ACE0` | 2.75 | N | N | N | N | 0 | 0 | 3 |
| 53 | 9 | `0x23BA50` | 2.25 | N | N | N | N | 0 | 0 | 64 |
| 92 | 8 | `0x237920` | 2.00 | N | N | N | N | 0 | 0 | 64 |
| 152 | 7 | `0x22DD00` | 8.95 | Y | N | N | N | 1 | 1 | 23 |

## Next move

- Keep fail-closed. The plan's 3x margin gate was not met.
- If you want to escalate without booting: rank top-3 candidates and inspect their slot 26 (draw_text) bodies in IDA/Ghidra; the one whose body actually invokes the font + render pipeline wins.
- The HV-side path (`SCA/HypeHookDraw.c::VguiProbeTick`) reads `Iface = guest[ImageBase + CandidateGlobalRva]` then `Vtable = guest[Iface]` and emits `VG1`/`VGV` markers; running the probe with `Enabled=1, ProbeOnly=1` once produces a drain that pins the live vtable definitively.

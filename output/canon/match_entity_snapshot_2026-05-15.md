# Live Match Entity Snapshot — 2026-05-15

Captured via `SCAhost.exe --dump-teams` mid-match. HV's `gEntCache[64]` mirror, populated by `ScanOneEntity` walking `APEX_OFF_ENTITY_LIST`.

## Session

| Field | Value |
|---|---|
| Date | 2026-05-15 |
| PEB (HV's Phase-4 hint) | `0xC69E8B4000` |
| TargetCr3 | `0x000000023B723000` |
| ImageBase | `0x00007FF7537D0000` |
| Slots populated | 49 / 64 (ENT[08]..ENT[56]) |
| All entities classified | `player=1 alive=1 downed=0 decoy=0 loot=0 hid=78` |

## What this confirms

1. **CR3 + ImageBase stable** — same values as earlier `live-memdump` (1:39 PM). Match session has not rolled over.
2. **Engine populates `m_iHealth=0x324` / `m_iTeamNum=0x334` correctly** — `team=N` field is the netvar at `+0x334`, parsed from live entity bytes. Validates `APEX_ENT_TEAM` canon (still 0x334).
3. **All 49 players are alive and visible in the entity list** — full BR lobby, mid-drop or post-drop early-game. 20 teams represented (IDs 2–21).
4. **`hid=78` universal** — engine has assigned the player highlight slot to every alive player. Our glow-write path at HID=78 is redundant with engine's default; the visible lever remains squad-glow (HID=28) and bucket-RGB override, both confirmed working last night.

## Entity table (raw `--dump-teams` output)

| Slot | Ent VA | Team | State |
|---|---|---:|---|
| 08 | `0x000002198A600610` | 15 | alive |
| 09 | `0x000002198A6051C0` | 2 | alive |
| 10 | `0x0000021999BB9DE0` | 12 | alive |
| 11 | `0x000002194B1306F0` | 3 | alive |
| 12 | `0x0000021A9C4C8D80` | 9 | alive |
| 13 | `0x000002194B1352A0` | 15 | alive |
| 14 | `0x0000021A91D6CA90` | 10 | alive |
| 15 | `0x0000021A91D72690` | 12 | alive |
| 16 | `0x0000021A91D78290` | 4 | alive |
| 17 | `0x0000021B1C4E1180` | 7 | alive |
| 18 | `0x0000021B1C4E6D80` | 4 | alive |
| 19 | `0x000002194B139E50` | 16 | alive |
| 20 | `0x0000021B1C4EC980` | 11 | alive |
| 21 | `0x0000021992E90010` | 16 | alive |
| 22 | `0x0000021A92734630` | 10 | alive |
| 23 | `0x0000021AEC0D5510` | 12 | alive |
| 24 | `0x0000021B1C4F2580` | 7 | alive |
| 25 | `0x0000021AEC0DB110` | 7 | alive |
| 26 | `0x0000021A643B4300` | 11 | alive |
| 27 | `0x0000021AEC0E0D10` | 6 | alive |
| 28 | `0x0000021992E94BC0` | 16 | alive |
| 29 | `0x00000219D65D2880` | 6 | alive |
| 30 | `0x0000021AA9C574B0` | 2 | alive |
| 31 | `0x0000021992E9A7C0` | 17 | alive |
| 32 | `0x0000021B1C4F8180` | 9 | alive |
| 33 | `0x0000021A645CA2B0` | 11 | alive |
| 34 | `0x0000021AA97F1C20` | 17 | alive |
| 35 | `0x0000021AEC0C4110` | 8 | alive |
| 36 | `0x0000021AEC0C9D10` | 8 | alive |
| 37 | `0x0000021B1BC42820` | 4 | alive |
| 38 | `0x0000021999BDD710` | 10 | alive |
| 39 | `0x00000219938FC440` | 5 | alive |
| 40 | `0x0000021A91D51F30` | 17 | alive |
| 41 | `0x00000219D65A91D0` | 5 | alive |
| 42 | `0x0000021A91D56AE0` | 18 | alive |
| 43 | `0x00000219CE1203C0` | 6 | alive |
| 44 | `0x00000219999DDEB0` | 5 | alive |
| 45 | `0x0000021A91D5B690` | 18 | alive |
| 46 | `0x0000021A91D61290` | 18 | alive |
| 47 | `0x0000021A91D66E90` | 19 | alive |
| 48 | `0x0000021AEC0CF910` | 19 | alive |
| 49 | `0x0000021A643B9F00` | 19 | alive |
| 50 | `0x0000021993902040` | 20 | alive |
| 51 | `0x0000021B1BC48420` | 20 | alive |
| 52 | `0x00000219861969D0` | 20 | alive |
| 53 | `0x0000021A9C8C2620` | 21 | alive |
| 54 | `0x0000021B1A6B0750` | 21 | alive |
| 55 | `0x0000021A8A566B20` | 21 | alive |
| 56 | `0x00000219D016EB60` | 8 | alive |

## Team distribution

| Team ID | Alive | Slots |
|---:|---:|---|
|  2 | 2 | 09, 30 |
|  3 | 1 | 11 |
|  4 | 3 | 16, 18, 37 |
|  5 | 3 | 39, 41, 44 |
|  6 | 3 | 27, 29, 43 |
|  7 | 3 | 17, 24, 25 |
|  8 | 3 | 35, 36, 56 |
|  9 | 2 | 12, 32 |
| 10 | 3 | 14, 22, 38 |
| 11 | 3 | 20, 26, 33 |
| 12 | 3 | 10, 15, 23 |
| 15 | 2 | 08, 13 |
| 16 | 3 | 19, 21, 28 |
| 17 | 3 | 31, 34, 40 |
| 18 | 3 | 42, 45, 46 |
| 19 | 3 | 47, 48, 49 |
| 20 | 3 | 50, 51, 52 |
| 21 | 3 | 53, 54, 55 |

**Total:** 49 alive across 18 distinct teams. Teams 13/14 absent (already wiped pre-snapshot). One full squad (team 3) reduced to 1 alive, several teams already at 2/3.

## Why these VAs matter

For the netvar visual sink probe (`m_szTeamname` / `m_customOwnerName`), we need a live entity VA. Every row above is a candidate writable address space (`ent + 0x334` already verified for team-ID field).

**Caveat:** `m_szTeamname` is on the DT_Team object, not on player entities. None of these 49 player ents directly hold the team name char array at `+0x990`. To hit the team object we'd need to walk `APEX_OFF_OBSERVER_LIST` (`0x0626AC08`) or find the team-list global.

Alternative: write a sentinel directly into `player_ent + 0x?` at a field that gets rendered in the squad sidebar (e.g., player name / display name slot). Needs RE pass to identify the exact offset.

## Other findings this session

- FPS sink at `image+0x0181B120` (`APEX_FPS_FMT_RVA`): **confirm-killed**. RDP/RDU/RDQ/RDH all fired correctly HV-side, sentinel `"HVPROBE_F01"` written, but **no visible paint** in FPS HUD. `.rdata` printf format-string family is dead in 2026 DX12 Apex (RUI took over).
- VGUI iface candidate `image+0x03D4C5A0`: peek returned `0xFFFFFFFFFFFFFFFF` × 4 (status=ok). Dump at same offset minutes earlier held float-shaped data (~0x3F4DBXXX). Animation/parameter table, **not an iface global**. Static probe's "iface" identification was wrong.
- `Tools/UNICORN_DUMPER/Japex_Jumper/probe_vgui_surface.py:infer_runtime_base()` has a bug: picks the dominant top-32-bit pointer cluster, which is the heap cluster (`0x21..`) instead of the image cluster (`0x7FF7..`). Result: 93 iface RVA validations all run against the wrong base, all reject. Fix: vote per top-32 cluster, pick the cluster with the highest section-band hit rate, not the most numerous one.

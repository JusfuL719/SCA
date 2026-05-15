# Phase-2 sink candidates — rev-4 (netvar pivot)

**Source:** dumper output 2026-05-14:
- `Tools/UNICORN_DUMPER/output/canon/netvar_offsets_2026-05-14.json` (NEW — strong)
- `Tools/UNICORN_DUMPER/output/canon/convar_sinks_2026-05-14.json` (older — weaker)

The dumper's `probe_netvar.py` validation gate **PASSED** (4/4 known-good
offsets matched at delta -36: `m_iHealth=0x324`, `m_iTeamNum=0x334`,
`m_lifeState=0x690`, `m_iMaxHealth=0x468`). The probing strategy is
sound — the candidate offsets below come from the same emulation
methodology, just on text-typed netvar slots.

---

## Tier 0 — netvar string sinks (highest priority, ship Phase 2 from here)

These are heap-resident `char[N]` slots inside entity structs. **Three
critical advantages over the convar approach:**

1. **No `.rdata` integrity-scan risk** — netvars live on entity heap
   allocations that Apex's netcode rewrites every frame. Writing into
   them is shape-identical to legitimate replication.
2. **Engine actively renders them** — `m_customOwnerName` paints when
   you look at a deathbox; `m_szTeamname` paints in the squad sidebar
   and on team-info HUD elements. Verified by xref counts (67-305 writes
   captured by the emulator means the value is being mutated by netcode
   constantly = it gets read by the renderer constantly too).
3. **No FCVAR_USERINFO concerns** — netvars don't trigger userinfo
   broadcasts to the server. Server already replicates them down to us;
   our HV write is invisible to that path.

| Slot | DT class | Offset | Quality | Notes |
|---|---|---|---|---|
| **`m_szTeamname`** | DT_Team | `0x0990` (2448) | **strong** | Team display name. Always exists in-match. **Most actionable starting candidate.** Renders in squad sidebar and team-info HUD. |
| **`m_customOwnerName`** | DT_DeathBoxProp | `0x1660` (5728) | strong | Renders when looking at any deathbox. Requires a deathbox to exist (someone has died). |
| `m_title` | DT_ScriptProp (offset 0x1680) | `0x1680` (5760) | weak (tie) | 305 writes captured — actively replicated. Second xref tied at 0x1980 — needs runtime disambiguation. |

### Operator-runnable procedure (replaces old convar flow)

The existing `--render-sink VA LEN` flag accepts ANY guest VA, so the
flow is unchanged from the rev-3 plan — just point it at
`entity_va + netvar_offset` instead of `image_base + convar_global_rva`.

#### Step A — find a live entity of the right class

For `m_szTeamname`: the local player's team object lives at a fixed
slot in the entity list per match. Use the existing entity-list walker:

1. With Apex running in-match, fire `SCAhost.exe --peb <peb> --dump-teams`
   to dump ENT_SNAPSHOT[64] mirror (HV-side entity cache).
2. Grep output for entries whose class is "DT_Team" / "Team" / similar.
3. Note the `ent=0x...` value — that's the team entity VA.

For `m_customOwnerName`: any deathbox in the match works. Same flow,
filter for "DT_DeathBoxProp" / "DeathBox" / similar in the dump-teams
output.

#### Step B — confirm the slot reads as text

```
SCAhost.exe --peb <peb> --peek-va <entity_va + 0x990>      # for m_szTeamname
SCAhost.exe --peb <peb> --peek-va <entity_va + 0x1660>     # for m_customOwnerName
```

You should see ASCII bytes (the team's actual name, or a deathbox owner
name). If you see all zeros / heap pointers, the offset isn't text on
this entity — try the next candidate.

#### Step C — sentinel write

```
SCAhost.exe -v --peb <peb> --render-sink <entity_va + offset> 32 --sentinel "ZZZZZZZZ"
```

Look at the in-match HUD for "ZZZZZZZZ":
- `m_szTeamname` → squad sidebar, team-info panel
- `m_customOwnerName` → deathbox tooltip when looking at one

If visible → that's the sink. Promote per `promote_winner.md`.

---

## Tier 1 — convar candidates (deprioritized, not deleted)

Original 16 from `convar_sinks_2026-05-14.json`. Only 3 had captured
ConVar globals (`mat_postprocess_enable`, `mat_debug_postprocess_allowed`,
`mat_autoexposure_force_value`). Probably won't render — RUI binding
unconfirmed. Keep as fallback if all three Tier-0 netvars fail.

`rui_args` is the most interesting Tier-1 outlier (RUI engine name
suggests direct binding) but ConVar global wasn't captured by the probe.

---

## Tier 2 — RUI heap-blob plain-text labels (research-grade, last resort)

Only pursue if Tier 0 + Tier 1 both dead-end. `.rui` script blobs are
loaded into heap at game start; they contain plain-text labels like
`"HEALTH"`, `"SHIELDS"`, `"REBOOT"` that the engine paints from heap
without going through any indirection. Find them via heap scan +
xref-back from rendering code. Multi-day RE.

---

## Validation criterion (unchanged from rev-3)

Every candidate must pass on-screen sentinel-write proof. NPT exec trap
proof of the renderer is a "nice-to-have" but not required when the
write is into a netcode-replicated heap slot — the renderer is
empirically known to read these (entity netvars drive the entire HUD by
construction).

---

## Why netvar > convar for this use case

The earlier RUI insight ("HUD is data-driven, no format-string anchors")
specifically pointed at convars because RUI scripts can bind `cvar:NAME`
directly. But RUI also binds **netvar fields** via `entityvar:` /
`netvar:` syntax, and **the netcode-replicated heap surface is much
larger and more reliably rendered.** Almost every text element in Apex
HUD that shows a name (player, team, weapon, deathbox owner, ping
target) is an entity netvar field.

The dumper's netvar probe pinned the entity-side offsets cleanly with
4/4 known-good gate validation. The convar probe was 0-for-many on
captured ConVar globals due to ctor variant complexity. Netvars win on
both methodology rigor and downstream consumer probability.

# Track 1.5 — winner promotion procedure (rev-4 netvar pivot)

When the operator's sentinel-test loop (Track 1.4) lands a candidate that
shows the sentinel string on screen in-match, perform the following
edits in this order. Total ~5 minutes of work.

Two flavors depending on the winning sink shape:

- **Netvar winner (rev-4 Tier-0):** entity-resident `char[N]` slot at
  `entity_va + offset`. The entity VA is dynamic (per-match heap
  allocation) — canon stores the OFFSET + entity-class identification,
  HV resolves the live entity VA at install time via the existing
  entity-list walker.
- **Convar winner (rev-3 Tier-1, fallback):** static ConVar struct in
  `.data` with a `pszString` pointer to a heap buffer. Canon stores the
  ConVar struct RVA + the offset to `pszString`.

## 1. Add canon entry — netvar flavor (preferred)

Edit `SCA/HypeApexCanon.h`. Find the section "render-path text sink (SCA
Phase 2)". Replace the `APEX_FPS_*` block with:

```c
// --- render-path text sink (SCA Phase 2, rev-4 netvar pivot winner) ---
// Pinned <DATE> via sentinel-test loop on the operator's 5950X / Apex
// build <BUILD_HASH>. Netvar: <DT_CLASS::SLOT_NAME>. Backed by heap
// (entity-resident char[N] inline array at entity+offset).
//
// Operator eyeball confirmed sentinel "ZZZZZZZZ" rendered at <screen
// location> (e.g. "squad sidebar team-name slot", "deathbox owner name
// tooltip"). HV `BackendStringHijack` writes here via runtime sink
// override path; canon defaults below describe the netvar shape so
// install-confirm can resolve a live entity at install time.
//
// IMPORTANT: this is ENTITY-RESIDENT, NOT STATIC .data. The HV must
// walk the entity list to find a live instance of the target DT class
// (typically by class-name match against APEX_ENT_NAME at +0x479) and
// add OFFSET below to that entity_va. See ResolveNetvarSink in
// HypeRender.c for the resolver pattern.
#define APEX_RENDER_SINK_NETVAR_OFFSET   0x<NETVAR_OFFSET>ULL
#define APEX_RENDER_SINK_NETVAR_LEN      0x<INLINE_BUF_CAPACITY>ULL
#define APEX_RENDER_SINK_DT_CLASS        "<DT_CLASS_NAME>"  // e.g. "DT_Team"
#define APEX_RENDER_SINK_NAME            "<SLOT_NAME>"      // e.g. "m_szTeamname"
```

Remove or comment-out the old:
- `APEX_FPS_FMT_RVA`, `APEX_FPS_FMT_LEN`
- `APEX_FPS_PRINTF_RVA`, `APEX_FPS_PRINTF_PROLOGUE_LE64`

Add `// DEAD — see SCA/research/hyperjacking_2026_state.md §6.4` next to
each removed pin.

## 1b. Add canon entry — convar flavor (fallback only)

If a convar candidate ends up winning (Tier-1 fallback path), the canon
shape is different — `.data` global with deref'd heap pointer:

```c
// --- render-path text sink (SCA Phase 2, rev-3 convar fallback winner) ---
#define APEX_RENDER_SINK_CONVAR_RVA   0x<CONVAR_GLOBAL_RVA>ULL
#define APEX_RENDER_SINK_PSTRING_OFF  0x<OFFSET_TO_PSTRING_PTR>ULL
#define APEX_RENDER_SINK_LEN          0x<HEAP_BUF_CAPACITY>ULL
#define APEX_RENDER_SINK_CONVAR_NAME  "<CONVAR_NAME>"
```

## 2. Update HypeRender.c default

The HV deref mechanism now exists in code as of rev-4 (`gRenderSinkIndirectOff`
+ `FpsBackupOnce` deref branch). Two ways to set canon:

**Convar winner (Tier-1 path):** flip the default to use the indirect-sink
deref by setting the default `gRenderSinkIndirectOff` to non-zero. Replace
`EffectiveSinkRva` / `EffectiveSinkLen` / `EffectiveSinkIndirectOff`
defaults so a fresh install with NO `--render-sink*` flag still derefs
the winning convar's pszString slot:

```c
static inline UINT64 EffectiveSinkRva(VOID) {
    UINT64 ov = __atomic_load_n(&gRenderSinkRvaOverride, __ATOMIC_ACQUIRE);
    return ov ? ov : APEX_RENDER_SINK_CONVAR_RVA;   // was APEX_FPS_FMT_RVA
}
static inline UINT32 EffectiveSinkLen(VOID) {
    UINT32 ov = __atomic_load_n(&gRenderSinkLenOverride, __ATOMIC_ACQUIRE);
    if (ov == 0) return (UINT32)APEX_RENDER_SINK_LEN;  // was APEX_FPS_FMT_LEN
    return (ov > RENDER_SINK_MAX_LEN) ? RENDER_SINK_MAX_LEN : ov;
}
static inline UINT32 EffectiveSinkIndirectOff(VOID) {
    UINT32 ov = __atomic_load_n(&gRenderSinkIndirectOff, __ATOMIC_ACQUIRE);
    return ov ? ov : (UINT32)APEX_RENDER_SINK_PSTRING_OFF;  // NEW: canon deref
}
```

**Netvar winner (Tier-0 path):** convars + netvar inline char[N] write
directly at an absolute heap VA. Canon shape stores the VA in a runtime-
resolved global (entity walker → entity_va + offset), then sets it via
`HypeRenderSetSink` with IndirectOff=0 and RVA encoded as `(absva -
ImageBase)` — see `BackendStringHijack` for the contract.

Either way, drop `FpsVerifyPrologueOnce`'s `APEX_FPS_PRINTF_RVA` check.
The new canon target has no canon prologue. Replace with a name-string
drift guard if desired (read 8 bytes at `ImageBase + APEX_RENDER_SINK_NAME_RVA`,
compare to `APEX_RENDER_SINK_NAME_LE64`).

Watch for these new log codes during install-confirm:
- `RDI heap_va` — deref succeeded (convar pszString resolved to heap)
- `RDJ ptr_cell_va` — deref read failed (RVA wrong / convar unmapped)
- `RDK heap_va` — deref returned NULL or < 64K (slot uninitialized / wrong offset)
- `RDN indirect_off` — paired with `RDS`/`RDM`, confirms what was installed

## 3. Update sca_render_path_pin.md

Edit `Tools/UNICORN_DUMPER/sca_render_path_pin.md`:
- Change rev to 4.
- Add the winning candidate to the "DEAD" → "LANDED" section.
- Note the sentinel-test outcome (which screen location, render duration).

## 4. Build + deploy + verify

```bash
bash SCA/tools/build_sca_hv.sh
ssh pc1@10.0.0.1 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Tmp\backup_pi.ps1'
bash SCA/tools/deploy_sca_hv.sh
bash SCA/tools/build_deploy_installer.sh
ssh pc1@10.0.0.1 'shutdown /r /t 5 /f'
# After reboot, in-match:
C:\SCA\SCAhost.exe -v --rva 0x54DBD0 --peb <peb>
# Default install-confirm should now visibly render "HV INSTALLED" on screen.
```

To test multiple candidates before promotion (no canon edit yet), use:

```bash
bash SCA/tools/sentinel_test_rui.sh --peb <peb>
```

It paces through the top-ranked candidates from the most recent
`Tools/UNICORN_DUMPER/output/canon/rui_sink_<date>.jsonl`, sentinel-tests
each, and prompts ENTER/p/q.

## 5. Mark Track 1.5 complete in plan
Update plan todos. Done.

---

## Edge cases the operator should think about before committing

- **Convar reset after match end:** if the convar's value is reset by
  Apex on map change / lobby return, our hijacked text disappears
  prematurely. The 300-tick expire + restore path handles this gracefully
  (HV's restore is a no-op if the value already changed). Document.
- **Multi-character set:** if the convar buffer is 32 bytes and we wrote
  "HV INSTALLED" (12 chars), the trailing 20 NUL bytes are fine for
  C-string consumers but RUI may treat the slot as fixed-width. Test
  with a sentinel that exactly fills the buffer and one that's shorter.
- **Server-replicated values:** triple-check the chosen convar isn't
  `FCVAR_USERINFO` or `FCVAR_REPLICATED` — write the FCVAR flags into
  the canon comment. Even if the dumper missed the flag dword, you can
  recover it from `*(uint32_t*)(convar_global + flags_offset)` —
  typically `+0x28` in Source family.
- **AC scan of convar value buffers:** unknown unknown. EAC could
  hash specific convar values to detect tampering. No public evidence,
  but if the rendered text suddenly disappears + game CTD pattern
  matches the previous `.rdata` test, fall back to the dumper-rerun
  path.

# VGUI RE Phase-0 Baseline

- Date: `2026-05-14`
- Scope: baseline anchors + hard-stop envelope before VGUI probing/call-gate work.

## Reconfirmed Render Anchors

Re-run:

```bash
/home/user/.venvs/re/bin/python3 /srv/nfs/shared/Shared/Tools/UNICORN_DUMPER/apex_dumper/probe_render_path.py --dumps /srv/nfs/shared/Shared/Tools/UNICORN_DUMPER/EAC/EAC/r5apex
```

Resolved:
- `render_path_entry = 0x816320`
- `render_gate = 0x8165F0`
- `collector_inner = 0x81646F`
- `set_highlight = 0x817600`
- `set_generic = 0x817700`
- `enable_render = 0x817340`
- `bucket_ptr_va = 0x1469B0600`
- `bucket_count_va = 0x1469B0618`

## Hard Stop Conditions

- `DRR`/`DRX` present in drain: canon/prologue drift; stop cycle and re-pin anchors before next hook install.
- `V0A`/`V0B`/`V0C` bursts or sustained `VAD` correlated to draw path: stop and roll back probe toggle.
- `XC3` seen while dry-run is armed: abort dry-run mode and keep rollback latched.
- VGUI probe fault budget reached (`VGQ` -> `VG3`): keep fail-closed rollback engaged.

## Evidence Pack (same session)

- `output/canon/vgui_candidates_2026-05-14.json`
- `output/canon/vgui_candidates_2026-05-14.md`
- `output/canon/vgui_vtable_map_2026-05-14.json`
- `output/canon/vgui_probe_trace_2026-05-14.md`
- `output/canon/vgui_dryrun_verdict_2026-05-14.md`
- `/srv/nfs/shared/Shared/Tools/UNICORN_DUMPER/output/canon/render_path_emu_2026-05-14.json`
- `/srv/nfs/shared/Shared/Tools/UNICORN_DUMPER/output/canon/render_path_emu_2026-05-14.md`

# VGUI Dry-Run Static Verdict

- Date: `2026-05-14`
- Verdict: `pass_static`
- Reason: `global_and_required_slots_present`

- Selected candidate: `All hud elements must derive from vgui::Panel * (%s)
`
- Selected global RVA: `0x025517E0`

Required slots:
- `draw_text` -> `1`
- `set_text_pos` -> `2`
- `set_text_color` -> `7`

Rollback policy:
- `enabled_default`: `0`
- `fail_closed`: `1`
- `max_faults`: `4`

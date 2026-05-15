# VGUI Dry-Run Static Verdict (v2)

- Date: `2026-05-14`
- Verdict: `pass_static`
- Reason: `global_and_required_slots_present`

- Selected candidate: `drawtext_vgui_centered`
- Selected iface RVA: `0x03D4C5A0`
- Selected iface VA: `0x0000000143D4C5A0`
- max_slot: `163`

Required slots (must be present, distinct, and draw_text slot >= 8):
- `draw_text` -> `26`
- `set_text_pos` -> `36`
- `set_text_color` -> `92`
- `set_font` (advisory) -> `25`

Rollback policy:
- `enabled_default`: `0`
- `fail_closed`: `1`
- `max_faults`: `4`

SDK ConVar cross-check on selected iface:
- exact match: `none` (iface is not a ConVar — sane)

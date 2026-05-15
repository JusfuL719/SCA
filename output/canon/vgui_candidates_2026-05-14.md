# VGUI Candidate Discovery

- Date: `2026-05-14`
- Dumps: `/srv/nfs/shared/Shared/Tools/UNICORN_DUMPER/EAC/EAC/r5apex`
- Keyword strings scanned: `40`
- Actionable candidates: `9`

## Hard Stop Conditions (Phase 0)

- Stop immediately on repeated render-gate prologue drift markers (`DRR`/`DRX`).
- Stop on unhandled NPF storms (`V0A`/`V0B`/`V0C`) or sustained VMEXIT slow-path markers (`VAD` with draw branch correlation).
- Stop on orphan guest-call return markers (`XC3`) once dry-run mode is armed.
- Fail closed if interface/vtable pointers become unstable across sampling windows.

## Ranked Candidates

| Rank | Name | Score | Global RVA | Slots (top) |
|---|---|---:|---:|---|
| 1 | `All hud elements must derive from vgui::Panel * (%s)
` | 153 | `0x025517E0` | `1, 7, 2` |
| 2 | `drawtext_vgui_centered` | 139 | `n/a` | `34` |
| 3 | `drawtext_vgui` | 137 | `n/a` | `34, 209` |
| 4 | `FooterPanel.ButtonTextFontHeightRui` | 123 | `n/a` | `1, 95` |
| 5 | `Label.RuiFontHeight` | 119 | `n/a` | `1, 265, 261` |
| 6 | `%s, string labelText, string associate, alignment textAlignment, int wrap, int dulltext, int brighttext, string font` | 118 | `n/a` | `1` |
| 7 | `Label.RuiFont` | 111 | `n/a` | `1, 265` |
| 8 | `DrawTextRui can only be set on RichText elements.` | 109 | `n/a` | `260, 208, 25` |
| 9 | `FooterPanel.TextFont` | 103 | `n/a` | `1` |

## Top Candidate Slot Map

- `name`: `All hud elements must derive from vgui::Panel * (%s)
`
- `global_rva`: `0x025517E0`
- `draw_text`: `1`
- `set_text_pos`: `2`
- `set_text_color`: `7`
- `set_font`: `7`

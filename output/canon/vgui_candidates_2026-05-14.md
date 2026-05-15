# VGUI Candidate Discovery (v2: provenance-tracked)

- Date: `2026-05-14`
- Dumps: `/srv/nfs/shared/Shared/Tools/UNICORN_DUMPER/EAC/EAC/r5apex`
- Keyword strings scanned: `60` (dropped `295` as assert/panic/script)
- Actionable candidates: `12`

## Hard Stop Conditions (Phase 0)

- Stop immediately on repeated render-gate prologue drift markers (`DRR`/`DRX`).
- Stop on unhandled NPF storms (`V0A`/`V0B`/`V0C`) or sustained VMEXIT slow-path markers (`VAD` with draw branch correlation).
- Stop on orphan guest-call return markers (`XC3`) once dry-run mode is armed.
- Fail closed if interface/vtable pointers become unstable across sampling windows.

## Ranked Candidates

| Rank | Name | Score | Iface RVA | Strong slots | max slot |
|---|---|---:|---:|---|---:|
| 1 | `drawtext_vgui_centered` | 315 | `0x03D4C5A0` | `14, 22, 23, 25, 26, 34, 36, 43` | 163 |
| 2 | `drawtext_vgui` | 305 | `0x03D4C5A0` | `13, 16, 22, 24, 26, 30, 34, 35` | 174 |
| 3 | `VGUI_Surface031` | 255 | `0x03EBD4C0` | `26, 39, 40, 42, 43, 44, 45, 46` | 102 |
| 4 | `drawtext_vgui_simple` | 227 | `0x03D4C520` | `32, 38, 42, 44, 65, 82` | 82 |
| 5 | `MenuTextFont` | 213 | `0x03D4C5A0` | `22, 24, 26, 35, 53, 88` | 88 |
| 6 | `vgui::HFont` | 209 | `0x03D4C5A0` | `14, 16, 22, 24, 26, 35, 53, 88` | 88 |
| 7 | `vgui::Label` | 205 | `0x03D4C5A0` | `14, 40, 43, 44, 45, 52, 53, 118` | 128 |
| 8 | `CHudMenu::MsgFunc_ShowMenu` | 183 | `0x03D4C5A0` | `22, 24, 26, 35, 53, 88` | 88 |
| 9 | `Label.RuiFont` | 167 | `0x03D4C5A0` | `16, 37, 157` | 157 |
| 10 | `Label.RuiFontHeight` | 167 | `0x03D4C5A0` | `16, 37, 157` | 157 |
| 11 | `FooterPanel.ButtonTextFontHeightRui` | 101 | `n/a` | `n/a` | n/a |
| 12 | `FooterPanel.TextFont` | 89 | `n/a` | `n/a` | n/a |

## Coherence Rejections

- none

## Top Candidate Slot Map

- `name`: `drawtext_vgui_centered`
- `iface_rva`: `0x03D4C5A0`
- `draw_text`: `26`
- `set_text_pos`: `36`
- `set_text_color`: `92`
- `set_font`: `25`
- `max_slot_strong`: `163`
- `vgui_convars_in_funcs`: `['rui_overrideVguiTextRendering']`

# VGUI decoy pre-flight (emulation)

- Date: `2026-05-14`
- Iface RVA: `0x03D4C5A0`
- Slots tested: `25 (set_font)`, `26 (draw_text)`, `36 (set_text_pos)`, `92 (set_text_color)`
- Runtime base inference: `medium` (base=`0x7FF659A67000`, in_text=135640)
- Vtable candidates: `16`
- Aggregate verdict: **`decoy_present`**
- Operator decision: **`hold_fail_closed`**
- Reason: 2 vtable(s) tripped EAC tripwire markers

## Data-cell iface resolution

- Status: `outside_ondisk_extent`

## Per-vtable verdicts

### vtable rva `0x014A5630` (runtime/rdata_run) -- **`decoy`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x0CA640` | `real` | `0` |  |
| `26` | draw_text | `0x0CDF00` | `real` | `0` |  |
| `36` | set_text_pos | `0x1FFCB0` | `real` | `0` |  |
| `92` | set_text_color | `0x003520` | `decoy` | `50` | out@+1 |

### vtable rva `0x017F6298` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x23A8F0` | `real` | `15` | early_ret@+1 |
| `26` | draw_text | `0x23A900` | `real` | `13` | early_ret@+2 |
| `36` | set_text_pos | `0x23AD00` | `real` | `-13` |  |
| `92` | set_text_color | `0x237920` | `real` | `0` |  |

### vtable rva `0x018113E0` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x39C480` | `real` | `0` |  |
| `26` | draw_text | `0x39C490` | `real` | `0` |  |
| `36` | set_text_pos | `0x39CA70` | `real` | `0` |  |
| `92` | set_text_color | `0x2378D0` | `real` | `0` |  |

### vtable rva `0x01815860` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0xE67DD0` | `real` | `-5` |  |
| `26` | draw_text | `0xE67DF0` | `real` | `0` |  |
| `36` | set_text_pos | `0x3DE860` | `real` | `-12` |  |
| `92` | set_text_color | `0xE69BA0` | `real` | `0` |  |

### vtable rva `0x01816038` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0xE67DD0` | `real` | `-5` |  |
| `26` | draw_text | `0xE67DF0` | `real` | `0` |  |
| `36` | set_text_pos | `0x3DEAD0` | `real` | `-3` |  |
| `92` | set_text_color | `0xE69BA0` | `real` | `0` |  |

### vtable rva `0x018169E0` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0xE67DD0` | `real` | `-5` |  |
| `26` | draw_text | `0xE67DF0` | `real` | `0` |  |
| `36` | set_text_pos | `0x3DE860` | `real` | `-12` |  |
| `92` | set_text_color | `0xE69BA0` | `real` | `0` |  |

### vtable rva `0x01817308` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0xE67DD0` | `real` | `-5` |  |
| `26` | draw_text | `0xE67DF0` | `real` | `0` |  |
| `36` | set_text_pos | `0x3DEAD0` | `real` | `-3` |  |
| `92` | set_text_color | `0xE69BA0` | `real` | `0` |  |

### vtable rva `0x01823508` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x4AA7B0` | `real` | `0` |  |
| `26` | draw_text | `0x4A9980` | `real` | `0` |  |
| `36` | set_text_pos | `0x4AC5C0` | `real` | `-3` |  |
| `92` | set_text_color | `0x14144E0` | `real` | `0` |  |

### vtable rva `0x01824020` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x4AA7B0` | `real` | `0` |  |
| `26` | draw_text | `0x4A9980` | `real` | `0` |  |
| `36` | set_text_pos | `0x4AC5C0` | `real` | `-3` |  |
| `92` | set_text_color | `0x4AF680` | `real` | `-3` |  |

### vtable rva `0x01826318` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x200370` | `real` | `0` |  |
| `26` | draw_text | `0x4CF580` | `real` | `5` | early_ret@+2 |
| `36` | set_text_pos | `0x4CF8A0` | `real` | `-12` |  |
| `92` | set_text_color | `0x4D1320` | `real` | `-2` |  |

### vtable rva `0x01826990` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x4C91D0` | `real` | `-6` |  |
| `26` | draw_text | `0x4E07D0` | `real` | `-3` |  |
| `36` | set_text_pos | `0x4C9420` | `real` | `-21` |  |
| `92` | set_text_color | `0x4CA9A0` | `real` | `0` |  |

### vtable rva `0x01827068` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x14144E0` | `real` | `0` |  |
| `26` | draw_text | `0x14144E0` | `real` | `0` |  |
| `36` | set_text_pos | `0x14144E0` | `real` | `0` |  |
| `92` | set_text_color | `0x14144E0` | `real` | `0` |  |

### vtable rva `0x01827860` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x14144E0` | `real` | `0` |  |
| `26` | draw_text | `0x14144E0` | `real` | `0` |  |
| `36` | set_text_pos | `0x14144E0` | `real` | `0` |  |
| `92` | set_text_color | `0x14144E0` | `real` | `0` |  |

### vtable rva `0x01827F38` (runtime/rdata_run) -- **`decoy`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x4E8140` | `decoy` | `87` | gs@+18, gs@+54, gs@+74, gs_access(88)@rip0x1404FAA4C |
| `26` | draw_text | `0x4E07D0` | `real` | `-3` |  |
| `36` | set_text_pos | `0x4E5B70` | `real` | `-3` |  |
| `92` | set_text_color | `0x200370` | `real` | `0` |  |

### vtable rva `0x0183DB28` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0x65BC40` | `real` | `-4` |  |
| `26` | draw_text | `0x200370` | `real` | `0` |  |
| `36` | set_text_pos | `0x22DD00` | `real` | `-8` |  |
| `92` | set_text_color | `0x7E6CE0` | `real` | `0` |  |

### vtable rva `0x0183E4B8` (runtime/rdata_run) -- **`all_real`** (confidence: `high`)

| slot | role | fn_rva | verdict | suspicion | markers |
|---|---|---|---|---|---|
| `25` | set_font | `0xE67DD0` | `real` | `-5` |  |
| `26` | draw_text | `0xE67DF0` | `real` | `0` |  |
| `36` | set_text_pos | `0x3DE860` | `real` | `-12` |  |
| `92` | set_text_color | `0xE69BA0` | `real` | `0` |  |


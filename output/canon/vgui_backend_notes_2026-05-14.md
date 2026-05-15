# VGUI Backend Skeleton Notes

- Date: `2026-05-14`
- Build side: `SCA` HV
- Goal: keep foreign-call blast radius closed while wiring probe + gate + rollback path.

## What Landed

- New covert command: `PMC_CMD_SET_VGUI_PARAMS (0x23)`.
- Runtime state in HV: `gVguiDrawParams` (`HypeHookDraw.c` / `HypeHookDraw.h`).
- Read-only probe path in `DrawHookPayloadTick`:
  - validates candidate global pointer
  - validates iface + vtable pointer range
  - samples configured draw/pos/color slots
  - tracks stability counter and gate-pass state
- Fail-closed guard:
  - probe faults increment `FaultCount` (`VGQ`)
  - exceeding budget with `FailClosed=1` latches rollback (`VG3`)
  - rollback forces backend bypass.
- Dry-run gate:
  - once gate passes and `DryRunArm=1`, reads draw slot prologue and emits `VG4`.
- Minimal backend skeleton:
  - no foreign guest call yet
  - emits bounded heartbeat marker (`VG5`) with fixed X/Y/RGBA payload
  - mirrored state exported in draw scratch buffer at `DRAWBUF_OFF_VGUI_STATE`.

## Runtime Rollback Rule

- Default is fail-closed (`Enabled=0`, `Rollback=1`).
- Any operator cycle should clear rollback explicitly when arming probe.
- On gate/probe anomalies, rollback re-latches and keeps call path bypassed.

## Marker Set

- `VG0` params command Arg1
- `VGP` params command Arg2
- `VG1` iface pointer sample
- `VGV` vtable pointer sample
- `VG2` gate passed
- `VG3` fail-closed rollback latch
- `VG4` dry-run precheck slot prologue
- `VG5` backend skeleton heartbeat
- `VGQ` probe fault code

# VGUI Probe Trace Spec

- Date: `2026-05-14`

Read-only probe evidence fields expected from HV runtime sampling:
- interface global VA read success/failure
- interface pointer stability (frame-to-frame)
- vtable pointer module-range sanity
- slot target pointer stability for draw/pos/color candidates
- fault counter and fail-closed latch state

Recommended marker namespace:
- `VG0` params applied
- `VG1` sample `(iface, vtable)`
- `VG2` gate passed
- `VG3` fail-closed latch / rollback
- `VG4` dry-run precheck pass
- `VG5` backend heartbeat (skeleton only)
- `VGQ` probe fault code

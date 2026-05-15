#ifndef HYPE_RENDER_H
#define HYPE_RENDER_H

#include "HypeUefi.h"
#include "HypeSvm.h"

#define RENDER_QUEUE_DEPTH   8
#define RENDER_TEXT_MAX      63   // chars excl. null

// Backend IDs stored in gRenderBackend.
#define RENDER_BACKEND_NONE          0
#define RENDER_BACKEND_STRING_HIJACK 1   // Phase 2: chat-ring or Cbuf_AddText write
#define RENDER_BACKEND_VGUI          2   // Phase 3: foreign vgui surface draw

// Hard cap on any sink size (canon APEX_FPS_FMT_LEN is 0x7B; allow runtime
// override up to this max for sentinel-test against larger string convars).
#define RENDER_SINK_MAX_LEN  256

// Arg1 packing for PMC_CMD_RENDER_TEXT — installer packs, HV unpacks.
//   [15:0]  duration_ticks   — #HypeRenderTick calls before expiry (0 → 1)
//   [31:16] x_q8             — screen X, Q8.8 fixed-point (reserved Phase 2+)
//   [47:32] y_q8             — screen Y, Q8.8 fixed-point (reserved Phase 2+)
//   [55:48] color_idx        — color preset index (reserved Phase 2+)
//   [63:56] flags            — see RENDER_FLAG_*
#define RENDER_ARG1_DURATION(a)   ((UINT16)( (a)        & 0xFFFFu))
#define RENDER_ARG1_XQ8(a)        ((UINT16)(((a) >> 16) & 0xFFFFu))
#define RENDER_ARG1_YQ8(a)        ((UINT16)(((a) >> 32) & 0xFFFFu))
#define RENDER_ARG1_COLOR(a)      ((UINT8) (((a) >> 48) & 0xFFu))
#define RENDER_ARG1_FLAGS(a)      ((UINT8) (((a) >> 56) & 0xFFu))

// Per-entry flags. Default = 0 (passive write into the sink, no convar poke).
//   FORCE_FPS_CONVAR — flip APEX_OFF_SHOW_FPS to 1 on hijack, restore on
//                      expiry. Specific to the legacy FPS sink path
//                      (`cl_showfps`); ignored for non-FPS sinks since the
//                      sink IS the rendered string in convar-value mode.
#define RENDER_FLAG_FORCE_FPS_CONVAR  0x01u

// Per-entry state. Text copied by HypeRenderPush from guest memory.
// Slot is considered inactive when TicksRemaining == 0.
typedef struct _RENDER_ENTRY {
    char     Text[RENDER_TEXT_MAX + 1]; // null-terminated guest string
    UINT32   TicksRemaining;            // countdown; 0 = slot free
    UINT16   XQ8;                       // reserved Phase 2+
    UINT16   YQ8;                       // reserved Phase 2+
    UINT8    ColorIdx;                  // reserved Phase 2+
    UINT8    Flags;                     // reserved
    UINT8    _Pad[2];
} RENDER_ENTRY;

// Global queue — written by VMEXIT (RENDER_TEXT cmd), read by DrawHookPayloadTick.
extern volatile RENDER_ENTRY gRenderQueue[RENDER_QUEUE_DEPTH];
extern volatile UINT8        gRenderBackend;

// Called from HypeVmexit.c mailbox dispatch. Text[] already copied from guest
// (null-terminated, ≤ RENDER_TEXT_MAX chars). Thread-safe: called from VMEXIT
// only; DrawHookPayloadTick reads TicksRemaining atomically.
// Returns COVERT_STATUS_OK or COVERT_STATUS_ERR (queue full).
UINT32 HypeRenderPush(const char *Text, UINT32 DurationTicks,
                      UINT16 XQ8, UINT16 YQ8, UINT8 ColorIdx, UINT8 Flags);

// Zero all active slots. Called from PMC_CMD_RENDER_CLEAR handler.
VOID HypeRenderClear(VOID);

// Atomic sink-override transition for sentinel-test iteration. NewRva=0
// reverts to canon APEX_FPS_FMT_RVA; NewLen=0 reverts to canon
// APEX_FPS_FMT_LEN. Caller MUST supply Cr3/ImageBase so the active sink
// (if any) gets restored using its original capture VA before the override
// flips — otherwise the new sink would be corrupted by the old backup on
// next RDV. Returns COVERT_STATUS_OK on success.
//
// IndirectOff (NEW, rev-4): when non-zero, the backend treats NewRva as a
// pointer cell at (ImageBase + NewRva + IndirectOff) and dereferences it
// to recover the live heap VA before writing. Use for convar pszString
// slots (Source ConVar layout: m_pszString at +0x40 of the struct) and
// other one-deref-from-image-data sink shapes. IndirectOff=0 preserves
// today's direct-write-at-RVA behavior bit-for-bit.
UINT32 HypeRenderSetSink(UINT64 Cr3, UINT64 ImageBase,
                         UINT64 NewRva, UINT32 NewLen,
                         UINT32 IndirectOff);

// Called from DrawHookPayloadTick once per payload tick. Dispatches active
// entries to the current backend, decrements timers, logs expiry.
VOID HypeRenderTick(PVCPU_DATA Vcpu, UINT64 Cr3, UINT64 ImageBase, UINT64 Fired);

#endif // HYPE_RENDER_H

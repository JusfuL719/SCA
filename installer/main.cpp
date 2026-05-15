// SCA installer — HANDSHAKE → SET_EPROCESS → NPT_CHANNEL_INIT → CR3 recovery
// → SET_CR3 (+ optional PMC_CMD_HOOK_INSTALL_DRAW in default mode). Mem-dump-only:
// `--live-memdump` stops after VIRT_READ pulls — never patches hook RVA sites.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#include "auth.h"
#include "vmmcall.h"
#include "covert.h"
#include "scratch_layout.h"

#define PMC_CMD_HANDSHAKE          0x60
#define PMC_CMD_SET_EPROCESS       0x0A
#define PMC_CMD_NPT_CHANNEL_INIT   0x50
#define PMC_CMD_SET_CR3            0x02
#define PMC_CMD_PROC_CR3           0x34
#define PMC_CMD_CR3_INTERCEPT      0x36
#define PMC_CMD_GET_INTERCEPT_PEB  0x37
#define PMC_CMD_GET_LAPIC_DISARMED_NPF_COUNT 0x38
#define PMC_CMD_HOOK_INSTALL_DRAW  0x1B
#define PMC_CMD_HOOK_DRAW_PEEK     0x1C
#define PMC_CMD_SET_GLOW_PARAMS    0x1D
#define PMC_CMD_HV_LOG_READ        0x1E
#define PMC_CMD_VIRT_CALL_INIT     0x1F
#define PMC_CMD_VIRT_CALL          0x20
#define PMC_CMD_SET_AIM_PARAMS     0x21
#define PMC_CMD_SET_MENU_ENABLE    0x22
#define PMC_CMD_SET_VGUI_PARAMS    0x23
#define PMC_CMD_RENDER_TEXT        0x29
#define PMC_CMD_RENDER_CLEAR       0x2A
#define PMC_CMD_RENDER_SET_SINK    0x2B
#define PMC_CMD_DIAG_MSR_TIMING    0x2C
#define PMC_CMD_SCAN_BOX           0x2D

// Per-entry flags for PMC_CMD_RENDER_TEXT (mirrors HypeRender.h).
#define RENDER_FLAG_FORCE_FPS_CONVAR  0x01u

// RENDER_TEXT Arg1 packing — mirrors HypeRender.h.
// [15:0]=duration_ticks [31:16]=x_q8 [47:32]=y_q8 [55:48]=color_idx [63:56]=flags
static inline uint64_t MakeRenderArg1(uint16_t dur, uint16_t xq8, uint16_t yq8,
                                      uint8_t color, uint8_t flags) {
    return (uint64_t)dur
         | ((uint64_t)xq8   << 16)
         | ((uint64_t)yq8   << 32)
         | ((uint64_t)color << 48)
         | ((uint64_t)flags << 56);
}
#define PMC_CMD_VIRT_READ8         0x31
#define PMC_CMD_VIRT_WRITE4        0x32

#define HYPE_MEM_OK                0

// Mirrors HypeDebug.h. 4 MB ring; reader pulls 8B at a time via 0x1E.
#define HV_DEBUG_LOG_SIZE          (4U * 1024U * 1024U)

// ----- args -------------------------------------------------------------

struct Args {
    uint64_t module_base   = 0;   // r5apex_dx12.exe ImageBase; 0 = recover from HV
    uint64_t target_cr3    = 0;   // Apex CR3; 0 = recover from HV
    uint64_t rva           = 0;   // hook RVA (e.g. 0x54DBD0 for L2 entry)
    uint64_t peb_va        = 0;   // Phase-4 PEB hint (Phase 1 EPROCESS scan is dead)
    bool     no_recover    = false;  // skip CR3 recovery (use --target-cr3 verbatim)
    bool     verbose       = false;
    bool     drain         = false;  // live HV-log drain via PMC_CMD_HV_LOG_READ
    const char* drain_out  = nullptr;  // output path; default hypedbg-live-<tsc>.bin
    bool     diag_m0f      = false;  // read PMC_CMD_GET_LAPIC_DISARMED_NPF_COUNT and exit
    bool     dump_teams    = false;  // PEEK ENT_SNAPSHOT[64] mirror + LocalSnapshot.Team
    uint64_t peek_ent      = 0;      // entity VA to dump +0x290..+0x2B0 via VIRT_READ8
    uint64_t peek_va       = 0;      // arbitrary VA, dump 4 qwords via VIRT_READ8
    // --drift-check: verify a handful of UNICORN_DUMPER-pinned RVAs still
    // hold at runtime. Replaces the legacy --dump-buckets / --dump-hsp-pins /
    // --dump-glow-map RE-grade probes — the layout itself is now pinned
    // statically (see UNICORN_DUMPER/OFFSETS.md), so we only need a fast
    // sanity check, not a full sweep.
    bool     drift_check   = false;
    // --bucket-write SLOT OFF VAL — 4B write into HIGHLIGHT_SETTINGS bucket.
    int      bw_slot       = -1;
    uint32_t bw_off        = 0;
    uint32_t bw_val        = 0;
    // --hid-set ENT_VA HID — invoke SetHighlightId via PMC_CMD_VIRT_CALL.
    bool     hid_set       = false;
    uint64_t hid_set_ent   = 0;
    uint32_t hid_set_hid   = 0;
    // --aim-params: PMC_CMD_SET_AIM_PARAMS (0x21). aim_set flips on any --aim-* flag.
    // Both features default disabled; operator enables each session.
    bool     aim_set           = false;
    uint8_t  aim_trigger_en    = 0;     // TriggerEnabled
    uint8_t  aim_aim_en        = 0;     // AimEnabled
    uint8_t  aim_trig_fov      = 0x0B;  // TriggerFovQ4_4 (~0.69°)
    uint8_t  aim_trig_thresh   = 0x05;  // TriggerThreshQ4_4 (~0.31°)
    uint8_t  aim_debounce      = 12;    // TriggerDebounceTicks
    uint8_t  aim_fov           = 0x40;  // AimFovQ4_4 (4.0°)
    // Menu controller — armed by default at install; --menu-disable boots HV without it.
    bool     menu_disabled     = false;
    // --no-install-confirm suppresses the post-install RENDER_TEXT push.
    bool     no_install_confirm = false;
    // --render-sink RVA LEN — runtime sink override pushed via
    // PMC_CMD_RENDER_SET_SINK before the install-confirm push. Both 0 means
    // "use canon APEX_FPS_FMT_*". Used by sentinel-test iteration loop —
    // operator probes one candidate per install run. render_sink_set flips
    // when --render-sink is supplied (RVA=0 with explicit flag still resets).
    // --render-sink-indirect CONVAR_RVA PSTRING_OFF LEN — same path but HV
    // dereferences a 64-bit pointer cell at (ImageBase + CONVAR_RVA +
    // PSTRING_OFF) and writes at the resolved heap VA. Used for ConVar
    // string-buffer sinks (Source layout: m_pszString at +0x40 of the
    // ConVar struct). Output of apex_dumper/probe_rui_sink.py feeds this
    // form one candidate per run.
    bool     render_sink_set = false;
    uint64_t render_sink_rva = 0;
    uint32_t render_sink_len = 0;
    uint32_t render_sink_indirect_off = 0;
    // --sentinel "TEXT" — payload string for install-confirm push. Default
    // is "HV INSTALLED". Useful for sentinel-tests where operator wants a
    // distinctive marker (e.g. "AAAAAAAAA") to pick out on screen.
    const char* sentinel_text = nullptr;
    // --diag-msr-timing MSR# — fire PMC_CMD_DIAG_MSR_TIMING and exit.
    // Source for SCA/research/hyperjacking_2026_state.md §6.2.2.
    bool     diag_msr_timing = false;
    uint32_t diag_msr_id     = 0;
    // --scan-box <seconds> — arm box-scan, poll for capture for N seconds,
    // disarm + report. HV widens ScanOneEntity to log unknown classes +
    // probe +0x1660 for ASCII text. First entity hit captured for the
    // installer to retrieve. See SCA/research/sentinel/convar_candidates.md
    // Tier-0 candidate m_customOwnerName.
    bool     scan_box = false;
    uint32_t scan_box_seconds = 0;
    // Tier-1 glow knobs. glow_set flips on any --glow-* flag.
    bool     glow_set      = false;
    bool     reconfig      = false;  // skip install entirely; just push knobs + exit
    uint8_t  glow_slot     = 78;
    uint8_t  glow_mask     = 0x01;
    uint8_t  glow_filter   = 0;      // 0=enemy 1=all 2=teammates 3=off
    uint8_t  glow_enabled  = 1;
    uint8_t  glow_vistype  = 1;
    uint8_t  glow_glowfix  = 2;
    uint8_t  glow_wvistype = 1;
    uint8_t  glow_wglowfix = 1;
    uint8_t  glow_squad    = 0;      // --squad-glow on|off → SET_GLOW_PARAMS Arg2[7:0]
    // VGUI probe/backend knobs. vgui_set flips on any --vgui-* flag.
    uint8_t  vgui_set          = 0;
    uint8_t  vgui_enabled      = 1;
    uint8_t  vgui_probe_only   = 1;           // probe_only_arm default
    uint8_t  vgui_dry_arm      = 0;
    uint8_t  vgui_rollback     = 0;           // operator explicitly clears fail-closed latch
    uint8_t  vgui_fail_closed  = 1;
    uint8_t  vgui_stable_need  = 8;
    uint8_t  vgui_max_faults   = 4;
    uint32_t vgui_global_rva   = 0x025517E0u; // canon from vgui_dryrun_verdict_2026-05-14.md
    uint8_t  vgui_slot_draw    = 1;
    uint8_t  vgui_slot_pos     = 2;
    uint8_t  vgui_slot_color   = 7;
    uint8_t  vgui_slot_font    = 0;
    uint16_t vgui_text_x       = 960;
    uint16_t vgui_text_y       = 120;
    uint32_t vgui_text_rgba    = 0xFFFFFFFFu;
    // --live-memdump <dir> — Phase-4 CR3 + SET_CR3 + VIRT_READ bulk → three
    // section files. No scratch, no HOOK_INSTALL_DRAW (won't butcher .text
    // when hook RVA drifted vs current build).
    const char* live_memdump_dir = nullptr;
    // --dump-canon — Phase-4 CR3 + SET_CR3 then live-read every named canon
    // global via VIRT_READ8 and classify (heap ptr / image ptr / zero / int).
    // For globals that read as 0, sweep ±0x200 around the slot to surface
    // heap-pointer candidates. Pins offset drift without rebuilding HV.
    bool        dump_canon          = false;
    // --find-ptr <heap_va> — scan .data (12 MB raw) via BulkVirtRead8 for any
    // 8-byte qword matching the target VA. Prints all matching RVAs.
    uint64_t    find_ptr_target     = 0;
};

static uint64_t ParseHex(const char* s) {
    return std::strtoull(s, nullptr, 0);  // 0x-prefix → hex; else decimal
}

// Single-pass dispatch. Every handler unconditionally `continue`s after
// consuming its argv slots so flag order can't double-consume or skip.
// Flags that take N args silently ignore the flag if argv runs out (parity
// with prior behavior — no unknown-arg diagnostics).
static Args ParseArgs(int argc, char** argv) {
    Args a;
    auto need = [&](int i, int n) { return (i + n) < argc; };

    for (int i = 1; i < argc; ++i) {
        const char* f = argv[i];

        if (std::strcmp(f, "-v") == 0)             { a.verbose       = true; continue; }
        if (std::strcmp(f, "--no-recover") == 0)   { a.no_recover    = true; continue; }
        if (std::strcmp(f, "--drain") == 0)        { a.drain         = true; continue; }
        if (std::strcmp(f, "--diag-m0f") == 0)     { a.diag_m0f      = true; continue; }
        if (std::strcmp(f, "--dump-teams") == 0)   { a.dump_teams    = true; continue; }
        if (std::strcmp(f, "--drift-check") == 0)  { a.drift_check   = true; continue; }
        if (std::strcmp(f, "--reconfig") == 0)     { a.reconfig      = true; continue; }

        if (std::strcmp(f, "--peek-ent") == 0 && need(i,1)) { a.peek_ent = ParseHex(argv[++i]); continue; }
        if (std::strcmp(f, "--peek-va")  == 0 && need(i,1)) { a.peek_va  = ParseHex(argv[++i]); continue; }

        if (std::strcmp(f, "--bucket-write") == 0 && need(i,3)) {
            a.bw_slot = (int)ParseHex(argv[++i]);
            a.bw_off  = (uint32_t)ParseHex(argv[++i]);
            a.bw_val  = (uint32_t)ParseHex(argv[++i]);
            continue;
        }
        if (std::strcmp(f, "--hid-set") == 0 && need(i,2)) {
            a.hid_set     = true;
            a.hid_set_ent = ParseHex(argv[++i]);
            a.hid_set_hid = (uint32_t)ParseHex(argv[++i]);
            continue;
        }

        if (std::strcmp(f, "--glow-slot") == 0 && need(i,1))     { a.glow_slot     = (uint8_t)ParseHex(argv[++i]); a.glow_set = true; continue; }
        if (std::strcmp(f, "--gate-mask") == 0 && need(i,1))     { a.glow_mask     = (uint8_t)ParseHex(argv[++i]); a.glow_set = true; continue; }
        if (std::strcmp(f, "--filter") == 0 && need(i,1)) {
            const char* v = argv[++i]; a.glow_set = true;
            if      (std::strcmp(v, "enemy")     == 0) a.glow_filter = 0;
            else if (std::strcmp(v, "all")       == 0) a.glow_filter = 1;
            else if (std::strcmp(v, "teammates") == 0) a.glow_filter = 2;
            else if (std::strcmp(v, "off")       == 0) a.glow_filter = 3;
            else                                       a.glow_filter = (uint8_t)ParseHex(v);
            continue;
        }
        if (std::strcmp(f, "--enabled")       == 0 && need(i,1)) { a.glow_enabled  = (uint8_t)ParseHex(argv[++i]); a.glow_set = true; continue; }
        if (std::strcmp(f, "--vis-type")      == 0 && need(i,1)) { a.glow_vistype  = (uint8_t)ParseHex(argv[++i]); a.glow_set = true; continue; }
        if (std::strcmp(f, "--glow-fix")      == 0 && need(i,1)) { a.glow_glowfix  = (uint8_t)ParseHex(argv[++i]); a.glow_set = true; continue; }
        if (std::strcmp(f, "--write-vistype") == 0 && need(i,1)) { a.glow_wvistype = (uint8_t)ParseHex(argv[++i]); a.glow_set = true; continue; }
        if (std::strcmp(f, "--write-glowfix") == 0 && need(i,1)) { a.glow_wglowfix = (uint8_t)ParseHex(argv[++i]); a.glow_set = true; continue; }
        if (std::strcmp(f, "--squad-glow")   == 0 && need(i,1)) {
            const char* v = argv[++i]; a.glow_set = true;
            if      (std::strcmp(v, "on")  == 0) a.glow_squad = 1;
            else if (std::strcmp(v, "off") == 0) a.glow_squad = 0;
            else                                 a.glow_squad = (uint8_t)ParseHex(v);
            continue;
        }
        if (std::strcmp(f, "--vgui-enable") == 0) {
            a.vgui_set = 1;
            a.vgui_enabled = 1;
            a.vgui_rollback = 0;
            continue;
        }
        if (std::strcmp(f, "--vgui-disable") == 0) {
            a.vgui_set = 1;
            a.vgui_enabled = 0;
            a.vgui_rollback = 1;
            continue;
        }
        if (std::strcmp(f, "--vgui-probe-only") == 0 && need(i,1)) { a.vgui_probe_only  = (uint8_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-dry-arm")    == 0)              { a.vgui_dry_arm     = 1;                             a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-rollback")   == 0 && need(i,1)) { a.vgui_rollback    = (uint8_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-fail-closed")== 0 && need(i,1)) { a.vgui_fail_closed = (uint8_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-stable-need")== 0 && need(i,1)) { a.vgui_stable_need = (uint8_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-max-faults") == 0 && need(i,1)) { a.vgui_max_faults  = (uint8_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-global-rva") == 0 && need(i,1)) { a.vgui_global_rva  = (uint32_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-slot-draw")  == 0 && need(i,1)) { a.vgui_slot_draw   = (uint8_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-slot-pos")   == 0 && need(i,1)) { a.vgui_slot_pos    = (uint8_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-slot-color") == 0 && need(i,1)) { a.vgui_slot_color  = (uint8_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-slot-font")  == 0 && need(i,1)) { a.vgui_slot_font   = (uint8_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-text-x")     == 0 && need(i,1)) { a.vgui_text_x      = (uint16_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-text-y")     == 0 && need(i,1)) { a.vgui_text_y      = (uint16_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }
        if (std::strcmp(f, "--vgui-text-rgba")  == 0 && need(i,1)) { a.vgui_text_rgba   = (uint32_t)ParseHex(argv[++i]); a.vgui_set = 1; continue; }

        // --aim-params: packed u64 → PMC_CMD_SET_AIM_PARAMS (0x21).
        // Individual flags mirror the q4.4 bitfield layout; see HypeAimTrigger.h.
        if (std::strcmp(f, "--aim-trigger")    == 0 && need(i,1)) { a.aim_trigger_en  = (uint8_t)ParseHex(argv[++i]); a.aim_set = true; continue; }
        if (std::strcmp(f, "--aim-enable")     == 0 && need(i,1)) { a.aim_aim_en      = (uint8_t)ParseHex(argv[++i]); a.aim_set = true; continue; }
        if (std::strcmp(f, "--aim-trig-fov")   == 0 && need(i,1)) { a.aim_trig_fov    = (uint8_t)ParseHex(argv[++i]); a.aim_set = true; continue; }
        if (std::strcmp(f, "--aim-trig-thresh")== 0 && need(i,1)) { a.aim_trig_thresh = (uint8_t)ParseHex(argv[++i]); a.aim_set = true; continue; }
        if (std::strcmp(f, "--aim-debounce")   == 0 && need(i,1)) { a.aim_debounce    = (uint8_t)ParseHex(argv[++i]); a.aim_set = true; continue; }
        if (std::strcmp(f, "--aim-fov")        == 0 && need(i,1)) { a.aim_fov         = (uint8_t)ParseHex(argv[++i]); a.aim_set = true; continue; }

        if (std::strcmp(f, "--menu-disable")       == 0) { a.menu_disabled       = true; continue; }
        if (std::strcmp(f, "--no-install-confirm") == 0) { a.no_install_confirm  = true; continue; }

        if (std::strcmp(f, "--render-sink") == 0 && need(i, 2)) {
            a.render_sink_rva          = ParseHex(argv[++i]);
            a.render_sink_len          = (uint32_t)ParseHex(argv[++i]);
            a.render_sink_indirect_off = 0;
            a.render_sink_set          = true;
            continue;
        }
        if (std::strcmp(f, "--render-sink-indirect") == 0 && need(i, 3)) {
            a.render_sink_rva          = ParseHex(argv[++i]);
            a.render_sink_indirect_off = (uint32_t)ParseHex(argv[++i]);
            a.render_sink_len          = (uint32_t)ParseHex(argv[++i]);
            a.render_sink_set          = true;
            continue;
        }
        if (std::strcmp(f, "--sentinel") == 0 && need(i, 1)) {
            a.sentinel_text = argv[++i];
            continue;
        }
        if (std::strcmp(f, "--diag-msr-timing") == 0 && need(i, 1)) {
            a.diag_msr_timing = true;
            a.diag_msr_id     = (uint32_t)ParseHex(argv[++i]);
            continue;
        }
        if (std::strcmp(f, "--scan-box") == 0 && need(i, 1)) {
            a.scan_box         = true;
            a.scan_box_seconds = (uint32_t)ParseHex(argv[++i]);
            continue;
        }

        if (std::strcmp(f, "--rva") == 0 && need(i,1))         { a.rva         = ParseHex(argv[++i]); continue; }
        if (std::strcmp(f, "--module-base") == 0 && need(i,1)) { a.module_base = ParseHex(argv[++i]); continue; }
        if (std::strcmp(f, "--target-cr3") == 0 && need(i,1))  { a.target_cr3  = ParseHex(argv[++i]); continue; }
        if (std::strcmp(f, "--peb") == 0 && need(i,1))         { a.peb_va      = ParseHex(argv[++i]); continue; }
        if (std::strcmp(f, "--drain-out") == 0 && need(i,1))   { a.drain_out   = argv[++i];          continue; }

        if (std::strcmp(f, "--live-memdump") == 0 && need(i, 1)) {
            a.live_memdump_dir = argv[++i];
            continue;
        }
        if (std::strcmp(f, "--dump-canon") == 0) { a.dump_canon = true; continue; }
        if (std::strcmp(f, "--find-ptr") == 0 && need(i, 1)) {
            a.find_ptr_target = ParseHex(argv[++i]);
            continue;
        }
    }
    return a;
}

// Apex PE sections — mirrors UNICORN_DUMPER/redump.sh defaults.
static constexpr uint64_t kSecTextRva   = 0x1000ULL;
static constexpr uint64_t kSecTextSz    = 0x014A1000ULL;
static constexpr uint64_t kSecRdataRva  = 0x014A2000ULL;
static constexpr uint64_t kSecRdataSz   = 0x0068E000ULL;
static constexpr uint64_t kSecDataRva   = 0x01B30000ULL;
static constexpr uint64_t kSecDataSz    = 0x00CA1000ULL;

static void Log(const Args& a, const char* fmt, ...) {
    if (!a.verbose) return;
    va_list ap; va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
}

// ----- VMMCALL wrappers (must be free of object-unwinding for __try) ----

// __try helper: no object unwinding (C2712). Returns 0 + faulted=true on SEH.

static uint64_t TryVmmcall(uint64_t auth, uint32_t cmd,
                           uint64_t a1, uint64_t a2, bool* faulted) {
    *faulted = false;
    __try {
        return vmmcall::Call(auth, cmd, a1, a2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return 0;
    }
}

// ----- handshake --------------------------------------------------------

static bool DoHandshake(uint64_t* out_auth_key, int* out_cpu, const Args& a) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const int num_cpus = static_cast<int>(si.dwNumberOfProcessors);

    const uint64_t key = auth::MixKey(auth::BuildSecret());

    // Try every logical CPU until one accepts the handshake (= virtualized).
    // Order randomized via RDTSC to avoid always probing CPU 0 first.
    int order[256];
    for (int i = 0; i < num_cpus && i < 256; ++i) order[i] = i;
    for (int i = num_cpus - 1; i > 0 && i < 256; --i) {
        const int j = static_cast<int>(__rdtsc() % static_cast<uint64_t>(i + 1));
        const int t = order[i]; order[i] = order[j]; order[j] = t;
    }

    for (int ci = 0; ci < num_cpus && ci < 256; ++ci) {
        const int cpu = order[ci];
        SetThreadAffinityMask(GetCurrentThread(),
                              static_cast<DWORD_PTR>(1ULL << cpu));
        SwitchToThread();
        bool faulted = false;
        const uint64_t r = TryVmmcall(key, PMC_CMD_HANDSHAKE, 0, 0, &faulted);
        if (!faulted && r != 0) {
            *out_auth_key = r;
            *out_cpu      = cpu;
            Log(a, "[OK] HANDSHAKE on CPU %d auth=0x%016llX\n",
                cpu, static_cast<unsigned long long>(r));
            return true;
        }
        // faulted = CPU not virtualized; r == 0 = HV said no — try next.
    }
    return false;
}

// ----- scratch buffer ---------------------------------------------------

// 64 KB scratch (16×4K) — HV captures GPA list at install; VirtualLock'd.

static constexpr size_t kScratchBytes = 16 * 4096;

static void* AllocScratch() {
    void* p = VirtualAlloc(nullptr, kScratchBytes,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return nullptr;
    if (!VirtualLock(p, kScratchBytes)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    // Zero everything past the header, then stamp the header. The magic
    // word at offset 0 doubles as the HV's cloak-copy sentinel.
    std::memset(p, 0, kScratchBytes);
    sca_scratch::StampHeader(p);
    return p;
}

static void FreeScratch(void* p) {
    if (!p) return;
    VirtualUnlock(p, kScratchBytes);
    VirtualFree(p, 0, MEM_RELEASE);
}

static bool RecoverPhase4Cr3(covert::Channel& ch, Args& args) {
    if (args.no_recover || args.target_cr3 != 0)
        return true;

    Log(args, "[..] CR3 recovery: arming Cr3PassiveSample (%s)\n",
        args.peb_va ? "Phase 4 — PEB hinted" : "blind");
    uint64_t arm_r = 0;
    const uint32_t arm_s =
        ch.Fire(PMC_CMD_CR3_INTERCEPT, 1, args.peb_va, 0, &arm_r);
    if (arm_s != covert::kStatusOk || arm_r != HYPE_MEM_OK) {
        Log(args, "[FAIL] CR3_INTERCEPT arm status=%u result=%llu\n",
            arm_s, static_cast<unsigned long long>(arm_r));
        return false;
    }

    uint64_t peb = 0;
    uint64_t phase = 0;
    const ULONGLONG t0 = GetTickCount64();
    for (int i = 0; peb == 0 && phase != 3 &&
                    (GetTickCount64() - t0) < 30000; ++i) {
        Sleep(5);
        uint64_t poll_r = 0;
        uint64_t poll_phase = 0;
        const uint32_t poll_s =
            ch.Fire(PMC_CMD_GET_INTERCEPT_PEB, 0, 0, 0, &poll_r, &poll_phase);
        if (poll_s != covert::kStatusOk) {
            Log(args, "[FAIL] GET_INTERCEPT_PEB status=%u (i=%d)\n", poll_s, i);
            return false;
        }
        peb   = poll_r;
        phase = poll_phase;
        if (args.verbose && (i % 200) == 199) {
            Log(args, "[..] %llums phase=%llu peb=0x%llx\n",
                (unsigned long long)(GetTickCount64() - t0),
                static_cast<unsigned long long>(phase),
                static_cast<unsigned long long>(peb));
        }
    }
    if (peb == 0 && phase == 3) {
        Log(args, "[FAIL] CR3 recovery: HV scan complete, no Apex "
                  "match (phase=3, peb=0).\n");
        return false;
    }
    if (peb == 0) {
        Log(args,
            "[FAIL] CR3 recovery wallclock timeout (30s, phase=%llu).\n",
            static_cast<unsigned long long>(phase));
        return false;
    }

    uint64_t cr3 = 0, img = 0;
    const uint32_t pc_s =
        ch.Fire(PMC_CMD_PROC_CR3, 0, 0, 0, &cr3, &img);
    if (pc_s != covert::kStatusOk || cr3 == 0) {
        Log(args, "[FAIL] PROC_CR3 status=%u cr3=0x%llx img=0x%llx\n",
            pc_s, static_cast<unsigned long long>(cr3),
            static_cast<unsigned long long>(img));
        return false;
    }
    args.target_cr3  = cr3;
    if (args.module_base == 0)
        args.module_base = img;
    Log(args, "[OK] CR3 recovery cr3=0x%016llx imagebase=0x%016llx\n",
        static_cast<unsigned long long>(cr3),
        static_cast<unsigned long long>(img));
    return true;
}

static bool HvCommitTargetCr3(covert::Channel& ch, const Args& a,
                             uint64_t cr3) {
    uint64_t r = 0;
    const uint32_t s = ch.Fire(PMC_CMD_SET_CR3, cr3, 0, 0, &r);
    if (s != covert::kStatusOk || r == 0) {
        std::printf("[FAIL] SET_CR3 status=%u result=0x%llX\n", s,
                    static_cast<unsigned long long>(r));
        return false;
    }
    Log(a, "[OK] SET_CR3 cr3=0x%016llX\n",
        static_cast<unsigned long long>(cr3));
    return true;
}

static bool DumpGuestRange(covert::Channel& ch, uint64_t base_va,
                           uint64_t size_bytes, const char* filepath,
                           const Args& log_args) {
    (void)log_args;
    FILE* f = std::fopen(filepath, "wb");
    if (!f) {
        std::printf("[FAIL] fopen(\"%s\") errno\n", filepath);
        return false;
    }
    uint64_t buf[80];
    uint64_t off = 0;
    while (off + 8 <= size_bytes) {
        const uint64_t remaining_qw = (size_bytes - off) / 8;
        const uint32_t nqw =
            remaining_qw > 80u ? 80u : static_cast<uint32_t>(remaining_qw);
        const uint32_t got =
            ch.BulkVirtRead8(base_va + off, buf, nqw);
        if (got != nqw) {
            std::printf("[FAIL] VIRT_READ8 @0x%llX partial got=%u/%u\n",
                        static_cast<unsigned long long>(base_va + off),
                        got, nqw);
            std::fclose(f);
            return false;
        }
        if (std::fwrite(buf, 8, got, f) != got) {
            std::printf("[FAIL] fwrite(w) short \"%s\"\n", filepath);
            std::fclose(f);
            return false;
        }
        off += static_cast<uint64_t>(got) * 8;
    }
    if (off < size_bytes) {
        uint64_t v = 0;
        const uint32_t s =
            ch.Fire(PMC_CMD_VIRT_READ8, base_va + off, 0, 0, &v);
        if (s != covert::kStatusOk) {
            std::printf("[FAIL] VIRT_READ8 tail @0x%llX status=%u\n",
                        static_cast<unsigned long long>(base_va + off), s);
            std::fclose(f);
            return false;
        }
        const size_t rem = static_cast<size_t>(size_bytes - off);
        if (std::fwrite(&v, 1, rem, f) != rem) {
            std::printf("[FAIL] fwrite(tail) short \"%s\"\n", filepath);
            std::fclose(f);
            return false;
        }
    }
    std::fclose(f);
    return true;
}

static int RunLiveMemdump(covert::Channel& ch, Args& args) {
    Log(args,
        "[OK] live-memdump starting (NO HOOK_INSTALL — read-only VIRT)\n");

    if (!RecoverPhase4Cr3(ch, args))
        return 50;
    if (args.target_cr3 == 0 || args.module_base == 0) {
        std::printf("[FAIL] need CR3 + ImageBase (--peb Phase-4 recover, or "
                    "--no-recover --target-cr3 --module-base)\n");
        return 51;
    }
    if (!HvCommitTargetCr3(ch, args, args.target_cr3))
        return 52;

    char path[MAX_PATH];
    const char* d = args.live_memdump_dir;

    auto path_ok = [&](const char* fname) {
        const int nh = std::snprintf(path, sizeof(path), "%s\\%s", d, fname);
        return nh > 0 && nh < static_cast<int>(sizeof(path));
    };

    if (!path_ok("r5apex_live_text.bin") ||
        !DumpGuestRange(ch, args.module_base + kSecTextRva, kSecTextSz,
                       path, args))
        return 53;
    std::printf("  text -> %s\n", path);

    if (!path_ok("r5apex_live_rdata.bin") ||
        !DumpGuestRange(ch, args.module_base + kSecRdataRva, kSecRdataSz,
                       path, args))
        return 54;
    std::printf("  rdata -> %s\n", path);

    if (!path_ok("r5apex_live_data.bin") ||
        !DumpGuestRange(ch, args.module_base + kSecDataRva, kSecDataSz,
                       path, args))
        return 55;
    std::printf("  data -> %s\n", path);

    std::puts("LIVE_MEMDUMP_OK");
    return 0;
}

// ---------- --dump-canon: live-read every named canon offset -------------
//
// Reads each `(name, rva)` slot via VIRT_READ8 against the captured CR3, then
// classifies the 8-byte content. For slots that read 0 (suspected drift),
// sweeps ±0x200 around the RVA in 8-byte strides and prints any heap-pointer
// candidates (likely re-pin targets).
//
// Output is human-readable + grep-able. No header is emitted — operator
// eyeballs the report and edits HypeApexCanon.h by hand.

namespace dump_canon {

struct Slot { const char* name; uint64_t rva; const char* kind; };

// Source of truth: SCA/HypeApexCanon.h. Keep in sync when offsets are added.
// kind ∈ { "heap_ptr", "image_ptr", "int", "u32", "func_rva", "kbutton" }
//   heap_ptr  — expect heap VA range (0x00000100..0x00000800 high dword)
//   image_ptr — expect Apex image VA range (0x00007FF6..)
//   int       — expect small integer / boolean / convar
//   u32       — explicit u32-sized field (only low 4B meaningful)
//   func_rva  — RVA pointing inside .text (resolved as ImageBase + RVA)
//   kbutton   — kbutton_t struct; +0x8 holds state dword
static const Slot kSlots[] = {
    { "APEX_OFF_ENTITY_LIST",        0x06268BE8, "heap_ptr"  },
    { "APEX_OFF_VIEW_RENDER",        0x03D9A008, "heap_ptr"  },
    { "APEX_OFF_NAME_LIST",          0x08C65160, "heap_ptr"  },
    { "APEX_OFF_OBSERVER_LIST",      0x0626AC08, "heap_ptr"  },
    { "APEX_OFF_LOCAL_PLAYER",       0x0268FA08, "heap_ptr"  },
    { "APEX_OFF_HIGHLIGHT_SETTINGS", 0x069B28C0, "heap_ptr"  },
    { "APEX_OFF_VIEW_MATRIX_DEREF",  0x0011A350, "int"       },
    { "APEX_OFF_IN_ATTACK_KBUTTON",  0x03D9A658, "kbutton"   },
    { "APEX_OFF_MP_GAMEMODE",        0x026CA650, "image_ptr" },
    { "APEX_OFF_CROSSPLAY_ENABLED",  0x01EADA60, "image_ptr" },
    { "APEX_OFF_SHOW_FPS",           0x01F05300, "image_ptr" },
    { "APEX_OFF_IN_JUMP",            0x03D9AF10, "kbutton"   },
    { "APEX_OFF_IN_DUCK",            0x03D9B008, "kbutton"   },
    { "APEX_OFF_IN_FORWARD",         0x03D9B048, "kbutton"   },
    { "APEX_OFF_IN_USE",             0x03D9AF80, "kbutton"   },
    { "APEX_OFF_IN_SPEED",           0x03D9A5E0, "kbutton"   },
    { "APEX_OFF_RENDER_GATE_FN",     0x008174F0, "func_rva"  },
};

static const char* Classify(uint64_t v) {
    if (v == 0) return "ZERO";
    const uint64_t hi = v >> 32;
    if (hi >= 0x00000100 && hi < 0x00000800)       return "heap_ptr";  // typical Win10/11 user heap
    if (hi >= 0x00007FF6 && hi < 0x00007FF9)       return "image_ptr"; // Apex .text/.rdata
    if (v < 0x10000)                               return "int";       // small int / bool
    if (v < 0x80000000)                            return "rva-shape"; // looks like an RVA literal
    return "other";
}

static int Run(covert::Channel& ch, const Args& args) {
    if (args.module_base == 0 || args.target_cr3 == 0) {
        std::printf("[FAIL] dump-canon needs CR3 + ImageBase (use --peb for "
                    "Phase-4 recover)\n");
        return 60;
    }

    std::printf("=== DUMP-CANON ImageBase=0x%016llX Cr3=0x%016llX ===\n",
                (unsigned long long)args.module_base,
                (unsigned long long)args.target_cr3);
    std::printf("%-32s  %-12s  %-18s  %-12s\n",
                "name", "rva", "value", "classify");
    std::printf("%-32s  %-12s  %-18s  %-12s\n",
                "----", "---", "-----", "--------");

    const uint32_t n = sizeof(kSlots) / sizeof(kSlots[0]);
    int zero_count = 0;

    for (uint32_t i = 0; i < n; ++i) {
        const Slot& s = kSlots[i];
        uint64_t va  = args.module_base + s.rva;
        uint64_t val = 0;
        const uint32_t st = ch.Fire(PMC_CMD_VIRT_READ8, va, 0, 0, &val);
        if (st != covert::kStatusOk) {
            std::printf("%-32s  0x%08llX  READ_ERR(st=%u)\n",
                        s.name, (unsigned long long)s.rva, st);
            continue;
        }
        const char* cls    = Classify(val);
        const char* expect = s.kind;
        const bool  mismatch = (val == 0) ||
            (std::strcmp(expect, cls) != 0 &&
             !(std::strcmp(expect, "func_rva") == 0 &&
               std::strcmp(cls, "image_ptr") == 0));
        const char* tag    = mismatch ? "  DRIFT?" : "";

        // kbutton heuristic: +0x8 is the state dword; emit it for context.
        if (std::strcmp(s.kind, "kbutton") == 0) {
            uint64_t state = 0;
            ch.Fire(PMC_CMD_VIRT_READ8, va + 0x8, 0, 0, &state);
            std::printf("%-32s  0x%08llX  0x%016llX  %-12s  state=0x%llX%s\n",
                        s.name, (unsigned long long)s.rva,
                        (unsigned long long)val, cls,
                        (unsigned long long)(state & 0xFFFFFFFF), tag);
        } else if (std::strcmp(s.kind, "func_rva") == 0) {
            // Pull 8B of prologue from ImageBase + RVA to verify entry.
            uint64_t prologue = 0;
            ch.Fire(PMC_CMD_VIRT_READ8, va, 0, 0, &prologue);
            std::printf("%-32s  0x%08llX  prologue=0x%016llX  %s%s\n",
                        s.name, (unsigned long long)s.rva,
                        (unsigned long long)prologue, cls, tag);
        } else {
            std::printf("%-32s  0x%08llX  0x%016llX  %-12s%s\n",
                        s.name, (unsigned long long)s.rva,
                        (unsigned long long)val, cls, tag);
        }

        if (val == 0 && std::strcmp(s.kind, "heap_ptr") == 0) {
            zero_count++;
        }
    }

    // For every heap_ptr slot that read 0, sweep ±0x200 (64 qwords) for
    // heap-shaped neighbors. Bulk read 0x80 qwords centered on the slot.
    if (zero_count > 0) {
        std::printf("\n=== NEIGHBOR SWEEP (heap_ptr slots that read ZERO) ===\n");
        std::printf("Listing heap-pointer-shaped values within +/-0x200 of "
                    "each null slot:\n\n");
        constexpr int kStrideQwords = 64;  // 0x200 bytes
        for (uint32_t i = 0; i < n; ++i) {
            const Slot& s = kSlots[i];
            if (std::strcmp(s.kind, "heap_ptr") != 0) continue;
            uint64_t base_va = args.module_base + s.rva;
            uint64_t test = 0;
            if (ch.Fire(PMC_CMD_VIRT_READ8, base_va, 0, 0, &test) != covert::kStatusOk
                || test != 0)
                continue;

            const uint64_t scan_base = base_va - (kStrideQwords / 2) * 8;
            uint64_t buf[kStrideQwords] = {};
            const uint32_t got = ch.BulkVirtRead8(scan_base, buf, kStrideQwords);
            std::printf("--- %s (slot read 0, scanning 0x%llX..0x%llX) ---\n",
                        s.name,
                        (unsigned long long)scan_base,
                        (unsigned long long)(scan_base + got * 8));
            for (uint32_t k = 0; k < got; ++k) {
                const uint64_t v  = buf[k];
                const uint64_t hi = v >> 32;
                if (hi >= 0x00000100 && hi < 0x00000800 && (v & 0xF) == 0) {
                    const int64_t  delta  = (int64_t)((scan_base + k * 8) - base_va);
                    const uint64_t cand_va = scan_base + k * 8;
                    const uint64_t cand_rva = cand_va - args.module_base;
                    std::printf("  delta=%+5lld   rva=0x%08llX  va=0x%016llX  val=0x%016llX\n",
                                (long long)delta,
                                (unsigned long long)cand_rva,
                                (unsigned long long)cand_va,
                                (unsigned long long)v);
                }
            }
            std::printf("\n");
        }
    }

    std::printf("=== DONE — %u slots scanned ===\n", n);
    return 0;
}

}  // namespace dump_canon

// ---------- --find-ptr <VA>: scan .data for matching qword -------------
//
// BulkVirtRead8 sweep of the raw .data section (12 MB, ~1.5M qwords) looking
// for any 8-byte slot equal to args.find_ptr_target. Prints every match
// with its RVA. Use to locate the .data slot where a known heap entity is
// referenced — e.g. find LocalPlayer slot by passing a confirmed alive
// player entity VA.

static int RunFindPtr(covert::Channel& ch, Args& args) {
    if (args.find_ptr_target == 0) {
        std::printf("[FAIL] --find-ptr needs <heap_va>\n");
        return 70;
    }
    Log(args, "[OK] find-ptr starting target=0x%016llX\n",
        (unsigned long long)args.find_ptr_target);

    if (!RecoverPhase4Cr3(ch, args)) return 71;
    if (args.target_cr3 == 0 || args.module_base == 0) {
        std::printf("[FAIL] find-ptr: CR3/ImageBase not recovered\n");
        return 72;
    }
    if (!HvCommitTargetCr3(ch, args, args.target_cr3)) return 73;

    const uint64_t target = args.find_ptr_target;
    const uint64_t data_va_base = args.module_base + kSecDataRva;
    // Cover .data + BSS extension up to ~0x09000000 (144 MB) — past every
    // canonical global we use (NAME_LIST at 0x08C65160 is the farthest).
    // Stops on first BulkVirtRead8 partial fault (got < n).
    constexpr uint64_t kScanBytes = 0x09000000ULL - 0x01B30000ULL;  // ~117 MB
    const uint64_t total_qwords = kScanBytes / 8;
    constexpr uint32_t kBatch = 80;  // BulkVirtRead8 max per NPF

    std::printf("=== FIND-PTR target=0x%016llX  scanning .data "
                "[0x%016llX..0x%016llX) ===\n",
                (unsigned long long)target,
                (unsigned long long)data_va_base,
                (unsigned long long)(data_va_base + kSecDataSz));

    uint64_t buf[kBatch] = {};
    uint32_t n_match = 0;
    uint32_t batches_done = 0;
    const uint32_t total_batches = (uint32_t)((total_qwords + kBatch - 1) / kBatch);

    for (uint64_t off = 0; off < total_qwords; off += kBatch) {
        const uint32_t n = (uint32_t)((total_qwords - off) > kBatch
                                       ? kBatch : (total_qwords - off));
        const uint64_t va = data_va_base + off * 8;
        const uint32_t got = ch.BulkVirtRead8(va, buf, n);
        for (uint32_t i = 0; i < got; ++i) {
            if (buf[i] == target) {
                const uint64_t match_va  = va + i * 8;
                const uint64_t match_rva = match_va - args.module_base;
                std::printf("  MATCH rva=0x%08llX  va=0x%016llX\n",
                            (unsigned long long)match_rva,
                            (unsigned long long)match_va);
                n_match++;
            }
        }
        batches_done++;
        if ((batches_done & 0x7FF) == 0) {
            std::printf("  ... %u/%u batches (%llu MB scanned), matches=%u\n",
                        batches_done, total_batches,
                        (unsigned long long)(batches_done * (uint64_t)kBatch * 8 / (1024*1024)),
                        n_match);
        }
        // Partial fault — skip this batch and keep going (BSS is sparse).
        // No bail; loop's for-step advances past this batch automatically.
        (void)got;
    }
    std::printf("=== DONE: %u matches in %u batches ===\n", n_match, batches_done);
    return 0;
}

static int RunDumpCanon(covert::Channel& ch, Args& args) {
    Log(args, "[OK] dump-canon starting\n");

    // Same setup as live-memdump: Phase-4 CR3 recovery + SET_CR3, then read.
    if (!RecoverPhase4Cr3(ch, args))
        return 60;
    if (args.target_cr3 == 0 || args.module_base == 0) {
        std::printf("[FAIL] dump-canon: CR3/ImageBase not recovered "
                    "(pass --peb for Phase-4)\n");
        return 61;
    }
    if (!HvCommitTargetCr3(ch, args, args.target_cr3))
        return 62;

    return dump_canon::Run(ch, args);
}

// ----- main -------------------------------------------------------------

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered for SSH

    // Non-const: CR3 recovery writes back into args.
    Args args = ParseArgs(argc, argv);

    if (!vmmcall::Init()) {
        Log(args, "[FAIL] vmmcall::Init\n");
        return 1;
    }

    uint64_t auth_key = 0;
    int hv_cpu = -1;
    if (!DoHandshake(&auth_key, &hv_cpu, args)) {
        Log(args, "[FAIL] no virtualized CPU found\n");
        vmmcall::Free();
        return 2;
    }

    // SET_EPROCESS — required by the HV before any covert command.
    {
        bool faulted = false;
        const uint64_t r = TryVmmcall(auth_key, PMC_CMD_SET_EPROCESS,
                                      0, 0, &faulted);
        if (faulted) {
            Log(args, "[FAIL] SET_EPROCESS exception\n");
            vmmcall::Free();
            return 3;
        }
        if (r != HYPE_MEM_OK) {
            Log(args, "[FAIL] SET_EPROCESS r=%llu\n",
                static_cast<unsigned long long>(r));
            vmmcall::Free();
            return 4;
        }
        Log(args, "[OK] SET_EPROCESS\n");
    }

    // NPT covert channel up.
    covert::Channel ch;
    if (!ch.Init(auth_key, 1ULL << hv_cpu)) {
        Log(args, "[FAIL] NPT_CHANNEL_INIT\n");
        vmmcall::Free();
        return 5;
    }
    Log(args, "[OK] NPT_CHANNEL_INIT\n");

    // VMMCALL stub no longer needed; mailbox is the sole runtime channel.
    vmmcall::Free();

    // --diag-m0f: one-shot read of gLapicDisarmedNpfCount. Source-only
    // diagnostic — no install, no glow push. Tells you whether the
    // disarmed-LAPIC NPF branch is firing without needing a fresh drain.
    if (args.diag_m0f) {
        uint64_t cnt = 0;
        const uint32_t s = ch.Fire(PMC_CMD_GET_LAPIC_DISARMED_NPF_COUNT,
                                   0, 0, 0, &cnt);
        std::printf("M0F_COUNT %llu status=%u\n",
                    (unsigned long long)cnt, s);
        return (s == covert::kStatusOk) ? 0 : 25;
    }

    // --scan-box <seconds> — arm box-scan mode, poll for first capture.
    // HV's ScanOneEntity widens to log non-player class names + probe
    // +0x1660 for ASCII (deathbox m_customOwnerName signature). Polls
    // every 500ms for up to <seconds>; on first hit prints the captured
    // ent VA + disarms scan mode automatically. Operator follow-up:
    //   SCAhost.exe --peb <peb> --render-sink <ent_va + 0x1660> 32 --sentinel "ZZZZZZZZ"
    if (args.scan_box) {
        uint64_t r = 0;
        const uint32_t s_arm = ch.Fire(PMC_CMD_SCAN_BOX, 1, 0, 0, &r);
        if (s_arm != covert::kStatusOk) {
            std::printf("[FAIL] SCAN_BOX arm status=%u\n", s_arm);
            return 27;
        }
        std::printf("[OK] SCAN_BOX armed — polling 500ms for up to %us\n",
                    args.scan_box_seconds);
        const uint32_t max_polls = (args.scan_box_seconds * 2);
        uint64_t captured = 0;
        for (uint32_t i = 0; i < max_polls; ++i) {
            Sleep(500);
            uint64_t got = 0;
            const uint32_t s = ch.Fire(PMC_CMD_SCAN_BOX, 2, 0, 0, &got);
            if (s != covert::kStatusOk) continue;
            if (got != 0) { captured = got; break; }
        }
        // Disarm regardless.
        ch.Fire(PMC_CMD_SCAN_BOX, 0, 0, 0, &r);
        if (captured) {
            std::printf("BOX_FOUND ent=0x%016llX  next: --render-sink 0x%llx 32 --sentinel \"ZZZZZZZZ\"\n",
                        (unsigned long long)captured,
                        (unsigned long long)(captured + 0x1660));
            return 0;
        }
        std::printf("BOX_NONE — no entity with ASCII text at +0x1660 in %us\n",
                    args.scan_box_seconds);
        return 28;
    }

    // --diag-msr-timing MSR# — one-shot RDMSR/RDTSC-paired timing probe.
    // Source for SCA/research/hyperjacking_2026_state.md §6.2.2 — measures
    // the VMEXIT round-trip cost of an MSR shadowed by SCA so we can
    // compare against bare-metal RDMSR delta (run from a non-HYPEBOOT boot
    // for the baseline). Decision rule: if HV avg < 2-3x bare-metal avg,
    // leave the EFER/VM_CR/VM_HSAVE_PA shadows in place — within
    // SMI/page-walk noise and not worth the compensation.
    if (args.diag_msr_timing) {
        uint64_t packed = 0;
        uint64_t max_cycles = 0;
        const uint32_t s = ch.Fire(PMC_CMD_DIAG_MSR_TIMING,
                                   (uint64_t)args.diag_msr_id, 0, 0,
                                   &packed, &max_cycles);
        const uint32_t min_cy = (uint32_t)(packed >> 32);
        const uint32_t avg_cy = (uint32_t)(packed & 0xFFFFFFFFu);
        std::printf("MSR_TIMING msr=0x%X min=%u avg=%u max=%llu status=%u\n",
                    args.diag_msr_id, min_cy, avg_cy,
                    (unsigned long long)max_cycles, s);
        return (s == covert::kStatusOk) ? 0 : 26;
    }

    // --drain: pull 4MB HV ring via PMC_CMD_HV_LOG_READ; HypeDrain.efi layout.
    if (args.drain) {
        constexpr uint32_t kQwords = HV_DEBUG_LOG_SIZE / 8;
        auto* buf = static_cast<uint64_t*>(VirtualAlloc(
            nullptr, HV_DEBUG_LOG_SIZE,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!buf) {
            Log(args, "[FAIL] drain alloc %u bytes\n", HV_DEBUG_LOG_SIZE);
            return 20;
        }

        const ULONGLONG t0 = GetTickCount64();
        const uint32_t got = ch.BulkHvLogRead(0, buf, kQwords);
        const ULONGLONG dt = GetTickCount64() - t0;
        if (got != kQwords) {
            Log(args, "[WARN] drain partial: %u/%u qwords (%llums)\n",
                got, kQwords, (unsigned long long)dt);
        } else {
            Log(args, "[OK] drained %u qwords (%u bytes, %llums)\n",
                got, got * 8u, (unsigned long long)dt);
        }

        const uint64_t magic     = buf[0];
        const uint64_t write_pos = buf[1];
        Log(args, "[..] Magic=0x%016llX WritePos=0x%llX\n",
            (unsigned long long)magic, (unsigned long long)write_pos);

        char default_path[64];
        const char* out_path = args.drain_out;
        if (!out_path) {
            std::snprintf(default_path, sizeof(default_path),
                          "hypedbg-live-%016llX.bin",
                          (unsigned long long)__rdtsc());
            out_path = default_path;
        }

        HANDLE h = CreateFileA(out_path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            Log(args, "[FAIL] CreateFile(%s) err=%lu\n",
                out_path, GetLastError());
            VirtualFree(buf, 0, MEM_RELEASE);
            return 21;
        }
        DWORD wrote = 0;
        const BOOL ok = WriteFile(h, buf, got * 8u, &wrote, nullptr);
        CloseHandle(h);
        VirtualFree(buf, 0, MEM_RELEASE);
        if (!ok || wrote != got * 8u) {
            Log(args, "[FAIL] WriteFile %s wrote=%lu err=%lu\n",
                out_path, wrote, GetLastError());
            return 22;
        }
        std::printf("DRAIN_OK %s %lu\n", out_path, (unsigned long)wrote);
        return 0;
    }

    // --drift-check: verify the small set of canonical pins from
    // UNICORN_DUMPER/OFFSETS.md still hold against the live Apex binary.
    // One scatter VIRT_READ8 batch (4 reads, ~1 NPF). Replaces the legacy
    // --dump-buckets/--dump-hsp-pins/--dump-glow-map exhaustive probes —
    // the layout itself is now statically pinned, so we just sanity-check
    // it hasn't drifted under us.
    if (args.drift_check) {
        if (args.module_base == 0) {
            std::printf("ERR --drift-check needs --module-base 0xVA "
                        "(from prior install [OK] HOOK_INSTALL_DRAW line)\n");
            return 0;
        }
        constexpr uint64_t kBucketPtrRva   = 0x69B0600;
        constexpr uint64_t kBucketCountRva = 0x69B0618;
        constexpr uint64_t kSetHidRva      = 0x818500;  // 2026-05-13: re-pinned, +0xF00 drift from 0x817600
        constexpr uint32_t kExpectedCount  = 89;

        const uint64_t vas[4] = {
            args.module_base + kBucketPtrRva,
            args.module_base + kBucketCountRva,
            args.module_base + kSetHidRva + 0x00,
            args.module_base + kSetHidRva + 0x08,
        };
        uint64_t out[4] = {};
        const uint32_t got = ch.BulkVirtRead8Scatter(vas, out, 4);
        if (got != 4) {
            std::printf("DRIFT-FAIL scatter read: got %u/4\n", got);
            return 33;
        }
        const uint64_t bucket_ptr   = out[0];
        const uint32_t bucket_count = static_cast<uint32_t>(out[1] & 0xFFFFFFFFu);

        bool ok = true;
        if (bucket_ptr < 0x10000ULL) {
            std::printf("DRIFT-FAIL BucketTable_Ptr@RVA 0x%llX = 0x%016llX "
                        "(expected non-null heap ptr)\n",
                        (unsigned long long)kBucketPtrRva,
                        (unsigned long long)bucket_ptr);
            ok = false;
        } else {
            std::printf("DRIFT-OK  BucketTable_Ptr@RVA 0x%llX = 0x%016llX\n",
                        (unsigned long long)kBucketPtrRva,
                        (unsigned long long)bucket_ptr);
        }
        if (bucket_count != kExpectedCount) {
            std::printf("DRIFT-WARN BucketCount@RVA 0x%llX = %u "
                        "(expected %u per OFFSETS.md)\n",
                        (unsigned long long)kBucketCountRva,
                        bucket_count, kExpectedCount);
            // Not fatal — count can shift between Apex patches without
            // breaking glow if everything else holds.
        } else {
            std::printf("DRIFT-OK  BucketCount@RVA 0x%llX = %u\n",
                        (unsigned long long)kBucketCountRva, bucket_count);
        }
        std::printf("DRIFT-INFO SetHighlightId@RVA 0x%llX prologue = "
                    "%016llX %016llX  (eyeball vs OFFSETS.md sig)\n",
                    (unsigned long long)kSetHidRva,
                    (unsigned long long)out[2],
                    (unsigned long long)out[3]);
        return ok ? 0 : 34;
    }

    // --live-memdump — read Apex .text/.rdata/.data via VIRT_READ8 only (no hook).
    if (args.live_memdump_dir != nullptr)
        return RunLiveMemdump(ch, args);

    // --dump-canon — live-read every named canon offset + classify + sweep.
    if (args.dump_canon)
        return RunDumpCanon(ch, args);

    // --find-ptr <VA> — scan .data for any qword matching the target VA.
    if (args.find_ptr_target != 0)
        return RunFindPtr(ch, args);

    // --peek-va: generic 4-qword VIRT_READ8 dump at VA.
    if (args.peek_va != 0) {
        std::printf("PEEK-VA 0x%016llX (4 qwords):\n",
                    (unsigned long long)args.peek_va);
        for (uint32_t off = 0; off < 0x20; off += 8) {
            uint64_t v = 0;
            const uint32_t s = ch.Fire(PMC_CMD_VIRT_READ8, args.peek_va + off,
                                       0, 0, &v);
            std::printf("  +0x%02X: 0x%016llX  status=%u\n",
                        off, (unsigned long long)v, s);
        }
        return 0;
    }

    // --peek-ent: dump +0x290..+0x2B0 (HID/STACK[0]/MASK gate bytes).
    if (args.peek_ent != 0) {
        const uint64_t base = args.peek_ent + 0x290;
        std::printf("PEEK-ENT 0x%016llX (+0x290..+0x2B0):\n",
                    (unsigned long long)args.peek_ent);
        for (uint32_t off = 0; off < 0x20; off += 8) {
            uint64_t v = 0;
            const uint32_t s = ch.Fire(PMC_CMD_VIRT_READ8, base + off,
                                       0, 0, &v);
            std::printf("  +0x%03X: 0x%016llX  status=%u\n",
                        0x290 + off, (unsigned long long)v, s);
        }
        // Decode the canon fields (little-endian byte indexing into the qwords)
        uint64_t q1 = 0, q2 = 0;
        ch.Fire(PMC_CMD_VIRT_READ8, args.peek_ent + 0x298, 0, 0, &q1);
        ch.Fire(PMC_CMD_VIRT_READ8, args.peek_ent + 0x2A0, 0, 0, &q2);
        const uint8_t hid     = static_cast<uint8_t>(q1 & 0xFF);
        const uint8_t stack0  = static_cast<uint8_t>((q1 >> 8) & 0xFF);
        const uint8_t mask    = static_cast<uint8_t>((q2 >> 8) & 0xFF);
        std::printf("DECODED  HID=%u  STACK[0]=%u  MASK=0x%02X\n",
                    hid, stack0, mask);
        return 0;
    }

    // --dump-teams: PEEK ENT_SNAPSHOT[64] + LocalSnapshot.Team from mirror.
    // Bulkified — one scatter-PEEK batch packs all ent_va probes, then a
    // second batch fans out (team, flags, hid) for slots that came back
    // non-zero. Drops 256+ NPFs (the original per-Fire-per-field design)
    // down to ~5 batches regardless of how many slots are populated.
    if (args.dump_teams) {
        constexpr uint64_t kOffSentinel  = 0x0000;
        constexpr uint64_t kOffLocal     = 0x0100;
        constexpr uint64_t kOffEnts      = 0x0800;
        constexpr uint64_t kStride       = 0x68;
        constexpr uint64_t kOffHeartbeat = 0x3F00;

        // Header batch: sentinel + heartbeat + local ent + local team pack.
        // (PEEK at LocalSnapshot+0x68 grabs InheritOwner(lo32)|Team(hi32).)
        const uint64_t hdr_offs[4] = {
            kOffSentinel, kOffHeartbeat, kOffLocal, kOffLocal + 0x68,
        };
        uint64_t hdr[4] = {};
        if (ch.BulkPeekScatter(hdr_offs, hdr, 4) != 4) {
            std::printf("ERR --dump-teams header PEEK batch failed\n");
            return 0;
        }
        const uint64_t sentinel  = hdr[0];
        const uint64_t hb        = hdr[1];
        const uint64_t local_ent = hdr[2];
        const int32_t  local_team = static_cast<int32_t>(hdr[3] >> 32);
        std::printf("MIRROR sentinel=0x%016llX heartbeat=%llu\n",
                    (unsigned long long)sentinel, (unsigned long long)hb);
        std::printf("LOCAL ent=0x%016llX team=%d\n",
                    (unsigned long long)local_ent, local_team);

        // Pass 1: pull all 64 ent_va slots in one ≤80-cmd batch.
        uint64_t ent_offs[64];
        uint64_t ents[64] = {};
        for (uint32_t i = 0; i < 64; ++i) {
            ent_offs[i] = kOffEnts + i * kStride;
        }
        if (ch.BulkPeekScatter(ent_offs, ents, 64) != 64) {
            std::printf("ERR --dump-teams pass-1 PEEK batch failed\n");
            return 0;
        }

        // Pass 2: for populated slots, pull (team@+0x38, flags@+0x40, hid@+0x48)
        // — 3 PEEKs per live slot. 64*3 = 192 worst case → 3 batches of 80.
        // Keep an index map so we can pair results back to slot i.
        uint64_t det_offs[192];
        uint32_t det_slot[64];   // det_slot[i] = original slot index for record i
        uint32_t live = 0;
        for (uint32_t i = 0; i < 64; ++i) {
            if (ents[i] == 0) continue;
            det_slot[live] = i;
            const uint64_t base = ent_offs[i];
            det_offs[live * 3 + 0] = base + 0x38;
            det_offs[live * 3 + 1] = base + 0x40;
            det_offs[live * 3 + 2] = base + 0x48;
            ++live;
        }
        uint64_t det[192] = {};
        if (live > 0) {
            const uint32_t n = live * 3;
            if (ch.BulkPeekScatter(det_offs, det, n) != n) {
                std::printf("ERR --dump-teams pass-2 PEEK batch failed\n");
                return 0;
            }
        }
        for (uint32_t r = 0; r < live; ++r) {
            const uint32_t i        = det_slot[r];
            const uint64_t ms_team  = det[r * 3 + 0];
            const uint64_t at_flags = det[r * 3 + 1];
            const uint64_t hid_pack = det[r * 3 + 2];
            const int32_t team   = static_cast<int32_t>(ms_team >> 32);
            const uint8_t player = static_cast<uint8_t>((at_flags >> 32) & 0xFF);
            const uint8_t alive  = static_cast<uint8_t>((at_flags >> 40) & 0xFF);
            const uint8_t downed = static_cast<uint8_t>((at_flags >> 48) & 0xFF);
            const uint8_t decoy  = static_cast<uint8_t>((at_flags >> 56) & 0xFF);
            const uint8_t loot   = static_cast<uint8_t>(hid_pack & 0xFF);
            const uint8_t hid    = static_cast<uint8_t>((hid_pack >> 8) & 0xFF);
            std::printf("ENT[%02u] ent=0x%016llX team=%4d "
                        "player=%u alive=%u downed=%u decoy=%u "
                        "loot=%u hid=%u\n",
                        i, (unsigned long long)ents[i], team,
                        player, alive, downed, decoy, loot, hid);
        }
        std::printf("SEEN %u/64\n", live);
        return 0;
    }

    // --bucket-write: VIRT_WRITE4 into HIGHLIGHT_SETTINGS[slot]+off.
    // Requires --module-base from a prior install's [OK] HOOK_INSTALL_DRAW.
    if (args.bw_slot >= 0) {
        if (args.module_base == 0) {
            std::printf("ERR --bucket-write needs --module-base 0xVA "
                        "(from prior install log)\n");
            return 0;
        }
        uint64_t settings = 0;
        ch.Fire(PMC_CMD_VIRT_READ8, args.module_base + 0x69B0600,
                0, 0, &settings);
        if (settings < 0x10000) {
            std::printf("ERR settings_ptr=0x%llX\n",
                        (unsigned long long)settings);
            return 0;
        }
        const uint64_t addr = settings + (uint64_t)args.bw_slot * 0x34
                              + (uint64_t)args.bw_off;
        uint64_t result = 0;
        const uint32_t s = ch.Fire(PMC_CMD_VIRT_WRITE4, addr,
                                   (uint64_t)args.bw_val, 0, &result);
        std::printf("BUCKET-WRITE slot=%d off=0x%X val=0x%08X "
                    "addr=0x%016llX status=%u\n",
                    args.bw_slot, args.bw_off, args.bw_val,
                    (unsigned long long)addr, s);
        return (s == covert::kStatusOk) ? 0 : 31;
    }

    // --hid-set ENT_VA HID — SetHighlightId via PMC_CMD_VIRT_CALL (module_base+0x818500, was 0x817600 pre +0xF00 drift).
    if (args.hid_set) {
        if (args.module_base == 0) {
            std::printf("ERR --hid-set needs --module-base 0xVA\n");
            return 0;
        }
        // Allocate trampoline page (separate from kScratchBytes). 4 KB,
        // VirtualLock'd so guest VA → GPA mapping is stable.
        void* tram = VirtualAlloc(nullptr, 4096,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!tram) {
            std::printf("ERR trampoline VirtualAlloc failed err=%lu\n",
                        GetLastError());
            return 40;
        }
        if (!VirtualLock(tram, 4096)) {
            std::printf("ERR trampoline VirtualLock failed err=%lu\n",
                        GetLastError());
            VirtualFree(tram, 0, MEM_RELEASE);
            return 41;
        }
        std::memset(tram, 0xCC, 4096);  // INT3 fill — defensive
        const uint64_t tram_va = reinterpret_cast<uint64_t>(tram);

        uint64_t init_r = 0;
        const uint32_t init_s = ch.Fire(PMC_CMD_VIRT_CALL_INIT,
                                        tram_va, 0, 0, &init_r);
        if (init_s != covert::kStatusOk) {
            std::printf("ERR VIRT_CALL_INIT status=%u\n", init_s);
            VirtualUnlock(tram, 4096);
            VirtualFree(tram, 0, MEM_RELEASE);
            return 42;
        }
        std::printf("VIRT_CALL_INIT tram_va=0x%016llX tram_gpa=0x%016llX\n",
                    (unsigned long long)tram_va,
                    (unsigned long long)init_r);

        const uint64_t fn_va = args.module_base + 0x00818500;  // SetHighlightId live RVA (was 0x00817600 pre +0xF00 drift)
        uint64_t call_r = 0;
        const uint32_t call_s = ch.Fire(PMC_CMD_VIRT_CALL,
                                        fn_va,
                                        args.hid_set_ent,
                                        (uint64_t)args.hid_set_hid,
                                        &call_r);
        std::printf("VIRT_CALL fn=0x%016llX ent=0x%016llX hid=0x%X "
                    "rax=0x%llX status=%u\n",
                    (unsigned long long)fn_va,
                    (unsigned long long)args.hid_set_ent,
                    args.hid_set_hid,
                    (unsigned long long)call_r, call_s);

        VirtualUnlock(tram, 4096);
        VirtualFree(tram, 0, MEM_RELEASE);
        return (call_s == covert::kStatusOk) ? 0 : 43;
    }

    // SendAimParams — packs aim knobs into Arg1 and fires PMC_CMD_SET_AIM_PARAMS.
    auto SendAimParams = [&](const Args& a) {
        const uint64_t packed =
              (uint64_t)a.aim_trigger_en
            | ((uint64_t)a.aim_aim_en      <<  8)
            | ((uint64_t)a.aim_trig_fov    << 16)
            | ((uint64_t)a.aim_trig_thresh << 24)
            | ((uint64_t)a.aim_debounce    << 32)
            | ((uint64_t)a.aim_fov         << 40);
        uint64_t result = 0;
        const uint32_t s = ch.Fire(PMC_CMD_SET_AIM_PARAMS, packed, 0, 0, &result);
        std::printf("AIM_PARAMS trigger=%u aim=%u trig_fov=0x%02X thresh=0x%02X "
                    "debounce=%u aim_fov=0x%02X packed=0x%016llX status=%u\n",
                    a.aim_trigger_en, a.aim_aim_en,
                    a.aim_trig_fov, a.aim_trig_thresh,
                    a.aim_debounce, a.aim_fov,
                    (unsigned long long)packed, s);
        return s;
    };

    auto PackVguiArg1 = [&](const Args& a) {
        return (uint64_t)a.vgui_enabled
             | ((uint64_t)a.vgui_probe_only  <<  8)
             | ((uint64_t)a.vgui_dry_arm     << 16)
             | ((uint64_t)a.vgui_rollback    << 24)
             | ((uint64_t)a.vgui_fail_closed << 32)
             | ((uint64_t)a.vgui_stable_need << 40)
             | ((uint64_t)a.vgui_max_faults  << 48);
    };
    auto PackVguiArg2 = [&](const Args& a) {
        return (uint64_t)a.vgui_global_rva
             | ((uint64_t)a.vgui_slot_draw  << 32)
             | ((uint64_t)a.vgui_slot_pos   << 40)
             | ((uint64_t)a.vgui_slot_color << 48)
             | ((uint64_t)a.vgui_slot_font  << 56);
    };
    auto PackVguiArg3 = [&](const Args& a) {
        return (uint64_t)a.vgui_text_x
             | ((uint64_t)a.vgui_text_y << 16)
             | ((uint64_t)a.vgui_text_rgba << 32);
    };

    // --reconfig: push gGlowParams + exit. HV must already be armed
    // from a prior install this boot. Iteration cost: ~3 sec round trip.
    auto SendGlowParams = [&](const Args& a) {
        const uint64_t packed =
              (uint64_t)a.glow_slot
            | ((uint64_t)a.glow_mask     << 8)
            | ((uint64_t)a.glow_filter   << 16)
            | ((uint64_t)a.glow_enabled  << 24)
            | ((uint64_t)a.glow_vistype  << 32)
            | ((uint64_t)a.glow_glowfix  << 40)
            | ((uint64_t)a.glow_wvistype << 48)
            | ((uint64_t)a.glow_wglowfix << 56);
        const uint64_t packed2 = (uint64_t)a.glow_squad;  // Arg2[7:0] = SquadGlow
        uint64_t result = 0;
        const uint32_t s = ch.Fire(PMC_CMD_SET_GLOW_PARAMS, packed, packed2, 0, &result);
        std::printf("GLOW_PARAMS slot=%u mask=0x%02X filter=%u enabled=%u "
                    "vt=%u(w=%u) gf=%u(w=%u) squad=%u packed=0x%016llX/0x%016llX status=%u\n",
                    a.glow_slot, a.glow_mask, a.glow_filter, a.glow_enabled,
                    a.glow_vistype, a.glow_wvistype,
                    a.glow_glowfix, a.glow_wglowfix, a.glow_squad,
                    (unsigned long long)packed, (unsigned long long)packed2, s);
        return s;
    };

    auto SendVguiParams = [&](const Args& a) {
        const uint64_t packed1 = PackVguiArg1(a);
        const uint64_t packed2 = PackVguiArg2(a);
        const uint64_t packed3 = PackVguiArg3(a);
        uint64_t result = 0;
        const uint32_t s = ch.Fire(PMC_CMD_SET_VGUI_PARAMS, packed1, packed2, packed3, &result);
        std::printf("VGUI_PARAMS en=%u po=%u dry=%u rb=%u fc=%u stable=%u "
                    "maxf=%u rva=0x%08X slots(d=%u,p=%u,c=%u,f=%u) "
                    "txt(x=%u,y=%u,rgba=0x%08X) result=0x%016llX status=%u\n",
                    a.vgui_enabled, a.vgui_probe_only, a.vgui_dry_arm,
                    a.vgui_rollback, a.vgui_fail_closed, a.vgui_stable_need,
                    a.vgui_max_faults, a.vgui_global_rva,
                    a.vgui_slot_draw, a.vgui_slot_pos, a.vgui_slot_color, a.vgui_slot_font,
                    a.vgui_text_x, a.vgui_text_y, a.vgui_text_rgba,
                    (unsigned long long)result, s);
        return s;
    };

    auto SendMenuEnable = [&](const Args& a) {
        uint64_t result = 0;
        const uint32_t s = ch.Fire(PMC_CMD_SET_MENU_ENABLE,
                                   a.menu_disabled ? 0 : 1,
                                   0, 0, &result);
        std::printf("MENU_ENABLE arg=%u status=%u\n",
                    a.menu_disabled ? 0u : 1u, s);
        return s;
    };

    if (args.reconfig) {
        uint32_t s = SendGlowParams(args);
        if (s != covert::kStatusOk) return 30;
        if (args.aim_set) {
            s = SendAimParams(args);
            if (s != covert::kStatusOk) return 31;
        }
        if (args.vgui_set) {
            s = SendVguiParams(args);
            if (s != covert::kStatusOk) return 33;
        }
        s = SendMenuEnable(args);
        if (s != covert::kStatusOk) return 32;
        return 0;
    }

    // 64 KB scratch buffer the HV walks at install.
    void* scratch_buf = AllocScratch();
    if (!scratch_buf) {
        Log(args, "[FAIL] AllocScratch (64 KB)\n");
        return 7;
    }
    const uint64_t scratch_gva = reinterpret_cast<uint64_t>(scratch_buf);
    Log(args, "[OK] scratch buf @ 0x%016llX (%zu bytes, locked)\n",
        static_cast<unsigned long long>(scratch_gva), kScratchBytes);

    // CR3 recovery via Cr3PassiveSample (HV-side scan). Pass --peb for Phase 4.
    if (!args.no_recover && args.target_cr3 == 0) {
        if (!RecoverPhase4Cr3(ch, args)) {
            FreeScratch(scratch_buf);
            return 9;
        }
    }

    // SET_CR3 updates Vcpu->TargetCr3 (guest CR3 unchanged). Belt-and-
    // suspenders after CR3 recovery; required on the --no-recover path.
    if (args.target_cr3 == 0 || args.module_base == 0) {
        Log(args, "[FAIL] need both target_cr3 and module_base "
                  "(cr3=0x%llx base=0x%llx)\n",
            static_cast<unsigned long long>(args.target_cr3),
            static_cast<unsigned long long>(args.module_base));
        FreeScratch(scratch_buf);
        return 8;
    }

    const uint64_t hook_va = args.module_base + args.rva;

    // Install tail in one mailbox round-trip. HV's mailbox loop dispatches
    // sequentially (HypeVmexit.c:1436), and SET_CR3 calls InvalidateSoftTlb
    // inside its own handler (line 1458) — so HOOK_INSTALL_DRAW sees the
    // refreshed TargetCr3 within the same batch. SET_GLOW_PARAMS,
    // SET_AIM_PARAMS, and SET_VGUI_PARAMS are packed conditionally.
    covert::Cmd tail[7] = {};
    const covert::Cmd* gp_tail_cmd = nullptr;
    const covert::Cmd* vg_tail_cmd = nullptr;
    uint32_t tail_n = 0;
    auto& cr3_cmd  = tail[tail_n++];
    cr3_cmd.cmd_id = PMC_CMD_SET_CR3;
    cr3_cmd.arg1   = args.target_cr3;
    auto& hook_cmd = tail[tail_n++];
    hook_cmd.cmd_id = PMC_CMD_HOOK_INSTALL_DRAW;
    hook_cmd.arg1   = hook_va;
    hook_cmd.arg2   = scratch_gva;
    auto& peek_cmd = tail[tail_n++];
    peek_cmd.cmd_id = PMC_CMD_HOOK_DRAW_PEEK;
    peek_cmd.arg1   = 0;
    if (args.glow_set) {
        auto& gp_cmd  = tail[tail_n++];
        gp_cmd.cmd_id = PMC_CMD_SET_GLOW_PARAMS;
        gp_cmd.arg1   =
              (uint64_t)args.glow_slot
            | ((uint64_t)args.glow_mask     << 8)
            | ((uint64_t)args.glow_filter   << 16)
            | ((uint64_t)args.glow_enabled  << 24)
            | ((uint64_t)args.glow_vistype  << 32)
            | ((uint64_t)args.glow_glowfix  << 40)
            | ((uint64_t)args.glow_wvistype << 48)
            | ((uint64_t)args.glow_wglowfix << 56);
        gp_cmd.arg2   = (uint64_t)args.glow_squad;  // Arg2[7:0] = SquadGlow
        gp_tail_cmd = &gp_cmd;
    }
    if (args.aim_set) {
        auto& ap_cmd  = tail[tail_n++];
        ap_cmd.cmd_id = PMC_CMD_SET_AIM_PARAMS;
        ap_cmd.arg1   =
              (uint64_t)args.aim_trigger_en
            | ((uint64_t)args.aim_aim_en      <<  8)
            | ((uint64_t)args.aim_trig_fov    << 16)
            | ((uint64_t)args.aim_trig_thresh << 24)
            | ((uint64_t)args.aim_debounce    << 32)
            | ((uint64_t)args.aim_fov         << 40);
    }
    if (args.vgui_set) {
        auto& vg_cmd  = tail[tail_n++];
        vg_cmd.cmd_id = PMC_CMD_SET_VGUI_PARAMS;
        vg_cmd.arg1   = PackVguiArg1(args);
        vg_cmd.arg2   = PackVguiArg2(args);
        vg_cmd.arg3   = PackVguiArg3(args);
        vg_tail_cmd = &vg_cmd;
    }

    // Menu controller — armed by default; --menu-disable sends arg1=0.
    {
        auto& m_cmd  = tail[tail_n++];
        m_cmd.cmd_id = PMC_CMD_SET_MENU_ENABLE;
        m_cmd.arg1   = args.menu_disabled ? 0 : 1;
    }

    if (!ch.FireBatch(tail, tail_n)) {
        Log(args, "[FAIL] install-tail FireBatch dispatch (n=%u)\n", tail_n);
        FreeScratch(scratch_buf);
        return 8;
    }

    if (cr3_cmd.status != covert::kStatusOk || cr3_cmd.result == 0) {
        Log(args, "[FAIL] SET_CR3 status=%u result=%llu\n",
            cr3_cmd.status, static_cast<unsigned long long>(cr3_cmd.result));
        FreeScratch(scratch_buf);
        return 8;
    }
    Log(args, "[OK] SET_CR3 cr3=0x%016llX\n",
        static_cast<unsigned long long>(args.target_cr3));

    if (hook_cmd.status != covert::kStatusOk) {
        Log(args, "[FAIL] HOOK_INSTALL_DRAW status=%u\n", hook_cmd.status);
        FreeScratch(scratch_buf);
        return 6;
    }
    Log(args, "[OK] HOOK_INSTALL_DRAW va=0x%llX (base=0x%llX rva=0x%llX) "
              "scratch=0x%llX result=0x%llX\n",
        static_cast<unsigned long long>(hook_va),
        static_cast<unsigned long long>(args.module_base),
        static_cast<unsigned long long>(args.rva),
        static_cast<unsigned long long>(scratch_gva),
        static_cast<unsigned long long>(hook_cmd.result));

    if (peek_cmd.status == covert::kStatusOk) {
        constexpr uint64_t kSentinel = 0x5045584550455845ULL;  // "PEXEPEXE"
        if (peek_cmd.result == kSentinel) {
            Log(args, "[OK] cloak-copy verified — HV[0:8] = sentinel\n");
        } else {
            Log(args, "[WARN] cloak-copy mismatch — HV[0:8] = 0x%016llX "
                      "(expected 0x%016llX)\n",
                static_cast<unsigned long long>(peek_cmd.result),
                static_cast<unsigned long long>(kSentinel));
        }
    } else {
        Log(args, "[WARN] HOOK_DRAW_PEEK status=%u\n", peek_cmd.status);
    }

    if (gp_tail_cmd != nullptr) {
        const covert::Cmd& gp = *gp_tail_cmd;
        Log(args, "GLOW_PARAMS slot=%u mask=0x%02X filter=%u enabled=%u "
                  "vt=%u(w=%u) gf=%u(w=%u) squad=%u packed=0x%016llX/0x%016llX status=%u\n",
            args.glow_slot, args.glow_mask, args.glow_filter, args.glow_enabled,
            args.glow_vistype, args.glow_wvistype,
            args.glow_glowfix, args.glow_wglowfix, args.glow_squad,
            (unsigned long long)gp.arg1, (unsigned long long)gp.arg2, gp.status);
    }
    if (vg_tail_cmd != nullptr) {
        const covert::Cmd& vg = *vg_tail_cmd;
        Log(args, "VGUI_PARAMS en=%u po=%u dry=%u rb=%u fc=%u stable=%u "
                  "maxf=%u rva=0x%08X slots(d=%u,p=%u,c=%u,f=%u) "
                  "txt(x=%u,y=%u,rgba=0x%08X) packed=0x%016llX/0x%016llX/0x%016llX status=%u\n",
            args.vgui_enabled, args.vgui_probe_only, args.vgui_dry_arm,
            args.vgui_rollback, args.vgui_fail_closed, args.vgui_stable_need,
            args.vgui_max_faults, args.vgui_global_rva,
            args.vgui_slot_draw, args.vgui_slot_pos, args.vgui_slot_color, args.vgui_slot_font,
            args.vgui_text_x, args.vgui_text_y, args.vgui_text_rgba,
            (unsigned long long)vg.arg1, (unsigned long long)vg.arg2,
            (unsigned long long)vg.arg3, vg.status);
    }

    // Optional sink override — sentinel-test iteration. Pushed BEFORE the
    // install-confirm RENDER_TEXT so the override is live when the entry
    // dispatches. SET_SINK is an atomic transition: HV restores the prior
    // sink (if any active) using its captured backup VA, drops the backup,
    // then installs the new override.
    if (args.render_sink_set) {
        uint64_t set_sink_result = 0;
        const uint32_t s = ch.Fire(PMC_CMD_RENDER_SET_SINK,
                                   args.render_sink_rva,
                                   (uint64_t)args.render_sink_len,
                                   (uint64_t)args.render_sink_indirect_off,
                                   &set_sink_result);
        std::printf("RENDER_SET_SINK rva=0x%llX len=0x%X indirect_off=0x%X status=%u\n",
                    (unsigned long long)args.render_sink_rva,
                    args.render_sink_len,
                    args.render_sink_indirect_off, s);
        if (s != covert::kStatusOk) {
            FreeScratch(scratch_buf);
            return 60;
        }
    }

    // Install-confirm render push — fires after a fully successful install.
    // Default text is "HV INSTALLED"; --sentinel "TEXT" overrides for
    // sentinel-test iteration. Writes the chosen text into the last 64
    // bytes of the VirtualLock'd scratch (scratch is still alive here).
    // HV reads the string synchronously during the NPF; buffer can be
    // freed immediately after Fire() returns.
    // --no-install-confirm skips this (useful for scripted / repeated installs).
    if (!args.no_install_confirm) {
        constexpr uint32_t kConfirmOff = kScratchBytes - 64;
        const char* msg = args.sentinel_text ? args.sentinel_text : "HV INSTALLED";
        // Bound copy at 63 chars + NUL (scratch slot is 64 bytes; HV caps at
        // RENDER_TEXT_MAX=63 anyway).
        std::memset(static_cast<char*>(scratch_buf) + kConfirmOff, 0, 64);
        std::strncpy(static_cast<char*>(scratch_buf) + kConfirmOff, msg, 63);

        // 300 payload ticks ≈ 5 s at 60 fps (one tick per render-hook NPF).
        // No flags by default — convar-value sinks ARE the rendered text;
        // the legacy FORCE_FPS_CONVAR flag only applies to canon FPS sink.
        const uint64_t render_arg1 = MakeRenderArg1(300, 0, 0, 0, 0);
        const uint64_t string_gva  = scratch_gva + kConfirmOff;
        uint64_t confirm_result = 0;
        const uint32_t confirm_s = ch.Fire(PMC_CMD_RENDER_TEXT,
                                           render_arg1, string_gva, 0,
                                           &confirm_result);
        if (confirm_s == covert::kStatusOk && confirm_result != 0) {
            std::printf("[OK] RENDER_TEXT queued (300 ticks, msg=\"%s\")"
                        " — watch RDQ/RDH in drain%s\n",
                        msg,
                        args.render_sink_set
                            ? " (sentinel-test on overridden sink)"
                            : "");
        } else {
            std::printf("[WARN] RENDER_TEXT status=%u result=%llu\n",
                        confirm_s,
                        static_cast<unsigned long long>(confirm_result));
        }
    }

    FreeScratch(scratch_buf);
    return 0;
}

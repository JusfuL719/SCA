// HookDraw.c — HV-side draw emitter at apex.text+0x26B73D (rsi=cmd-list).

#include <stdint.h>

// D3D12 vtable indices

#define VT_GCL_CLOSE                   9
#define VT_GCL_RESET                  10
#define VT_GCL_CLEARSTATE             11
#define VT_GCL_DRAWINSTANCED          12
#define VT_GCL_DRAWINDEXEDINSTANCED   13
#define VT_GCL_DISPATCH               14
#define VT_GCL_IASETPRIMITIVETOPOLOGY 20
#define VT_GCL_RSSETVIEWPORTS         21
#define VT_GCL_RSSETSCISSORRECTS      22
#define VT_GCL_OMSETSTENCILREF        24
#define VT_GCL_SETPIPELINESTATE       25
#define VT_GCL_RESOURCEBARRIER        26
#define VT_GCL_SETDESCRIPTORHEAPS     28
#define VT_GCL_SETGRAPHICSROOTSIG     30
#define VT_GCL_SETGRAPHICSROOTDT      32
#define VT_GCL_SETGRAPHICSROOTCBV     38
#define VT_GCL_IASETINDEXBUFFER       43
#define VT_GCL_IASETVERTEXBUFFERS     44
#define VT_GCL_OMSETRENDERTARGETS     46

// minimal D3D12 types
typedef struct ID3D12Vtbl ID3D12Vtbl;
typedef struct { ID3D12Vtbl *vtbl; } ID3D12Obj;
typedef ID3D12Obj ID3D12GraphicsCommandList;
typedef ID3D12Obj ID3D12PipelineState;
typedef ID3D12Obj ID3D12RootSignature;
typedef ID3D12Obj ID3D12Resource;
typedef ID3D12Obj ID3D12DescriptorHeap;

typedef struct {
    uint64_t BufferLocation;
    uint32_t SizeInBytes;
    uint32_t StrideInBytes;
} D3D12_VERTEX_BUFFER_VIEW;

typedef struct {
    uint64_t BufferLocation;
    uint32_t SizeInBytes;
    uint32_t Format;
} D3D12_INDEX_BUFFER_VIEW;

typedef struct {
    float TopLeftX, TopLeftY;
    float Width, Height;
    float MinDepth, MaxDepth;
} D3D12_VIEWPORT;

typedef struct { int32_t L, T, R, B; } D3D12_RECT;

// HV state — created once by initializer, read-only here.
extern ID3D12PipelineState   *g_hud_pso;
extern ID3D12RootSignature   *g_hud_rootsig;
extern ID3D12DescriptorHeap  *g_hud_srvheap;
extern D3D12_VERTEX_BUFFER_VIEW g_hud_vbv;
extern D3D12_INDEX_BUFFER_VIEW  g_hud_ibv;
extern uint64_t g_srv_gpu_handle;

typedef struct {
    uint32_t index_count;
    uint32_t start_index;
    int32_t  base_vertex;
    uint32_t color_rgba;
} HudBatch;

#define HUD_MAX_BATCHES 256
extern HudBatch  g_hud_batches[HUD_MAX_BATCHES];
extern volatile uint32_t g_hud_batch_count;

extern uint32_t g_screen_w;
extern uint32_t g_screen_h;

// vtable call helpers (ms_abi for cross-compile from Linux)

#if defined(__GNUC__) && !defined(_MSC_VER)
# define WINABI __attribute__((ms_abi))
#else
# define WINABI
#endif

static inline void *vtbl_slot(ID3D12Obj *o, unsigned idx) {
    void **vt = *(void ***)o;
    return vt[idx];
}

typedef void WINABI (*pfn_void_thiscall)(ID3D12Obj *);
typedef void WINABI (*pfn_setpso)(ID3D12Obj *, ID3D12PipelineState *);
typedef void WINABI (*pfn_setrootsig)(ID3D12Obj *, ID3D12RootSignature *);
typedef void WINABI (*pfn_setdescheaps)(ID3D12Obj *, uint32_t, ID3D12DescriptorHeap **);
typedef void WINABI (*pfn_setroot_descriptor_table)(ID3D12Obj *, uint32_t, uint64_t);
typedef void WINABI (*pfn_setroot_32bit_const)(ID3D12Obj *, uint32_t, uint32_t, uint32_t);
typedef void WINABI (*pfn_iatopo)(ID3D12Obj *, uint32_t);
typedef void WINABI (*pfn_rsvp)(ID3D12Obj *, uint32_t, const D3D12_VIEWPORT *);
typedef void WINABI (*pfn_rsscissor)(ID3D12Obj *, uint32_t, const D3D12_RECT *);
typedef void WINABI (*pfn_iaibv)(ID3D12Obj *, const D3D12_INDEX_BUFFER_VIEW *);
typedef void WINABI (*pfn_iavbv)(ID3D12Obj *, uint32_t, uint32_t, const D3D12_VERTEX_BUFFER_VIEW *);
typedef void WINABI (*pfn_drawidx)(ID3D12Obj *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);

// HudDraw_Emit — INT3 vmexit reads guest rsi, calls here, advances past stamp.

WINABI void
HudDraw_Emit(ID3D12GraphicsCommandList *cmd)
{
    if (!cmd)                              return;
    if (g_hud_batch_count == 0)            return;
    if (!g_hud_pso || !g_hud_rootsig)      return;

    // Snapshot batch count once; producer keeps writing.
    uint32_t n = g_hud_batch_count;
    if (n > HUD_MAX_BATCHES) n = HUD_MAX_BATCHES;

    // Apex's next state-setter clobbers our bindings before its next draw.
    ((pfn_setpso)     vtbl_slot(cmd, VT_GCL_SETPIPELINESTATE))   (cmd, g_hud_pso);
    ((pfn_setrootsig) vtbl_slot(cmd, VT_GCL_SETGRAPHICSROOTSIG)) (cmd, g_hud_rootsig);

    {
        ID3D12DescriptorHeap *heaps[1] = { g_hud_srvheap };
        ((pfn_setdescheaps) vtbl_slot(cmd, VT_GCL_SETDESCRIPTORHEAPS)) (cmd, 1, heaps);
        ((pfn_setroot_descriptor_table) vtbl_slot(cmd, VT_GCL_SETGRAPHICSROOTDT))
            (cmd, 0, g_srv_gpu_handle);
    }

    {
        D3D12_VIEWPORT vp = {
            .TopLeftX = 0.0f, .TopLeftY = 0.0f,
            .Width    = (float)g_screen_w,
            .Height   = (float)g_screen_h,
            .MinDepth = 0.0f, .MaxDepth = 1.0f,
        };
        D3D12_RECT sr = {
            .L = 0, .T = 0,
            .R = (int32_t)g_screen_w, .B = (int32_t)g_screen_h,
        };
        ((pfn_rsvp)      vtbl_slot(cmd, VT_GCL_RSSETVIEWPORTS))   (cmd, 1, &vp);
        ((pfn_rsscissor) vtbl_slot(cmd, VT_GCL_RSSETSCISSORRECTS))(cmd, 1, &sr);
    }

    ((pfn_iatopo) vtbl_slot(cmd, VT_GCL_IASETPRIMITIVETOPOLOGY)) (cmd, 4u);
    ((pfn_iaibv)  vtbl_slot(cmd, VT_GCL_IASETINDEXBUFFER))      (cmd, &g_hud_ibv);
    ((pfn_iavbv)  vtbl_slot(cmd, VT_GCL_IASETVERTEXBUFFERS))    (cmd, 0, 1, &g_hud_vbv);

    {
        pfn_setroot_32bit_const set_const =
            (pfn_setroot_32bit_const) vtbl_slot(cmd, 34);
        pfn_drawidx draw =
            (pfn_drawidx) vtbl_slot(cmd, VT_GCL_DRAWINDEXEDINSTANCED);

        for (uint32_t i = 0; i < n; ++i) {
            HudBatch b = g_hud_batches[i];
            if (b.index_count == 0)              continue;
            if (b.index_count > 0x10000)         continue;

            set_const(cmd, 1, b.color_rgba, 0);
            draw(cmd, b.index_count, 1, b.start_index, b.base_vertex, 0);
        }
    }

    // Apex's per-frame fn calls Close later at apex.text+0x26B87D.
}

// Trampoline: shim loads rcx from guest rsi, advances past int3 on return.

WINABI void
HookDraw_Trampoline(ID3D12GraphicsCommandList *cmd)
{
    HudDraw_Emit(cmd);
}

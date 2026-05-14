

#ifndef HYPE_SVM_H
#define HYPE_SVM_H

#include "HypeUefi.h"
#include "HypeVmcb.h"

struct _VCPU_DATA;
typedef struct _VCPU_DATA VCPU_DATA, *PVCPU_DATA;
struct _NPT_CONTEXT;
typedef struct _NPT_CONTEXT NPT_CONTEXT, *PNPT_CONTEXT;

#define MSR_EFER            0xC0000080
#define MSR_IA32_APIC_BASE  0x0000001B
#define MSR_VM_CR           0xC0010114
#define MSR_VM_HSAVE_PA     0xC0010117

#define PMC_CMD_PING            0x00
#define PMC_CMD_SET_CR3         0x02
#define PMC_CMD_SET_EPROCESS    0x0A
#define PMC_CMD_VIRT_READ4      0x30
#define PMC_CMD_VIRT_READ8      0x31
#define PMC_CMD_VIRT_WRITE4     0x32
#define PMC_CMD_VIRT_WRITE8     0x33
#define PMC_CMD_PROC_CR3        0x34
#define PMC_CMD_VIRT_WRITE1     0x35
#define PMC_CMD_CR3_INTERCEPT   0x36
#define PMC_CMD_GET_INTERCEPT_PEB 0x37
#define PMC_CMD_GET_LAPIC_DISARMED_NPF_COUNT 0x38

#define PMC_CMD_PHYS_READ       0x41

#define PMC_CMD_HOOK_INSTALL_DRAW 0x1B
#define PMC_CMD_HOOK_DRAW_PEEK    0x1C
#define PMC_CMD_SET_GLOW_PARAMS   0x1D
#define PMC_CMD_HV_LOG_READ       0x1E
// VIRT_CALL flow: INIT(trampoline_va) one-shot; then CALL(fn_va, rcx=Arg2, rdx=Arg3).
// CALL must be LAST in batch — guest runs fn_va, RETs to trampoline, NPF restores state.
#define PMC_CMD_VIRT_CALL_INIT    0x1F
#define PMC_CMD_VIRT_CALL         0x20
#define PMC_CMD_SET_AIM_PARAMS    0x21
#define PMC_CMD_SET_MENU_ENABLE   0x22

#define PMC_CMD_NPT_CHANNEL_INIT 0x50

#define PMC_CMD_HANDSHAKE        0x60

#define HYPE_BUILD_SECRET  0x7A3F9B2E5D1C8064ULL

#define EPROCESS_PEB_OFFSET     0x2E0
#define EPROCESS_PID_OFFSET     0x1D0
#define EPROCESS_DTB_OFFSET     0x028

#define KPCR_PRCB_OFFSET        0x180
#define KPRCB_CURRENTTHREAD     0x008
#define KPRCB_IDLETHREAD        0x018
#define KTHREAD_PROCESS_OFFSET  0x220
#define KTHREAD_SCAN_START      0x090
#define KTHREAD_SCAN_END        0x300
#define SYSTEM_PID              4

#define APIC_BASE_X2APIC_ENABLE  (1ULL << 10)

#define EFER_SVME           (1ULL << 12)
#define VM_CR_R_INIT        (1ULL << 1)

#define LAPIC_ICR_LOW_OFFSET   0x300
#define LAPIC_ICR_HIGH_OFFSET  0x310
#define LAPIC_EOI_OFFSET       0x0B0
#define LAPIC_DELIVERY_INIT    5
#define LAPIC_DELIVERY_STARTUP 6

typedef struct _VCPU_DATA {
    PVMCB           Vmcb;
    UINT64          VmcbPhysical;

    VOID            *HostSaveArea;
    UINT64          HostSavePhysical;

    VOID            *HostStack;
    UINT64          HostStackSize;

    UINT64          GuestRbx;
    UINT64          GuestRcx;
    UINT64          GuestRdx;
    UINT64          GuestRsi;
    UINT64          GuestRdi;
    UINT64          GuestRbp;
    UINT64          GuestR8;
    UINT64          GuestR9;
    UINT64          GuestR10;
    UINT64          GuestR11;
    UINT64          GuestR12;
    UINT64          GuestR13;
    UINT64          GuestR14;
    UINT64          GuestR15;

    UINT32          CpuNumber;
    UINT32          Launched;

    VOID            *XSaveArea;
    UINT64          XSavePhysical;
    UINT32          XSaveSize;
    UINT32          Reserved1;
    UINT64          XSaveMask;

    UINT8           *Msrpm;
    UINT64          MsrpmPhysical;
    UINT8           *Iopm;
    UINT64          IopmPhysical;

    UINT64          *NptPml4;
    UINT64          NptPml4Physical;

    PVCPU_DATA      Self;
    struct _HYPERVISOR_STATE *HvState;

    // Replay defense — UINT64 to preserve retired _Reserved_TscPad0 slot.
    volatile UINT64 LastCovertSequence;
    UINT64          _Reserved_TscPad1;

    volatile INT32  ShouldExit;

    UINT64          OriginalEfer;

    UINT64          Reserved_Staging[4];
    UINT64          TargetCr3;
    UINT64          _Reserved_SharedPage[2];

    UINT64          SystemEprocessVa;
    UINT64          _Reserved_TargetPebVa;

    volatile UINT64 RingExitCode[8];
    volatile UINT64 RingRip[8];
    volatile UINT64 RingIndex;
    volatile UINT64 VmexitCount;
    volatile UINT64 StateMarker;
    volatile UINT64 LastExitInfo1;
    volatile UINT64 LastExitInfo2;
    volatile UINT64 LastRax;

    UINT64          VmcbCanaryValue;
    volatile UINT64 VmcbCanaryCorruptedAt;

    UINT64          _Reserved_TscPad2;
    UINT64          _Reserved_TscPad3;

    UINT64          _Reserved_AperfMperf[4];

    UINT64          _Reserved_P2_A;
    UINT64          _Reserved_P2_B;

    UINT64          KernelCr3;

    UINT64          _Reserved_NmiSlot;

    volatile UINT64 CovertSavedRflags;
    volatile UINT32 CovertTriggerActive;
    UINT32          Reserved5;

    volatile UINT32 Authenticated;
    UINT32          LastCf8Value;

    #define SOFT_TLB_ENTRIES    16
    #define SOFT_TLB_INDEX(va)  (((va) >> 12) & (SOFT_TLB_ENTRIES - 1))
    UINT64          TlbCr3;
    UINT64          TlbVaPage[SOFT_TLB_ENTRIES];
    UINT64          TlbPaPage[SOFT_TLB_ENTRIES];

    UINT32          ApicId;
    volatile UINT32 ActivityState;
    volatile UINT8  SipiVector;
    volatile UINT8  LastNpfBranch;
    volatile UINT8  HeartbeatLogged;
    volatile UINT8  SipiApplied;
    volatile UINT8  LastCovertCmdId;
    volatile UINT8  LastCovertCmdSlot;
    volatile UINT8  LastCovertNumCmds;
    UINT8           _pad_p3_tail0;

    UINT64          NonMsrRingExitCode[16];
    UINT64          NonMsrRingRip[16];
    UINT64          NonMsrRingExitInfo2[16];
    UINT32          NonMsrRingIndex;
    UINT32          NonMsrRingCount;

    volatile UINT64 LastUnhandledNpfGpa;
    volatile UINT32 UnhandledNpfRepeats;
    volatile UINT32 UnhandledNpfTotal;

    volatile UINT64 Cr3InterceptCapturedPeb;
    volatile UINT64 Cr3InterceptCapturedImageBase;

    // Draw-hook NPT exec-trap state. SavedRflags preserves guest TF across multi-step.
    volatile UINT32 DrawHookActive;
    UINT32          _Pad_DrawHook;
    volatile UINT64 DrawHookSavedRflags;
    volatile UINT64 FrameCount;

    // VIRT_CALL state — active from RIP-redirect until trampoline-return NPF.
    volatile UINT32 VirtCallActive;
    UINT32          _Pad_VirtCall0;
    volatile UINT64 VirtCallSavedRip;
    volatile UINT64 VirtCallSavedRsp;
    volatile UINT64 VirtCallSavedRax;
    volatile UINT64 VirtCallSavedRbx;
    volatile UINT64 VirtCallSavedRcx;
    volatile UINT64 VirtCallSavedRdx;
    volatile UINT64 VirtCallSavedR8;
    volatile UINT64 VirtCallSavedR9;
    volatile UINT64 VirtCallSavedRflags;
    // Mailbox VA of COVERT_CMD.Result for trampoline-return RAX stash.
    volatile UINT64 VirtCallResultVa;

} VCPU_DATA, *PVCPU_DATA;

#define _PEX_PIN(field, expected) \
    _Static_assert(__builtin_offsetof(VCPU_DATA, field) == (expected), \
                   "VCPU_DATA." #field " moved — update matching EQU in HypeVmrun.nasm / HypeIsr.nasm")

_PEX_PIN(Vmcb,             0x000);
_PEX_PIN(VmcbPhysical,     0x008);
_PEX_PIN(HostSaveArea,     0x010);
_PEX_PIN(HostSavePhysical, 0x018);
_PEX_PIN(HostStack,        0x020);
_PEX_PIN(HostStackSize,    0x028);
_PEX_PIN(GuestRbx,         0x030);
_PEX_PIN(GuestRcx,         0x038);
_PEX_PIN(GuestRdx,         0x040);
_PEX_PIN(GuestRsi,         0x048);
_PEX_PIN(GuestRdi,         0x050);
_PEX_PIN(GuestRbp,         0x058);
_PEX_PIN(GuestR8,          0x060);
_PEX_PIN(GuestR9,          0x068);
_PEX_PIN(GuestR10,         0x070);
_PEX_PIN(GuestR11,         0x078);
_PEX_PIN(GuestR12,         0x080);
_PEX_PIN(GuestR13,         0x088);
_PEX_PIN(GuestR14,         0x090);
_PEX_PIN(GuestR15,         0x098);
_PEX_PIN(XSaveArea,        0x0A8);
_PEX_PIN(XSaveSize,        0x0B8);
_PEX_PIN(XSaveMask,        0x0C0);
_PEX_PIN(ShouldExit,       0x118);
_PEX_PIN(RingExitCode,     0x170);
_PEX_PIN(RingRip,          0x1B0);
_PEX_PIN(RingIndex,        0x1F0);
_PEX_PIN(VmexitCount,      0x1F8);
_PEX_PIN(StateMarker,      0x200);
_PEX_PIN(_Reserved_TscPad3, 0x238);

#undef _PEX_PIN

static inline __attribute__((always_inline))
VOID
InvalidateSoftTlb(
    PVCPU_DATA  V
    )
{
    V->TlbCr3 = 0;
    for (UINT32 i = 0; i < SOFT_TLB_ENTRIES; i++) {
        V->TlbVaPage[i] = 0;
    }
}

typedef enum _GUEST_ACTIVITY {
    GUEST_ACTIVE       = 0,
    GUEST_WFS          = 1,
    GUEST_SIPI_ISSUED  = 2
} GUEST_ACTIVITY;

#define COVERT_MAX_BATCH    80

#pragma pack(push, 1)

typedef struct _COVERT_CMD {
    UINT32  CmdId;
    UINT32  Status;
    UINT64  Arg1;
    UINT64  Arg2;
    UINT64  Arg3;
    UINT64  Result;
    UINT64  Reserved;
} COVERT_CMD;

typedef struct _COVERT_MAILBOX {
    UINT64      AuthKey;
    UINT32      NumCommands;
    UINT32      Sequence;
    COVERT_CMD  Cmd[COVERT_MAX_BATCH];
} COVERT_MAILBOX;

#pragma pack(pop)

#define COVERT_STATUS_OK        0
#define COVERT_STATUS_ERR       1
#define COVERT_STATUS_BAD_CMD   2
#define COVERT_STATUS_BAD_ADDR  3
#define COVERT_STATUS_BAD_AUTH  4
#define COVERT_STATUS_NO_SPARE  5

typedef struct _HYPERVISOR_STATE {
    PVCPU_DATA      VcpuTable;
    UINT32          NumCpus;
    UINT32          MaxProcessorId;
    UINT32          ProcessorIndexMap[256];

    UINT8           *SharedMsrpm;
    UINT64          SharedMsrpmPhysical;
    UINT8           *SharedIopm;
    UINT64          SharedIopmPhysical;

    volatile INT32  Active;
    volatile INT32  InitComplete;

    volatile INT32  ApsInVmrun;

    UINT64          HostCr3Physical;

    EFI_PHYSICAL_ADDRESS VcpuBulkBlock;
    UINTN               VcpuBulkBlockPages;

} HYPERVISOR_STATE, *PHYPERVISOR_STATE;

extern UINT64           gLapicGpa;
extern volatile UINT8   *gHostLapicVa;
extern volatile INT32   gRemainingSipiCount;
extern volatile UINT8   gLapicInterceptArmed;

NTSTATUS InstallLapicNptShadow(PNPT_CONTEXT NptCtx);
VOID     DisableLapicIntercept(VOID);

BOOLEAN SvmCheckSupport(VOID);
BOOLEAN SvmCheckNripsSupport(VOID);
BOOLEAN SvmCheckLbrvSupport(VOID);

NTSTATUS SvmEnableOnCpu(PVCPU_DATA Vcpu);

NTSTATUS VmcbInitialize(PVCPU_DATA Vcpu);
VOID VmcbSetupIntercepts(PVMCB Vmcb);
VOID VmcbSetupMsrpm(UINT8 *Msrpm);
VOID VmcbSetupIopm(UINT8 *Iopm);
VOID VmcbCaptureGuestState(PVMCB Vmcb, PVCPU_DATA Vcpu);

VOID VmexitHandler(PVCPU_DATA Vcpu);

extern VOID SvmLaunch(PVCPU_DATA Vcpu);

#define HOST_STACK_SIZE     (32 * 1024)
#define MSRPM_SIZE          (8 * 1024)
#define IOPM_SIZE           (12 * 1024)

#define VMCB_CANARY_OFFSET_CTL  0x2E0
#define VMCB_CANARY_OFFSET_SAVE 0x6F0

#endif

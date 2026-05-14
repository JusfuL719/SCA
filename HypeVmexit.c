#include "HypeContext.h"
#include "HypeVmcb.h"
#include "HypeMemory.h"
#include "HypeDebug.h"
#include "HypeHookDraw.h"
#include "HypeMenu.h"

extern DRIVER_CONTEXT g_DriverContext;
extern UINT64 gHypeLoaderImageBase;
extern UINT64 gHypeLoaderImageSize;
extern UINT64 gHandshakeExpected;

volatile UINT64 gCovertTriggerGpa = 0;
volatile UINT64 gCovertMailboxGpa = 0;
volatile UINT64 gCovertClientCr3  = 0;
volatile UINT64 gCovertMailboxVa  = 0;

// VIRT_CALL trampoline page — installer-allocated 4 KB scratch in guest VA
// space, NPT-pinned NX after PMC_CMD_VIRT_CALL_INIT. RET from a redirected
// guest fn lands here, NPF fires (exec on NX page), HandleNpf detects via
// VirtCallActive flag and restores saved state.
volatile UINT64 gVirtCallTrampolineGpa = 0;
volatile UINT64 gVirtCallTrampolineVa  = 0;

volatile UINT32 gCr3SampleArmed     = 0;
volatile UINT32 gCr3SamplePid       = 0;
volatile UINT64 gCr3SamplePebVa     = 0;
volatile UINT64 gCr3SampleImageBase = 0;
volatile UINT64 gCr3SampleCursor    = 0;

// 64MB chunk took 5.7ms per VMEXIT — caused 0x101 IPI-stall under sustained
// installer polling (one VMEXIT per 50ms with IF=0 for 5.7ms = 11% black-out).
// 4MB chunk = ~356us per VMEXIT — under one Win11 clock tick (1ms).
#define CR3_SCAN_CHUNK_BYTES  (4ULL * 1024 * 1024)

// === SECTION: GLOBALS+HELPERS ===
static inline __attribute__((always_inline))
VOID
FlushAllTlb(
    PVCPU_DATA Vcpu
    )
{
    Vcpu->Vmcb->Control.TlbControl = 1;
    UINT32 N = g_DriverContext.HvState.NumCpus;
    if (N > 1) {
        for (UINT32 i = 0; i < N; i++) {
            PVCPU_DATA Other = &g_DriverContext.HvState.VcpuTable[i];
            if (Other != Vcpu && Other->Vmcb) {
                Other->Vmcb->Control.TlbControl = 1;
            }
        }
    }
}

static inline __attribute__((always_inline))
VOID
CovertChannelTeardown(
    PVCPU_DATA  Vcpu,
    PVMCB       Vmcb,
    UINT64      TriggerGpa
    )
{
    UINT64 *Pte = NptGetPte(&g_DriverContext.NptContext, TriggerGpa, FALSE);
    if (Pte && !(*Pte & NPT_LARGE_PAGE)) {
        *Pte |= NPT_WRITE;
    }

    UINT64 OldGpa = __atomic_exchange_n(&gCovertTriggerGpa, (UINT64)0, __ATOMIC_ACQ_REL);
    if (OldGpa != 0) {
        HvLogHex("VC1", OldGpa);
    }
    gCovertMailboxGpa = 0;
    gCovertMailboxVa  = 0;
    gCovertClientCr3  = 0;
    Vcpu->CovertTriggerActive = 0;
    FlushAllTlb(Vcpu);
}

STATIC
VOID
InjectException(
    PVMCB   Vmcb,
    UINT8   Vector,
    BOOLEAN HasErrorCode,
    UINT32  ErrorCode
    )
{

    if (Vmcb->Control.EventInj & EVENT_INJ_VALID)

        return;

    Vmcb->Control.EventInj = EVENT_INJ_VALID | EVENT_INJ_TYPE_EXCEPT | Vector |
                             (HasErrorCode ? (EVENT_INJ_EV | ((UINT64)ErrorCode << 32)) : 0);
}

STATIC VOID HandleLapicIcrWrite(PVCPU_DATA Vcpu, UINT64 FaultGpa);
STATIC VOID PassThroughLapicMmio(PVCPU_DATA Vcpu, UINT64 FaultGpa);
STATIC PVCPU_DATA FindVcpuByApicId(UINT8 ApicId);
STATIC BOOLEAN DecodeLapicMmioAccess(PVCPU_DATA Vcpu, BOOLEAN *OutIsWrite,
    UINT8 *OutReg, UINT32 *OutInstrLen, BOOLEAN *OutIsImm, UINT32 *OutImmVal);
STATIC VOID DispatchIpi(PVCPU_DATA Self, UINT32 Mode, UINT32 IcrLow, UINT8 DestApicId);

#define LAPIC_SHORTHAND_NONE      0
#define LAPIC_SHORTHAND_SELF      1
#define LAPIC_SHORTHAND_ALL_INCL  2
#define LAPIC_SHORTHAND_ALL_EXCL  3

STATIC
UINT64
ReadGuestGpr(
    PVCPU_DATA Vcpu,
    UINT8      Idx
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;
    switch (Idx & 0xF) {
        case 0:  return Vmcb->Save.Rax;
        case 1:  return Vcpu->GuestRcx;
        case 2:  return Vcpu->GuestRdx;
        case 3:  return Vcpu->GuestRbx;
        case 4:  return Vmcb->Save.Rsp;
        case 5:  return Vcpu->GuestRbp;
        case 6:  return Vcpu->GuestRsi;
        case 7:  return Vcpu->GuestRdi;
        case 8:  return Vcpu->GuestR8;
        case 9:  return Vcpu->GuestR9;
        case 10: return Vcpu->GuestR10;
        case 11: return Vcpu->GuestR11;
        case 12: return Vcpu->GuestR12;
        case 13: return Vcpu->GuestR13;
        case 14: return Vcpu->GuestR14;
    }
    return Vcpu->GuestR15;
}

STATIC
VOID
WriteGuestGpr32Zx(
    PVCPU_DATA Vcpu,
    UINT8      Idx,
    UINT32     Val32
    )
{
    PVMCB  Vmcb  = Vcpu->Vmcb;
    UINT64 Val64 = (UINT64)Val32;
    switch (Idx & 0xF) {
        case 0:  Vmcb->Save.Rax = Val64; break;
        case 1:  Vcpu->GuestRcx = Val64; break;
        case 2:  Vcpu->GuestRdx = Val64; break;
        case 3:  Vcpu->GuestRbx = Val64; break;
        case 4:  Vmcb->Save.Rsp = Val64; break;
        case 5:  Vcpu->GuestRbp = Val64; break;
        case 6:  Vcpu->GuestRsi = Val64; break;
        case 7:  Vcpu->GuestRdi = Val64; break;
        case 8:  Vcpu->GuestR8  = Val64; break;
        case 9:  Vcpu->GuestR9  = Val64; break;
        case 10: Vcpu->GuestR10 = Val64; break;
        case 11: Vcpu->GuestR11 = Val64; break;
        case 12: Vcpu->GuestR12 = Val64; break;
        case 13: Vcpu->GuestR13 = Val64; break;
        case 14: Vcpu->GuestR14 = Val64; break;
        case 15: Vcpu->GuestR15 = Val64; break;
    }
}

STATIC
BOOLEAN
MsrIsInValidRange(UINT32 Msr)
{
    if (Msr <= 0x00001FFF) return TRUE;
    if (Msr >= 0xC0000000 && Msr <= 0xC0002FFF) return TRUE;
    if (Msr >= 0xC0010000 && Msr <= 0xC0011FFF) return TRUE;
    return FALSE;
}

// === SECTION: MSR ===
STATIC volatile UINT32 gMsrTraceCount = 0;
#define MSR_TRACE_LIMIT 32

STATIC
VOID
HandleMsr(
    PVCPU_DATA Vcpu
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;
    UINT32 Msr = (UINT32)Vcpu->GuestRcx;
    BOOLEAN Write = (Vmcb->Control.ExitInfo1 & 1) != 0;

    if (gMsrTraceCount < MSR_TRACE_LIMIT && !(Msr == MSR_EFER && !Write) &&
        Msr != 0x6E0 && Msr != 0x80B) {
        HvLogHex(Write ? "VAW" : "VAR", (UINT64)Msr);
        HvLogHex(Write ? "VBW" : "VBR", Vmcb->Save.Rip);
        __sync_add_and_fetch((volatile INT32 *)&gMsrTraceCount, 1);
    }

    if (Write) {
        UINT64 Value = (Vcpu->GuestRdx << 32) | (Vmcb->Save.Rax & 0xFFFFFFFF);

        switch (Msr) {
        case MSR_EFER: {

            #define EFER_VALID_MASK ( \
                (1ULL << 0)  |  \
                (1ULL << 8)  |  \
                (1ULL << 10) |  \
                (1ULL << 11) |  \
                EFER_SVME    |  \
                (1ULL << 13) |  \
                (1ULL << 14) |  \
                (1ULL << 15) |  \
                (1ULL << 17) |  \
                (1ULL << 18))
            if (Value & ~EFER_VALID_MASK) {
                HvLog("VGA\n"); HvLogHex("VGARIP", Vmcb->Save.Rip); HvLogHex("VGAMSR", Msr);
                InjectException(Vmcb, EXCEPTION_GP, TRUE, 0);
                return;
            }

            if ((Value & (1ULL << 8)) && (Vmcb->Save.Cr0 & (1ULL << 31))) {
                Value |= (1ULL << 10);
            } else {
                Value &= ~(1ULL << 10);
            }

            if (!(Value & (1ULL << 8)) && (Vmcb->Save.Cr0 & (1ULL << 31))) {
                HvLog("VGB\n"); HvLogHex("VGBRIP", Vmcb->Save.Rip); HvLogHex("VGBMSR", Msr);
                InjectException(Vmcb, EXCEPTION_GP, TRUE, 0);
                return;
            }
            if ((Value & (1ULL << 8)) && !(Vmcb->Save.Cr4 & (1ULL << 5))) {
                HvLog("VGC\n"); HvLogHex("VGCRIP", Vmcb->Save.Rip); HvLogHex("VGCMSR", Msr);
                InjectException(Vmcb, EXCEPTION_GP, TRUE, 0);
                return;
            }

            if (Value & EFER_SVME) {
                static volatile UINT32 gEferSvmeStripCount = 0;
                if (gEferSvmeStripCount < 4) {
                    HvLogHex("VA1", Value);
                    __sync_add_and_fetch((volatile INT32 *)&gEferSvmeStripCount, 1);
                }
                Value &= ~EFER_SVME;
            }

            Vcpu->OriginalEfer = Value & ~EFER_SVME;
            Vmcb->Save.Efer = Value | EFER_SVME;
            break;
        }

        case MSR_VM_CR:
        case MSR_VM_HSAVE_PA:
            break;

        case MSR_IA32_APIC_BASE: {

            if (Value & APIC_BASE_X2APIC_ENABLE) {
                HvLog("V85\n");
                if (gLapicInterceptArmed) {
                    HvLog("VGD\n"); HvLogHex("VGDRIP", Vmcb->Save.Rip); HvLogHex("VGDMSR", Msr);
                    InjectException(Vmcb, EXCEPTION_GP, TRUE, 0);
                    return;
                }
            }
            __writemsr(Msr, Value);
            break;
        }

        case 0x6E0: {
            // Cycle 13.1: per-CPU one-shot — proves WRMSR(0x6E0) intercept is firing.
            if ((Vcpu->HeartbeatLogged & 0x10) == 0) {
                Vcpu->HeartbeatLogged |= 0x10;
                HvLogHex("M0D", ((UINT64)Vcpu->CpuNumber << 56) | (Value & 0xFFFFFFFFFFFFFFULL));
            }
            // Cycle 13: shorten guest TSC_DEADLINE to bound heartbeat ceiling.
            // ~0x4000000 TSC ticks ≈ 20ms @ 3.4 GHz nominal (~17ms @ 4.0 GHz boost).
            // Pass through writes shorter than cap (don't lengthen guest scheduler).
            // Skip Value==0 (guest masking timer — leave masked).
            UINT64 Now = __rdtsc();
            UINT64 Cap = Now + 0x4000000ULL;
            UINT64 Chosen = (Value != 0 && Value > Cap) ? Cap : Value;
            __writemsr(0x6E0, Chosen);
            // M1L: late-fire — gap between successive deadline writes > ~40ms
            static volatile UINT64 gLastDeadlineTsc = 0;
            UINT64 Last = __atomic_load_n(&gLastDeadlineTsc, __ATOMIC_RELAXED);
            if (Last != 0 && (Now - Last) > 0x8000000ULL) {
                HvLogHex("M1L", Now - Last);
            }
            __atomic_store_n(&gLastDeadlineTsc, Now, __ATOMIC_RELAXED);
            break;
        }

        case 0x80B: {
            // Cycle 13.1: per-CPU one-shot — proves WRMSR(0x80B) intercept is firing.
            if ((Vcpu->HeartbeatLogged & 0x08) == 0) {
                Vcpu->HeartbeatLogged |= 0x08;
                HvLogHex("M0E", (UINT64)Vcpu->CpuNumber);
            }
            // Cycle 13: x2APIC EOI heartbeat.
            // DrawHookRearm() already fired at VmexitHandler prologue (HypeVmexit.c:2030).
            // We just pass through. EOI register treats writes as ack — Value ignored.
            __writemsr(0x80B, 0);
            // M2E: EOI counter — sampled every 64k writes per HV boot
            static volatile INT32 gEoiCount = 0;
            UINT32 Cnt = (UINT32)__sync_add_and_fetch(&gEoiCount, 1);
            if ((Cnt & 0xFFFF) == 0) {
                HvLogHex("M2E", (UINT64)Cnt);
            }
            break;
        }

        case 0x830: {
            if (!gLapicInterceptArmed) {
                HvLogHex("V9D", Vmcb->Save.Rip);  // post-disarm 0x830 — MSRPM stale
            }
            UINT32 IcrLow = (UINT32)Value;
            UINT32 Mode   = (IcrLow >> 8) & 7;
            UINT32 DestId = (UINT32)(Value >> 32);

            if (Mode == LAPIC_DELIVERY_INIT || Mode == LAPIC_DELIVERY_STARTUP) {
                DispatchIpi(Vcpu, Mode, IcrLow, (UINT8)DestId);
            }

            if ((__readmsr(MSR_IA32_APIC_BASE) & APIC_BASE_X2APIC_ENABLE)
                && gHostLapicVa != NULL) {
                __writemsr(Msr, Value);
            } else if (gHostLapicVa != NULL) {
                *(volatile UINT32 *)(gHostLapicVa + LAPIC_ICR_HIGH_OFFSET) =
                    (UINT32)(Value >> 32) << 24;
                *(volatile UINT32 *)(gHostLapicVa + LAPIC_ICR_LOW_OFFSET) =
                    IcrLow;
            }
            break;
        }

        default:
            if (!MsrIsInValidRange(Msr)) {
                HvLog("VGE\n"); HvLogHex("VGERIP", Vmcb->Save.Rip); HvLogHex("VGEMSR", Msr);
                InjectException(Vmcb, EXCEPTION_GP, TRUE, 0);
                return;
            }
            __writemsr(Msr, Value);
            break;
        }
    } else {
        UINT64 Value;

        switch (Msr) {
        case MSR_EFER:
            Value = Vcpu->OriginalEfer;
            break;

        case MSR_VM_CR:

            Value = (__readmsr(MSR_VM_CR) & ~(1ULL << 1))
                    | (1ULL << 4)
                    | (1ULL << 3);
            break;

        case MSR_VM_HSAVE_PA:
            Value = 0;
            break;

        default:
            if (!MsrIsInValidRange(Msr)) {
                HvLog("VGF\n"); HvLogHex("VGFRIP", Vmcb->Save.Rip); HvLogHex("VGFMSR", Msr);
                InjectException(Vmcb, EXCEPTION_GP, TRUE, 0);
                return;
            }
            Value = __readmsr(Msr);
            break;
        }

        Vmcb->Save.Rax = Value & 0xFFFFFFFF;
        Vcpu->GuestRdx = Value >> 32;
    }

    Vmcb->Save.Rip += 2;
}

// === SECTION: CR3_CAPTURE ===
#define APEX_NAME_LEN 15
STATIC const UINT8 gApexName_DX12[APEX_NAME_LEN] =
    {'r','5','a','p','e','x','_','d','x','1','2','.','e','x','e'};
STATIC const UINT8 gApexName_DX11[APEX_NAME_LEN] =
    {'r','5','a','p','e','x','.','e','x','e',0,0,0,0,0};

STATIC
EFI_STATUS
Cr3ScanEprocChunk(
    UINT64  Cursor,
    UINT64  ChunkEnd,
    UINT64  Limit,
    UINT64  *OutImageBase,
    UINT64  *OutPebVa,
    UINT64  *OutPid,
    UINT64  *OutDtb,
    UINT64  *OutNextCursor
    )
{
    UINT8 Page[0x1000];
    const UINT32 MaxBodyOff = 0x1000 - 0x340;

    UINT64 HvLo = gHypeLoaderImageBase;
    UINT64 HvHi = gHypeLoaderImageBase + gHypeLoaderImageSize;

    for (UINT64 Pa = Cursor; Pa < ChunkEnd; Pa += 0x1000) {

        if (Pa >= 0xA0000 && Pa < 0x100000)
            continue;
        if (Pa >= 0xC0000000 && Pa < 0x100000000) {
            Pa = 0x100000000ULL - 0x1000;
            continue;
        }
        if (HvLo != 0 && Pa >= HvLo && Pa < HvHi)
            continue;
        if (EFI_ERROR(ReadGuestPhysical(Pa, Page, 0x1000)))
            continue;

        for (UINT32 Off = 0; Off < MaxBodyOff; Off += 0x10) {
            UINT8 *Name = &Page[Off + 0x338];

            if (Name[0] != 'r' || Name[1] != '5')
                continue;

            BOOLEAN IsDx12 = TRUE, IsDx11 = TRUE;
            for (UINT32 i = 0; i < APEX_NAME_LEN; i++) {
                if (Name[i] != gApexName_DX12[i]) IsDx12 = FALSE;
                if (Name[i] != gApexName_DX11[i]) IsDx11 = FALSE;
            }
            if (!IsDx12 && !IsDx11)
                continue;

            UINT64 ImageBase = *(UINT64*)&Page[Off + 0x2B0];
            if (ImageBase < 0x10000 || ImageBase >= 0x800000000000ULL)
                continue;

            UINT64 PebVa = *(UINT64*)&Page[Off + EPROCESS_PEB_OFFSET];
            if (PebVa < 0x10000 || PebVa >= 0x800000000000ULL)
                continue;

            UINT64 PidVal = *(UINT64*)&Page[Off + EPROCESS_PID_OFFSET];

            UINT64 DtbVal = *(UINT64*)&Page[Off + EPROCESS_DTB_OFFSET];  // KPROCESS+0x28

            *OutImageBase = ImageBase;
            *OutPebVa = PebVa;
            *OutPid = PidVal & 0xFFFFFFFFULL;
            *OutDtb = DtbVal & ~0xFFFULL;  // strip PCID/flag bits
            *OutNextCursor = Pa + 0x1000;
            return EFI_SUCCESS;
        }
    }
    *OutNextCursor = ChunkEnd;
    return (ChunkEnd >= Limit) ? EFI_NOT_FOUND : EFI_NOT_READY;
}

STATIC
EFI_STATUS
Cr3ScanPml4Chunk(
    UINT64  ImageBase,
    UINT64  Cursor,
    UINT64  ChunkEnd,
    UINT64  Limit,
    UINT64  *OutCr3,
    UINT64  *OutNextCursor
    )
{
    for (UINT64 Pa = Cursor; Pa < ChunkEnd; Pa += 0x1000) {

        if (Pa >= 0xA0000 && Pa < 0x100000)
            continue;
        if (Pa >= 0xC0000000 && Pa < 0x100000000) {
            Pa = 0x100000000ULL - 0x1000;
            continue;
        }
        UINT64 ImageBasePa = 0;
        if (EFI_ERROR(TranslateGuestVirtual(Pa, ImageBase, &ImageBasePa)))
            continue;

        UINT16 Magic = 0;
        if (EFI_ERROR(ReadGuestPhysical(ImageBasePa, &Magic, sizeof(UINT16))))
            continue;
        if (Magic != 0x5A4D)
            continue;

        UINT32 Lfanew = 0;
        if (EFI_ERROR(ReadGuestPhysical(ImageBasePa + 0x3C, &Lfanew, sizeof(UINT32))))
            continue;
        if (Lfanew < 0x40 || Lfanew > 0xF00)
            continue;

        UINT32 NtSig = 0;
        if (EFI_ERROR(ReadGuestPhysical(ImageBasePa + Lfanew, &NtSig, sizeof(UINT32))))
            continue;
        if (NtSig != 0x00004550)
            continue;

        *OutCr3 = Pa;
        *OutNextCursor = Pa + 0x1000;
        return EFI_SUCCESS;
    }
    *OutNextCursor = ChunkEnd;
    return (ChunkEnd >= Limit) ? EFI_NOT_FOUND : EFI_NOT_READY;
}

STATIC
EFI_STATUS
Cr3ScanPml4PebChunk(
    UINT64  PebVa,
    UINT64  Cursor,
    UINT64  ChunkEnd,
    UINT64  Limit,
    UINT64  *OutCr3,
    UINT64  *OutImageBase,
    UINT64  *OutNextCursor
    )
{
    UINT64 PebPml4Off = GUEST_PML4_INDEX(PebVa) * sizeof(UINT64);

    for (UINT64 Pa = Cursor; Pa < ChunkEnd; Pa += 0x1000) {

        if (Pa >= 0xA0000 && Pa < 0x100000)
            continue;
        if (Pa >= 0xC0000000 && Pa < 0x100000000) {
            Pa = 0x100000000ULL - 0x1000;
            continue;
        }
        UINT64 Pml4Entry = 0;
        if (EFI_ERROR(ReadGuestPhysical(Pa + PebPml4Off, &Pml4Entry, sizeof(UINT64))))
            continue;
        if (!(Pml4Entry & GUEST_PTE_PRESENT))
            continue;

        UINT64 LdrPa = 0, LdrVa = 0;
        if (EFI_ERROR(TranslateGuestVirtual(Pa, PebVa + 0x18, &LdrPa)))
            continue;
        if (EFI_ERROR(ReadGuestPhysical(LdrPa, &LdrVa, sizeof(UINT64))))
            continue;
        if (LdrVa < 0x10000 || LdrVa >= 0x800000000000ULL)
            continue;

        UINT64 ImagePa = 0, ImageBase = 0;
        if (EFI_ERROR(TranslateGuestVirtual(Pa, PebVa + 0x10, &ImagePa)))
            continue;
        if (EFI_ERROR(ReadGuestPhysical(ImagePa, &ImageBase, sizeof(UINT64))))
            continue;
        if (ImageBase < 0x10000 || ImageBase >= 0x800000000000ULL)
            continue;

        UINT64 MzPa = 0;
        UINT16 Magic = 0;
        if (EFI_ERROR(TranslateGuestVirtual(Pa, ImageBase, &MzPa)))
            continue;
        if (EFI_ERROR(ReadGuestPhysical(MzPa, &Magic, sizeof(UINT16))))
            continue;
        if (Magic != 0x5A4D)
            continue;

        *OutCr3 = Pa;
        *OutImageBase = ImageBase;
        *OutNextCursor = Pa + 0x1000;
        return EFI_SUCCESS;
    }
    *OutNextCursor = ChunkEnd;
    return (ChunkEnd >= Limit) ? EFI_NOT_FOUND : EFI_NOT_READY;
}

static inline __attribute__((always_inline))
VOID
Cr3PassiveSample(
    VOID
    )
{
    UINT32 Phase = __atomic_load_n(&gCr3SampleArmed, __ATOMIC_ACQUIRE);
    if (Phase == 0 || Phase == 3)
        return;

    UINT64 Limit = gEffectiveLimit;
    if (Limit == 0) Limit = HOST_PT_MAX - 1;

    UINT64 Cursor = gCr3SampleCursor;
    UINT64 ChunkEnd = Cursor + CR3_SCAN_CHUNK_BYTES;
    if (ChunkEnd > Limit) ChunkEnd = Limit;

    if (Phase == 1) {
        UINT64 ImageBase = 0, PebVa = 0, FoundPid = 0, Dtb = 0;
        UINT64 NextCursor = 0;
        EFI_STATUS S = Cr3ScanEprocChunk(Cursor, ChunkEnd, Limit,
                                         &ImageBase, &PebVa, &FoundPid,
                                         &Dtb, &NextCursor);
        if (S == EFI_SUCCESS) {
            gCr3SamplePebVa = PebVa;
            gCr3SampleImageBase = ImageBase;
            gCr3SamplePid = (UINT32)FoundPid;
            gCr3SampleCursor = 0x1000;
            HvLogHex("V20", ImageBase);
            HvLogHex("V24", PebVa);
            HvLogHex("V25", FoundPid);

            // Path A: KPROCESS.DTB direct read (Apex stopped rotating CR3 ~June 2025).
            if (Dtb == 0 || (Dtb & 0xFFFULL) != 0 || Dtb >= Limit) {
                HvLogHex("V2F", Dtb);
                __atomic_store_n(&gCr3SampleArmed, 3, __ATOMIC_RELEASE);
                return;
            }
            UINT32 N = g_DriverContext.HvState.NumCpus;
            for (UINT32 i = 0; i < N; i++) {
                PVCPU_DATA V = &g_DriverContext.HvState.VcpuTable[i];
                if (V->Vmcb == NULL) continue;
                V->TargetCr3 = Dtb;
                V->Cr3InterceptCapturedPeb = PebVa;
                V->Cr3InterceptCapturedImageBase = ImageBase;
            }
            __atomic_store_n(&gCr3SampleArmed, 3, __ATOMIC_RELEASE);
            HvLogHex("V72", Dtb);
            HvLogHex("V73", PebVa);
            return;
        } else if (S == EFI_NOT_FOUND) {
            __atomic_store_n(&gCr3SampleArmed, 3, __ATOMIC_RELEASE);
            HvLogHex("V21", 0);
        } else {
            gCr3SampleCursor = NextCursor;
        }
        return;
    }

    if (Phase == 2 || Phase == 4) {
        UINT64 Cr3 = 0;
        UINT64 ImageBaseFound = 0;
        UINT64 NextCursor = 0;
        EFI_STATUS S;
        if (Phase == 4) {
            S = Cr3ScanPml4PebChunk(gCr3SamplePebVa, Cursor, ChunkEnd, Limit,
                                    &Cr3, &ImageBaseFound, &NextCursor);
        } else {
            S = Cr3ScanPml4Chunk(gCr3SampleImageBase, Cursor, ChunkEnd, Limit,
                                 &Cr3, &NextCursor);
        }
        if (S == EFI_SUCCESS) {
            if (Phase == 4) gCr3SampleImageBase = ImageBaseFound;

            UINT64 BroadcastPeb = (gCr3SamplePebVa != 0) ? gCr3SamplePebVa
                                                          : gCr3SampleImageBase;
            UINT32 N = g_DriverContext.HvState.NumCpus;
            for (UINT32 i = 0; i < N; i++) {
                PVCPU_DATA V = &g_DriverContext.HvState.VcpuTable[i];
                if (V->Vmcb == NULL) continue;
                V->TargetCr3 = Cr3;
                V->Cr3InterceptCapturedPeb = BroadcastPeb;
                V->Cr3InterceptCapturedImageBase = gCr3SampleImageBase;
            }
            __atomic_store_n(&gCr3SampleArmed, 3, __ATOMIC_RELEASE);
            HvLogHex("V72", Cr3);
            HvLogHex("V73", BroadcastPeb);
            if (Phase == 4) HvLogHex("V26", ImageBaseFound);
        } else if (S == EFI_NOT_FOUND) {
            __atomic_store_n(&gCr3SampleArmed, 3, __ATOMIC_RELEASE);
            HvLogHex("V23", gCr3SampleImageBase);
        } else {
            gCr3SampleCursor = NextCursor;
        }
        return;
    }
}

// === SECTION: VMRUN_VMMCALL ===
STATIC
VOID
HandleVmrun(
    PVCPU_DATA Vcpu
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;
    InjectException(Vmcb, EXCEPTION_UD, FALSE, 0);
}

STATIC
VOID
HandleVmmcall(
    PVCPU_DATA Vcpu
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;

    if ((UINT32)Vcpu->GuestRcx == PMC_CMD_HANDSHAKE) {
        if (Vcpu->GuestRbx == gHandshakeExpected) {
            Vmcb->Save.Rax = gBootAuthKey;
            Vmcb->Save.Rip += 3;
        } else {
            InjectException(Vmcb, EXCEPTION_UD, FALSE, 0);
        }
        return;
    }

    if (Vcpu->GuestRbx != gBootAuthKey) {
        InjectException(Vmcb, EXCEPTION_UD, FALSE, 0);
        return;
    }

    Vcpu->Authenticated = 1;
    UINT32 CmdId = (UINT32)Vcpu->GuestRcx;
    UINT64 Result = 0;

    switch (CmdId) {

    case PMC_CMD_NPT_CHANNEL_INIT: {

        PNPT_CONTEXT Npt = &g_DriverContext.NptContext;
        UINT64 GuestCr3 = Vmcb->Save.Cr3 & ~0xFFFULL;
        UINT64 TriggerVa = Vcpu->GuestRdx;
        UINT64 MailboxVa = Vcpu->GuestR8;
        UINT64 TriggerGpa = 0, MailboxGpa = 0;

        if (EFI_ERROR(TranslateGuestVirtual(GuestCr3, TriggerVa, &TriggerGpa))) {
            Result = COVERT_STATUS_BAD_ADDR;
            break;
        }
        if (EFI_ERROR(TranslateGuestVirtual(GuestCr3, MailboxVa, &MailboxGpa))) {
            Result = COVERT_STATUS_BAD_ADDR;
            break;
        }
        if (TriggerGpa >= HOST_PT_MAX || MailboxGpa >= HOST_PT_MAX) {
            Result = COVERT_STATUS_BAD_ADDR;
            break;
        }
        if (NptIsProtectedAddress(Npt, TriggerGpa) ||
            NptIsProtectedAddress(Npt, MailboxGpa)) {
            Result = COVERT_STATUS_BAD_ADDR;
            break;
        }
        if ((TriggerGpa & ~(PAGE_SIZE_4KB - 1)) == (MailboxGpa & ~(PAGE_SIZE_4KB - 1))) {
            Result = COVERT_STATUS_BAD_ADDR;
            break;
        }

        UINT64 TriggerGpaAligned = TriggerGpa & ~(PAGE_SIZE_4KB - 1);
        UINT64 *Pte = NptGetPte(Npt, TriggerGpaAligned, FALSE);
        if (!Pte) {
            Result = COVERT_STATUS_BAD_ADDR;
            break;
        }
        if (*Pte & NPT_LARGE_PAGE) {
            NTSTATUS SplitSt = NptSplitLargePageRuntime(Npt, TriggerGpaAligned);
            if (SplitSt != STATUS_SUCCESS) {
                Result = (SplitSt == STATUS_INSUFFICIENT_RESOURCES)
                       ? COVERT_STATUS_NO_SPARE
                       : COVERT_STATUS_ERR;
                break;
            }
            Pte = NptGetPte(Npt, TriggerGpaAligned, FALSE);
            if (!Pte || (*Pte & NPT_LARGE_PAGE)) {
                Result = COVERT_STATUS_ERR;
                break;
            }
        }

        UINT64 OldTriggerGpa = __atomic_load_n(&gCovertTriggerGpa, __ATOMIC_ACQUIRE);

        if (OldTriggerGpa != 0 && OldTriggerGpa != TriggerGpaAligned) {
            UINT64 *OldPte = NptGetPte(Npt, OldTriggerGpa, FALSE);
            if (OldPte && !(*OldPte & NPT_LARGE_PAGE)) {
                *OldPte |= NPT_WRITE;
            }
            __atomic_store_n(&gCovertTriggerGpa, (UINT64)0, __ATOMIC_RELEASE);
            Vcpu->CovertTriggerActive = 0;
        }

        *Pte &= ~NPT_WRITE;

        gCovertMailboxGpa = MailboxGpa & ~(PAGE_SIZE_4KB - 1);
        gCovertMailboxVa  = MailboxVa;
        gCovertClientCr3  = GuestCr3;

        UINT64 PrevGpa = __atomic_exchange_n(&gCovertTriggerGpa, TriggerGpaAligned, __ATOMIC_ACQ_REL);
        if (PrevGpa == 0) {
            HvLogHex("VC0", TriggerGpaAligned);
            HvLogHex("VC2", gCovertMailboxGpa);
        }
        Vcpu->CovertTriggerActive = 0;

        // Auth-gated reset of per-VCPU replay counter — new channel starts at seq 1.
        for (UINT32 i = 0; i < g_DriverContext.HvState.NumCpus; i++)
            g_DriverContext.HvState.VcpuTable[i].LastCovertSequence = 0;

        FlushAllTlb(Vcpu);

        Result = COVERT_STATUS_OK;
        break;
    }

    case PMC_CMD_SET_EPROCESS: {
        Vcpu->KernelCr3 = 0;

        UINT64 WalkCr3 = SanitizeDtb(Vmcb->Save.Cr3);
        {
            UINT64 KpcrVa = (Vmcb->Save.Cpl == 0)
                ? Vmcb->Save.GS.Base
                : Vmcb->Save.KernelGsBase;
            if (KpcrVa != 0 && KpcrVa >= 0xFFFF800000000000ULL) {
                UINT64 ThreadPa = 0;
                if (!EFI_ERROR(TranslateGuestVirtual(WalkCr3, KpcrVa + KPCR_PRCB_OFFSET + KPRCB_CURRENTTHREAD, &ThreadPa))) {
                    UINT64 ThreadVa = 0;
                    ReadGuestPhysical(ThreadPa, &ThreadVa, sizeof(UINT64));
                    if (ThreadVa != 0 && ThreadVa >= 0xFFFF800000000000ULL) {
                        UINT64 ProcPa = 0;
                        if (!EFI_ERROR(TranslateGuestVirtual(WalkCr3, ThreadVa + KTHREAD_PROCESS_OFFSET, &ProcPa))) {
                            UINT64 ProcVa = 0;
                            ReadGuestPhysical(ProcPa, &ProcVa, sizeof(UINT64));
                            if (ProcVa != 0 && ProcVa >= 0xFFFF800000000000ULL) {
                                UINT64 DtbPa = 0;
                                if (!EFI_ERROR(TranslateGuestVirtual(WalkCr3, ProcVa + EPROCESS_DTB_OFFSET, &DtbPa))) {
                                    UINT64 KernDtb = 0;
                                    ReadGuestPhysical(DtbPa, &KernDtb, sizeof(UINT64));
                                    UINT64 KernCr3 = SanitizeDtb(KernDtb);
                                    if (KernCr3 != 0)
                                        WalkCr3 = KernCr3;
                                }
                            }
                        }
                    }
                }
            }
        }

        UINT64 EprocArg = Vcpu->GuestRdx;
        if (EprocArg == 0) {
            UINT64 KpcrVa = (Vmcb->Save.Cpl == 0)
                ? Vmcb->Save.GS.Base
                : Vmcb->Save.KernelGsBase;

            if (KpcrVa == 0 || KpcrVa < 0xFFFF800000000000ULL) {
                Result = HYPE_MEM_ERR_NOT_FOUND;
                break;
            }

            UINT64 KprbPa = 0;
            if (EFI_ERROR(TranslateGuestVirtual(WalkCr3, KpcrVa + KPCR_PRCB_OFFSET + KPRCB_IDLETHREAD, &KprbPa))) {
                Result = HYPE_MEM_ERR_UNMAPPED;
                break;
            }
            UINT64 IdleThreadVa = 0;
            ReadGuestPhysical(KprbPa, &IdleThreadVa, sizeof(UINT64));
            if (IdleThreadVa == 0 || IdleThreadVa < 0xFFFF800000000000ULL) {
                Result = HYPE_MEM_ERR_NOT_FOUND;
                break;
            }

            UINT64 FoundEproc = 0;
            UINT64 FoundEprocPidPa = 0;
            for (UINT64 Off = KTHREAD_SCAN_START; Off < KTHREAD_SCAN_END; Off += 8) {
                UINT64 CandPa = 0;
                if (EFI_ERROR(TranslateGuestVirtual(WalkCr3, IdleThreadVa + Off, &CandPa)))
                    continue;
                UINT64 CandVal = 0;
                ReadGuestPhysical(CandPa, &CandVal, sizeof(UINT64));
                if (CandVal == 0 || CandVal < 0xFFFF800000000000ULL)
                    continue;
                UINT64 PidPa = 0;
                if (EFI_ERROR(TranslateGuestVirtual(WalkCr3, CandVal + EPROCESS_PID_OFFSET, &PidPa)))
                    continue;
                UINT64 PidVal = 0;
                ReadGuestPhysical(PidPa, &PidVal, sizeof(UINT64));
                if ((PidVal & 0xFFFFFFFF) == SYSTEM_PID) {
                    FoundEproc = CandVal;
                    FoundEprocPidPa = PidPa;
                    break;
                }
            }

            if (FoundEproc == 0) {
                Result = HYPE_MEM_ERR_NOT_FOUND;
                break;
            }
            Vcpu->SystemEprocessVa = FoundEproc;

            {
                UINT64 DtbPa = 0;
                EFI_STATUS DtbSt = TranslateGuestVirtual(WalkCr3, FoundEproc + EPROCESS_DTB_OFFSET, &DtbPa);
                if (EFI_ERROR(DtbSt) && FoundEprocPidPa != 0) {
                    DtbPa = FoundEprocPidPa - EPROCESS_PID_OFFSET + EPROCESS_DTB_OFFSET;
                }
                if (DtbPa != 0) {
                    UINT64 SysCr3 = 0;
                    ReadGuestPhysical(DtbPa, &SysCr3, sizeof(UINT64));
                    if (SysCr3 != 0) {
                        Vcpu->KernelCr3 = SanitizeDtb(SysCr3);
                    }
                }
            }
        } else {
            Vcpu->SystemEprocessVa = EprocArg;
        }

        if (Vcpu->KernelCr3 == 0) {
            UINT64 DtbPa = 0;
            EFI_STATUS St = TranslateGuestVirtual(WalkCr3, Vcpu->SystemEprocessVa + EPROCESS_DTB_OFFSET, &DtbPa);
            if (!EFI_ERROR(St) && DtbPa != 0) {
                UINT64 SysCr3 = 0;
                ReadGuestPhysical(DtbPa, &SysCr3, sizeof(UINT64));
                if (SysCr3 != 0) {
                    Vcpu->KernelCr3 = SanitizeDtb(SysCr3);
                }
            }
        }

        if (Vcpu->KernelCr3 == 0 && Vcpu->SystemEprocessVa != 0) {
            Vcpu->KernelCr3 = WalkCr3;
        }

        Result = (Vcpu->SystemEprocessVa != 0) ? HYPE_MEM_OK : HYPE_MEM_ERR_NOT_FOUND;
        break;
    }

    default:
        InjectException(Vmcb, EXCEPTION_UD, FALSE, 0);
        return;
    }

    Vmcb->Save.Rax = Result;
    Vmcb->Save.Rip += 3;
}

// === SECTION: VIRT_ACCESS ===
STATIC
EFI_STATUS
BatchTranslateVa(

    PVCPU_DATA  Vcpu,
    UINT64      Cr3,
    UINT64      Va,
    UINT64      *Pa
    )
{
    UINT64 VaPage = Va & ~0xFFFULL;
    UINT32 Idx = SOFT_TLB_INDEX(Va);
    if (Vcpu->TlbCr3 == Cr3 && Vcpu->TlbVaPage[Idx] == VaPage) {
        *Pa = Vcpu->TlbPaPage[Idx] | (Va & 0xFFF);
        return EFI_SUCCESS;
    }
    EFI_STATUS St = TranslateGuestVirtual(Cr3, Va, Pa);
    if (!EFI_ERROR(St)) {
        Vcpu->TlbCr3 = Cr3;
        Vcpu->TlbVaPage[Idx] = VaPage;
        Vcpu->TlbPaPage[Idx] = *Pa & ~0xFFFULL;
    }
    return St;
}

static inline __attribute__((always_inline))
EFI_STATUS
VirtAccessResolve(
    PVCPU_DATA  Vcpu,
    UINT64      Va,
    UINTN       Size,
    UINT64      *OutPa
    )
{
    if (Vcpu->TargetCr3 == 0) return EFI_NOT_READY;
    if ((Va & 0xFFF) > (0x1000 - Size)) return EFI_BAD_BUFFER_SIZE;
    if (EFI_ERROR(BatchTranslateVa(Vcpu, Vcpu->TargetCr3, Va, OutPa))) return EFI_NOT_FOUND;
    if (NptIsProtectedAddress(&g_DriverContext.NptContext, *OutPa)) return EFI_ACCESS_DENIED;
    return EFI_SUCCESS;
}

// === SECTION: LAPIC_IPI ===
STATIC
PVCPU_DATA
FindVcpuByApicId(
    UINT8 ApicId
    )
{
    PHYPERVISOR_STATE Hv = &g_DriverContext.HvState;
    for (UINT32 i = 0; i < Hv->NumCpus; i++) {
        if ((Hv->VcpuTable[i].ApicId & 0xFF) == ApicId)
            return &Hv->VcpuTable[i];
    }
    return NULL;
}

STATIC
BOOLEAN
DecodeLapicMmioAccess(
    PVCPU_DATA  Vcpu,
    BOOLEAN    *OutIsWrite,
    UINT8      *OutReg,
    UINT32     *OutInstrLen,
    BOOLEAN    *OutIsImm,
    UINT32     *OutImmVal
    )
{
    PVMCB   Vmcb     = Vcpu->Vmcb;
    UINT8  *Insn     = Vmcb->Control.GuestInstructionBytes;
    UINT32  NumBytes = Vmcb->Control.NumBytes;
    UINT32  Off      = 0;
    UINT8   Rex      = 0;
    BOOLEAN IsImm    = FALSE;

    *OutIsImm = FALSE;
    *OutImmVal = 0;

    if (NumBytes == 0) return FALSE;

    while (Off < NumBytes && (Insn[Off] == 0x66 || Insn[Off] == 0x67 ||
                              Insn[Off] == 0xF0 || Insn[Off] == 0xF2 ||
                              Insn[Off] == 0xF3)) {
        Off++;
    }
    if (Off >= NumBytes) return FALSE;

    if ((Insn[Off] & 0xF0) == 0x40) {
        Rex = Insn[Off++];
        if (Off >= NumBytes) return FALSE;
    }

    UINT8   Opcode = Insn[Off++];
    BOOLEAN IsWrite;

    if (Opcode == 0x89) {
        IsWrite = TRUE;
    } else if (Opcode == 0x8B) {
        IsWrite = FALSE;
    } else if (Opcode == 0x0F && Off < NumBytes && Insn[Off] == 0xC3) {
        IsWrite = TRUE;
        Off++;
    } else if (Opcode == 0xC7) {
        IsWrite = TRUE;
        IsImm   = TRUE;
    } else {
        return FALSE;
    }

    if (Off >= NumBytes) return FALSE;
    UINT8 ModRM = Insn[Off++];
    UINT8 Mod   = (ModRM >> 6) & 3;
    UINT8 Reg   = ((ModRM >> 3) & 7) | ((Rex & 0x04) ? 8 : 0);
    UINT8 RM    = ModRM & 7;

    if (IsImm && ((ModRM >> 3) & 7) != 0) return FALSE;

    if (Mod != 3 && RM == 4 && Off < NumBytes) Off++;
    if (Mod == 1) Off += 1;
    else if (Mod == 2) Off += 4;
    else if (Mod == 0 && RM == 5) Off += 4;

    if (IsImm) {
        if (Off + 4 > NumBytes) return FALSE;
        *OutImmVal = (UINT32)Insn[Off]
                   | ((UINT32)Insn[Off + 1] << 8)
                   | ((UINT32)Insn[Off + 2] << 16)
                   | ((UINT32)Insn[Off + 3] << 24);
        Off += 4;
    }

    *OutIsWrite  = IsWrite;
    *OutReg      = Reg;
    *OutInstrLen = Off;
    *OutIsImm    = IsImm;
    return TRUE;
}

STATIC
VOID
AdvanceRipDecoded(
    PVMCB Vmcb,
    UINT32 FallbackLen
    )
{
    if (Vmcb->Control.NRip != 0) {
        Vmcb->Save.Rip = Vmcb->Control.NRip;
    } else {
        Vmcb->Save.Rip += FallbackLen;
    }
}

STATIC volatile UINT32 gLapicTraceCount = 0;
#define LAPIC_TRACE_LIMIT 64

// Cycle 15 diagnostic — counts NPFs landing on gLapicGpa after disarm.
// gLapicInterceptArmed=0 path should be unreachable (PTE restored). Any
// non-zero count proves the disarmed branch fires; zero proves NPFs are
// not landing on gLapicGpa at all (cycle 14 M0F=0 root-cause split).
STATIC volatile UINT32 gLapicDisarmedNpfCount = 0;

STATIC
VOID
PassThroughLapicMmio(
    PVCPU_DATA Vcpu,
    UINT64     FaultGpa
    )
{
    PVMCB   Vmcb = Vcpu->Vmcb;
    BOOLEAN IsWrite;
    UINT8   Reg;
    UINT32  InstrLen;
    BOOLEAN IsImm;
    UINT32  ImmVal;
    volatile UINT32 *HostVa = (volatile UINT32 *)(UINTN)FaultGpa;
    UINT32  Val32 = 0;

    if (!DecodeLapicMmioAccess(Vcpu, &IsWrite, &Reg, &InstrLen, &IsImm, &ImmVal)) {
        HvLogHex("V86", FaultGpa);
        InjectException(Vmcb, EXCEPTION_UD, FALSE, 0);
        return;
    }

    if (IsWrite) {
        Val32 = IsImm ? ImmVal : (UINT32)ReadGuestGpr(Vcpu, Reg);
        *HostVa = Val32;
    } else {
        Val32 = *HostVa;
        WriteGuestGpr32Zx(Vcpu, Reg, Val32);
    }

    if (gLapicTraceCount < LAPIC_TRACE_LIMIT) {
        UINT64 Packed = ((FaultGpa & 0xFFFULL) << 32) | Val32;
        HvLogHex(IsWrite ? "V94" : "V95", Packed);
        __sync_add_and_fetch((volatile INT32 *)&gLapicTraceCount, 1);
    }

    AdvanceRipDecoded(Vmcb, InstrLen);
}

STATIC
VOID
StampInit(PVCPU_DATA Target)
{
    InterlockedCompareExchange(
        (volatile LONG *)&Target->ActivityState,
        (LONG)GUEST_WFS,
        (LONG)GUEST_ACTIVE
    );
    Target->SipiVector = 0;
}

STATIC
VOID
StampStartup(PVCPU_DATA Target, UINT8 Vector)
{

    if (Target->SipiApplied) {
        // H3: late STARTUP on already-applied AP — kernel re-issued post-Phase-3.
        HvLogHex("VAF", (UINT64)Target->ApicId);
        return;
    }

    Target->SipiVector = Vector;
    __sync_synchronize();
    InterlockedExchange(
        (volatile LONG *)&Target->ActivityState,
        (LONG)GUEST_SIPI_ISSUED
    );

    // H3: CAS-clamp at zero — prevents underflow when spurious STARTUPs fire
    // for un-applied APs after Phase 3 ends.
    INT32 Rem;
    INT32 Cur;
    do {
        Cur = __atomic_load_n(&gRemainingSipiCount, __ATOMIC_RELAXED);
        if (Cur <= 0) { Rem = Cur; break; }
        if (__atomic_compare_exchange_n(&gRemainingSipiCount, &Cur, Cur - 1,
                                         FALSE,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED)) {
            Rem = Cur - 1;
            break;
        }
    } while (1);

    if (Rem == 0) {
        HvLogHex("V89", 0);
        DisableLapicIntercept();
    }
}

STATIC
VOID
DispatchIpi(PVCPU_DATA Self, UINT32 Mode, UINT32 IcrLow, UINT8 DestApicId)
{
    UINT32 Shorthand = (IcrLow >> 18) & 3;
    UINT8  Vector    = (UINT8)(IcrLow & 0xFF);
    PHYPERVISOR_STATE Hv = &g_DriverContext.HvState;

    if (Shorthand == LAPIC_SHORTHAND_ALL_EXCL ||
        Shorthand == LAPIC_SHORTHAND_ALL_INCL)
    {
        for (UINT32 i = 0; i < Hv->NumCpus; i++) {
            PVCPU_DATA Target = &Hv->VcpuTable[i];
            if (Shorthand == LAPIC_SHORTHAND_ALL_EXCL && Target == Self) {
                continue;
            }
            if (Mode == LAPIC_DELIVERY_INIT) {
                StampInit(Target);
            } else {
                StampStartup(Target, Vector);
                if (!gLapicInterceptArmed) break;
            }
        }
        HvLogHex(Mode == LAPIC_DELIVERY_INIT ? "V88" : "V8C", (UINT64)IcrLow);
        return;
    }
    if (Shorthand == LAPIC_SHORTHAND_SELF) {
        return;
    }

    PVCPU_DATA Target = FindVcpuByApicId(DestApicId);
    if (!Target) {
        HvLogHex("V87", (UINT64)DestApicId);
        return;
    }
    if (Mode == LAPIC_DELIVERY_INIT) {
        StampInit(Target);
        HvLogHex("V88", ((UINT64)DestApicId << 24) | IcrLow);
    } else {
        StampStartup(Target, Vector);
    }
}

STATIC
VOID
HandleLapicIcrWrite(
    PVCPU_DATA Vcpu,
    UINT64     FaultGpa
    )
{
    PVMCB   Vmcb = Vcpu->Vmcb;
    BOOLEAN IsWrite;
    UINT8   Reg;
    UINT32  InstrLen;
    BOOLEAN IsImm;
    UINT32  ImmVal;

    if (!DecodeLapicMmioAccess(Vcpu, &IsWrite, &Reg, &InstrLen, &IsImm, &ImmVal)) {
        HvLogHex("V86", FaultGpa);
        InjectException(Vmcb, EXCEPTION_UD, FALSE, 0);
        return;
    }
    if (!IsWrite) {
        // Shadow is write-protect-only; reads shouldn't NPF here, but if one does
        // (PTE regression / unexpected access width), pass it through instead of #UD.
        HvLogHex("V8B", FaultGpa);
        PassThroughLapicMmio(Vcpu, FaultGpa);
        return;
    }

    UINT32 IcrLow = IsImm ? ImmVal : (UINT32)ReadGuestGpr(Vcpu, Reg);
    UINT32 Mode   = (IcrLow >> 8) & 7;
    volatile UINT32 *HostIcrLow =
        (volatile UINT32 *)(UINTN)(gLapicGpa + LAPIC_ICR_LOW_OFFSET);

    if (Mode == LAPIC_DELIVERY_INIT || Mode == LAPIC_DELIVERY_STARTUP) {
        UINT32 IcrHigh =
            *(volatile UINT32 *)(UINTN)(gLapicGpa + LAPIC_ICR_HIGH_OFFSET);
        UINT8  DestApicId = (UINT8)((IcrHigh >> 24) & 0xFF);
        DispatchIpi(Vcpu, Mode, IcrLow, DestApicId);
    }

    *HostIcrLow = IcrLow;

    if (gLapicTraceCount < LAPIC_TRACE_LIMIT) {
        UINT64 Packed = ((UINT64)LAPIC_ICR_LOW_OFFSET << 32) | IcrLow;
        HvLogHex("V94", Packed);
        __sync_add_and_fetch((volatile INT32 *)&gLapicTraceCount, 1);
    }

    AdvanceRipDecoded(Vmcb, InstrLen);
}

// === SECTION: NPF_AND_COVERT ===
STATIC
VOID
HandleNpf(
    PVCPU_DATA Vcpu
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;
    UINT64 FaultGpa = Vmcb->Control.ExitInfo2;
    UINT64 ErrorCode = Vmcb->Control.ExitInfo1;
    BOOLEAN IsExec = (ErrorCode >> 4) & 1;

    if (IsExec) {
        static volatile INT32 gDvnLogged = 0;
        if (__atomic_load_n(&gDvnLogged, __ATOMIC_RELAXED) < 64) {
            __sync_add_and_fetch(&gDvnLogged, 1);
            HvLogHex("DVN", FaultGpa);
        }
        if (DrawHookGpaMatches(FaultGpa)) {
            static volatile INT32 gDvhLogged = 0;
            if (__atomic_load_n(&gDvhLogged, __ATOMIC_RELAXED) < 16) {
                __sync_add_and_fetch(&gDvhLogged, 1);
                HvLogHex("DVH", Vmcb->Save.Rip);
            }
        }
    }

    if (gLapicGpa != 0 && (FaultGpa & ~0xFFFULL) == gLapicGpa) {
        UINT64 Off = FaultGpa & 0xFFF;
        if (gLapicInterceptArmed && Off == LAPIC_ICR_LOW_OFFSET) {
            Vcpu->LastNpfBranch = 0x01;
            HandleLapicIcrWrite(Vcpu, FaultGpa);
        } else {
            // Cycle 15 diag: count disarmed-branch hits and log one-shot
            // M0F per LP. Counter > 0 → disarmed handler reached (PTE not
            // actually restored, or armed-but-non-ICR path); counter == 0
            // → NPFs not landing on gLapicGpa.
            if (!gLapicInterceptArmed) {
                __sync_add_and_fetch((volatile INT32 *)&gLapicDisarmedNpfCount, 1);
                if ((Vcpu->HeartbeatLogged & 0x40) == 0) {
                    Vcpu->HeartbeatLogged |= 0x40;
                    HvLogHex("M0F", ((UINT64)Vcpu->CpuNumber << 56) | (Off & 0xFFFFULL));
                }
            }
            Vcpu->LastNpfBranch = 0x02;
            PassThroughLapicMmio(Vcpu, FaultGpa);
        }
        return;
    }

    // VIRT_CALL trampoline-return: target fn RETed to trampoline VA, NX
    // page exec-faulted here. Restore saved guest state + write target's
    // RAX into the cmd's Result slot. One-shot per VirtCall: clears flag.
    // Diag: if NPF lands on the trampoline page but VirtCallActive=0,
    // log XC3 — means a stale return arrived after we already cleared
    // state (could indicate AP race or repeated trampoline traffic).
    {
        UINT64 TramGpaSnap = __atomic_load_n(&gVirtCallTrampolineGpa, __ATOMIC_ACQUIRE);
        if (IsExec && TramGpaSnap != 0 &&
            (FaultGpa & ~(PAGE_SIZE_4KB - 1)) == TramGpaSnap &&
            !__atomic_load_n(&Vcpu->VirtCallActive, __ATOMIC_ACQUIRE))
        {
            HvLogHex("XC3", FaultGpa);
        }
    }
    if (IsExec && __atomic_load_n(&Vcpu->VirtCallActive, __ATOMIC_ACQUIRE)) {
        UINT64 TramGpa = __atomic_load_n(&gVirtCallTrampolineGpa, __ATOMIC_ACQUIRE);
        if (TramGpa != 0 && (FaultGpa & ~(PAGE_SIZE_4KB - 1)) == TramGpa) {
            Vcpu->LastNpfBranch = 0x0A;
            // Stash target's return value (RAX) into the cmd's Result slot.
            UINT64 ResultVa = Vcpu->VirtCallResultVa;
            UINT64 TargetRax = Vmcb->Save.Rax;
            if (ResultVa != 0) {
                UINT64 ResultPa = 0;
                if (!EFI_ERROR(TranslateGuestVirtual(gCovertClientCr3,
                                                    ResultVa, &ResultPa))) {
                    WriteGuestPhysical(ResultPa, &TargetRax, 8);
                }
            }
            // Restore guest state for the original RIP after the VMMCALL.
            Vmcb->Save.Rip    = Vcpu->VirtCallSavedRip;
            Vmcb->Save.Rsp    = Vcpu->VirtCallSavedRsp;
            Vmcb->Save.Rax    = Vcpu->VirtCallSavedRax;
            Vcpu->GuestRbx    = Vcpu->VirtCallSavedRbx;
            Vcpu->GuestRcx    = Vcpu->VirtCallSavedRcx;
            Vcpu->GuestRdx    = Vcpu->VirtCallSavedRdx;
            Vcpu->GuestR8     = Vcpu->VirtCallSavedR8;
            Vcpu->GuestR9     = Vcpu->VirtCallSavedR9;
            Vmcb->Save.Rflags = Vcpu->VirtCallSavedRflags;
            __atomic_store_n(&Vcpu->VirtCallActive, 0, __ATOMIC_RELEASE);
            HvLogHex("XC2", TargetRax);  // VIRT_CALL trampoline-return
            return;
        }
    }

    UINT64 SnapTriggerGpa = __atomic_load_n(&gCovertTriggerGpa, __ATOMIC_ACQUIRE);
    if (SnapTriggerGpa != 0 &&
        (FaultGpa & ~(PAGE_SIZE_4KB - 1)) == SnapTriggerGpa)
    {
        UINT64 SnapMailboxGpa = gCovertMailboxGpa;
        UINT64 SnapMailboxVa  = gCovertMailboxVa;
        UINT64 SnapClientCr3  = gCovertClientCr3;

        if (SnapMailboxGpa == 0 || SnapMailboxVa == 0 || SnapClientCr3 == 0) {
            Vcpu->LastNpfBranch = 0x03;
            CovertChannelTeardown(Vcpu, Vmcb, SnapTriggerGpa);
            return;
        }

        UINT64 CurrentCr3 = Vmcb->Save.Cr3 & ~0xFFFULL;
        if (CurrentCr3 != SnapClientCr3) {
            Vcpu->LastNpfBranch = 0x03;
            CovertChannelTeardown(Vcpu, Vmcb, SnapTriggerGpa);
            return;
        }

        UINT64 CurrentMailboxGpa = 0;
        if (EFI_ERROR(TranslateGuestVirtual(SnapClientCr3,
                                            SnapMailboxVa,
                                            &CurrentMailboxGpa)) ||
            (CurrentMailboxGpa & ~(PAGE_SIZE_4KB - 1)) != SnapMailboxGpa)
        {
            Vcpu->LastNpfBranch = 0x03;
            CovertChannelTeardown(Vcpu, Vmcb, SnapTriggerGpa);
            return;
        }

        COVERT_MAILBOX *Mailbox = (COVERT_MAILBOX *)(UINTN)SnapMailboxGpa;

        if (Mailbox->AuthKey != gBootAuthKey) {
            Vcpu->LastNpfBranch = 0x04;
            PNPT_CONTEXT Npt = &g_DriverContext.NptContext;
            UINT64 *Pte = NptGetPte(Npt, SnapTriggerGpa, FALSE);
            if (Pte && !(*Pte & NPT_LARGE_PAGE)) {
                *Pte |= NPT_WRITE;
            }
            Vmcb->Control.InterceptExceptions |= (1U << EXCEPTION_DB);
            Vcpu->CovertSavedRflags = Vmcb->Save.Rflags;
            Vmcb->Save.Rflags |= (1ULL << 8);
            Vcpu->CovertTriggerActive = 1;
            Vmcb->Control.TlbControl = 1;
            return;
        }

        // Replay defense — reject mailbox if Sequence <= last accepted.
        // Sequence is a UINT32 per-channel monotonic counter; clients
        // already generate it as such. Without this check a captured
        // mailbox can be replayed indefinitely.
        if ((UINT64)Mailbox->Sequence <= Vcpu->LastCovertSequence) {
            Vcpu->LastNpfBranch = 0x0A;
            PNPT_CONTEXT Npt = &g_DriverContext.NptContext;
            UINT64 *Pte = NptGetPte(Npt, SnapTriggerGpa, FALSE);
            if (Pte && !(*Pte & NPT_LARGE_PAGE)) {
                *Pte |= NPT_WRITE;
            }
            Vmcb->Control.InterceptExceptions |= (1U << EXCEPTION_DB);
            Vcpu->CovertSavedRflags = Vmcb->Save.Rflags;
            Vmcb->Save.Rflags |= (1ULL << 8);
            Vcpu->CovertTriggerActive = 1;
            Vmcb->Control.TlbControl = 1;
            return;
        }
        Vcpu->LastCovertSequence = (UINT64)Mailbox->Sequence;

        Vcpu->LastNpfBranch = 0x05;
        UINT32 NumCmds = Mailbox->NumCommands;
        if (NumCmds > COVERT_MAX_BATCH) {
            NumCmds = COVERT_MAX_BATCH;
        }
        Vcpu->LastCovertNumCmds = (UINT8)(NumCmds > 255 ? 255 : NumCmds);

        InvalidateSoftTlb(Vcpu);

        for (UINT32 ci = 0; ci < NumCmds; ci++) {
            COVERT_CMD *Cmd = &Mailbox->Cmd[ci];
            UINT64 CmdResult = 0;
            UINT32 CmdStatus = COVERT_STATUS_OK;

            Vcpu->LastCovertCmdSlot = (UINT8)(ci > 255 ? 255 : ci);
            Vcpu->LastCovertCmdId   = (UINT8)Cmd->CmdId;

            switch (Cmd->CmdId) {

            case PMC_CMD_PING:
                CmdResult = 0x48595045ULL ^ (UINT32)(gBootAuthKey >> 32);
                break;

            case PMC_CMD_SET_CR3:
                if (Cmd->Arg1 != 0) {
                    Vcpu->TargetCr3 = Cmd->Arg1;
                } else if (Vcpu->KernelCr3 != 0) {
                    Vcpu->TargetCr3 = Vcpu->KernelCr3;
                } else {
                    Vcpu->TargetCr3 = SanitizeDtb(Vmcb->Save.Cr3);
                }
                InvalidateSoftTlb(Vcpu);
                CmdResult = (Vcpu->TargetCr3 != 0) ? Vcpu->TargetCr3 : HYPE_MEM_ERR_RANGE;
                break;

            case PMC_CMD_VIRT_READ4: {
                UINT64 Pa = 0;
                if (EFI_ERROR(VirtAccessResolve(Vcpu, Cmd->Arg1, 4, &Pa))) {
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                    break;
                }
                UINT32 Val = 0;
                if (EFI_ERROR(ReadGuestPhysical(Pa, &Val, 4))) {
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                    break;
                }
                CmdResult = Val;
                break;
            }

            case PMC_CMD_VIRT_READ8: {
                UINT64 Pa = 0;
                if (EFI_ERROR(VirtAccessResolve(Vcpu, Cmd->Arg1, 8, &Pa))) {
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                    break;
                }
                UINT64 Val = 0;
                if (EFI_ERROR(ReadGuestPhysical(Pa, &Val, 8))) {
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                    break;
                }
                CmdResult = Val;
                break;
            }

            case PMC_CMD_PHYS_READ: {
                UINT64 Val = 0;
                if (EFI_ERROR(ReadGuestPhysical(Cmd->Arg1, &Val, 8))) {
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                    break;
                }
                CmdResult = Val;
                break;
            }

            case PMC_CMD_VIRT_WRITE4: {
                UINT64 Pa = 0;
                if (EFI_ERROR(VirtAccessResolve(Vcpu, Cmd->Arg1, 4, &Pa))) { CmdStatus = COVERT_STATUS_BAD_ADDR; break; }
                UINT32 Val = (UINT32)Cmd->Arg2;
                WriteGuestPhysical(Pa, &Val, 4);
                CmdResult = HYPE_MEM_OK;
                break;
            }

            case PMC_CMD_VIRT_WRITE1: {
                UINT64 Pa = 0;
                if (EFI_ERROR(VirtAccessResolve(Vcpu, Cmd->Arg1, 1, &Pa))) { CmdStatus = COVERT_STATUS_BAD_ADDR; break; }
                UINT8 Val = (UINT8)Cmd->Arg2;
                WriteGuestPhysical(Pa, &Val, 1);
                CmdResult = HYPE_MEM_OK;
                break;
            }

            case PMC_CMD_VIRT_WRITE8: {
                UINT64 Pa = 0;
                if (EFI_ERROR(VirtAccessResolve(Vcpu, Cmd->Arg1, 8, &Pa))) { CmdStatus = COVERT_STATUS_BAD_ADDR; break; }
                WriteGuestPhysical(Pa, &Cmd->Arg2, 8);
                CmdResult = HYPE_MEM_OK;
                break;
            }

            case PMC_CMD_PROC_CR3: {

                CmdResult = Vcpu->TargetCr3;
                Cmd->Arg2 = Vcpu->Cr3InterceptCapturedImageBase;
                CmdStatus = COVERT_STATUS_OK;
                HvLogHex("V6C", CmdResult);
                HvLogHex("V6D", Cmd->Arg2);
                HvLogHex("V6E", (UINT64)CmdStatus);
                break;
            }

            case PMC_CMD_CR3_INTERCEPT: {

                UINT32 NumCpus = g_DriverContext.HvState.NumCpus;
                for (UINT32 i = 0; i < NumCpus; i++) {
                    PVCPU_DATA V = &g_DriverContext.HvState.VcpuTable[i];
                    if (V->Vmcb == NULL) continue;
                    V->TargetCr3 = 0;
                    V->Cr3InterceptCapturedPeb = 0;
                    V->Cr3InterceptCapturedImageBase = 0;
                }

                if (Cmd->Arg1 == 0) {
                    __atomic_store_n(&gCr3SampleArmed, 0, __ATOMIC_RELEASE);
                    gCr3SampleCursor    = 0;
                    gCr3SamplePebVa     = 0;
                    gCr3SampleImageBase = 0;
                    gCr3SamplePid       = 0;
                } else {
                    if (Cmd->Arg2 != 0 && Cmd->Arg2 >= 0x800000000000ULL) {
                        CmdStatus = COVERT_STATUS_BAD_ADDR;
                        break;
                    }
                    if (Cmd->Arg3 != 0 && Cmd->Arg3 >= 0x800000000000ULL) {
                        CmdStatus = COVERT_STATUS_BAD_ADDR;
                        break;
                    }
                    gCr3SamplePebVa     = Cmd->Arg2;
                    gCr3SampleImageBase = Cmd->Arg3;
                    gCr3SamplePid       = 0;
                    gCr3SampleCursor    = 0x1000;
                    UINT32 StartPhase   = (Cmd->Arg3 != 0) ? 2
                                        : ((Cmd->Arg2 != 0) ? 4 : 1);
                    __atomic_store_n(&gCr3SampleArmed, StartPhase, __ATOMIC_RELEASE);
                }
                CmdResult = HYPE_MEM_OK;
                break;
            }

            case PMC_CMD_GET_INTERCEPT_PEB: {

                // Drain chunks until phase terminal or TSC budget burned.
                // 16 Mcy ≈ 4.7 ms @ 3.4 GHz — rides the 5 ms/chunk cmd-handler
                // TSC budget ceiling. 2 Mcy was undersized — 180s wallclock
                // covered ~14 GB on a 64 GB box; Apex EPROCESS lived past
                // that on some boots.
                UINT64 ScanT0 = __rdtsc();
                const UINT64 ScanBudget = 16000000;
                UINT32 Phase;
                do {
                    Cr3PassiveSample();
                    Phase = __atomic_load_n(&gCr3SampleArmed,
                                            __ATOMIC_ACQUIRE);
                } while (Phase != 0 && Phase != 3 &&
                         (__rdtsc() - ScanT0) < ScanBudget);
                UINT64 ScanDt = __rdtsc() - ScanT0;
                if (ScanDt > 2500000) {
                    HvLogHex("VAJ", ScanDt);
                    HvLogHex("VAK",
                        ((UINT64)gCr3SampleCursor << 32) | Phase);
                }
                CmdResult = Vcpu->Cr3InterceptCapturedPeb;
                Cmd->Arg2 = Phase;
                break;
            }

            case PMC_CMD_HOOK_INSTALL_DRAW: {
                CmdStatus = HookDrawHandleInstall(Vcpu, Cmd);
                CmdResult = Cmd->Result;
                break;
            }

            case PMC_CMD_HOOK_DRAW_PEEK: {
                CmdStatus = HookDrawHandlePeek(Vcpu, Cmd);
                CmdResult = Cmd->Result;
                break;
            }

            case PMC_CMD_SET_GLOW_PARAMS: {
                CmdStatus = HookDrawHandleSetGlowParams(Vcpu, Cmd);
                CmdResult = Cmd->Result;
                break;
            }

            case PMC_CMD_SET_AIM_PARAMS: {
                CmdStatus = HookAimHandleSetParams(Vcpu, Cmd);
                CmdResult = Cmd->Result;
                break;
            }

            case PMC_CMD_SET_MENU_ENABLE: {
                CmdStatus = MenuHandleSetEnable(Vcpu, Cmd);
                CmdResult = Cmd->Result;
                break;
            }

            // Trampoline registration (one-shot per boot). Client supplies
            // Arg1 = trampoline GVA (4KB-aligned scratch in client process).
            // HV translates → GPA, splits if large, sets NX so a guest RET
            // landing here exec-faults. The fault is recognized by the
            // top-of-HandleNpf VIRT_CALL trampoline branch.
            case PMC_CMD_VIRT_CALL_INIT: {
                UINT64 TramVa = Cmd->Arg1 & ~(PAGE_SIZE_4KB - 1);
                if (TramVa == 0) { CmdStatus = COVERT_STATUS_BAD_ADDR; break; }
                UINT64 TramGpa = 0;
                if (EFI_ERROR(TranslateGuestVirtual(SnapClientCr3, TramVa, &TramGpa))) {
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                    break;
                }
                UINT64 TramGpaAligned = TramGpa & ~(PAGE_SIZE_4KB - 1);
                PNPT_CONTEXT Npt = &g_DriverContext.NptContext;
                UINT64 *Pte = NptGetPte(Npt, TramGpaAligned, FALSE);
                if (!Pte) { CmdStatus = COVERT_STATUS_BAD_ADDR; break; }
                if (*Pte & NPT_LARGE_PAGE) {
                    NTSTATUS Split = NptSplitLargePageRuntime(Npt, TramGpaAligned);
                    if (!NT_SUCCESS(Split)) { CmdStatus = COVERT_STATUS_NO_SPARE; break; }
                    Pte = NptGetPte(Npt, TramGpaAligned, FALSE);
                    if (!Pte || (*Pte & NPT_LARGE_PAGE)) {
                        CmdStatus = COVERT_STATUS_NO_SPARE;
                        break;
                    }
                }
                __atomic_fetch_or(Pte, NPT_NX, __ATOMIC_ACQ_REL);
                __atomic_store_n(&gVirtCallTrampolineGpa, TramGpaAligned, __ATOMIC_RELEASE);
                __atomic_store_n(&gVirtCallTrampolineVa,  TramVa, __ATOMIC_RELEASE);
                FlushAllTlb(Vcpu);
                HvLogHex("XC0", TramGpaAligned);  // VIRT_CALL init
                CmdResult = TramGpaAligned;
                break;
            }

            // VIRT_CALL — redirect guest RIP to fn_va with rcx=Arg2, rdx=Arg3.
            // Must be the LAST cmd in batch — remaining cmds are skipped and
            // get STATUS_BAD_CMD. Trampoline must be initialized via INIT.
            // Target's RAX lands in this cmd's Result on trampoline-return.
            case PMC_CMD_VIRT_CALL: {
                UINT64 TramVa = __atomic_load_n(&gVirtCallTrampolineVa, __ATOMIC_ACQUIRE);
                if (TramVa == 0) { CmdStatus = COVERT_STATUS_BAD_ADDR; break; }
                UINT64 FnVa = Cmd->Arg1;
                if (FnVa == 0) { CmdStatus = COVERT_STATUS_BAD_ADDR; break; }
                // Verify fn_va is mapped in guest's TargetCr3 — refuses to
                // redirect into unmapped or kernel-side pages.
                UINT64 FnPa = 0;
                if (EFI_ERROR(VirtAccessResolve(Vcpu, FnVa, 1, &FnPa))) {
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                    break;
                }
                // Push trampoline VA onto guest stack: write 8B at Save.Rsp-8.
                UINT64 NewRsp = Vmcb->Save.Rsp - 8;
                UINT64 StackPa = 0;
                if (EFI_ERROR(VirtAccessResolve(Vcpu, NewRsp, 8, &StackPa))) {
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                    break;
                }
                if (EFI_ERROR(WriteGuestPhysical(StackPa, &TramVa, 8))) {
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                    break;
                }
                // Save state for the trampoline-return path to restore.
                Vcpu->VirtCallSavedRip    = Vmcb->Save.Rip;
                Vcpu->VirtCallSavedRsp    = Vmcb->Save.Rsp;
                Vcpu->VirtCallSavedRax    = Vmcb->Save.Rax;
                Vcpu->VirtCallSavedRbx    = Vcpu->GuestRbx;
                Vcpu->VirtCallSavedRcx    = Vcpu->GuestRcx;
                Vcpu->VirtCallSavedRdx    = Vcpu->GuestRdx;
                Vcpu->VirtCallSavedR8     = Vcpu->GuestR8;
                Vcpu->VirtCallSavedR9     = Vcpu->GuestR9;
                Vcpu->VirtCallSavedRflags = Vmcb->Save.Rflags;
                // Stash mailbox VA of Cmd->Result so trampoline-return can
                // write the target's RAX into it. Mailbox lives at
                // SnapMailboxVa; Cmd is at offset (cmd_idx*sizeof + header).
                Vcpu->VirtCallResultVa =
                    SnapMailboxVa
                    + __builtin_offsetof(COVERT_MAILBOX, Cmd)
                    + (UINT64)ci * sizeof(COVERT_CMD)
                    + __builtin_offsetof(COVERT_CMD, Result);

                // Inject args + redirect RIP. RCX/RDX are guest GPRs stored
                // in the per-VCPU shadow (HypeVmrun.nasm restores from
                // Vcpu->GuestRcx/Rdx before VMRUN). RSP/RIP live in VMCB.
                Vcpu->GuestRcx = Cmd->Arg2;
                Vcpu->GuestRdx = Cmd->Arg3;
                Vmcb->Save.Rsp = NewRsp;
                Vmcb->Save.Rip = FnVa;
                // Clear TF — guest fn runs free, not single-stepped.
                Vmcb->Save.Rflags &= ~(1ULL << 8);
                __atomic_store_n(&Vcpu->VirtCallActive, 1, __ATOMIC_RELEASE);

                HvLogHex("XC1", FnVa);  // VIRT_CALL fire — RIP redirected
                Cmd->Result = 0;  // placeholder; trampoline-return overwrites
                Cmd->Status = COVERT_STATUS_OK;
                // Skip remaining cmds in this batch — we're handing control
                // to the guest fn. Re-arm trigger-page write trap, then DO
                // NOT arm single-step (already cleared TF). Exit early.
                {
                    PNPT_CONTEXT Npt = &g_DriverContext.NptContext;
                    UINT64 *Pte = NptGetPte(Npt, SnapTriggerGpa, FALSE);
                    if (Pte && !(*Pte & NPT_LARGE_PAGE)) {
                        *Pte |= NPT_WRITE;
                    }
                }
                Vcpu->CovertTriggerActive = 1;
                Vmcb->Control.TlbControl = 1;
                return;
            }

            // Live HV-log read — cycle 10d-r2. Returns 8 bytes at Arg1
            // offset within the 4 MB HV debug log. Client drains by
            // iterating offsets 0..sizeof(HV_DEBUG_LOG)-8 in batches.
            case PMC_CMD_HV_LOG_READ: {
                UINT64 Off = Cmd->Arg1;
                if (!g_DebugLog || Off + 8 > sizeof(HV_DEBUG_LOG)) {
                    CmdResult = 0;
                    CmdStatus = COVERT_STATUS_BAD_ADDR;
                } else {
                    CmdResult = *(UINT64 *)((UINT8 *)g_DebugLog + Off);
                    CmdStatus = COVERT_STATUS_OK;
                }
                break;
            }

            // Cycle 15 diag: read gLapicDisarmedNpfCount without rebooting.
            // Result is the global tally; non-zero means disarmed-branch
            // hits exist (sentinel logic broken), zero means NPFs aren't
            // landing on gLapicGpa post-disarm at all.
            case PMC_CMD_GET_LAPIC_DISARMED_NPF_COUNT: {
                CmdResult = (UINT64)__atomic_load_n(&gLapicDisarmedNpfCount,
                                                   __ATOMIC_RELAXED);
                CmdStatus = COVERT_STATUS_OK;
                break;
            }

            default:
                CmdStatus = COVERT_STATUS_BAD_CMD;
                break;
            }

            Cmd->Result = CmdResult;
            Cmd->Status = CmdStatus;
        }

        {
            PNPT_CONTEXT Npt = &g_DriverContext.NptContext;
            UINT64 *Pte = NptGetPte(Npt, SnapTriggerGpa, FALSE);
            if (Pte && !(*Pte & NPT_LARGE_PAGE)) {
                *Pte |= NPT_WRITE;
            }
        }

        Vmcb->Control.InterceptExceptions |= (1U << EXCEPTION_DB);
        Vcpu->CovertSavedRflags = Vmcb->Save.Rflags;
        Vmcb->Save.Rflags |= (1ULL << 8);
        Vcpu->CovertTriggerActive = 1;
        Vmcb->Control.TlbControl = 1;
        return;
    }

    if (IsExec && DrawHookGpaMatches(FaultGpa)) {
        Vcpu->LastNpfBranch = 0x06;
        DrawHookOnNpfHit(Vcpu);
        return;
    }

    if (NptIsProtectedAddress(&g_DriverContext.NptContext, FaultGpa)) {
        Vcpu->LastNpfBranch = 0x07;

        if (IsExec) {
            HvLogHex("V01", FaultGpa);
        }
        UINT64 RemapT0 = __rdtsc();
        NTSTATUS Status = NptRemapToDecoy(&g_DriverContext.NptContext, FaultGpa);
        if (!NT_SUCCESS(Status)) {
            HvLogHex("V02", FaultGpa);
            HvLog("VGG\n"); HvLogHex("VGGRIP", Vmcb->Save.Rip); HvLogHex("VGGGPA", FaultGpa);
            InjectException(Vmcb, EXCEPTION_GP, TRUE, 0);
            return;
        }

        if (IsExec || ((ErrorCode >> 37) & 1)) {
            UINT64 *DecoyPte = NptGetPte(&g_DriverContext.NptContext,
                                         FaultGpa, FALSE);
            if (DecoyPte && (*DecoyPte & NPT_NX)) {
                *DecoyPte &= ~NPT_NX;
            }
        }

        FlushAllTlb(Vcpu);
        UINT64 RemapDt = __rdtsc() - RemapT0;
        if (RemapDt > 200000) {
            HvLogHex("VAM", RemapDt);
            HvLogHex("VAN", FaultGpa);
        }
        return;
    }

    if (FaultGpa > g_DriverContext.NptContext.MaxPhysicalAddress) {
        Vcpu->LastNpfBranch = 0x08;
        HvLogHex("V03", FaultGpa);
        HvLogHex("V04", ErrorCode);
        HvLogHex("V05", Vmcb->Save.Rip);
        HvLog("VGH\n"); HvLogHex("VGHRIP", Vmcb->Save.Rip); HvLogHex("VGHGPA", FaultGpa);
        InjectException(Vmcb, EXCEPTION_GP, TRUE, 0);
        return;
    }

    Vcpu->LastNpfBranch = 0x09;
    if (FaultGpa == Vcpu->LastUnhandledNpfGpa) {
        Vcpu->UnhandledNpfRepeats++;
    } else {
        Vcpu->LastUnhandledNpfGpa = FaultGpa;
        Vcpu->UnhandledNpfRepeats = 1;
        Vcpu->UnhandledNpfTotal++;
        static volatile UINT32 gUnhandledNpfLogCount = 0;
        if (gUnhandledNpfLogCount < 100) {
            HvLogHex("V0A", FaultGpa);
            HvLogHex("V0B", Vmcb->Save.Rip);
            HvLogHex("V0C", ((UINT64)Vcpu->CpuNumber << 32) | (UINT32)ErrorCode);
            __sync_add_and_fetch((volatile INT32 *)&gUnhandledNpfLogCount, 1);
        }
    }
    Vmcb->Control.TlbControl = 1;
}

#define IOIO_IN         (1U << 0)
#define IOIO_SZ8        (1U << 4)
#define IOIO_SZ16       (1U << 5)
#define IOIO_SZ32       (1U << 6)
#define IOIO_PORT_SHIFT 16

#define PCI_CONFIG_ADDR 0x0CF8
#define PCI_CONFIG_DATA 0x0CFC

// === SECTION: IOIO ===
UINT32 g_HiddenPciBdf = 0;

STATIC
VOID
HandleIoio(
    PVCPU_DATA Vcpu
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;
    UINT64 Info = Vmcb->Control.ExitInfo1;
    UINT16 Port = (UINT16)(Info >> IOIO_PORT_SHIFT);
    BOOLEAN IsIn = (Info & IOIO_IN) != 0;

    Vmcb->Save.Rip = Vmcb->Control.ExitInfo2;

    if (Port >= PCI_CONFIG_ADDR && Port <= PCI_CONFIG_ADDR + 3) {

        if (!IsIn) {
            if (Port == PCI_CONFIG_ADDR && (Info & IOIO_SZ32)) {
                UINT32 Val = (UINT32)Vmcb->Save.Rax;
                __asm__ volatile("outl %0, %w1" :: "a"(Val), "Nd"((UINT16)PCI_CONFIG_ADDR));
            } else if (Info & IOIO_SZ16) {
                __asm__ volatile("outw %w0, %w1" :: "a"((UINT16)Vmcb->Save.Rax), "Nd"(Port));
            } else {
                __asm__ volatile("outb %b0, %w1" :: "a"((UINT8)Vmcb->Save.Rax), "Nd"(Port));
            }

            {
                UINT32 Current;
                __asm__ volatile("inl %w1, %0" : "=a"(Current) : "Nd"((UINT16)PCI_CONFIG_ADDR));
                Vcpu->LastCf8Value = Current;
            }
        } else if (IsIn && Port == PCI_CONFIG_ADDR && (Info & IOIO_SZ32)) {
            UINT32 Val;
            __asm__ volatile("inl %w1, %0" : "=a"(Val) : "Nd"((UINT16)PCI_CONFIG_ADDR));
            Vmcb->Save.Rax = (Vmcb->Save.Rax & ~0xFFFFFFFFULL) | Val;
        } else {
            goto ioio_emulate;
        }
    } else if (Port >= PCI_CONFIG_DATA && Port <= PCI_CONFIG_DATA + 3) {

        UINT32 Cf8 = Vcpu->LastCf8Value;
        BOOLEAN Hide = FALSE;

        if (g_HiddenPciBdf != 0 && (Cf8 & 0x80000000U)) {
            UINT16 Bdf = (UINT16)((Cf8 >> 8) & 0xFFFF);
            Hide = (Bdf == (UINT16)g_HiddenPciBdf);
        }

        if (Hide) {
            if (IsIn) {

                if (Info & IOIO_SZ32) {
                    Vmcb->Save.Rax = (Vmcb->Save.Rax & ~0xFFFFFFFFULL) | 0xFFFFFFFFU;
                } else if (Info & IOIO_SZ16) {
                    Vmcb->Save.Rax = (Vmcb->Save.Rax & ~0xFFFFULL) | 0xFFFFU;
                } else {
                    Vmcb->Save.Rax = (Vmcb->Save.Rax & ~0xFFULL) | 0xFFU;
                }
            }

        } else {
            goto ioio_emulate;
        }
    } else {
        goto ioio_emulate;
    }
    return;

ioio_emulate:
    if (IsIn) {
        if (Info & IOIO_SZ32) {
            UINT32 Val;
            __asm__ volatile("inl %w1, %0" : "=a"(Val) : "Nd"(Port));
            Vmcb->Save.Rax = (Vmcb->Save.Rax & ~0xFFFFFFFFULL) | Val;
        } else if (Info & IOIO_SZ16) {
            UINT16 Val;
            __asm__ volatile("inw %w1, %w0" : "=a"(Val) : "Nd"(Port));
            Vmcb->Save.Rax = (Vmcb->Save.Rax & ~0xFFFFULL) | Val;
        } else {
            UINT8 Val;
            __asm__ volatile("inb %w1, %b0" : "=a"(Val) : "Nd"(Port));
            Vmcb->Save.Rax = (Vmcb->Save.Rax & ~0xFFULL) | Val;
        }
    } else {
        if (Info & IOIO_SZ32) {
            __asm__ volatile("outl %0, %w1" :: "a"((UINT32)Vmcb->Save.Rax), "Nd"(Port));
        } else if (Info & IOIO_SZ16) {
            __asm__ volatile("outw %w0, %w1" :: "a"((UINT16)Vmcb->Save.Rax), "Nd"(Port));
        } else {
            __asm__ volatile("outb %b0, %w1" :: "a"((UINT8)Vmcb->Save.Rax), "Nd"(Port));
        }
    }
}

// === SECTION: SX_INIT ===
STATIC
VOID
HandleSx(
    PVCPU_DATA Vcpu
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;
    UINT64 OldCr0 = Vmcb->Save.Cr0;
    UINT8  Vec;

    HvLogHex("V91", (UINT64)Vcpu->ApicId);

    if (Vcpu->SipiApplied) {
        HvLogHex("V9A", (UINT64)Vcpu->ApicId);
        return;
    }

    InterlockedCompareExchange(
        (volatile LONG *)&Vcpu->ActivityState,
        (LONG)GUEST_WFS,
        (LONG)GUEST_ACTIVE
    );

    Vmcb->Control.EventInj = 0;

    Vmcb->Save.CS.Selector = 0xF000;
    Vmcb->Save.CS.Base     = 0xFFFF0000ULL;
    Vmcb->Save.CS.Limit    = 0xFFFF;
    Vmcb->Save.CS.Attrib   = 0x9B;

    Vmcb->Save.DS.Selector = 0; Vmcb->Save.DS.Base = 0;
    Vmcb->Save.DS.Limit    = 0xFFFF; Vmcb->Save.DS.Attrib = 0x93;
    Vmcb->Save.ES.Selector = 0; Vmcb->Save.ES.Base = 0;
    Vmcb->Save.ES.Limit    = 0xFFFF; Vmcb->Save.ES.Attrib = 0x93;
    Vmcb->Save.FS.Selector = 0; Vmcb->Save.FS.Base = 0;
    Vmcb->Save.FS.Limit    = 0xFFFF; Vmcb->Save.FS.Attrib = 0x93;
    Vmcb->Save.GS.Selector = 0; Vmcb->Save.GS.Base = 0;
    Vmcb->Save.GS.Limit    = 0xFFFF; Vmcb->Save.GS.Attrib = 0x93;
    Vmcb->Save.SS.Selector = 0; Vmcb->Save.SS.Base = 0;
    Vmcb->Save.SS.Limit    = 0xFFFF; Vmcb->Save.SS.Attrib = 0x93;

    Vmcb->Save.Rip    = 0xFFF0ULL;
    Vmcb->Save.Rflags = 0x2ULL;

    Vmcb->Save.Cr0 = (OldCr0 & ((1ULL << 29) | (1ULL << 30))) | (1ULL << 4);
    Vmcb->Save.Cr2 = 0;
    Vmcb->Save.Cr3 = 0;
    Vmcb->Save.Cr4 = 0;

    Vmcb->Save.Dr6 = 0xFFFF0FF0ULL;
    Vmcb->Save.Dr7 = 0x00000400ULL;

    Vmcb->Save.GDTR.Base = 0; Vmcb->Save.GDTR.Limit = 0xFFFF;
    Vmcb->Save.IDTR.Base = 0; Vmcb->Save.IDTR.Limit = 0xFFFF;
    Vmcb->Save.LDTR.Selector = 0; Vmcb->Save.LDTR.Base = 0;
    Vmcb->Save.LDTR.Limit    = 0xFFFF; Vmcb->Save.LDTR.Attrib = 0x82;
    Vmcb->Save.TR.Selector   = 0; Vmcb->Save.TR.Base   = 0;
    Vmcb->Save.TR.Limit      = 0xFFFF; Vmcb->Save.TR.Attrib = 0x8B;

    Vmcb->Save.Efer = EFER_SVME;

    {
        UINT64 SpinIters = 0;
        while (InterlockedCompareExchange(
                   (volatile LONG *)&Vcpu->ActivityState,
                   (LONG)GUEST_ACTIVE,
                   (LONG)GUEST_SIPI_ISSUED
               ) != (LONG)GUEST_SIPI_ISSUED) {
            CpuPause();
            if ((++SpinIters & ((1ULL << 20) - 1)) == 0) {
                HvLogHex("V8A", ((UINT64)Vcpu->ApicId << 32) | (UINT32)Vcpu->ActivityState);
                if (SpinIters >= (64ULL << 20)) {
                    HvLogHex("V90", (UINT64)Vcpu->ApicId);
                    break;
                }
            }
        }
    }

    Vec = Vcpu->SipiVector;
    Vmcb->Save.CS.Selector = (UINT16)Vec << 8;
    Vmcb->Save.CS.Base     = (UINT64)Vec << 12;
    Vmcb->Save.Rip         = 0;
    HvLogHex("V92", (UINT64)Vec);

    Vcpu->SipiApplied = 1;

    Vmcb->Control.VmcbClean = 0;
    Vmcb->Control.TlbControl = 1;
}

// === SECTION: INVALID ===
STATIC
VOID
HandleInvalid(
    PVCPU_DATA Vcpu
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;

    HvLog("V51\n");
    HvLogHex("V06", (UINT64)Vcpu->CpuNumber);
    HvLogHex("V07", Vmcb->Save.Rip);
    HvLogHex("V08", Vmcb->Save.Cr0);
    HvLogHex("V09", Vmcb->Save.Cr3);
    HvLogHex("V10", Vmcb->Save.Cr4);
    HvLogHex("V11", Vmcb->Save.Efer);
    HvLogHex("V12", (UINT64)Vmcb->Control.GuestAsid);
    HvLogHex("V13", (UINT64)Vmcb->Control.InterceptMisc2);
    HvLogHex("V14", Vmcb->Control.MsrpmBasePA);
    HvLogHex("V15", Vmcb->Control.NCr3);
    HvLogHex("V16", (UINT64)Vmcb->Save.CS.Attrib);
    HvLogHex("V17", (UINT64)Vmcb->Save.TR.Attrib);

    HvLog("V52\n");
    while (1) { __asm__ volatile("cli; hlt"); }
}

// === SECTION: DISPATCH ===
VOID
VmexitHandler(
    PVCPU_DATA Vcpu
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;

    UINT64 ExitCode = Vmcb->Control.ExitCode;
    UINT64 EntryTsc = __rdtsc();

    if ((Vcpu->HeartbeatLogged & 1) == 0) {
        Vcpu->HeartbeatLogged |= 1;
        HvLogHex("VB0", (UINT64)Vcpu->CpuNumber);
    }
    // H1: per-CPU cache-attr snapshot (PAT/MTRRdef/MTRRcap). One-shot per CPU.
    if ((Vcpu->HeartbeatLogged & 2) == 0) {
        Vcpu->HeartbeatLogged |= 2;
        HvLogHex("VAA", __readmsr(0x277));
        HvLogHex("VAB", ((UINT64)Vcpu->CpuNumber << 32)
                        | (UINT32)__readmsr(0x2FF));
        HvLogHex("VAC", __readmsr(0xFE));
    }
    // Cycle 13.1: per-CPU one-shot APIC_BASE snapshot. Bit 10 = x2APIC mode.
    // Pairs with M0D/M0E below to definitively answer xAPIC vs x2APIC.
    if ((Vcpu->HeartbeatLogged & 4) == 0) {
        Vcpu->HeartbeatLogged |= 4;
        HvLogHex("M0A", ((UINT64)Vcpu->CpuNumber << 56) | __readmsr(0x1B));
    }
    if (Vcpu->CpuNumber == 0 && (Vcpu->VmexitCount & 0x3FF) == 0) {
        HvLogHex("VB1", Vcpu->VmexitCount);
    }

    // Cycle 5: max-cadence rearm. Call DrawHookRearm() on every VMEXIT.
    // Cheap fast-path return when NX is already set (most VMEXITs); the
    // expensive path (atomic OR + per-VCPU TLB-flush bits) only runs when
    // NPF just cleared NX. Result: rearm happens ~1 ms after each NPF
    // instead of waiting for the next 228 ms heartbeat. DR0 / DRH now
    // fire at NPF rate (bounded by guest exec rate, not HV cadence).
    DrawHookRearm();

    // Rearm on ANY CPU every 256 exits — BSP can be parked on Win11+SMT-off,
    // but the render-thread CPU has plenty of VMEXITs we should ride.
    if ((Vcpu->VmexitCount & 0xFF) == 0) {
        // Diag: prove this branch is reached. Global cap at 256.
        static volatile INT32 gVbeLogged = 0;
        if (__atomic_load_n(&gVbeLogged, __ATOMIC_RELAXED) < 256) {
            __sync_add_and_fetch(&gVbeLogged, 1);
            HvLogHex("VBE", ((UINT64)Vcpu->CpuNumber << 32) | Vcpu->VmexitCount);
        }
        // RIP-histogram sampler: every 256 VMEXITs per CPU, log guest RIP.
        // CPL=3 filter — HLT intercept makes kernel-idle dominate otherwise.
        // Cap 2048 globally (~50 KB log). Per-frame user code emerges as
        // repeating RIP-pages.
        if (Vmcb->Save.Cpl == 3) {
            static volatile INT32 gVrpLogged = 0;
            if (__atomic_load_n(&gVrpLogged, __ATOMIC_RELAXED) < 2048) {
                __sync_add_and_fetch(&gVrpLogged, 1);
                HvLogHex("VRP", Vmcb->Save.Rip);
            }
        }
        // Cycle 5: rearm now happens at every-VMEXIT cadence (top of handler).
        // Heartbeat is just the log-flush point — sample NPF total and emit
        // DRH/DRF if anything happened this window. DRH = NPF count (true
        // exec rate), DRF = hook GPA (sanity-confirm same page is armed).
        UINT64 NpfDelta = DrawHookSampleNpfTotal();
        if (NpfDelta != 0) {
            HvLogHex("DRH", NpfDelta);
            HvLogHex("DRF", DrawHookCurrentGpa());
        }
    }

    {
        static volatile UINT32 gFirstExitTraceCount = 0;
        if (Vcpu->CpuNumber == 0 && gFirstExitTraceCount < 32) {
            HvLogHex("VE1", ExitCode);
            HvLogHex("VE2", Vmcb->Save.Rip);
            __sync_add_and_fetch((volatile INT32 *)&gFirstExitTraceCount, 1);
        }
    }

    if (ExitCode != VMEXIT_MSR) {
        UINT32 Slot = Vcpu->NonMsrRingIndex & 0xF;
        Vcpu->NonMsrRingExitCode[Slot]   = ExitCode;
        Vcpu->NonMsrRingRip[Slot]        = Vmcb->Save.Rip;
        Vcpu->NonMsrRingExitInfo2[Slot]  = Vmcb->Control.ExitInfo2;
        Vcpu->NonMsrRingIndex            = (Slot + 1) & 0xF;
        Vcpu->NonMsrRingCount++;
    }

    if ((Vcpu->VmexitCount & 0x3F) == 0 && Vcpu->VmcbCanaryCorruptedAt == 0) {
        UINT8 *VmcbRaw = (UINT8 *)Vmcb;
        UINT64 CanaryCtl  = *(UINT64 *)(VmcbRaw + VMCB_CANARY_OFFSET_CTL);
        UINT64 CanarySave = *(UINT64 *)(VmcbRaw + VMCB_CANARY_OFFSET_SAVE);
        if (CanaryCtl != gBootCanary || CanarySave != gBootCanary) {
            Vcpu->VmcbCanaryCorruptedAt = Vcpu->VmexitCount;
            HvLog("V53\n");
            HvLogHex("VK0", (UINT64)Vcpu->CpuNumber);
            HvLogHex("VK1", Vcpu->VmexitCount);
            HvLogHex("VK2", CanaryCtl);
            HvLogHex("VK3", CanarySave);
            HvLogHex("VK4", Vmcb->Save.Rip);
            HvLogHex("VK5", Vmcb->Save.Rsp);
            HvLogHex("VK6", Vmcb->Save.Rflags);
            HvLogHex("VK7", Vmcb->Save.IDTR.Base);
            HvLogHex("VK8", (UINT64)Vmcb->Save.IDTR.Limit);
            HvLogHex("VK9", ExitCode);
        }
    }

    if ((Vcpu->VmexitCount & 0x3FF) == 0) {
        UINT64 *VmcbPte = NptGetPte(&g_DriverContext.NptContext,
                                     Vcpu->VmcbPhysical, FALSE);
        UINT64 PteVal = VmcbPte ? *VmcbPte : 0xBAD;
        BOOLEAN PtePresent = (PteVal & NPT_PRESENT) != 0;
        UINT64 PtePageFrame = PteVal & NPT_ADDR_MASK;
        UINT64 VmcbPageFrame = Vcpu->VmcbPhysical & NPT_ADDR_MASK;
        if (PtePresent && PtePageFrame == VmcbPageFrame) {

            HvLog("V54\n");
            HvLogHex("VKA", (UINT64)Vcpu->CpuNumber);
            HvLogHex("VKB", Vcpu->VmexitCount);
            HvLogHex("VKC", PteVal);
            HvLogHex("VKD", Vcpu->VmcbPhysical);
        } else if (Vcpu->VmexitCount <= 1024) {
            HvLogHex("VKE", PteVal);
            HvLogHex("VKF", Vcpu->VmcbPhysical);
            if (!PtePresent) {
                HvLog("V55\n");
            } else {
                HvLog("V56\n");
            }
        }
    }

    #define CLEAN_SAVE_ALL (VMCB_CLEAN_TPR | VMCB_CLEAN_NP | VMCB_CLEAN_CRX | \
                            VMCB_CLEAN_DRX | VMCB_CLEAN_DT | VMCB_CLEAN_SEG | \
                            VMCB_CLEAN_CR2)

    #define CLEAN_BASE     (VMCB_CLEAN_IOMSRPM | VMCB_CLEAN_LBR | VMCB_CLEAN_AVIC)

    UINT32 CleanBits = CLEAN_BASE;

    BOOLEAN HasPendingExitIntInfo =
        (Vmcb->Control.ExitIntInfo & EVENT_INJ_VALID) != 0;

    switch (ExitCode) {

    case (VMEXIT_EXCP_BASE + EXCEPTION_SX): {
        HandleSx(Vcpu);
        CleanBits = 0;
        break;
    }

    case (VMEXIT_EXCP_BASE + EXCEPTION_DB): {
        BOOLEAN HandledDb = FALSE;

        if (Vcpu->CovertTriggerActive) {
            PNPT_CONTEXT Npt = &g_DriverContext.NptContext;

            UINT64 TriggerGpa = __atomic_load_n(&gCovertTriggerGpa, __ATOMIC_ACQUIRE);
            if (TriggerGpa != 0) {
                UINT64 *Pte = NptGetPte(Npt, TriggerGpa, FALSE);
                if (Pte && !(*Pte & NPT_LARGE_PAGE)) {
                    *Pte &= ~NPT_WRITE;
                }
            }
            if (!HandledDb) {
                Vmcb->Save.Rflags = (Vmcb->Save.Rflags & ~(1ULL << 8)) |
                                    (Vcpu->CovertSavedRflags & (1ULL << 8));
            }
            Vcpu->CovertTriggerActive = 0;
            Vmcb->Control.TlbControl = 1;
            HandledDb = TRUE;
        }

        if (HandledDb && !Vcpu->CovertTriggerActive) {
            Vmcb->Control.InterceptExceptions &= ~(1U << EXCEPTION_DB);
        }

        if (!HandledDb) {
            // APM §15.12.1: intercepted #DB doesn't auto-update DR6; merge ExitInfo1 trigger bits.
            Vmcb->Save.Dr6 |= (Vmcb->Control.ExitInfo1 & 0x0000E00FULL);
            InjectException(Vmcb, EXCEPTION_DB, FALSE, 0);
            CleanBits |= (CLEAN_SAVE_ALL & ~VMCB_CLEAN_DRX);
        } else {
            CleanBits |= CLEAN_SAVE_ALL;
        }
        break;
    }

    case VMEXIT_IOIO:
        HandleIoio(Vcpu);
        CleanBits |= CLEAN_SAVE_ALL | VMCB_CLEAN_ASID;
        break;

    case VMEXIT_MSR: {
        HandleMsr(Vcpu);
        UINT32 Bits = VMCB_CLEAN_TPR | VMCB_CLEAN_NP | VMCB_CLEAN_DRX |
                      VMCB_CLEAN_DT | VMCB_CLEAN_SEG | VMCB_CLEAN_CR2 |
                      VMCB_CLEAN_ASID;
        if ((UINT32)Vcpu->GuestRcx != MSR_EFER ||
            !(Vmcb->Control.ExitInfo1 & 1)) {
            Bits |= VMCB_CLEAN_CRX;
        }
        CleanBits |= Bits;
        break;
    }

    case VMEXIT_VMRUN:
        HandleVmrun(Vcpu);
        CleanBits |= CLEAN_SAVE_ALL | VMCB_CLEAN_ASID;
        break;

    case VMEXIT_VMMCALL:
        HandleVmmcall(Vcpu);
        CleanBits |= CLEAN_SAVE_ALL;
        break;

    case VMEXIT_VMLOAD:
    case VMEXIT_VMSAVE:
    case VMEXIT_STGI:
    case VMEXIT_CLGI:
    case VMEXIT_SKINIT:
        InjectException(Vmcb, EXCEPTION_UD, FALSE, 0);
        CleanBits |= CLEAN_SAVE_ALL | VMCB_CLEAN_ASID;
        break;

    case VMEXIT_NPF:
        HandleNpf(Vcpu);
        CleanBits |= VMCB_CLEAN_TPR | VMCB_CLEAN_CRX | VMCB_CLEAN_DRX |
                     VMCB_CLEAN_DT | VMCB_CLEAN_SEG;
        if (!((Vmcb->Control.EventInj & EVENT_INJ_VALID) &&
              (Vmcb->Control.EventInj & 0xFF) == EXCEPTION_PF)) {
            CleanBits |= VMCB_CLEAN_CR2;
        }
        break;

    case VMEXIT_SHUTDOWN:
        HvLog("V58\n");
        HvLogHex("V35", Vmcb->Save.Rip);
        HvLogHex("V36", Vmcb->Save.Rsp);
        HvLogHex("V37", Vmcb->Save.Cr3);
        HvLogHex("V38", Vmcb->Save.Cr0);
        HvLogHex("V39", Vmcb->Save.Efer);
        HvLogHex("V40", Vmcb->Control.ExitInfo1);
        HvLogHex("V41", Vmcb->Control.ExitInfo2);
        HvLogHex("V9B", (UINT64)Vcpu->CpuNumber);
        __sync_synchronize();

        if (Vcpu->CpuNumber == 0) {
            InterlockedExchange(&Vcpu->ShouldExit, 1);
            break;
        }
        while (1) { __asm__ volatile("cli; hlt"); }

    case VMEXIT_INVALID:
        HandleInvalid(Vcpu);
        break;

    default:
        HvLogHex("V42", ExitCode);
        HvLogHex("V43", Vmcb->Save.Rip);
        HvLogHex("V44", Vmcb->Save.Cr3);
        HvLogHex("V45", Vmcb->Control.ExitInfo1);
        HvLogHex("V46", Vmcb->Control.ExitInfo2);
        while (1) { __asm__ volatile("cli; hlt"); }
    }

    if (HasPendingExitIntInfo && !(Vmcb->Control.EventInj & EVENT_INJ_VALID)) {
        Vmcb->Control.EventInj = Vmcb->Control.ExitIntInfo;
    }

    if (Vmcb->Control.TlbControl != 0) {
        CleanBits &= ~VMCB_CLEAN_ASID;
    }
    Vmcb->Control.VmcbClean = CleanBits;

    // H2: slow-VMEXIT detector. ~60us at 3.4GHz = 200K cycles.
    UINT64 DeltaTsc = __rdtsc() - EntryTsc;
    if (DeltaTsc > 200000) {
        HvLogHex("VAD", DeltaTsc);
        HvLogHex("VAE", ((UINT64)Vcpu->CpuNumber << 32) | (UINT32)ExitCode);
        UINT64 BranchedGpa =
            ((UINT64)Vcpu->LastNpfBranch << 56)
            | (Vmcb->Control.ExitInfo2 & 0x00FFFFFFFFFFFFFFULL);
        HvLogHex("VAG", BranchedGpa);
        HvLogHex("VAH", Vmcb->Save.Rip);
        HvLogHex("VAI", Vmcb->Save.Cr3);
        if (Vcpu->LastNpfBranch == 0x05) {
            UINT64 CovertPacked =
                ((UINT64)Vcpu->LastCovertNumCmds << 32)
                | ((UINT64)Vcpu->LastCovertCmdSlot << 16)
                | (UINT64)Vcpu->LastCovertCmdId;
            HvLogHex("VAL", CovertPacked);
        }
    }
}

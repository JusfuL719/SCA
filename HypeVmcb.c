#include "HypeSvm.h"
#include "HypeVmcb.h"

#pragma pack(push, 1)
typedef struct _SEGMENT_DESCRIPTOR {
    UINT16  LimitLow;
    UINT16  BaseLow;
    UINT8   BaseMiddle;
    UINT8   Access;
    UINT8   Granularity;
    UINT8   BaseHigh;
} SEGMENT_DESCRIPTOR;

typedef struct _SEGMENT_DESCRIPTOR_64 {
    UINT16  LimitLow;
    UINT16  BaseLow;
    UINT8   BaseMiddle;
    UINT8   Access;
    UINT8   Granularity;
    UINT8   BaseHigh;
    UINT32  BaseUpper;
    UINT32  Reserved;
} SEGMENT_DESCRIPTOR_64;
#pragma pack(pop)

STATIC
VOID
CaptureSegment(
    VMCB_SEGMENT    *Seg,
    UINT16          Selector,
    UINT64          GdtBase
    )
{
    Seg->Selector = Selector;

    if (Selector == 0) {
        Seg->Base = 0;
        Seg->Limit = 0;

        Seg->Attrib = 0x93;
        return;
    }

    SEGMENT_DESCRIPTOR *Desc = (SEGMENT_DESCRIPTOR *)(GdtBase + (Selector & ~7));

    UINT64 Base = Desc->BaseLow |
                  ((UINT64)Desc->BaseMiddle << 16) |
                  ((UINT64)Desc->BaseHigh << 24);
    if (!(Desc->Access & 0x10)) {
        SEGMENT_DESCRIPTOR_64 *Desc64 = (SEGMENT_DESCRIPTOR_64 *)Desc;
        Base |= ((UINT64)Desc64->BaseUpper << 32);
    }
    Seg->Base = Base;

    UINT32 Limit = Desc->LimitLow | ((UINT32)(Desc->Granularity & 0x0F) << 16);
    if (Desc->Granularity & 0x80) {
        Limit = (Limit << 12) | 0xFFF;
    }
    Seg->Limit = Limit;

    Seg->Attrib = (Desc->Access & 0xFF) |
                  (((UINT16)Desc->Granularity >> 4) << 8);
}

VOID
VmcbSetupMsrpm(
    UINT8 *Msrpm
    )
{
    ZeroMem(Msrpm, MSRPM_SIZE);

    Msrpm[0x0820] |= 0x03;

    Msrpm[0x1045] |= 0x03;

    Msrpm[0x1045] |= 0xC0;

    Msrpm[0x0006] |= 0x80;

    Msrpm[0x020C] |= 0x02;

    // Cycle 13: timer-rearm intercepts. Two MSRPM write bits.
    //   MSR 0x6E0 IA32_TSC_DEADLINE: bit_index 0x6E0*2 = 0xDC0 → byte 0x1B8 bit 1
    //   MSR 0x80B x2APIC EOI:        bit_index 0x80B*2 = 0x1016 → byte 0x202 bit 7
    // Offline audit: zero observed reads/writes of either (Unicorn sweep, 4,865 fns).
    Msrpm[0x01B8] |= 0x02;
    Msrpm[0x0202] |= 0x80;
}

extern UINT32 g_HiddenPciBdf;

VOID
VmcbSetupIopm(
    UINT8 *Iopm
    )
{
    ZeroMem(Iopm, IOPM_SIZE);

    if (g_HiddenPciBdf != 0) {
        Iopm[0x19F] = 0xFF;
    }
}

VOID
VmcbSetupIntercepts(
    PVMCB Vmcb
    )
{

    Vmcb->Control.InterceptMisc1 = INTERCEPT_MSR |
                                   INTERCEPT_SHUTDOWN;

    // IOIO intercept is only useful while the hide-FPGA-BDF protocol is
    // active (Communication.md → HandleIoio). Gate on the same flag
    // VmcbSetupIopm uses to skip the per-PCI-cfg-cycle VMEXIT tax otherwise.
    if (g_HiddenPciBdf != 0) {
        Vmcb->Control.InterceptMisc1 |= INTERCEPT_IOIO;
    }

    Vmcb->Control.InterceptMisc2 = INTERCEPT_VMRUN | INTERCEPT_VMMCALL |
                                   INTERCEPT_VMLOAD | INTERCEPT_VMSAVE |
                                   INTERCEPT_STGI | INTERCEPT_CLGI |
                                   INTERCEPT_SKINIT;

    Vmcb->Control.InterceptExceptions = (1U << EXCEPTION_SX);
}

NTSTATUS
VmcbInitialize(
    PVCPU_DATA Vcpu
    )
{
    PVMCB Vmcb = Vcpu->Vmcb;

    ZeroMem(Vmcb, sizeof(VMCB));

    VmcbSetupIntercepts(Vmcb);

    Vmcb->Control.IopmBasePA = Vcpu->IopmPhysical;
    Vmcb->Control.MsrpmBasePA = Vcpu->MsrpmPhysical;

    Vmcb->Control.GuestAsid = Vcpu->CpuNumber + 1;

    Vmcb->Control.NpEnable = 3;
    Vmcb->Control.NCr3 = Vcpu->NptPml4Physical;

    if (SvmCheckLbrvSupport()) {
        Vmcb->Control.LbrVirt = 1;
    }

    {
        UINT8 *VmcbRaw = (UINT8 *)Vmcb;
        *(UINT64 *)(VmcbRaw + VMCB_CANARY_OFFSET_CTL)  = gBootCanary;
        *(UINT64 *)(VmcbRaw + VMCB_CANARY_OFFSET_SAVE) = gBootCanary;
        Vcpu->VmcbCanaryValue = gBootCanary;
        Vcpu->VmcbCanaryCorruptedAt = 0;
    }

    return STATUS_SUCCESS;
}

VOID
VmcbCaptureGuestState(
    PVMCB       Vmcb,
    PVCPU_DATA  Vcpu
    )
{
    DESCRIPTOR_TABLE_REGISTER Gdtr, Idtr;

    (VOID)Vcpu;

    __sgdt(&Gdtr);
    __sidt(&Idtr);

    CaptureSegment(&Vmcb->Save.CS, __readcs(), Gdtr.Base);

    ASSERT((Vmcb->Save.CS.Attrib & 0x600) == 0x200);

    CaptureSegment(&Vmcb->Save.DS, __readds(), Gdtr.Base);
    CaptureSegment(&Vmcb->Save.ES, __reades(), Gdtr.Base);
    CaptureSegment(&Vmcb->Save.SS, __readss(), Gdtr.Base);
    CaptureSegment(&Vmcb->Save.FS, __readfs(), Gdtr.Base);
    CaptureSegment(&Vmcb->Save.GS, __readgs(), Gdtr.Base);
    CaptureSegment(&Vmcb->Save.TR, __readtr(), Gdtr.Base);

    {
        UINT8 TrType = Vmcb->Save.TR.Attrib & 0x0F;
        BOOLEAN TrPresent = (Vmcb->Save.TR.Attrib & 0x80) != 0;
        if ((TrType != 0x09 && TrType != 0x0B) || !TrPresent) {
            Vmcb->Save.TR.Attrib = 0x008B;
            Vmcb->Save.TR.Limit = 0x67;
        }
    }

    CaptureSegment(&Vmcb->Save.LDTR, __readldtr(), Gdtr.Base);

    Vmcb->Save.GDTR.Base = Gdtr.Base;
    Vmcb->Save.GDTR.Limit = Gdtr.Limit;
    Vmcb->Save.IDTR.Base = Idtr.Base;
    Vmcb->Save.IDTR.Limit = Idtr.Limit;

    Vmcb->Save.Cr0 = __readcr0();
    ASSERT((Vmcb->Save.Cr0 & 0x1FFAFFC0) == 0);

    Vmcb->Save.Cr3 = __readcr3();
    Vmcb->Save.Cr4 = __readcr4();
    Vmcb->Save.Cr2 = __readcr2();
    Vmcb->Save.Efer = __readmsr(MSR_EFER) & 0xFFFFULL;
    Vmcb->Save.Rflags = __readeflags();

    Vmcb->Save.Cpl = 0;

    Vmcb->Save.Dr6 = __readdr(6) & 0xFFFFFFFFULL;
    Vmcb->Save.Dr7 = __readdr(7) & 0xFFFFFFFFULL;

    Vmcb->Save.Star = __readmsr(0xC0000081);
    Vmcb->Save.Lstar = __readmsr(0xC0000082);
    Vmcb->Save.Cstar = __readmsr(0xC0000083);
    Vmcb->Save.Sfmask = __readmsr(0xC0000084);
    Vmcb->Save.KernelGsBase = __readmsr(0xC0000102);
    Vmcb->Save.GPat = __readmsr(0x277);

    Vmcb->Save.SysenterCs  = __readmsr(0x174);
    Vmcb->Save.SysenterEsp = __readmsr(0x175);
    Vmcb->Save.SysenterEip = __readmsr(0x176);

    Vmcb->Save.DbgCtl = __readmsr(0x1D9);
}

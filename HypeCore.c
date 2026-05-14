#include "HypeContext.h"
#include "HypeDebug.h"

DRIVER_CONTEXT g_DriverContext = {0};

// Plain UINT64 — immune to SVAM PA→KVA pointer rewrite that would break
// NPT_CONTEXT* deref in identity-mapped host context.
UINT64 gEffectiveLimit = 0;

extern EFI_MP_SERVICES_PROTOCOL *gMpServices;
extern UINT64 gHypeLoaderImageBase;
extern UINT64 gHypeLoaderImageSize;

#define AllocateContiguousPages(Size, OutPa)  AllocateContiguousUefi((Size), (UINT64 *)(OutPa))
#define FreeContiguousPages(Va, Size)         FreeContiguousUefi((Va), (Size))

#ifndef PROCESSOR_ENABLED_BIT
#define PROCESSOR_ENABLED_BIT   0x01
#endif
#ifndef PROCESSOR_AS_BSP_BIT
#define PROCESSOR_AS_BSP_BIT    0x02
#endif

STATIC
NTSTATUS
AllocateVcpuTable(
    PHYPERVISOR_STATE HvState
    )
{
    UINTN   NumProcessors;
    UINTN   NumEnabled;
    UINTN   Size;
    UINT32  VcpuIdx;
    EFI_PROCESSOR_INFORMATION ProcInfo;
    EFI_STATUS Status;

    if (gMpServices) {
        Status = gMpServices->GetNumberOfProcessors(gMpServices, &NumProcessors, &NumEnabled);
        if (EFI_ERROR(Status)) {
            return STATUS_UNSUCCESSFUL;
        }
    } else {
        NumProcessors = 1;
        NumEnabled = 1;
    }

    for (UINTN i = 0; i < 256; i++) {
        HvState->ProcessorIndexMap[i] = 0xFFFFFFFF;
    }
    HvState->MaxProcessorId = (UINT32)NumProcessors;

    // RuntimeServicesData survives EBS.
    Size = NumEnabled * sizeof(VCPU_DATA);
    {
        EFI_PHYSICAL_ADDRESS TablePa = HV_ALLOC_MAX_ADDRESS;
        EFI_STATUS AllocStatus = gBS->AllocatePages(
            AllocateMaxAddress,
            EfiRuntimeServicesData,
            EFI_SIZE_TO_PAGES(Size),
            &TablePa
        );
        if (EFI_ERROR(AllocStatus)) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        HvState->VcpuTable = (PVCPU_DATA)(UINTN)TablePa;
    }
    ZeroMem(HvState->VcpuTable, Size);
    HvState->NumCpus = (UINT32)NumEnabled;

    // INVARIANT: BSP always at VcpuTable[0]; APs fill from index 1.
    VcpuIdx = 0;
    for (UINTN i = 0; i < NumProcessors && i < 256; i++) {
        if (!gMpServices) {
            HvState->ProcessorIndexMap[0] = 0;
            HvState->VcpuTable[0].CpuNumber = 0;
            HvState->VcpuTable[0].Self = &HvState->VcpuTable[0];
            HvState->VcpuTable[0].HvState = HvState;
            break;
        }

        Status = gMpServices->GetProcessorInfo(gMpServices, i, &ProcInfo);
        if (EFI_ERROR(Status)) {
            continue;
        }

        if (ProcInfo.StatusFlag & PROCESSOR_AS_BSP_BIT) {
            HvState->ProcessorIndexMap[i] = 0;
            HvState->VcpuTable[0].CpuNumber = (UINT32)i;
            HvState->VcpuTable[0].ApicId = (UINT32)ProcInfo.ProcessorId;
            HvState->VcpuTable[0].Self = &HvState->VcpuTable[0];
            HvState->VcpuTable[0].HvState = HvState;
            if (VcpuIdx == 0) VcpuIdx = 1;
        } else if (ProcInfo.StatusFlag & PROCESSOR_ENABLED_BIT) {
            if (VcpuIdx == 0) VcpuIdx = 1;
            if (VcpuIdx < NumEnabled) {
                HvState->ProcessorIndexMap[i] = VcpuIdx;
                HvState->VcpuTable[VcpuIdx].CpuNumber = (UINT32)i;
                HvState->VcpuTable[VcpuIdx].ApicId = (UINT32)ProcInfo.ProcessorId;
                HvState->VcpuTable[VcpuIdx].Self = &HvState->VcpuTable[VcpuIdx];
                HvState->VcpuTable[VcpuIdx].HvState = HvState;
                VcpuIdx++;
            }
        }
    }

    HvLogHex("C01", (UINT64)VcpuIdx);
    HvLogHex("C02", (UINT64)(UINTN)HvState->VcpuTable);
    HvLogHex("C03", (UINT64)sizeof(VCPU_DATA));

    return STATUS_SUCCESS;
}

STATIC
NTSTATUS
AllocateVcpuStructures(
    PVCPU_DATA Vcpu,
    PHYPERVISOR_STATE HvState
    )
{
    UINT64 Pa;

    Vcpu->Vmcb = (PVMCB)AllocateContiguousPages(PAGE_SIZE, &Pa);
    if (!Vcpu->Vmcb) return STATUS_INSUFFICIENT_RESOURCES;
    Vcpu->VmcbPhysical = Pa;

    Vcpu->HostSaveArea = AllocateContiguousPages(PAGE_SIZE, &Pa);
    if (!Vcpu->HostSaveArea) return STATUS_INSUFFICIENT_RESOURCES;
    Vcpu->HostSavePhysical = Pa;

    Vcpu->HostStack = AllocateContiguousPages(HOST_STACK_SIZE, NULL);
    if (!Vcpu->HostStack) return STATUS_INSUFFICIENT_RESOURCES;
    Vcpu->HostStackSize = HOST_STACK_SIZE;

    Vcpu->Msrpm = HvState->SharedMsrpm;
    Vcpu->MsrpmPhysical = HvState->SharedMsrpmPhysical;
    Vcpu->Iopm = HvState->SharedIopm;
    Vcpu->IopmPhysical = HvState->SharedIopmPhysical;

    return STATUS_SUCCESS;
}

STATIC
VOID
FreeVcpuStructures(
    PVCPU_DATA Vcpu
    )
{
    // Bulk mode skips this path — FreeVcpuTable checks VcpuBulkBlock.
    if (Vcpu->Vmcb) FreeContiguousPages(Vcpu->Vmcb, PAGE_SIZE);
    if (Vcpu->HostSaveArea) FreeContiguousPages(Vcpu->HostSaveArea, PAGE_SIZE);
    if (Vcpu->HostStack) FreeContiguousPages(Vcpu->HostStack, HOST_STACK_SIZE);
}

STATIC
VOID
FreeVcpuTable(
    PHYPERVISOR_STATE HvState
    )
{
    if (HvState->VcpuTable) {
        if (HvState->VcpuBulkBlock != 0) {
            gBS->FreePages(HvState->VcpuBulkBlock, HvState->VcpuBulkBlockPages);
            HvState->VcpuBulkBlock = 0;
            HvState->VcpuBulkBlockPages = 0;
        } else {
            for (UINT32 i = 0; i < HvState->NumCpus; i++) {
                FreeVcpuStructures(&HvState->VcpuTable[i]);
            }
        }
        UINTN Size = HvState->NumCpus * sizeof(VCPU_DATA);
        gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)HvState->VcpuTable, EFI_SIZE_TO_PAGES(Size));
        HvState->VcpuTable = NULL;
    }
}

// Bulk alloc: VMCB + host save + stack per CPU in one block.
#define VCPU_BULK_STRIDE  (PAGE_SIZE + PAGE_SIZE + HOST_STACK_SIZE)

STATIC
NTSTATUS
BulkAllocateVcpuStructures(
    PHYPERVISOR_STATE HvState
    )
{
    EFI_STATUS          Status;
    EFI_PHYSICAL_ADDRESS BlockPa;
    UINTN               TotalSize;
    UINTN               TotalPages;
    UINT32              NumCpus = HvState->NumCpus;

    TotalSize = (UINTN)NumCpus * VCPU_BULK_STRIDE;
    TotalPages = EFI_SIZE_TO_PAGES(TotalSize);

    BlockPa = HV_ALLOC_MAX_ADDRESS;
    Status = gBS->AllocatePages(
        AllocateMaxAddress,
        EfiRuntimeServicesData,
        TotalPages,
        &BlockPa
    );
    if (EFI_ERROR(Status)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ZeroMem((VOID *)(UINTN)BlockPa, TotalSize);

    // Per CPU: VMCB @ +0, HostSave @ +PAGE, Stack @ +2*PAGE.
    for (UINT32 i = 0; i < NumCpus; i++) {
        UINT64 CpuBase = BlockPa + (UINT64)i * VCPU_BULK_STRIDE;
        PVCPU_DATA Vcpu = &HvState->VcpuTable[i];

        Vcpu->Vmcb = (PVMCB)(UINTN)CpuBase;
        Vcpu->VmcbPhysical = CpuBase;

        Vcpu->HostSaveArea = (VOID *)(UINTN)(CpuBase + PAGE_SIZE);
        Vcpu->HostSavePhysical = CpuBase + PAGE_SIZE;

        Vcpu->HostStack = (VOID *)(UINTN)(CpuBase + 2 * PAGE_SIZE);
        Vcpu->HostStackSize = HOST_STACK_SIZE;

        Vcpu->Msrpm = HvState->SharedMsrpm;
        Vcpu->MsrpmPhysical = HvState->SharedMsrpmPhysical;
        Vcpu->Iopm = HvState->SharedIopm;
        Vcpu->IopmPhysical = HvState->SharedIopmPhysical;
    }

    HvState->VcpuBulkBlock = BlockPa;
    HvState->VcpuBulkBlockPages = TotalPages;

    HvLogHex("C04", (UINT64)BlockPa);
    HvLogHex("C05", (UINT64)TotalPages);

    return STATUS_SUCCESS;
}

// HypeVmrun.nasm reads this to select XSAVEOPT64 vs XSAVE64. Volatile so GCC
// can't reorder the store past VMRUN.
volatile UINT8 gUseXsaveopt = 0;

// Guest SSE/AVX preservation across VMEXIT — without it, C handler clobbers
// XMM/YMM and freezes under AVX load.
STATIC
NTSTATUS
AllocateXSaveArea(
    PVCPU_DATA Vcpu
    )
{
    INT32   CpuInfo[4];
    UINT32  XSaveSize;
    UINT64  XSaveMask;
    EFI_STATUS Status;
    EFI_PHYSICAL_ADDRESS XSavePa;

    __cpuid(CpuInfo, 1);
    if (!(CpuInfo[2] & (1 << 26))) {
        // No XSAVE — VMCB saves x87/SSE natively (APM §15.5.1).
        Vcpu->XSaveArea = NULL;
        Vcpu->XSaveSize = 0;
        Vcpu->XSaveMask = 0;
        return STATUS_SUCCESS;
    }

    __cpuidex(CpuInfo, 0x0D, 0);
    XSaveSize = (UINT32)CpuInfo[1];      // current XCR0 size
    if (XSaveSize == 0) {
        XSaveSize = (UINT32)CpuInfo[0];
    }
    if (XSaveSize < 576) {
        XSaveSize = 576;  // legacy 512 + 64 header
    }

    XSaveMask = ((UINT64)(UINT32)CpuInfo[3] << 32) | (UINT32)CpuInfo[0];
    XSaveMask |= 0x3;  // x87 (bit 0) + SSE (bit 1) — required minimum

    UINT32 AlignedSize = (XSaveSize + 63) & ~63U;  // 64-byte XSAVE alignment
    UINTN  AllocPages = EFI_SIZE_TO_PAGES(AlignedSize);
    if (AllocPages == 0) AllocPages = 1;

    XSavePa = HV_ALLOC_MAX_ADDRESS;
    Status = gBS->AllocatePages(
        AllocateMaxAddress,
        EfiRuntimeServicesData,
        AllocPages,
        &XSavePa
    );
    if (EFI_ERROR(Status)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ZeroMem((VOID *)(UINTN)XSavePa, AllocPages * PAGE_SIZE);

    Vcpu->XSaveArea = (VOID *)(UINTN)XSavePa;
    Vcpu->XSavePhysical = XSavePa;
    Vcpu->XSaveSize = AlignedSize;
    Vcpu->XSaveMask = XSaveMask;

    return STATUS_SUCCESS;
}

STATIC
NTSTATUS
AllocateSharedBitmaps(
    PHYPERVISOR_STATE HvState
    )
{
    UINT64 Pa;

    HvState->SharedMsrpm = (UINT8 *)AllocateContiguousPages(MSRPM_SIZE, &Pa);
    if (!HvState->SharedMsrpm) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    HvState->SharedMsrpmPhysical = Pa;

    HvState->SharedIopm = (UINT8 *)AllocateContiguousPages(IOPM_SIZE, &Pa);
    if (!HvState->SharedIopm) {
        FreeContiguousPages(HvState->SharedMsrpm, MSRPM_SIZE);
        HvState->SharedMsrpm = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    HvState->SharedIopmPhysical = Pa;

    VmcbSetupMsrpm(HvState->SharedMsrpm);
    VmcbSetupIopm(HvState->SharedIopm);

    return STATUS_SUCCESS;
}

STATIC
VOID
FreeSharedBitmaps(
    PHYPERVISOR_STATE HvState
    )
{
    if (HvState->SharedMsrpm) {
        FreeContiguousPages(HvState->SharedMsrpm, MSRPM_SIZE);
        HvState->SharedMsrpm = NULL;
    }
    if (HvState->SharedIopm) {
        FreeContiguousPages(HvState->SharedIopm, IOPM_SIZE);
        HvState->SharedIopm = NULL;
    }
}

// Host page tables — identity-mapped, RuntimeServicesData. PML4 + PDPT + PDs.
STATIC
NTSTATUS
BuildHostPageTables(
    PHYPERVISOR_STATE   HvState,
    PNPT_CONTEXT        NptCtx
    )
{
    UINT64              MaxPhys;
    UINT64              LapicBase;
    UINT64              Lapic2MbBase;
    EFI_PHYSICAL_ADDRESS BlockPa;
    EFI_STATUS          Status;
    UINT32              PdCount;
    UINT32              TotalPages;
    UINT32              LapicPtPageIndex;
    UINT64              *Pml4;
    UINT64              *Pdpt;

    MaxPhys = NptCtx->MaxPhysicalAddress;
    LapicBase = 0xFEE00000ULL;
    Lapic2MbBase = LapicBase & NPT_LARGE_2MB_MASK;

    // 1 PD per GB, capped at 64 (HOST_PT_MAX clamps reads beyond coverage).
    PdCount = (UINT32)((MaxPhys + PAGE_SIZE_1GB) / PAGE_SIZE_1GB);
    if (PdCount > 64) PdCount = 64;
    TotalPages = 3 + PdCount;
    LapicPtPageIndex = 2 + PdCount;

    BlockPa = HV_ALLOC_MAX_ADDRESS;
    Status = gBS->AllocatePages(
        AllocateMaxAddress,
        EfiRuntimeServicesData,
        TotalPages,
        &BlockPa
    );
    if (EFI_ERROR(Status)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ZeroMem((VOID *)(UINTN)BlockPa, (UINTN)TotalPages * PAGE_SIZE);

    Pml4 = (UINT64 *)(UINTN)BlockPa;
    Pdpt = (UINT64 *)(UINTN)(BlockPa + PAGE_SIZE);

    // Host PTs are standard x86 (not NPT). MUST be SUPERVISOR (U/S=0) — U/S=1
    // hits SMEP/SMAP on first VMEXIT and triple-faults.
    #define HOST_PT_RW      (NPT_PRESENT | NPT_WRITE)
    #define HOST_PT_2MB     (NPT_PRESENT | NPT_WRITE | NPT_LARGE_PAGE)
    #define HOST_PT_UC      (NPT_PRESENT | NPT_WRITE | NPT_PWT | NPT_PCD)

    Pml4[0] = (BlockPa + PAGE_SIZE) | HOST_PT_RW;

    for (UINT32 i = 0; i < PdCount; i++) {
        UINT64 PdPa = BlockPa + ((UINT64)(2 + i) * PAGE_SIZE);
        UINT64 *Pd = (UINT64 *)(UINTN)PdPa;

        Pdpt[i] = PdPa | HOST_PT_RW;

        for (UINT32 j = 0; j < 512; j++) {
            UINT64 Addr = ((UINT64)i * PAGE_SIZE_1GB) + ((UINT64)j * PAGE_SIZE_2MB);
            if (Addr > MaxPhys) break;

            if (Addr == Lapic2MbBase) {
                UINT64 PtPa = BlockPa + ((UINT64)LapicPtPageIndex * PAGE_SIZE);
                UINT64 *Pt = (UINT64 *)(UINTN)PtPa;

                for (UINT32 k = 0; k < 512; k++) {
                    UINT64 PageAddr = Addr + ((UINT64)k * PAGE_SIZE_4KB);
                    if (PageAddr > MaxPhys) {
                        break;
                    }
                    Pt[k] = (PageAddr & NPT_ADDR_MASK) | HOST_PT_RW;
                }

                Pt[NPT_PT_INDEX(LapicBase)] = (LapicBase & NPT_ADDR_MASK) | HOST_PT_UC;
                Pd[j] = PtPa | HOST_PT_RW;
            } else {
                Pd[j] = (Addr & NPT_LARGE_2MB_MASK) | HOST_PT_2MB;
            }
        }
    }

    HvState->HostCr3Physical = BlockPa;

    NptAddProtectedRegion(NptCtx, BlockPa, (UINT64)TotalPages * PAGE_SIZE);

    HvLogHex("C06", BlockPa);

    return STATUS_SUCCESS;
}

typedef struct _INIT_CONTEXT {
    PVCPU_DATA  Vcpu;
    NTSTATUS    Status;
    UINT32      Operation;
} INIT_CONTEXT;

#define OP_INIT     1
#define OP_START    2

typedef struct _AP_CONTEXT {
    PHYPERVISOR_STATE   HvState;
    UINT32              Operation;
} AP_CONTEXT;

// CRITICAL: DEBUG/ASSERT/Print UNSAFE after HostIdtLoad — triple-fault risk.

STATIC
VOID
EFIAPI
ApProcedure(
    IN VOID *Context
    )
{
    AP_CONTEXT *ApCtx = (AP_CONTEXT *)Context;
    UINTN ProcessorNumber;
    PVCPU_DATA Vcpu;
    EFI_STATUS Status;

    Status = gMpServices->WhoAmI(gMpServices, &ProcessorNumber);
    if (EFI_ERROR(Status)) {
        return;
    }

    if (ProcessorNumber >= ApCtx->HvState->MaxProcessorId || ProcessorNumber >= 256) {
        HvLog("C39\n");
        return;
    }

    UINT32 VcpuIndex = ApCtx->HvState->ProcessorIndexMap[ProcessorNumber];
    if (VcpuIndex == 0xFFFFFFFF || VcpuIndex >= ApCtx->HvState->NumCpus) {
        HvLog("C40\n");
        return;
    }

    if (VcpuIndex == 0) {
        HvLog("C41\n");  // AP mapped to BSP slot — fatal
        return;
    }

    Vcpu = &ApCtx->HvState->VcpuTable[VcpuIndex];

    if (ApCtx->Operation == OP_INIT) {
        Vcpu->OriginalEfer = __readmsr(MSR_EFER) & ~EFER_SVME;

        NTSTATUS InitStatus = SvmEnableOnCpu(Vcpu);
        if (!NT_SUCCESS(InitStatus)) {
            HvLog("C42\n");
            return;
        }

        InitStatus = VmcbInitialize(Vcpu);
        if (!NT_SUCCESS(InitStatus)) {
            HvLog("C43\n");
            return;
        }
    } else if (ApCtx->Operation == OP_START) {
        HvLogHex("V96", ((UINT64)Vcpu->ApicId << 16) | (UINT64)Vcpu->CpuNumber);

        // Capture guest state BEFORE switching IDTs — snapshot UEFI IDTR.
        VmcbCaptureGuestState(Vcpu->Vmcb, Vcpu);

        // CLI: host IDT only covers vectors 0-14; device IRQ between
        // HostIdtLoad and CLGI would #GP. Guest IF already captured.
        __asm__ volatile ("cli");

        HostIdtLoad(&g_DriverContext.HostIdtContext);

        HvLogHex("V97", (UINT64)Vcpu->ApicId);

        {
            UINT64 HsavePa = __readmsr(MSR_VM_HSAVE_PA);
            if (HsavePa != Vcpu->HostSavePhysical) {
                HvLogHex("C44", HsavePa);
                __writemsr(MSR_VM_HSAVE_PA, Vcpu->HostSavePhysical);
            }
        }

        // Clear SMEP/SMAP, set OSXSAVE before host CR3 switch.
        {
            UINT64 Cr4 = __readcr4();
            UINT64 NewCr4 = Cr4 & ~((1ULL << 20) | (1ULL << 21));
            if (Vcpu->XSaveArea) {
                NewCr4 |= (1ULL << 18);  // OSXSAVE
            }
            if (NewCr4 != Cr4) {
                __asm__ volatile ("mov %0, %%cr4" :: "r"(NewCr4) : "memory");
            }
        }

        {
            UINT64 HostCr3 = ApCtx->HvState->HostCr3Physical;
            if (HostCr3) {
                __asm__ volatile ("mov %0, %%cr3" :: "r"(HostCr3) : "memory");
            }
        }

        HvLogHex("V98", (UINT64)Vcpu->ApicId);

        SvmLaunch(Vcpu);

        InterlockedIncrement(&ApCtx->HvState->ApsInVmrun);
        HvLogHex("V99", (UINT64)Vcpu->ApicId);
        HvLogHex("C79", (UINT64)Vcpu->CpuNumber);
        Vcpu->Launched = TRUE;
    }
}

STATIC
NTSTATUS
InitializeAllAPs(
    PHYPERVISOR_STATE HvState
    )
{
    AP_CONTEXT ApCtx;
    EFI_STATUS Status;
    UINTN *FailedList = NULL;

    if (!gMpServices || HvState->NumCpus <= 1) {
        return STATUS_SUCCESS;
    }

    ApCtx.HvState = HvState;
    ApCtx.Operation = OP_INIT;

    Status = gMpServices->StartupAllAPs(
        gMpServices,
        ApProcedure,
        FALSE,          // parallel
        NULL,           // blocking
        5000000,        // 5s timeout
        &ApCtx,
        &FailedList
    );

    if (EFI_ERROR(Status)) {
        if (Status == EFI_TIMEOUT) {
            HvLog("C47\n");
        }
        if (FailedList) {
            for (UINTN *Fp = FailedList; *Fp != END_OF_CPU_LIST; Fp++) {
                HvLogHex("C08", *Fp);
            }
            FreePool(FailedList);
        }
        return Status;  // non-fatal — BSP works alone
    }

    return STATUS_SUCCESS;
}

STATIC
VOID
EFIAPI
CpuInitProcedure(
    IN VOID *Context
    )
{
    INIT_CONTEXT *Ctx = (INIT_CONTEXT *)Context;

    if (Ctx->Operation == OP_INIT) {
        // Strip SVME from snapshot so guest RDMSR can't see hypervisor.
        Ctx->Vcpu->OriginalEfer = __readmsr(MSR_EFER) & ~EFER_SVME;

        Ctx->Status = SvmEnableOnCpu(Ctx->Vcpu);
        if (NT_SUCCESS(Ctx->Status)) {
            Ctx->Status = VmcbInitialize(Ctx->Vcpu);
        }
    }
    // OP_START handled by ApProcedure (APs) / HypeStartBsp (BSP).
}

STATIC
NTSTATUS
RunOnCpu(
    UINT32      CpuNumber,
    UINT32      Operation,
    PVCPU_DATA  Vcpu
    )
{
    INIT_CONTEXT Ctx = {0};

    Ctx.Vcpu = Vcpu;
    Ctx.Operation = Operation;
    Ctx.Status = STATUS_SUCCESS;

    if (CpuNumber == 0 || !gMpServices) {
        CpuInitProcedure(&Ctx);
    } else {
        EFI_STATUS Status = gMpServices->StartupThisAP(
            gMpServices,
            CpuInitProcedure,
            CpuNumber,
            NULL,
            0,
            &Ctx,
            NULL
        );

        if (EFI_ERROR(Status)) {
            return Status;
        }
    }

    return Ctx.Status;
}

NTSTATUS
HypeInit(
    VOID
    )
{
    PHYPERVISOR_STATE Hv = &g_DriverContext.HvState;
    NTSTATUS Status;

    if (!SvmCheckSupport()) {
        return STATUS_NOT_SUPPORTED;
    }

    Status = HostIdtInitialize(&g_DriverContext.HostIdtContext);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = AllocateVcpuTable(Hv);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = AllocateSharedBitmaps(Hv);
    if (!NT_SUCCESS(Status)) {
        FreeVcpuTable(Hv);
        return Status;
    }

    // Bulk alloc (1 call vs 96), per-CPU fallback on failure.
    Status = BulkAllocateVcpuStructures(Hv);
    if (!NT_SUCCESS(Status)) {
        HvLog("C51\n");
        for (UINT32 i = 0; i < Hv->NumCpus; i++) {
            Status = AllocateVcpuStructures(&Hv->VcpuTable[i], Hv);
            if (!NT_SUCCESS(Status)) {
                FreeSharedBitmaps(Hv);
                FreeVcpuTable(Hv);
                return Status;
            }
        }
    }

    HvLogHex("C14", (UINT64)Hv->NumCpus);
    HvLogHex("C15", Hv->VcpuTable[0].VmcbPhysical);

    // XSAVEOPT (Zen1+). Set once on BSP, must be visible before first XSAVE in VMRUN.
    {
        INT32 XsaveSubleaf[4];
        __cpuidex(XsaveSubleaf, 0x0D, 1);
        if (XsaveSubleaf[0] & 1) {
            gUseXsaveopt = 1;
        }
    }

    for (UINT32 i = 0; i < Hv->NumCpus; i++) {
        Status = AllocateXSaveArea(&Hv->VcpuTable[i]);
        if (!NT_SUCCESS(Status)) {
            HvLogHex("C16", (UINT64)i);  // non-fatal — asm skips if NULL
        }
    }
    if (Hv->VcpuTable[0].XSaveArea) {
        HvLogHex("C17", (UINT64)Hv->VcpuTable[0].XSaveSize);
        HvLogHex("C18", Hv->VcpuTable[0].XSaveMask);
    }

    Status = NptBuildIdentityMap(&g_DriverContext.NptContext);
    if (EFI_ERROR(Status)) {
        FreeSharedBitmaps(Hv);
        FreeVcpuTable(Hv);
        return Status;
    }

    for (UINT32 i = 0; i < Hv->NumCpus; i++) {
        Hv->VcpuTable[i].NptPml4 = g_DriverContext.NptContext.Pml4;
        Hv->VcpuTable[i].NptPml4Physical = g_DriverContext.NptContext.Pml4Physical;
    }

    PNPT_CONTEXT NptCtx = &g_DriverContext.NptContext;

    // Must be before NptProtectOwnPages so host PT pages get tracked.
    Status = BuildHostPageTables(Hv, NptCtx);
    if (!NT_SUCCESS(Status)) {
        HvLog("C52\n");
        NptDestroy(NptCtx);
        FreeSharedBitmaps(Hv);
        FreeVcpuTable(Hv);
        return Status;
    }

    for (UINT32 i = 0; i < Hv->NumCpus; i++) {
        PVCPU_DATA Vcpu = &Hv->VcpuTable[i];

        NptAddProtectedRegion(NptCtx, Vcpu->VmcbPhysical, PAGE_SIZE);
        NptAddProtectedRegion(NptCtx, Vcpu->HostSavePhysical, PAGE_SIZE);

        UINT64 HostStackPa = (UINT64)(UINTN)Vcpu->HostStack;
        NptAddProtectedRegion(NptCtx, HostStackPa, HOST_STACK_SIZE);

        if (Vcpu->XSaveArea) {
            UINT64 XSavePa = (UINT64)(UINTN)Vcpu->XSaveArea;
            UINTN  XSavePages = EFI_SIZE_TO_PAGES(Vcpu->XSaveSize);
            if (XSavePages == 0) XSavePages = 1;
            NptAddProtectedRegion(NptCtx, XSavePa, XSavePages * PAGE_SIZE);
        }
    }

    // MSRPM/IOPM NOT protected — guest-writable by design. Protection would
    // NPF-decoy-remap them and silently disable all intercepts.

    UINT64 VcpuTablePa = (UINT64)(UINTN)Hv->VcpuTable;
    UINT64 VcpuTableSize = Hv->NumCpus * sizeof(VCPU_DATA);
    NptAddProtectedRegion(NptCtx, VcpuTablePa, VcpuTableSize);

    Status = NptProtectOwnPages(NptCtx);
    if (!NT_SUCCESS(Status)) {
        NptDestroy(NptCtx);
        FreeSharedBitmaps(Hv);
        FreeVcpuTable(Hv);
        return Status;
    }

    Status = NptApplyProtections(NptCtx);
    if (!NT_SUCCESS(Status)) {
        NptDestroy(NptCtx);
        FreeSharedBitmaps(Hv);
        FreeVcpuTable(Hv);
        return Status;
    }

    // Convergence loop — NptApplyProtections splits 2MB pages, allocating new
    // PT pages. Those pages need protection too or guest can remap GPAs.
    // Converges in 2-3 passes (new PT pages cluster in already-split regions).
    {
        UINT32 PassCount = 0;
        UINT32 TotalNewPages = 0;
        for (UINT32 Pass = 0; Pass < 4; Pass++) {
            UINT32 CountBefore = NptCtx->PageCount;

            // ProtectionsFinalized=FALSE so NptIsProtectedAddress unusable —
            // walk PageList and check each PTE: PRESENT+self-mapped = unprotected.
            UINT32 NewRegions = 0;
            for (PLIST_ENTRY Le = NptCtx->PageList.Flink;
                 Le != &NptCtx->PageList; Le = Le->Flink) {
                PNPT_PAGE_ENTRY Page = CONTAINING_RECORD(Le, NPT_PAGE_ENTRY, ListEntry);
                UINT64 *Pte = NptGetPte(NptCtx, Page->PhysicalAddress, FALSE);
                if (Pte && (*Pte & NPT_PRESENT) && !(*Pte & NPT_LARGE_PAGE)) {
                    UINT64 PteFrame = *Pte & NPT_ADDR_MASK;
                    UINT64 PageFrame = Page->PhysicalAddress & NPT_ADDR_MASK;
                    if (PteFrame == PageFrame) {
                        NptAddProtectedRegion(NptCtx, Page->PhysicalAddress, PAGE_SIZE);
                        NewRegions++;
                    }
                }
            }

            if (NewRegions == 0) break;

            TotalNewPages += NewRegions;
            NptApplyProtections(NptCtx);
            PassCount++;

            if (NptCtx->PageCount == CountBefore) break;
        }
        if (TotalNewPages > 0) {
            HvLogHex("C82", (UINT64)TotalNewPages);
            HvLogHex("C83", (UINT64)PassCount);
        }
    }

    Status = NptFinalizeProtections(NptCtx);
    if (!NT_SUCCESS(Status)) {
        NptDestroy(NptCtx);
        FreeSharedBitmaps(Hv);
        FreeVcpuTable(Hv);
        return Status;
    }

    // +128 headroom for deferred image protection + non-protected NPFs.
    UINT32 DecoyPoolPages = NptCtx->ProtectedCount + 128;
    Status = NptInitDecoyPool(NptCtx, DecoyPoolPages);
    if (!NT_SUCCESS(Status)) {
        HvLog("C84\n");
        HvLogHex("C85", DecoyPoolPages);
        // Non-fatal — protected NPFs will #GP instead of decoy-remap.
    }

    {
        UINT32 VerifyErrors = 0;
        for (UINT32 i = 0; i < Hv->NumCpus; i++) {
            PVCPU_DATA Vcpu = &Hv->VcpuTable[i];
            UINT64 HostStackPa = (UINT64)(UINTN)Vcpu->HostStack;

            if (!NptIsProtectedAddress(NptCtx, Vcpu->VmcbPhysical)) {
                HvLog("C86\n");
                HvLogHex("C19", (UINT64)Vcpu->CpuNumber);
                HvLogHex("C20", Vcpu->VmcbPhysical);
                VerifyErrors++;
            }

            UINT64 *VmcbPte = NptGetPte(NptCtx, Vcpu->VmcbPhysical, FALSE);
            if (VmcbPte) {
                UINT64 PteVal = *VmcbPte;
                if (PteVal & NPT_LARGE_PAGE) {
                    HvLog("C54\n");
                    HvLogHex("C21", (UINT64)Vcpu->CpuNumber);
                    VerifyErrors++;
                } else if (PteVal & NPT_PRESENT) {
                    UINT64 PageFrame = PteVal & NPT_ADDR_MASK;
                    UINT64 VmcbPage = Vcpu->VmcbPhysical & NPT_ADDR_MASK;
                    if (PageFrame == VmcbPage) {
                        HvLog("C55\n");
                        HvLogHex("C22", (UINT64)Vcpu->CpuNumber);
                        VerifyErrors++;
                    }
                }
            } else {
                HvLog("C56\n");
                HvLogHex("C23", (UINT64)Vcpu->CpuNumber);
                VerifyErrors++;
            }

            if (!NptIsProtectedAddress(NptCtx, Vcpu->HostSavePhysical)) {
                HvLogHex("C24", Vcpu->HostSavePhysical);
                VerifyErrors++;
            }

            if (!NptIsProtectedAddress(NptCtx, HostStackPa) ||
                !NptIsProtectedAddress(NptCtx, HostStackPa + HOST_STACK_SIZE - 1)) {
                HvLogHex("C25", HostStackPa);
                VerifyErrors++;
            }

            // PD entry must not still be a 2MB large page.
            {
                UINTN Pml4Idx = NPT_PML4_INDEX(Vcpu->VmcbPhysical);
                UINTN PdptIdx = NPT_PDPT_INDEX(Vcpu->VmcbPhysical);
                UINTN PdIdx   = NPT_PD_INDEX(Vcpu->VmcbPhysical);

                UINT64 *Pml4 = NptCtx->Pml4;
                if (Pml4 && (Pml4[Pml4Idx] & NPT_PRESENT)) {
                    UINT64 *Pdpt = (UINT64 *)(UINTN)(Pml4[Pml4Idx] & NPT_ADDR_MASK);
                    if (Pdpt[PdptIdx] & NPT_PRESENT) {
                        UINT64 *Pd = (UINT64 *)(UINTN)(Pdpt[PdptIdx] & NPT_ADDR_MASK);
                        if (Pd[PdIdx] & NPT_LARGE_PAGE) {
                            HvLog("C57\n");
                            HvLogHex("C26", (UINT64)Vcpu->CpuNumber);
                            VerifyErrors++;
                        }
                    }
                }
            }
        }

        {
            UINT64 VtPa = (UINT64)(UINTN)Hv->VcpuTable;
            if (!NptIsProtectedAddress(NptCtx, VtPa)) {
                HvLog("C58\n");
                HvLogHex("C27", VtPa);
                VerifyErrors++;
            }
        }

        if (VerifyErrors == 0) {
            HvLog("C59\n");
        } else {
            HvLogHex("C28", (UINT64)VerifyErrors);
        }
    }

    // SipiApplied gates 2nd STARTUP (APM §16.6.5) — one decrement per AP.
    if (Hv->NumCpus > 1) {
        gRemainingSipiCount = (INT32)(Hv->NumCpus - 1);
    } else {
        gRemainingSipiCount = 0;
    }
    HvLogHex("V89", (UINT64)(UINT32)gRemainingSipiCount);

    {
        NTSTATUS LapicStatus = InstallLapicNptShadow(NptCtx);
        if (!NT_SUCCESS(LapicStatus)) {
            HvLogHex("V84", 0xFA11ULL);
        }
    }

    // APs first (parallel), then BSP.
    Status = InitializeAllAPs(Hv);
    if (!NT_SUCCESS(Status)) {
        HvLogHex("C29", (UINT64)Status);
    }

    Status = RunOnCpu(0, OP_INIT, &Hv->VcpuTable[0]);
    if (!NT_SUCCESS(Status)) {
        NptDestroy(NptCtx);
        FreeSharedBitmaps(Hv);
        FreeVcpuTable(Hv);
        return Status;
    }

    InterlockedExchange(&Hv->InitComplete, TRUE);

    return STATUS_SUCCESS;
}

// Pre-EBS only — needs gMpServices.
NTSTATUS
LaunchAllAPs(
    VOID
    )
{
    PHYPERVISOR_STATE Hv = &g_DriverContext.HvState;
    AP_CONTEXT ApCtx;
    EFI_STATUS Status;
    UINTN *FailedList = NULL;

    if (!gMpServices || Hv->NumCpus <= 1) {
        HvLog("C60\n");
        return STATUS_SUCCESS;
    }

    if (!Hv->InitComplete) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    ApCtx.HvState = Hv;
    ApCtx.Operation = OP_START;

    HvLogHex("C30", (UINT64)(Hv->NumCpus - 1));

    Status = gMpServices->StartupAllAPs(
        gMpServices,
        ApProcedure,
        TRUE,           // serial (matches tandasat)
        NULL,           // blocking
        10000000,       // 10s timeout
        &ApCtx,
        &FailedList
    );

    if (EFI_ERROR(Status)) {
        HvLogHex("C31", (UINT64)Status);
        if (Status == EFI_TIMEOUT) {
            HvLog("C61\n");
        }
        if (FailedList) {
            for (UINTN *Fp = FailedList; *Fp != END_OF_CPU_LIST; Fp++) {
                HvLogHex("C32", *Fp);
            }
            FreePool(FailedList);
        }
        return Status;
    }

    HvLog("C62\n");
    HvLogHex("C80", (UINT64)Hv->ApsInVmrun);
    HvLogHex("C81", (UINT64)(Hv->NumCpus - 1));
    return STATUS_SUCCESS;
}

NTSTATUS
HypeStartBsp(
    VOID
    )
{
    PHYPERVISOR_STATE Hv = &g_DriverContext.HvState;
    PVCPU_DATA Vcpu = &Hv->VcpuTable[0];

    if (!Hv->InitComplete) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (Hv->Active) {
        return STATUS_ALREADY_COMMITTED;
    }

    HvLog("C63\n");

    // SVM enabled during OP_INIT. Capture guest state, load host IDT/CR3, VMRUN.
    VmcbCaptureGuestState(Vcpu->Vmcb, Vcpu);

    // CLI before IDT switch — host IDT covers only vectors 0-14 + 32-255;
    // a UEFI interrupt between IDT switch and CLGI could fault.
    __asm__ volatile ("cli");

    HostIdtLoad(&g_DriverContext.HostIdtContext);

    {
        UINT64 HsavePa = __readmsr(MSR_VM_HSAVE_PA);
        if (HsavePa != Vcpu->HostSavePhysical) {
            HvLog("C64\n");
            __writemsr(MSR_VM_HSAVE_PA, Vcpu->HostSavePhysical);
        }
    }

    HvLogHex("C33", Vcpu->VmcbPhysical);
    HvLogHex("C34", Vcpu->Vmcb->Control.NCr3);
    HvLogHex("C35", Vcpu->Vmcb->Save.Efer);
    HvLogHex("C36", Vcpu->Vmcb->Save.TR.Attrib);

    // Clear SMEP/SMAP, set OSXSAVE — affects host save area on VMRUN.
    {
        UINT64 Cr4 = __readcr4();
        UINT64 NewCr4 = Cr4 & ~((1ULL << 20) | (1ULL << 21));
        if (Vcpu->XSaveArea) {
            NewCr4 |= (1ULL << 18);
        }
        if (NewCr4 != Cr4) {
            __asm__ volatile ("mov %0, %%cr4" :: "r"(NewCr4) : "memory");
            HvLogHex("C37", Cr4);
        }
    }

    {
        UINT64 HostCr3 = Hv->HostCr3Physical;
        if (HostCr3) {
            __asm__ volatile ("mov %0, %%cr3" :: "r"(HostCr3) : "memory");
            HvLogHex("C38", HostCr3);
        }
    }

    HvLog("C65\n");
    SvmLaunch(Vcpu);
    HvLog("C66\n");

    Vcpu->Launched = TRUE;
    InterlockedExchange(&Hv->Active, TRUE);
    HvLog("C68\n");

    return STATUS_SUCCESS;
}

BOOLEAN
SvmCheckSupport(
    VOID
    )
{
    int CpuInfo[4];
    UINT64 VmCr;

    __cpuid(CpuInfo, 0);
    HvLogHex("S01", (UINT64)CpuInfo[1]);
    HvLogHex("S02", (UINT64)CpuInfo[3]);
    HvLogHex("S03", (UINT64)CpuInfo[2]);
    if (CpuInfo[1] != 0x68747541 ||  // "Auth"
        CpuInfo[3] != 0x69746E65 ||  // "enti"
        CpuInfo[2] != 0x444D4163) {  // "cAMD"
        HvLog("S04\n");
        return FALSE;
    }

    // SVM = Fn8000_0001h ECX[2]
    __cpuid(CpuInfo, 0x80000001);
    HvLogHex("S05", (UINT64)CpuInfo[2]);
    if (!(CpuInfo[2] & (1 << 2))) {
        HvLog("S06\n");
        return FALSE;
    }

    // 8000_000A EDX: bit 0 = NPT, bit 26 = VNMI.
    __cpuid(CpuInfo, 0x8000000A);
    HvLogHex("S07", (UINT64)CpuInfo[3]);
    if (!(CpuInfo[3] & 1)) {
        HvLog("S08\n");
        return FALSE;
    }
    if (!(CpuInfo[3] & (1 << 26))) {
        HvLog("S09\n");  // VNMI not advertised — proceed anyway
    }

    // VM_CR: bit 4 = SVMDIS, bit 3 = LOCK.
    VmCr = __readmsr(MSR_VM_CR);
    HvLogHex("S0A", VmCr);
    if (VmCr & (1ULL << 4)) {
        if (VmCr & (1ULL << 3)) {
            HvLog("S0B\n");
            return FALSE;
        }
        __writemsr(MSR_VM_CR, VmCr & ~(1ULL << 4));
        VmCr = __readmsr(MSR_VM_CR);
        HvLogHex("S0C", VmCr);
        if (VmCr & (1ULL << 4)) {
            HvLog("S0D\n");
            return FALSE;
        }
    }

    HvLog("S10\n");
    return TRUE;
}

BOOLEAN
SvmCheckNripsSupport(
    VOID
    )
{
    static INT32 Cached = -1;  // CPUID is serializing (~100 cy)
    if (Cached < 0) {
        int CpuInfo[4];
        __cpuid(CpuInfo, 0x8000000A);
        Cached = (CpuInfo[3] & (1 << 3)) ? 1 : 0;
    }
    return Cached != 0;
}

BOOLEAN
SvmCheckLbrvSupport(
    VOID
    )
{
    static INT32 Cached = -1;
    if (Cached < 0) {
        int CpuInfo[4];
        __cpuid(CpuInfo, 0x8000000A);
        Cached = (CpuInfo[3] & (1 << 1)) ? 1 : 0;
    }
    return Cached != 0;
}

NTSTATUS
SvmEnableOnCpu(
    PVCPU_DATA Vcpu
    )
{
    UINT64 Efer;

    __writemsr(MSR_VM_HSAVE_PA, Vcpu->HostSavePhysical);

    if (__readmsr(MSR_VM_HSAVE_PA) != Vcpu->HostSavePhysical) {
        HvLog("C69\n");
    }

    // Set R_INIT before EFER.SVME=1 so VMRUN sees the redirect bit (APM §15.30.1) —
    // INIT IPIs become #SX instead of legacy reset.
    {
        UINT64 VmCr = __readmsr(MSR_VM_CR);
        HvLogHex("V80", VmCr);
        __writemsr(MSR_VM_CR, VmCr | VM_CR_R_INIT);
        UINT64 VmCrAfter = __readmsr(MSR_VM_CR);
        HvLogHex("V81", VmCrAfter);
        if (!(VmCrAfter & VM_CR_R_INIT)) {
            HvLog("V82\n");  // bit didn't stick — locked/reserved on this uarch
        }
    }

    Efer = __readmsr(MSR_EFER);
    Efer |= EFER_SVME;
    __writemsr(MSR_EFER, Efer);

    if (!(__readmsr(MSR_EFER) & EFER_SVME)) {
        return STATUS_UNSUCCESSFUL;
    }

    {
        int CpuInfo[4];
        __cpuid(CpuInfo, 1);
        Vcpu->ApicId = (UINT32)((CpuInfo[1] >> 24) & 0xFF);
        Vcpu->ActivityState = GUEST_ACTIVE;
        Vcpu->SipiVector = 0;
    }

    return STATUS_SUCCESS;
}

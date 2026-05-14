

#include "HypeIdt.h"
#include "HypeSvm.h"
#include "HypeContext.h"
#include "HypeDebug.h"

#pragma pack(push, 1)
typedef struct _TSS64 {
    UINT32  Reserved0;
    UINT64  Rsp0;
    UINT64  Rsp1;
    UINT64  Rsp2;
    UINT64  Reserved1;
    UINT64  Ist1;
    UINT64  Ist2;
    UINT64  Ist3;
    UINT64  Ist4;
    UINT64  Ist5;
    UINT64  Ist6;
    UINT64  Ist7;
    UINT64  Reserved2;
    UINT16  Reserved3;
    UINT16  IoMapBase;
} TSS64, *PTSS64;

typedef struct _TSS_DESCRIPTOR {
    UINT16  LimitLow;
    UINT16  BaseLow;
    UINT8   BaseMid;
    UINT8   Type;
    UINT8   LimitHighFlags;
    UINT8   BaseHigh;
    UINT32  BaseUpper;
    UINT32  Reserved;
} TSS_DESCRIPTOR, *PTSS_DESCRIPTOR;
#pragma pack(pop)

#define TSS_TYPE_AVAILABLE      0x89
#define TSS_SIZE                sizeof(TSS64)

#define DF_STACK_SIZE           (8 * 1024)

STATIC
VOID
SetIdtEntry(
    PIDT_ENTRY  Entry,
    UINT64      Handler,
    UINT16      Selector,
    UINT8       Ist,
    UINT8       Type
    )
{
    Entry->OffsetLow = (UINT16)(Handler & 0xFFFF);
    Entry->Selector = Selector;
    Entry->Ist = Ist & 0x07;
    Entry->TypeAttr = Type | IDT_ATTR_PRESENT | IDT_ATTR_DPL0;
    Entry->OffsetMid = (UINT16)((Handler >> 16) & 0xFFFF);
    Entry->OffsetHigh = (UINT32)((Handler >> 32) & 0xFFFFFFFF);
    Entry->Reserved = 0;
}

NTSTATUS
HostIdtInitialize(
    PHOST_IDT_CONTEXT Context
    )
{
    UINT16 Cs;

    if (!Context) {
        return STATUS_INVALID_PARAMETER;
    }

    ZeroMem(Context, sizeof(HOST_IDT_CONTEXT));

    Cs = __readcs();

    SetIdtEntry(&Context->Entries[HOST_IDT_VECTOR_DE], (UINT64)HostIsrDe, Cs, 0, IDT_TYPE_INTERRUPT_GATE);
    SetIdtEntry(&Context->Entries[HOST_IDT_VECTOR_NMI], (UINT64)HostIsrNmi, Cs, 0, IDT_TYPE_INTERRUPT_GATE);

    SetIdtEntry(&Context->Entries[HOST_IDT_VECTOR_UD], (UINT64)HostIsrUd, Cs, 0, IDT_TYPE_INTERRUPT_GATE);

    SetIdtEntry(&Context->Entries[HOST_IDT_VECTOR_DF], (UINT64)HostIsrDoubleFault, Cs, HOST_IDT_IST_DF, IDT_TYPE_INTERRUPT_GATE);
    SetIdtEntry(&Context->Entries[HOST_IDT_VECTOR_GP], (UINT64)HostIsrGp, Cs, 0, IDT_TYPE_INTERRUPT_GATE);
    SetIdtEntry(&Context->Entries[HOST_IDT_VECTOR_PF], (UINT64)HostIsrPf, Cs, 0, IDT_TYPE_INTERRUPT_GATE);

    {
        for (UINT32 v = 0; v < HOST_IDT_ENTRY_COUNT; v++) {
            if (v == HOST_IDT_VECTOR_DE  || v == HOST_IDT_VECTOR_NMI ||
                v == HOST_IDT_VECTOR_UD  || v == HOST_IDT_VECTOR_DF  ||
                v == HOST_IDT_VECTOR_GP  || v == HOST_IDT_VECTOR_PF) {
                continue;
            }

            UINT64 Handler;
            if (v < 32) {
                Handler = (UINT64)HostIsrExcTable + (v * HOST_ISR_EXC_STUB_SIZE);
            } else {
                Handler = (UINT64)HostIsrCatchall;
            }

            SetIdtEntry(
                &Context->Entries[v],
                Handler,
                Cs,
                0,
                IDT_TYPE_INTERRUPT_GATE
            );
        }
    }

    Context->Idtr.Limit = sizeof(Context->Entries) - 1;
    Context->Idtr.Base = (UINT64)&Context->Entries[0];

    {
        EFI_PHYSICAL_ADDRESS DfPa = HV_ALLOC_MAX_ADDRESS;
        EFI_STATUS AllocStatus = gBS->AllocatePages(
            AllocateMaxAddress,
            EfiRuntimeServicesData,
            EFI_SIZE_TO_PAGES(DF_STACK_SIZE),
            &DfPa
        );
        if (EFI_ERROR(AllocStatus)) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Context->DfStack = (VOID *)(UINTN)DfPa;
    }
    ZeroMem(Context->DfStack, DF_STACK_SIZE);
    Context->DfStackSize = DF_STACK_SIZE;

    {
        EFI_PHYSICAL_ADDRESS TssPa = HV_ALLOC_MAX_ADDRESS;
        EFI_STATUS AllocStatus = gBS->AllocatePages(
            AllocateMaxAddress,
            EfiRuntimeServicesData,
            EFI_SIZE_TO_PAGES(TSS_SIZE),
            &TssPa
        );
        if (EFI_ERROR(AllocStatus)) {
            gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)Context->DfStack,
                           EFI_SIZE_TO_PAGES(DF_STACK_SIZE));
            Context->DfStack = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Context->Tss = (VOID *)(UINTN)TssPa;
    }
    ZeroMem(Context->Tss, TSS_SIZE);
    Context->TssSize = TSS_SIZE;

    PTSS64 Tss = (PTSS64)Context->Tss;
    Tss->Ist1 = (UINT64)Context->DfStack + DF_STACK_SIZE;

    HvLog("I09\n");
    HvLogHex("I01", Context->Idtr.Base);

    return STATUS_SUCCESS;
}

VOID
HostIdtLoad(
    PHOST_IDT_CONTEXT Context
    )
{
    DESCRIPTOR_TABLE_REGISTER LoadedIdtr;
    DESCRIPTOR_TABLE_REGISTER Gdtr;
    UINT16 TrSel;

    if (!Context) {
        return;
    }

    __lidt(&Context->Idtr);
    __sidt(&LoadedIdtr);

    if (LoadedIdtr.Base != Context->Idtr.Base) {
        HvLog("I10\n");
    }

    if (Context->Tss) {
        __sgdt(&Gdtr);
        TrSel = __readtr();

        if (TrSel != 0 && (TrSel + 15) <= Gdtr.Limit) {
            PTSS_DESCRIPTOR TssDesc = (PTSS_DESCRIPTOR)(Gdtr.Base + (TrSel & ~7));
            UINT64 TssBase = (UINT64)(UINTN)Context->Tss;

            TssDesc->LimitLow = (UINT16)(TSS_SIZE - 1);
            TssDesc->BaseLow = (UINT16)(TssBase & 0xFFFF);
            TssDesc->BaseMid = (UINT8)((TssBase >> 16) & 0xFF);
            TssDesc->Type = TSS_TYPE_AVAILABLE;
            TssDesc->LimitHighFlags = (UINT8)(((TSS_SIZE - 1) >> 16) & 0x0F);
            TssDesc->BaseHigh = (UINT8)((TssBase >> 24) & 0xFF);
            TssDesc->BaseUpper = (UINT32)(TssBase >> 32);
            TssDesc->Reserved = 0;

            AsmWriteTr(TrSel);
        }
    }
}

VOID
HostExceptionHandler(
    PEXCEPTION_FRAME Frame
    )
{

    __asm__ volatile("clgi");

    HvLog("I11\n");
    HvLogHex("I03", Frame->Vector);
    HvLogHex("I04", Frame->ErrorCode);
    HvLogHex("I05", Frame->Rip);
    HvLogHex("I06", Frame->Rsp);
    HvLogHex("I07", __readcr3());
    if (Frame->Vector == HOST_IDT_VECTOR_PF) {
        HvLogHex("I08", __readcr2());
    }
    HvLog("I12\n");

    for (;;) {
        DisableInterrupts(); CpuDeadLoop();
    }
}

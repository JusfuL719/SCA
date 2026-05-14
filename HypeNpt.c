

#include "HypeContext.h"
#include "HypeNpt.h"
#include "HypeDebug.h"

STATIC
UINT64
GetMaxPhysicalAddress(
    UINT32 *OutBits
    )
{
    int CpuInfo[4];
    UINT32 MaxPhysBits;

    __cpuid(CpuInfo, 0x80000008);
    MaxPhysBits = CpuInfo[0] & 0xFF;

    if (MaxPhysBits < 36 || MaxPhysBits > 52) {
        MaxPhysBits = 40;
    }

    if (OutBits) {
        *OutBits = MaxPhysBits;
    }

    return (1ULL << MaxPhysBits) - 1;
}

STATIC
UINT64 *
AllocateNptPage(
    PNPT_CONTEXT    Context,
    UINT64          *OutPhysical
    )
{
    EFI_STATUS          Status;
    EFI_PHYSICAL_ADDRESS PhysAddr;
    UINT64              *Va;
    PNPT_PAGE_ENTRY     Entry;

    PhysAddr = HV_ALLOC_MAX_ADDRESS;
    Status = gBS->AllocatePages(
        AllocateMaxAddress,
        EfiRuntimeServicesData,
        1,
        &PhysAddr
    );

    if (EFI_ERROR(Status)) {
        return NULL;
    }

    Va = (UINT64 *)(UINTN)PhysAddr;
    ZeroMem(Va, PAGE_SIZE);

    if (OutPhysical) {
        *OutPhysical = PhysAddr;
    }

    Entry = (PNPT_PAGE_ENTRY)AllocatePool(sizeof(NPT_PAGE_ENTRY));
    if (Entry) {
        Entry->VirtualAddress = Va;
        Entry->PhysicalAddress = PhysAddr;
        InsertTailList(&Context->PageList, &Entry->ListEntry);
        Context->PageCount++;
    } else {
        gBS->FreePages(PhysAddr, 1);
        return NULL;
    }

    return Va;
}

NTSTATUS
NptBuildIdentityMap(
    PNPT_CONTEXT Context
    )
{
    UINT64  MaxPhys;
    UINT64  PhysAddr;
    UINT64  *Pml4, *Pd;

    ZeroMem(Context, sizeof(NPT_CONTEXT));
    InitializeListHead(&Context->PageList);
    InitializeListHead(&Context->ProtectedList);

    MaxPhys = GetMaxPhysicalAddress(&Context->PhysicalAddressBits);

    #define NPT_MAP_LIMIT  (1ULL << 40)
    if (MaxPhys >= NPT_MAP_LIMIT) {
        MaxPhys = NPT_MAP_LIMIT - 1;
    }
    Context->MaxPhysicalAddress = MaxPhys;

    Context->EffectiveLimit = (MaxPhys >= HOST_PT_MAX) ? (HOST_PT_MAX - 1) : MaxPhys;

    {
        extern UINT64 gEffectiveLimit;
        gEffectiveLimit = Context->EffectiveLimit;
    }

    {
        EFI_STATUS          EfiStatus;
        EFI_PHYSICAL_ADDRESS BlockPa;
        UINT32              PdptCount, PdCount, TotalPages, PageIdx;
        BOOLEAN             BulkOk = FALSE;

        PdptCount = (UINT32)(((MaxPhys + 1) + (512ULL * PAGE_SIZE_1GB) - 1) / (512ULL * PAGE_SIZE_1GB));
        if (PdptCount == 0) PdptCount = 1;
        PdCount   = (UINT32)(((MaxPhys + 1) + PAGE_SIZE_1GB - 1) / PAGE_SIZE_1GB);
        if (PdCount == 0) PdCount = 1;
        TotalPages = 1 + PdptCount + PdCount;

        BlockPa = HV_ALLOC_MAX_ADDRESS;
        EfiStatus = gBS->AllocatePages(
            AllocateMaxAddress,
            EfiRuntimeServicesData,
            TotalPages,
            &BlockPa
        );

        if (!EFI_ERROR(EfiStatus)) {
            ZeroMem((VOID *)(UINTN)BlockPa, (UINTN)TotalPages * PAGE_SIZE);

            BulkOk = TRUE;
            for (PageIdx = 0; PageIdx < TotalPages; PageIdx++) {
                PNPT_PAGE_ENTRY Entry = (PNPT_PAGE_ENTRY)AllocatePool(sizeof(NPT_PAGE_ENTRY));
                if (!Entry) {
                    while (!IsListEmpty(&Context->PageList)) {
                        PLIST_ENTRY Le = Context->PageList.Flink;
                        RemoveEntryList(Le);
                        FreePool(CONTAINING_RECORD(Le, NPT_PAGE_ENTRY, ListEntry));
                    }
                    Context->PageCount = 0;
                    gBS->FreePages(BlockPa, TotalPages);
                    BulkOk = FALSE;
                    HvLog("N14\n");
                    break;
                }
                Entry->VirtualAddress = (VOID *)(UINTN)(BlockPa + (UINT64)PageIdx * PAGE_SIZE);
                Entry->PhysicalAddress = BlockPa + (UINT64)PageIdx * PAGE_SIZE;
                InsertTailList(&Context->PageList, &Entry->ListEntry);
                Context->PageCount++;
            }

            if (BulkOk) {

                Pml4 = (UINT64 *)(UINTN)BlockPa;
                Context->Pml4 = Pml4;
                Context->Pml4Physical = BlockPa;
                HvLogHex("N01", BlockPa);

                for (UINT32 i = 0; i < PdptCount; i++) {
                    UINT64 PdptPa = BlockPa + (UINT64)(1 + i) * PAGE_SIZE;
                    Pml4[i] = PdptPa | NPT_PRESENT | NPT_WRITE | NPT_USER;
                }

                for (UINT32 i = 0; i < PdCount; i++) {
                    UINT32 PdptIdx = i / 512;
                    UINT32 EntryIdx = i % 512;
                    UINT64 PdPa = BlockPa + (UINT64)(1 + PdptCount + i) * PAGE_SIZE;
                    UINT64 *PdptPage = (UINT64 *)(UINTN)(BlockPa + (UINT64)(1 + PdptIdx) * PAGE_SIZE);
                    PdptPage[EntryIdx] = PdPa | NPT_PRESENT | NPT_WRITE | NPT_USER;
                }

                for (PhysAddr = 0; PhysAddr <= MaxPhys; ) {
                    UINTN PdGlobalIdx = (UINTN)(PhysAddr / PAGE_SIZE_1GB);
                    UINTN PdEntryIdx  = NPT_PD_INDEX(PhysAddr);

                    Pd = (UINT64 *)(UINTN)(BlockPa + (UINT64)(1 + PdptCount + PdGlobalIdx) * PAGE_SIZE);
                    Pd[PdEntryIdx] = (PhysAddr & NPT_LARGE_2MB_MASK) | NPT_DEFAULT_2MB;

                    PhysAddr += PAGE_SIZE_2MB;
                    if (PhysAddr < PAGE_SIZE_2MB) break;
                }

                HvLogHex("N02", (UINT64)TotalPages);
                goto alloc_spare_pages;
            }
        } else {
            HvLog("N15\n");
        }

        {
            UINT64 Pml4Pa, PdptPa, PdPa;
            UINT64 *Pdpt;

            Pml4 = AllocateNptPage(Context, &Pml4Pa);
            if (!Pml4) {
                HvLog("N16\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            Context->Pml4 = Pml4;
            Context->Pml4Physical = Pml4Pa;
            HvLogHex("N03", Pml4Pa);

            for (PhysAddr = 0; PhysAddr <= MaxPhys; ) {
                UINTN Pml4Idx = NPT_PML4_INDEX(PhysAddr);
                UINTN PdptIdx = NPT_PDPT_INDEX(PhysAddr);
                UINTN PdIdx   = NPT_PD_INDEX(PhysAddr);

                if (!(Pml4[Pml4Idx] & NPT_PRESENT)) {
                    Pdpt = AllocateNptPage(Context, &PdptPa);
                    if (!Pdpt) goto fail;
                    Pml4[Pml4Idx] = PdptPa | NPT_PRESENT | NPT_WRITE | NPT_USER;
                } else {
                    Pdpt = (UINT64 *)(UINTN)(Pml4[Pml4Idx] & NPT_ADDR_MASK);
                }

                if (!(Pdpt[PdptIdx] & NPT_PRESENT)) {
                    Pd = AllocateNptPage(Context, &PdPa);
                    if (!Pd) goto fail;
                    Pdpt[PdptIdx] = PdPa | NPT_PRESENT | NPT_WRITE | NPT_USER;
                } else {
                    Pd = (UINT64 *)(UINTN)(Pdpt[PdptIdx] & NPT_ADDR_MASK);
                }

                Pd[PdIdx] = (PhysAddr & NPT_LARGE_2MB_MASK) | NPT_DEFAULT_2MB;

                PhysAddr += PAGE_SIZE_2MB;
                if (PhysAddr < PAGE_SIZE_2MB) break;
            }

            HvLog("N17\n");
            goto alloc_spare_pages;

        fail:
            NptDestroy(Context);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

    alloc_spare_pages:

        Context->SparePageCount = 0;
        for (UINT32 s = 0; s < NPT_SPARE_PT_PAGES; s++) {
            UINT64 SparePa = 0;
            UINT64 *SpareVa = AllocateNptPage(Context, &SparePa);
            if (!SpareVa) break;
            Context->SparePageVa[s] = SpareVa;
            Context->SparePagePa[s] = SparePa;
            Context->SparePageCount++;
        }
        HvLogHex("NSP", (UINT64)Context->SparePageCount);

        return STATUS_SUCCESS;
    }
}

VOID
NptDestroy(
    PNPT_CONTEXT Context
    )
{
    PLIST_ENTRY Entry;

    if (!Context) return;

    while (!IsListEmpty(&Context->PageList)) {
        Entry = Context->PageList.Flink;
        RemoveEntryList(Entry);

        PNPT_PAGE_ENTRY Page = CONTAINING_RECORD(Entry, NPT_PAGE_ENTRY, ListEntry);
        if (Page->VirtualAddress) {
            gBS->FreePages(Page->PhysicalAddress, 1);
        }
        FreePool(Page);
    }

    while (!IsListEmpty(&Context->ProtectedList)) {
        Entry = Context->ProtectedList.Flink;
        RemoveEntryList(Entry);

        PNPT_PROTECTED_REGION Region = CONTAINING_RECORD(Entry, NPT_PROTECTED_REGION, ListEntry);
        FreePool(Region);
    }

    ZeroMem(Context, sizeof(NPT_CONTEXT));
}

NTSTATUS
NptAddProtectedRegion(
    PNPT_CONTEXT    Context,
    UINT64          PhysicalBase,
    UINT64          Size
    )
{
    PNPT_PROTECTED_REGION Region;

    if (!Context || Size == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (PhysicalBase > MAX_UINT64 - Size) {
        return STATUS_INVALID_PARAMETER;
    }

    UINT64 AlignedBase = PhysicalBase & ~(PAGE_SIZE_4KB - 1);
    UINT64 RawEnd = PhysicalBase + Size;
    UINT64 AlignedEnd = (RawEnd + PAGE_SIZE_4KB - 1) & ~(PAGE_SIZE_4KB - 1);
    UINT64 AlignedSize = AlignedEnd - AlignedBase;

    Region = (PNPT_PROTECTED_REGION)AllocatePool(sizeof(NPT_PROTECTED_REGION));
    if (!Region) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Region->PhysicalBase = AlignedBase;
    Region->Size = AlignedSize;

    InsertTailList(&Context->ProtectedList, &Region->ListEntry);
    Context->ProtectedCount++;

    if (AlignedEnd > Context->MaxProtectedEnd)
        Context->MaxProtectedEnd = AlignedEnd;

    return STATUS_SUCCESS;
}

NTSTATUS
NptProtectOwnPages(
    PNPT_CONTEXT Context
    )
{
    PLIST_ENTRY Entry;
    UINT32 Count = 0;
    UINT32 RegionCount = 0;

    if (!Context || IsListEmpty(&Context->PageList)) {
        return STATUS_SUCCESS;
    }

    {
        LIST_ENTRY Sorted;
        InitializeListHead(&Sorted);

        while (!IsListEmpty(&Context->PageList)) {
            PLIST_ENTRY Le = Context->PageList.Flink;
            RemoveEntryList(Le);
            PNPT_PAGE_ENTRY Page = CONTAINING_RECORD(Le, NPT_PAGE_ENTRY, ListEntry);

            PLIST_ENTRY Pos = Sorted.Flink;
            while (Pos != &Sorted) {
                PNPT_PAGE_ENTRY Existing = CONTAINING_RECORD(Pos, NPT_PAGE_ENTRY, ListEntry);
                if (Page->PhysicalAddress < Existing->PhysicalAddress) break;
                Pos = Pos->Flink;
            }
            InsertTailList(Pos, &Page->ListEntry);
        }

        if (!IsListEmpty(&Sorted)) {
            Context->PageList.Flink = Sorted.Flink;
            Context->PageList.Blink = Sorted.Blink;
            Sorted.Flink->Blink = &Context->PageList;
            Sorted.Blink->Flink = &Context->PageList;
        }
    }

    {
        UINT64 RunBase = 0;
        UINT64 RunEnd  = 0;

        for (Entry = Context->PageList.Flink;
             Entry != &Context->PageList;
             Entry = Entry->Flink) {

            PNPT_PAGE_ENTRY Page = CONTAINING_RECORD(Entry, NPT_PAGE_ENTRY, ListEntry);
            Count++;

            if (RunEnd == 0) {
                RunBase = Page->PhysicalAddress;
                RunEnd  = Page->PhysicalAddress + PAGE_SIZE_4KB;
            } else if (Page->PhysicalAddress == RunEnd) {
                RunEnd += PAGE_SIZE_4KB;
            } else {
                NTSTATUS Status = NptAddProtectedRegion(Context, RunBase, RunEnd - RunBase);
                if (!NT_SUCCESS(Status)) return Status;
                RegionCount++;
                RunBase = Page->PhysicalAddress;
                RunEnd  = Page->PhysicalAddress + PAGE_SIZE_4KB;
            }
        }

        if (RunEnd > RunBase) {
            NTSTATUS Status = NptAddProtectedRegion(Context, RunBase, RunEnd - RunBase);
            if (!NT_SUCCESS(Status)) return Status;
            RegionCount++;
        }
    }

    HvLogHex("N04", (UINT64)Count);
    HvLogHex("N05", (UINT64)RegionCount);

    return STATUS_SUCCESS;
}

NTSTATUS
NptApplyProtections(
    PNPT_CONTEXT Context
    )
{
    PLIST_ENTRY Entry;
    UINT32 SplitCount = 0;
    UINT32 RevokeCount = 0;

    if (!Context || IsListEmpty(&Context->ProtectedList)) {
        return STATUS_SUCCESS;
    }

    for (Entry = Context->ProtectedList.Flink;
         Entry != &Context->ProtectedList;
         Entry = Entry->Flink) {

        PNPT_PROTECTED_REGION Region =
            CONTAINING_RECORD(Entry, NPT_PROTECTED_REGION, ListEntry);

        UINT64 RegionBase = Region->PhysicalBase;
        UINT64 RegionEnd  = RegionBase + Region->Size;

        {
            UINT64 SplitBase = RegionBase & ~(PAGE_SIZE_2MB - 1);
            UINT64 SplitEnd  = (RegionEnd + PAGE_SIZE_2MB - 1) & ~(PAGE_SIZE_2MB - 1);

            for (UINT64 Addr = SplitBase; Addr < SplitEnd; Addr += PAGE_SIZE_2MB) {
                NTSTATUS Status = NptSplitLargePage(Context, Addr);
                if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) {
                    HvLogHex("N06", Addr);
                    HvLogHex("N07", (UINT64)Status);
                }
                if (NT_SUCCESS(Status)) {
                    SplitCount++;
                }
            }
        }

        for (UINT64 Addr = RegionBase; Addr < RegionEnd; Addr += PAGE_SIZE_4KB) {
            UINT64 *Pte = NptGetPte(Context, Addr, FALSE);
            if (Pte && (*Pte & NPT_PRESENT)) {
                *Pte &= ~NPT_PRESENT;
                RevokeCount++;
            } else if (!Pte) {
                HvLogHex("N10", Addr);
            }
        }
    }

    HvLogHex("N11", (UINT64)SplitCount);
    HvLogHex("N12", (UINT64)RevokeCount);

    return STATUS_SUCCESS;
}

NTSTATUS
NptInitDecoyPool(
    PNPT_CONTEXT    Context,
    UINT32          PageCount
    )
{
    EFI_STATUS      Status;
    EFI_PHYSICAL_ADDRESS PhysAddr;
    UINTN           Pages;

    if (!Context || PageCount == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    Pages = PageCount;
    PhysAddr = HV_ALLOC_MAX_ADDRESS;

    Status = gBS->AllocatePages(
        AllocateMaxAddress,
        EfiRuntimeServicesData,
        Pages,
        &PhysAddr
    );

    if (EFI_ERROR(Status)) {
        return Status;
    }

    ZeroMem((VOID *)(UINTN)PhysAddr, Pages * PAGE_SIZE_4KB);

    Context->DecoyPoolBase = PhysAddr;
    Context->DecoyPoolSize = Pages * PAGE_SIZE_4KB;
    Context->DecoyPoolNext = 0;

    {
        EFI_PHYSICAL_ADDRESS ZeroPa = HV_ALLOC_MAX_ADDRESS;
        EFI_STATUS ZeroStatus = gBS->AllocatePages(
            AllocateMaxAddress,
            EfiRuntimeServicesData,
            1,
            &ZeroPa
        );
        if (!EFI_ERROR(ZeroStatus)) {
            ZeroMem((VOID *)(UINTN)ZeroPa, PAGE_SIZE_4KB);
            Context->ZeroPage = (UINT64 *)(UINTN)ZeroPa;
            Context->ZeroPagePhysical = ZeroPa;
            HvLog("N18\n");
            HvLogHex("N13", ZeroPa);
        } else {
            Context->ZeroPage = NULL;
            Context->ZeroPagePhysical = 0;
            HvLog("N19\n");
        }
    }

    return STATUS_SUCCESS;
}

UINT64
NptAllocateDecoyPage(
    PNPT_CONTEXT Context
    )
{
    UINT64 OldOffset;
    UINT64 NewOffset;
    UINT64 DecoyPa;

    if (!Context || Context->DecoyPoolBase == 0) {
        return 0;
    }

    do {
        OldOffset = Context->DecoyPoolNext;
        NewOffset = OldOffset + PAGE_SIZE_4KB;
        if (NewOffset > Context->DecoyPoolSize) {
            return 0;
        }
    } while (!__sync_bool_compare_and_swap(&Context->DecoyPoolNext, OldOffset, NewOffset));

    DecoyPa = Context->DecoyPoolBase + OldOffset;
    return DecoyPa;
}

NTSTATUS
NptRemapToDecoy(
    PNPT_CONTEXT    Context,
    UINT64          GuestPhysical
    )
{
    UINT64 *Pte;
    UINT64 DecoyPa;

    if (!Context) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&Context->RemapLock, NULL);

    Pte = NptGetPte(Context, GuestPhysical, FALSE);
    if (!Pte) {
        KeReleaseSpinLock(&Context->RemapLock, NULL);
        return STATUS_NOT_FOUND;
    }

    if (*Pte & NPT_PRESENT) {

        KeReleaseSpinLock(&Context->RemapLock, NULL);
        return STATUS_SUCCESS;
    }

    DecoyPa = NptAllocateDecoyPage(Context);
    if (DecoyPa == 0) {
        if (Context->ZeroPagePhysical != 0) {
            DecoyPa = Context->ZeroPagePhysical;
        } else {
            KeReleaseSpinLock(&Context->RemapLock, NULL);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    *Pte = (DecoyPa & NPT_ADDR_MASK) | NPT_PRESENT | NPT_WRITE | NPT_USER | NPT_NX;

    KeReleaseSpinLock(&Context->RemapLock, NULL);

    return STATUS_SUCCESS;
}

NTSTATUS
NptFinalizeProtections(
    PNPT_CONTEXT Context
    )
{
    PLIST_ENTRY Entry;
    UINT32 Count, i;
    PNPT_PROTECTED_RANGE Ranges;

    if (!Context) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Context->ProtectionsFinalized) {
        return STATUS_SUCCESS;
    }

    Count = Context->ProtectedCount;
    if (Count == 0) {
        Context->SortedRanges = NULL;
        Context->SortedRangeCount = 0;
        Context->ProtectionsFinalized = TRUE;
        return STATUS_SUCCESS;
    }

    UINTN ArraySize = (Count + 64) * sizeof(NPT_PROTECTED_RANGE);
    UINTN SortedPages = EFI_SIZE_TO_PAGES(ArraySize);
    EFI_PHYSICAL_ADDRESS PhysAddr = HV_ALLOC_MAX_ADDRESS;
    EFI_STATUS Status = gBS->AllocatePages(
        AllocateMaxAddress,
        EfiRuntimeServicesData,
        SortedPages,
        &PhysAddr
    );
    if (EFI_ERROR(Status)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Ranges = (PNPT_PROTECTED_RANGE)(UINTN)PhysAddr;
    ZeroMem(Ranges, SortedPages * PAGE_SIZE);

    i = 0;
    for (Entry = Context->ProtectedList.Flink;
         Entry != &Context->ProtectedList && i < Count;
         Entry = Entry->Flink) {
        PNPT_PROTECTED_REGION Region =
            CONTAINING_RECORD(Entry, NPT_PROTECTED_REGION, ListEntry);
        Ranges[i].Start = Region->PhysicalBase;
        Ranges[i].End = Region->PhysicalBase + Region->Size;
        i++;
    }

    for (UINT32 j = 1; j < i; j++) {
        NPT_PROTECTED_RANGE Key = Ranges[j];
        INT32 k = (INT32)j - 1;
        while (k >= 0 && Ranges[k].Start > Key.Start) {
            Ranges[k + 1] = Ranges[k];
            k--;
        }
        Ranges[k + 1] = Key;
    }

    Context->SortedRanges = Ranges;
    Context->SortedRangeCount = i;
    Context->SortedRangeCapacity = (SortedPages * PAGE_SIZE) / sizeof(NPT_PROTECTED_RANGE);
    Context->ProtectionsFinalized = TRUE;

    Context->MaxProtectedEnd = 0;
    for (UINT32 m = 0; m < i; m++) {
        if (Ranges[m].End > Context->MaxProtectedEnd)
            Context->MaxProtectedEnd = Ranges[m].End;
    }

    return STATUS_SUCCESS;
}

BOOLEAN
NptIsProtectedAddress(
    PNPT_CONTEXT    Context,
    UINT64          GuestPhysical
    )
{
    PNPT_PROTECTED_RANGE Ranges;
    UINT32 Count;
    INT32 Lo, Hi, Mid;

    if (!Context || !Context->ProtectionsFinalized) {
        return FALSE;
    }

    Count = __atomic_load_n(&Context->SortedRangeCount, __ATOMIC_ACQUIRE);
    Ranges = Context->SortedRanges;

    if (!Ranges || Count == 0) {
        return FALSE;
    }

    if (GuestPhysical >= Context->MaxProtectedEnd) {
        return FALSE;
    }

    Lo = 0;
    Hi = (INT32)Count - 1;

    while (Lo <= Hi) {
        Mid = Lo + (Hi - Lo) / 2;

        if (GuestPhysical < Ranges[Mid].Start) {
            Hi = Mid - 1;
        } else if (GuestPhysical >= Ranges[Mid].End) {
            Lo = Mid + 1;
        } else {
            return TRUE;
        }
    }

    return FALSE;
}

NTSTATUS
NptSplitLargePage(
    PNPT_CONTEXT    Context,
    UINT64          GuestPhysical
    )
{
    UINT64  *Pml4, *Pdpt, *Pd, *Pt;
    UINT64  PtPa;
    UINTN   Pml4Idx, PdptIdx, PdIdx;
    UINT64  OldEntry;
    UINT64  BaseAddr;

    if (!Context || !Context->Pml4) {
        return STATUS_INVALID_PARAMETER;
    }

    Pml4Idx = NPT_PML4_INDEX(GuestPhysical);
    PdptIdx = NPT_PDPT_INDEX(GuestPhysical);
    PdIdx   = NPT_PD_INDEX(GuestPhysical);

    Pml4 = Context->Pml4;
    if (!(Pml4[Pml4Idx] & NPT_PRESENT)) {
        return STATUS_NOT_FOUND;
    }

    Pdpt = (UINT64 *)(UINTN)(Pml4[Pml4Idx] & NPT_ADDR_MASK);
    if (!(Pdpt[PdptIdx] & NPT_PRESENT)) {
        return STATUS_NOT_FOUND;
    }

    Pd = (UINT64 *)(UINTN)(Pdpt[PdptIdx] & NPT_ADDR_MASK);
    OldEntry = Pd[PdIdx];

    if (!(OldEntry & NPT_LARGE_PAGE)) {
        return STATUS_SUCCESS;
    }

    Pt = AllocateNptPage(Context, &PtPa);
    if (!Pt) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    BaseAddr = OldEntry & NPT_LARGE_2MB_MASK;

    UINT64 OrigFlags = OldEntry & (NPT_PRESENT | NPT_WRITE | NPT_USER | NPT_NX);
    for (UINTN i = 0; i < 512; i++) {
        Pt[i] = (BaseAddr + (i * PAGE_SIZE_4KB)) | OrigFlags;
    }

    Pd[PdIdx] = PtPa | NPT_PRESENT | NPT_WRITE | NPT_USER;

    return STATUS_SUCCESS;
}

STATIC
UINT64 *
NptAllocateSparePage(
    PNPT_CONTEXT    Context,
    UINT64          *OutPhysical
    )
{
    LONG Idx = InterlockedDecrement(&Context->SparePageCount);
    if (Idx < 0) {
        InterlockedIncrement(&Context->SparePageCount);
        return NULL;
    }
    UINT64 *Va = Context->SparePageVa[Idx];
    if (OutPhysical) {
        *OutPhysical = Context->SparePagePa[Idx];
    }
    Context->SparePageVa[Idx] = NULL;
    Context->SparePagePa[Idx] = 0;
    ZeroMem(Va, PAGE_SIZE_4KB);
    return Va;
}

NTSTATUS
NptSplitLargePageRuntime(
    PNPT_CONTEXT    Context,
    UINT64          GuestPhysical
    )
{
    UINT64  *Pml4, *Pdpt, *Pd, *Pt;
    UINT64  PtPa;
    UINTN   Pml4Idx, PdptIdx, PdIdx;
    UINT64  OldEntry;
    UINT64  BaseAddr;

    if (!Context || !Context->Pml4) {
        return STATUS_INVALID_PARAMETER;
    }

    Pml4Idx = NPT_PML4_INDEX(GuestPhysical);
    PdptIdx = NPT_PDPT_INDEX(GuestPhysical);
    PdIdx   = NPT_PD_INDEX(GuestPhysical);

    Pml4 = Context->Pml4;
    if (!(Pml4[Pml4Idx] & NPT_PRESENT)) {
        return STATUS_NOT_FOUND;
    }

    Pdpt = (UINT64 *)(UINTN)(Pml4[Pml4Idx] & NPT_ADDR_MASK);
    if (!(Pdpt[PdptIdx] & NPT_PRESENT)) {
        return STATUS_NOT_FOUND;
    }

    Pd = (UINT64 *)(UINTN)(Pdpt[PdptIdx] & NPT_ADDR_MASK);

    KeAcquireSpinLock(&Context->RemapLock, NULL);

    OldEntry = Pd[PdIdx];

    if (!(OldEntry & NPT_LARGE_PAGE)) {
        KeReleaseSpinLock(&Context->RemapLock, NULL);
        return STATUS_SUCCESS;
    }

    Pt = NptAllocateSparePage(Context, &PtPa);
    if (!Pt) {
        KeReleaseSpinLock(&Context->RemapLock, NULL);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    BaseAddr = OldEntry & NPT_LARGE_2MB_MASK;
    UINT64 OrigFlags = OldEntry & (NPT_PRESENT | NPT_WRITE | NPT_USER | NPT_NX);
    for (UINTN i = 0; i < 512; i++) {
        Pt[i] = (BaseAddr + (i * PAGE_SIZE_4KB)) | OrigFlags;
    }

    Pd[PdIdx] = PtPa | NPT_PRESENT | NPT_WRITE | NPT_USER;

    KeReleaseSpinLock(&Context->RemapLock, NULL);

    return STATUS_SUCCESS;
}

UINT64 *
NptGetPte(
    PNPT_CONTEXT    Context,
    UINT64          GuestPhysical,
    BOOLEAN         Allocate
    )
{
    UINT64  *Pml4, *Pdpt, *Pd, *Pt;
    UINTN   Pml4Idx, PdptIdx, PdIdx, PtIdx;

    (VOID)Allocate;

    if (!Context || !Context->Pml4) {
        return NULL;
    }

    Pml4Idx = NPT_PML4_INDEX(GuestPhysical);
    PdptIdx = NPT_PDPT_INDEX(GuestPhysical);
    PdIdx   = NPT_PD_INDEX(GuestPhysical);
    PtIdx   = NPT_PT_INDEX(GuestPhysical);

    Pml4 = Context->Pml4;
    if (!(Pml4[Pml4Idx] & NPT_PRESENT)) {
        return NULL;
    }

    Pdpt = (UINT64 *)(UINTN)(Pml4[Pml4Idx] & NPT_ADDR_MASK);
    if (!(Pdpt[PdptIdx] & NPT_PRESENT)) {
        return NULL;
    }

    Pd = (UINT64 *)(UINTN)(Pdpt[PdptIdx] & NPT_ADDR_MASK);

    if (Pd[PdIdx] & NPT_LARGE_PAGE) {
        return &Pd[PdIdx];
    }

    if (!(Pd[PdIdx] & NPT_PRESENT)) {
        return NULL;
    }

    Pt = (UINT64 *)(UINTN)(Pd[PdIdx] & NPT_ADDR_MASK);
    return &Pt[PtIdx];
}

UINT64           gLapicGpa             = 0;
volatile UINT8  *gHostLapicVa          = NULL;
volatile INT32   gRemainingSipiCount   = 0;
volatile UINT8   gLapicInterceptArmed  = 0;

extern DRIVER_CONTEXT g_DriverContext;

NTSTATUS
InstallLapicNptShadow(
    PNPT_CONTEXT NptCtx
    )
{
    UINT64 ApicBase;
    UINT64 LapicGpa;
    NTSTATUS Status;
    UINT64 *Pte;

    ApicBase = __readmsr(MSR_IA32_APIC_BASE);
    if (ApicBase & APIC_BASE_X2APIC_ENABLE) {
        HvLog("V83\n");
        return STATUS_NOT_SUPPORTED;
    }

    LapicGpa = ApicBase & 0x000FFFFFFFFFF000ULL;
    gLapicGpa = LapicGpa;

    gHostLapicVa = (volatile UINT8 *)(UINTN)LapicGpa;

    Status = NptSplitLargePageRuntime(NptCtx, LapicGpa);
    if (!NT_SUCCESS(Status) && Status != STATUS_SUCCESS) {
        HvLogHex("V84", 0xDEADULL);
        return Status;
    }

    Pte = NptGetPte(NptCtx, LapicGpa, FALSE);
    if (!Pte) {
        return STATUS_NOT_FOUND;
    }

    *Pte |= NPT_PRESENT | NPT_NX;
    *Pte &= ~NPT_WRITE;

    gLapicInterceptArmed = 1;

    {
        UINT32 N = g_DriverContext.HvState.NumCpus;
        for (UINT32 i = 0; i < N; i++) {
            PVCPU_DATA Other = &g_DriverContext.HvState.VcpuTable[i];
            if (Other && Other->Vmcb) {
                Other->Vmcb->Control.TlbControl = 3;
            }
        }
    }

    HvLogHex("V84", LapicGpa);
    return STATUS_SUCCESS;
}

VOID
DisableLapicIntercept(
    VOID
    )
{
    PNPT_CONTEXT NptCtx = &g_DriverContext.NptContext;
    UINT64 *Pte;

    if (!gLapicInterceptArmed || gLapicGpa == 0) {
        return;
    }

    // Cycle 14 [B] reverted: EOI trap caused ~16k NPF/sec from timer ISRs
    // across 16 LPs → "client out of snapshots" hitches in-match, and the
    // M0F sentinel never fired anyway (drain showed M0F=0). Restore PTE
    // to PRESENT|WRITE|USER + TLB broadcast like pre-cycle-14.

    Pte = NptGetPte(NptCtx, gLapicGpa, FALSE);
    if (Pte) {
        *Pte = (gLapicGpa & NPT_ADDR_MASK) | NPT_PRESENT | NPT_WRITE | NPT_USER;
    }

    UINT32 N = g_DriverContext.HvState.NumCpus;
    for (UINT32 i = 0; i < N; i++) {
        PVCPU_DATA Other = &g_DriverContext.HvState.VcpuTable[i];
        if (!Other || !Other->Vmcb) continue;
        Other->Vmcb->Control.TlbControl = 3;
        if (Other->Msrpm) {
            Other->Msrpm[0x020C] &= (UINT8)~0x02;
        }
        Other->Vmcb->Control.VmcbClean &= ~VMCB_CLEAN_IOMSRPM;
    }

    __atomic_store_n(&gLapicInterceptArmed, (UINT8)0, __ATOMIC_RELEASE);

    HvLog("V93\n");
}

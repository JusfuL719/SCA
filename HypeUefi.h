

#ifndef HYPE_UEFI_H
#define HYPE_UEFI_H

#if defined(_MSC_VER)
#pragma warning(disable: 4201)
#pragma warning(disable: 4245)
#endif

#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Protocol/MpService.h>

#define HV_ALLOC_MAX_ADDRESS    0x3FFFFFFFULL

typedef EFI_STATUS NTSTATUS;
#define NT_SUCCESS(Status)              (!EFI_ERROR(Status))
#define STATUS_SUCCESS                  EFI_SUCCESS
#define STATUS_UNSUCCESSFUL             EFI_DEVICE_ERROR
#define STATUS_INSUFFICIENT_RESOURCES   EFI_OUT_OF_RESOURCES
#define STATUS_NOT_SUPPORTED            EFI_UNSUPPORTED
#define STATUS_INVALID_PARAMETER        EFI_INVALID_PARAMETER
#define STATUS_INVALID_DEVICE_STATE     EFI_NOT_READY
#define STATUS_ALREADY_COMMITTED        EFI_ALREADY_STARTED
#define STATUS_NOT_FOUND                EFI_NOT_FOUND

typedef INT32   LONG;
typedef UINTN   SIZE_T;
typedef UINTN   ULONG_PTR;

typedef VOID*   PVOID;
typedef UINT64  PHYSICAL_ADDRESS;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#if defined(__GNUC__) || defined(__clang__)

static inline UINT64 __readcr0(void) {
    UINT64 val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline UINT64 __readcr2(void) {
    UINT64 val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline UINT64 __readcr3(void) {
    UINT64 val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline UINT64 __readcr4(void) {
    UINT64 val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline UINT64 __readdr(UINT32 reg) {
    UINT64 val;
    switch (reg) {
        case 0: __asm__ volatile ("mov %%dr0, %0" : "=r"(val)); break;
        case 1: __asm__ volatile ("mov %%dr1, %0" : "=r"(val)); break;
        case 2: __asm__ volatile ("mov %%dr2, %0" : "=r"(val)); break;
        case 3: __asm__ volatile ("mov %%dr3, %0" : "=r"(val)); break;
        case 6: __asm__ volatile ("mov %%dr6, %0" : "=r"(val)); break;
        case 7: __asm__ volatile ("mov %%dr7, %0" : "=r"(val)); break;
        default: val = 0;
    }
    return val;
}

static inline UINT64 __readmsr(UINT32 msr) {
    UINT32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}

static inline void __writemsr(UINT32 msr, UINT64 val) {
    UINT32 lo = (UINT32)val;
    UINT32 hi = (UINT32)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline void __cpuid(int info[4], int leaf) {
    __asm__ volatile (
        "cpuid"
        : "=a"(info[0]), "=b"(info[1]), "=c"(info[2]), "=d"(info[3])
        : "a"(leaf), "c"(0)
    );
}

static inline void __cpuidex(int info[4], int leaf, int subleaf) {
    __asm__ volatile (
        "cpuid"
        : "=a"(info[0]), "=b"(info[1]), "=c"(info[2]), "=d"(info[3])
        : "a"(leaf), "c"(subleaf)
    );
}

static inline UINT64 __rdtsc(void) {
    UINT32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((UINT64)hi << 32) | lo;
}

static inline UINT64 __readeflags(void) {
    UINT64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return flags;
}

static inline UINT16 __readcs(void) {
    UINT16 val;
    __asm__ volatile ("mov %%cs, %0" : "=r"(val));
    return val;
}

static inline UINT16 __readds(void) {
    UINT16 val;
    __asm__ volatile ("mov %%ds, %0" : "=r"(val));
    return val;
}

static inline UINT16 __reades(void) {
    UINT16 val;
    __asm__ volatile ("mov %%es, %0" : "=r"(val));
    return val;
}

static inline UINT16 __readss(void) {
    UINT16 val;
    __asm__ volatile ("mov %%ss, %0" : "=r"(val));
    return val;
}

static inline UINT16 __readfs(void) {
    UINT16 val;
    __asm__ volatile ("mov %%fs, %0" : "=r"(val));
    return val;
}

static inline UINT16 __readgs(void) {
    UINT16 val;
    __asm__ volatile ("mov %%gs, %0" : "=r"(val));
    return val;
}

static inline UINT16 __sldt(void) {
    UINT16 val;
    __asm__ volatile ("sldt %0" : "=r"(val));
    return val;
}

static inline UINT16 __str(void) {
    UINT16 val;
    __asm__ volatile ("str %0" : "=r"(val));
    return val;
}

#define __readtr()    __str()
#define __readldtr()  __sldt()

#pragma pack(push, 1)
typedef struct _DTR_INTERNAL {
    UINT16  Limit;
    UINT64  Base;
} DTR_INTERNAL;
#pragma pack(pop)

static inline void __lidt(const void *idtr) {
    __asm__ volatile ("lidt %0" : : "m"(*(const DTR_INTERNAL *)idtr));
}

static inline void __sidt(void *idtr) {
    __asm__ volatile ("sidt %0" : "=m"(*(DTR_INTERNAL *)idtr));
}

static inline void __sgdt(void *gdtr) {
    __asm__ volatile ("sgdt %0" : "=m"(*(DTR_INTERNAL *)gdtr));
}

#elif defined(_MSC_VER)

#include <intrin.h>
#include <Library/CpuLib.h>

#define __readcs()  AsmReadCs()

#define __readds()  AsmReadDs()
#define __reades()  AsmReadEs()
#define __readss()  AsmReadSs()
#define __readfs()  AsmReadFs()
#define __readgs()  AsmReadGs()
#define __sldt()      AsmReadLdtr()
#define __readldtr()  AsmReadLdtr()
#define __readtr()    AsmReadTr()

#pragma pack(push, 1)
typedef struct _DTR_INTERNAL {
    UINT16  Limit;
    UINT64  Base;
} DTR_INTERNAL;
#pragma pack(pop)

static inline void __sgdt(void *gdtr) {

    IA32_DESCRIPTOR Desc;
    AsmReadGdtr(&Desc);
    ((DTR_INTERNAL *)gdtr)->Limit = Desc.Limit;
    ((DTR_INTERNAL *)gdtr)->Base  = Desc.Base;
}

#endif

#define RtlCopyMemory(Dest, Src, Len)   CopyMem((Dest), (Src), (Len))

#if defined(__GNUC__) || defined(__clang__)

static inline LONG InterlockedIncrement(volatile LONG *val) {
    return __sync_add_and_fetch(val, 1);
}

static inline LONG InterlockedDecrement(volatile LONG *val) {
    return __sync_sub_and_fetch(val, 1);
}

static inline LONG InterlockedExchange(volatile LONG *target, LONG value) {
    return __sync_lock_test_and_set(target, value);
}

static inline LONG InterlockedCompareExchange(volatile LONG *dst, LONG exchange, LONG compare) {
    return __sync_val_compare_and_swap(dst, compare, exchange);
}

#elif defined(_MSC_VER)

static inline LONG InterlockedIncrement(volatile LONG *val) {
    return _InterlockedIncrement(val);
}

static inline LONG InterlockedDecrement(volatile LONG *val) {
    return _InterlockedDecrement(val);
}

static inline LONG InterlockedExchange(volatile LONG *target, LONG value) {
    return _InterlockedExchange(target, value);
}

static inline LONG InterlockedCompareExchange(volatile LONG *dst, LONG exchange, LONG compare) {
    return _InterlockedCompareExchange(dst, exchange, compare);
}

#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE               4096
#endif

static inline PVOID AllocateContiguousUefi(SIZE_T Size, PHYSICAL_ADDRESS *OutPhysical) {

    EFI_STATUS Status;
    EFI_PHYSICAL_ADDRESS PhysAddr;
    UINTN Pages = EFI_SIZE_TO_PAGES(Size);

    PhysAddr = HV_ALLOC_MAX_ADDRESS;
    Status = gBS->AllocatePages(
        AllocateMaxAddress,
        EfiRuntimeServicesData,
        Pages,
        &PhysAddr
    );

    if (EFI_ERROR(Status)) {
        return NULL;
    }

    ZeroMem((VOID *)(UINTN)PhysAddr, Size);

    if (OutPhysical) {
        *OutPhysical = PhysAddr;
    }

    return (PVOID)(UINTN)PhysAddr;
}

static inline VOID FreeContiguousUefi(PVOID Va, SIZE_T Size) {
    if (Va) {
        gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)Va, EFI_SIZE_TO_PAGES(Size));
    }
}

#pragma pack(push, 1)
typedef struct _DESCRIPTOR_TABLE_REGISTER {
    UINT16  Limit;
    UINT64  Base;
} DESCRIPTOR_TABLE_REGISTER;
#pragma pack(pop)

extern EFI_MP_SERVICES_PROTOCOL *gMpServices;

typedef volatile UINT32 KSPIN_LOCK;

static inline VOID KeAcquireSpinLock(KSPIN_LOCK *Lock, VOID *OldIrql) {
    (VOID)OldIrql;
#if defined(__GNUC__) || defined(__clang__)
    while (__sync_lock_test_and_set(Lock, 1)) {
        while (*Lock) {
            __asm__ volatile ("pause");
        }
    }
#elif defined(_MSC_VER)
    while (_InterlockedExchange((volatile long *)Lock, 1)) {
        while (*Lock) {
            CpuPause();
        }
    }
#endif
}

static inline VOID KeReleaseSpinLock(KSPIN_LOCK *Lock, VOID *OldIrql) {
    (VOID)OldIrql;
#if defined(__GNUC__) || defined(__clang__)
    __sync_lock_release(Lock);
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
    *Lock = 0;
#endif
}

#define Flink   ForwardLink
#define Blink   BackLink

typedef LIST_ENTRY *PLIST_ENTRY;

#define CONTAINING_RECORD(address, type, field)  BASE_CR(address, type, field)

#define DRIVER_VERSION_MAJOR    1
#define DRIVER_VERSION_MINOR    0

extern UINT64 gBootCanary;
extern UINT64 gBootAuthKey;
extern UINT64 gBootLogMagic;

VOID HvBootKeyInit(VOID);

#endif

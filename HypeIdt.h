#ifndef HYPE_IDT_H
#define HYPE_IDT_H

#include "HypeUefi.h"

#pragma pack(push, 1)
typedef struct _IDT_ENTRY {
    UINT16  OffsetLow;
    UINT16  Selector;
    UINT8   Ist;
    UINT8   TypeAttr;
    UINT16  OffsetMid;
    UINT32  OffsetHigh;
    UINT32  Reserved;
} IDT_ENTRY, *PIDT_ENTRY;

typedef struct _IDT_REGISTER {
    UINT16  Limit;
    UINT64  Base;
} IDT_REGISTER, *PIDT_REGISTER;
#pragma pack(pop)

#define IDT_TYPE_INTERRUPT_GATE     0x0E

#define IDT_ATTR_PRESENT            (1 << 7)
#define IDT_ATTR_DPL0               (0 << 5)

#define HOST_IDT_VECTOR_DE          0
#define HOST_IDT_VECTOR_NMI         2
#define HOST_IDT_VECTOR_UD          6
#define HOST_IDT_VECTOR_DF          8
#define HOST_IDT_VECTOR_GP          13
#define HOST_IDT_VECTOR_PF          14

#define HOST_IDT_ENTRY_COUNT        256

#define HOST_IDT_IST_DF             1

typedef struct _HOST_IDT_CONTEXT {
    IDT_ENTRY       Entries[HOST_IDT_ENTRY_COUNT];
    IDT_REGISTER    Idtr;
    VOID            *DfStack;
    UINT64          DfStackSize;
    VOID            *Tss;
    UINT64          TssSize;
} HOST_IDT_CONTEXT, *PHOST_IDT_CONTEXT;

typedef struct _EXCEPTION_FRAME {
    UINT64  R15;
    UINT64  R14;
    UINT64  R13;
    UINT64  R12;
    UINT64  R11;
    UINT64  R10;
    UINT64  R9;
    UINT64  R8;
    UINT64  Rbp;
    UINT64  Rdi;
    UINT64  Rsi;
    UINT64  Rdx;
    UINT64  Rcx;
    UINT64  Rbx;
    UINT64  Rax;

    UINT64  Vector;
    UINT64  ErrorCode;

    UINT64  Rip;
    UINT64  Cs;
    UINT64  Rflags;
    UINT64  Rsp;
    UINT64  Ss;
} EXCEPTION_FRAME, *PEXCEPTION_FRAME;

NTSTATUS HostIdtInitialize(PHOST_IDT_CONTEXT Context);
VOID HostIdtLoad(PHOST_IDT_CONTEXT Context);
VOID HostExceptionHandler(PEXCEPTION_FRAME Frame);

extern VOID HostIsrDe(VOID);
extern VOID HostIsrNmi(VOID);
extern VOID HostIsrUd(VOID);
extern VOID HostIsrDoubleFault(VOID);
extern VOID HostIsrGp(VOID);
extern VOID HostIsrPf(VOID);
extern VOID HostIsrCatchall(VOID);

extern UINT8 HostIsrExcTable[];
#define HOST_ISR_EXC_STUB_SIZE  16

#endif

    SECTION .text

extern HostExceptionHandler

%macro ISR_NO_ERROR 2
global ASM_PFX(HostIsr%1)
ASM_PFX(HostIsr%1):
    push    qword 0         ; fake error code for uniform frame
    push    qword %2
    jmp     IsrCommon
%endmacro

%macro ISR_WITH_ERROR 2
global ASM_PFX(HostIsr%1)
ASM_PFX(HostIsr%1):
    push    qword %2
    jmp     IsrCommon
%endmacro

ISR_NO_ERROR De, 0

; Vector 2 stub. NMI is no longer intercepted (Misc1 bit 1 cleared); guest NMIs
; route to guest IDT directly. This handler exists only to absorb a stray NMI
; on the host between VMEXIT and VMRUN re-entry — bare iretq, no R15 deref.
global ASM_PFX(HostIsrNmi)
ASM_PFX(HostIsrNmi):
    iretq

; #UD fires on VMLOAD/VMRUN when EFER.SVME=0.
ISR_NO_ERROR Ud, 6

ISR_WITH_ERROR DoubleFault, 8
ISR_WITH_ERROR Gp, 13
ISR_WITH_ERROR Pf, 14

; Error-code bitmap (CPU pushes error code): 8, 10, 11, 12, 13, 14, 17, 21, 29, 30.
; Vectors with dedicated handlers (0,2,6,8,13,14) have entries here too but are
; never referenced — IDT points to their individual stubs instead.

%define EXC_STUB_SIZE 16

%define EXC_ERROR_BITMAP ((1<<8)|(1<<10)|(1<<11)|(1<<12)|(1<<13)|(1<<14)|(1<<17)|(1<<21)|(1<<29)|(1<<30))

global ASM_PFX(HostIsrExcTable)
align 16
ASM_PFX(HostIsrExcTable):

%assign vec 0
%rep 32
  %if (EXC_ERROR_BITMAP >> vec) & 1
    push    qword vec
    jmp     IsrCommon
  %else
    push    qword 0
    push    qword vec
    jmp     IsrCommon
  %endif
    times (EXC_STUB_SIZE - ($ - ASM_PFX(HostIsrExcTable) - vec * EXC_STUB_SIZE)) db 0xCC
  %assign vec vec+1
%endrep

; Catch-all for vectors 32-255. Host runs IF=0 between STGI/CLGI so these
; should never fire; plain iretq prevents triple-fault if one somehow does.
global ASM_PFX(HostIsrCatchall)
ASM_PFX(HostIsrCatchall):
    iretq

; Stack on entry to IsrCommon (jmp, not call — no return address):
;   [RSP+0]  = vector
;   [RSP+8]  = error code (real or fake 0)
;   [RSP+16] = RIP   (CPU push)
;   [RSP+24] = CS
;   [RSP+32] = RFLAGS
;   [RSP+40] = RSP
;   [RSP+48] = SS
IsrCommon:
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    ; RSP -> EXCEPTION_FRAME, RCX = frame
    mov     rcx, rsp
    sub     rsp, 20h        ; shadow space
    call    HostExceptionHandler
    ; Never returns
.halt:
    cli
    hlt
    jmp     .halt


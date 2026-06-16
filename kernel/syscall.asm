; ============================================================================
;  BoltOS  -  SYSCALL entry stub (IA32_LSTAR target).
;  On entry (from a ring-3 `syscall`): RCX = user RIP, R11 = user RFLAGS,
;  IF cleared by FMASK, and RSP is still the *user* stack. We switch to a
;  kernel stack, marshal args to the SysV C ABI, dispatch, then SYSRET back.
;
;  syscall ABI:  rax = number, rdi/rsi/rdx = arg1/arg2/arg3, rax = return.
; ============================================================================
[BITS 64]

extern syscall_dispatch
extern syscall_kstack_top
extern syscall_user_rsp
global syscall_entry

section .text
syscall_entry:
    mov     [rel syscall_user_rsp], rsp     ; stash user RSP
    mov     rsp, [rel syscall_kstack_top]   ; kernel syscall stack

    push    rcx                             ; save user RIP
    push    r11                             ; save user RFLAGS

    ; (rax,rdi,rsi,rdx) -> (rdi,rsi,rdx,rcx) for syscall_dispatch(num,a1,a2,a3)
    mov     rcx, rdx
    mov     rdx, rsi
    mov     rsi, rdi
    mov     rdi, rax
    call    syscall_dispatch                ; result in rax

    pop     r11                             ; restore user RFLAGS
    pop     rcx                             ; restore user RIP
    mov     rsp, [rel syscall_user_rsp]     ; restore user RSP
    o64 sysret                              ; 64-bit SYSRET -> ring 3

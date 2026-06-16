; ============================================================================
;  BoltOS  -  64-bit kernel entry stub (.text.boot, linked first at 0x100000)
;  Stage 2 jumps here already in long mode with RDI = bootinfo pointer.
;  Sets up the kernel stack, zeroes BSS, then calls kmain(bootinfo).
; ============================================================================
[BITS 64]

global _start
extern kmain
extern __bss_start
extern __bss_end

section .text.boot
_start:
    cli
    ; Entered at the physical load address (identity-mapped). Stage 2 already
    ; installed the higher-half kernel mapping, so jump to our linked virtual
    ; address and run from the higher half from here on.
    mov     rax, .high
    jmp     rax
.high:
    lea     rsp, [rel stack_top]    ; real kernel stack (higher-half)
    mov     rbx, rdi                ; preserve bootinfo pointer (phys, identity-mapped)

    ; zero .bss
    lea     rdi, [rel __bss_start]
    lea     rcx, [rel __bss_end]
    sub     rcx, rdi
    xor     eax, eax
    rep     stosb

    mov     rdi, rbx                ; arg0 = bootinfo
    xor     rbp, rbp
    call    kmain

.hang:
    cli
    hlt
    jmp     .hang

section .bss
align 16
stack_bottom:
    resb 65536                      ; 64 KiB kernel stack
stack_top:

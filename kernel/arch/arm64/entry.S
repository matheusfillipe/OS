/* Exception vectors. Every vector saves a struct regs frame on SP_EL1 (x0-x30,
 * the interrupted sp, pc, pstate, esr, far) and calls
 * exception_dispatch(struct regs *, vector index 0-15). */

#define FRAME 288

.macro VECTOR n
    .balign 128
    sub sp, sp, #FRAME
    stp x0, x1, [sp]
    mov x0, #\n
    b exception_entry
.endm

.text
.balign 2048
.global exception_vectors
exception_vectors:
.irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    VECTOR \n
.endr

exception_entry:
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    mrs x1, spsr_el1
    mrs x2, elr_el1
    /* From EL0 the interrupted sp is SP_EL0; from EL1 it is ours, above the frame. */
    tst x1, #0xf
    b.ne 1f
    mrs x3, sp_el0
    b 2f
1:  add x3, sp, #FRAME
2:  stp x30, x3, [sp, #240]
    stp x2, x1, [sp, #256]
    mrs x2, esr_el1
    mrs x3, far_el1
    stp x2, x3, [sp, #272]
    mov x1, x0
    mov x0, sp
    bl exception_dispatch

/* Restores the frame at sp and returns to where it was taken. */
exception_return:
    msr daifset, #0xf
    ldp x1, x2, [sp, #256]
    ldr x3, [sp, #248]
    msr elr_el1, x1
    msr spsr_el1, x2
    tst x2, #0xf
    b.ne 3f
    msr sp_el0, x3
3:  ldp x0, x1, [sp]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #FRAME
    eret

/* void return_to_user(struct regs *frame)
 * Enters user mode from a frame at the top of the current thread's stack, so
 * SP_EL1 is back at that top for the next exception from EL0. */
.global return_to_user
return_to_user:
    mov sp, x0
    b exception_return

/* void fpu_save(void *area), void fpu_restore(void *area)
 * Copy V0-V31, FPSR and FPCR to and from a struct fpsimd_context. */
.global fpu_save
fpu_save:
    mrs x2, fpsr
    mrs x3, fpcr
    stp w2, w3, [x0, #8]
    add x1, x0, #16
    st1 {v0.2d-v3.2d}, [x1], #64
    st1 {v4.2d-v7.2d}, [x1], #64
    st1 {v8.2d-v11.2d}, [x1], #64
    st1 {v12.2d-v15.2d}, [x1], #64
    st1 {v16.2d-v19.2d}, [x1], #64
    st1 {v20.2d-v23.2d}, [x1], #64
    st1 {v24.2d-v27.2d}, [x1], #64
    st1 {v28.2d-v31.2d}, [x1]
    ret

.global fpu_restore
fpu_restore:
    ldp w2, w3, [x0, #8]
    msr fpsr, x2
    msr fpcr, x3
    add x1, x0, #16
    ld1 {v0.2d-v3.2d}, [x1], #64
    ld1 {v4.2d-v7.2d}, [x1], #64
    ld1 {v8.2d-v11.2d}, [x1], #64
    ld1 {v12.2d-v15.2d}, [x1], #64
    ld1 {v16.2d-v19.2d}, [x1], #64
    ld1 {v20.2d-v23.2d}, [x1], #64
    ld1 {v24.2d-v27.2d}, [x1], #64
    ld1 {v28.2d-v31.2d}, [x1]
    ret

/* void switch_context(uint64_t *save_sp, uint64_t new_sp)
 * Saves callee-saved registers on the current stack, stores the stack pointer
 * into *save_sp, then resumes the thread whose stack is new_sp. */
.global switch_context
switch_context:
    sub sp, sp, #96
    stp x19, x20, [sp]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #96
    ret

.section .note.GNU-stack, "", %progbits

/* Kernel accesses to user memory. Each instruction that touches user memory
 * has an entry in __ex_table (instruction address, fixup address): when it
 * faults and the page fault handler cannot resolve the fault, it resumes at
 * the fixup instead of panicking, and the routine returns -EFAULT. These are
 * leaf routines that never move sp, so one shared fixup can just return. */

#define EFAULT 14

.macro EX insn, fixup
    .pushsection __ex_table, "a"
    .balign 8
    .quad \insn, \fixup
    .popsection
.endm

.text

/* int __copy_user(void *dst, const void *src, size_t len): 0 or -EFAULT */
.global __copy_user
__copy_user:
    cbz x2, 3f
1:  ldrb w3, [x1], #1
2:  strb w3, [x0], #1
    subs x2, x2, #1
    b.ne 1b
3:  mov x0, #0
    ret
    EX 1b, .Lfault
    EX 2b, .Lfault

/* int64_t __strncpy_user(char *dst, const char *src, size_t size): length of
 * the string (the NUL is copied), `size` if there is no NUL within size
 * bytes, or -EFAULT. */
.global __strncpy_user
__strncpy_user:
    mov x3, #0
4:  cmp x3, x2
    b.hs 6f
5:  ldrb w4, [x1, x3]
    strb w4, [x0, x3]
    cbz w4, 6f
    add x3, x3, #1
    b 4b
6:  mov x0, x3
    ret
    EX 5b, .Lfault

/* int __cmpxchg_user32(uint64_t uaddr, uint32_t expected, uint32_t desired,
 *                      uint32_t *cur): *cur = value found; 0 or -EFAULT */
.global __cmpxchg_user32
__cmpxchg_user32:
7:  ldaxr w4, [x0]
    cmp w4, w1
    b.ne 9f
8:  stlxr w5, w2, [x0]
    cbnz w5, 7b
9:  clrex
    str w4, [x3]
    mov x0, #0
    ret
    EX 7b, .Lfault
    EX 8b, .Lfault

.Lfault:
    mov x0, #-EFAULT
    ret

.section .note.GNU-stack, "", %progbits

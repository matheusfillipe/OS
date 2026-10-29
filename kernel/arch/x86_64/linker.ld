OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    /* Higher half, top 2 GiB: required by -mcmodel=kernel. */
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_start = .;
    .text : { *(.text .text.*) } :text
    __text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
        /* (faulting instruction, fixup) pairs for user-memory access */
        . = ALIGN(8);
        __ex_table_start = .;
        KEEP(*(__ex_table))
        __ex_table_end = .;
    } :rodata
    __rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_start = .;
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    __data_end = .;
    __kernel_end = .;

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) }
}

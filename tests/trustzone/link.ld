/* Secure code and data, the Non-secure-callable gateways, then Non-secure code, data and stack, as
   trustzone.c configures the SAU. */
MEMORY {
    S (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
    NSC (rx) : ORIGIN = 0x20002000, LENGTH = 32
    NS (rwx) : ORIGIN = 0x20002020, LENGTH = 0x5fe0
}
SECTIONS {
    .s : { KEEP(*(.s_vectors)) *(.text*) *(.rodata*) *(.data*) . = ALIGN(4); } > S
    .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > S
    .nsc : { KEEP(*(.nsc)) } > NSC
    /* VTOR needs its 68-entry table aligned to the next power of two. */
    .ns : { . = ALIGN(512); KEEP(*(.ns_vectors)) *(.ns_text*) *(.ns_data*) . = ALIGN(4); } > NS
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}

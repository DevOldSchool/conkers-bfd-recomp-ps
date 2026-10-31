/* Linker script for Conker recomp mods. The loader relocates every section, so the base address
 * only has to stay clear of the game's own addresses. Hook, callback and import sections are left
 * out on purpose: as orphans they keep their own names (.recomp_hook.func_X and so on), which is
 * where RecompModTool reads their targets from. */

SECTIONS {
    . = 0x81000000;
    .ctors : { *(.ctors*) *(.init_array*) }
    .dtors : { *(.dtors*) }
    .text : { *(.text*) }
    .recomp_patch : { *(.recomp_patch*) }
    .recomp_force_patch : { *(.recomp_force_patch*) }
    .recomp_export : { *(.recomp_export*) }
    .recomp_event : { *(.recomp_event*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.sdata*) }
    .bss (NOLOAD) : { *(.bss*) *(.sbss*) *(COMMON) }

    /DISCARD/ : {
        *(.MIPS.*) *(.reginfo) *(.comment) *(.pdr) *(.mdebug*) *(.gnu.attributes) *(.note*) *(.eh_frame*)
    }
}

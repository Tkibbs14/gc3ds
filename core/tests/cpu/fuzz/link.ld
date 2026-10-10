ENTRY(_start)
SECTIONS
{
    . = 0x01000000;
    .text : { *(.text) }
    . = ALIGN(0x10000);
    .data : { *(.data) }
    .bss : { *(.bss) }
}

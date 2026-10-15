ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS
{
  . = 0x40010000;
  .text : { KEEP(*(.text.start)) *(.text*) } :code
  .rodata : { *(.rodata*) } :code
  .ARM.extab : { *(.ARM.extab*) } :code
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } :code
  .data ALIGN(4096) : { __data_start__ = .; *(.data*) . = ALIGN(4); __data_end__ = .; } :data
  __data_load__ = LOADADDR(.data);
  .init_array : { KEEP(*(.init_array*)) } :data
  .fini_array : { KEEP(*(.fini_array*)) } :data
  .bss ALIGN(128) (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end__ = .; } :data
  .stack ALIGN(16) (NOLOAD) : { __stack_bottom__ = .; . += 128K; __stack_top__ = .; } :data
  ASSERT(__stack_top__ < 0x48000000, "Diagnostic exceeds 128 MiB RAM")
  /DISCARD/ : { *(.comment) *(.note*) }
}

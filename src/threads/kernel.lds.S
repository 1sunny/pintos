#include "threads/loader.h"

/* 指定输出文件的格式为elf32-i386，表示32位的ELF格式，适用于x86架构。*/
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH("i386")
ENTRY(start)			/* Kernel starts at "start" symbol. */
SECTIONS
{
  /* Specify the kernel base address. */
  _start = LOADER_PHYS_BASE + LOADER_KERN_BASE;

  /* 设置当前地址（.）为_start加上文件头的大小，确保文件头和实际的代码不重叠。*/
  /* Make room for the ELF headers. */
  . = _start + SIZEOF_HEADERS;

  /* 定义.text段，包含所有输入文件中的.start和.text段。*/
  /* = 0x90表示用0x90（即x86的NOP指令）填充该段未使用的部分。*/
  /* Kernel starts with code, followed by read-only data and writable data. */
  .text : { *(.start) *(.text) } = 0x90
  .rodata : { *(.rodata) *(.rodata.*) 
	      . = ALIGN(0x1000); 
	      _end_kernel_text = .; }
  /* 可能和i686有关? */
  .eh_frame : { *(.eh_frame) }
  .data : { *(.data) 
	    _signature = .; LONG(0xaa55aa55) }

  .plt : { *(.plt*) }

  /* 定义符号_start_bss，表示.bss段的开始地址。*/
  /* BSS (zero-initialized data) is after everything else. */
  _start_bss = .;
  .bss : { *(.bss) }
  _end_bss = .;

  _end = .;

  ASSERT (_end - _start <= 512K, "Kernel image is too big.")
}

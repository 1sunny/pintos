	#include "threads/loader.h"

#### Kernel startup code.

#### The loader (in loader.S) loads the kernel at physical address
#### 0x20000 (128 kB) and jumps to "start", defined here.  This code
#### switches from real mode to 32-bit protected mode and calls
#### pintos_init().

/* Flags in control register 0. */
#define CR0_PE 0x00000001      /* Protection Enable. */
#define CR0_EM 0x00000004      /* (Floating-point) Emulation. */
#define CR0_PG 0x80000000      /* Paging. */
#define CR0_WP 0x00010000      /* Write-Protect enable in kernel mode. */

	.section .start

# The following code runs in real mode, which is a 16-bit code segment.
	.code16

.func start
.globl start
start:

# The loader called into us with CS = 0x2000, SS = 0x0000, ESP = 0xf000,
# but we should initialize the other segment registers.

	mov $0x2000, %ax
	mov %ax, %ds
	mov %ax, %es

# Set string instructions to go upward.
	cld

#### Get memory size, via interrupt 15h function 88h (see [IntrList]),
#### which returns AX = (kB of physical memory) - 1024.  This only
#### works for memory sizes <= 65 MB, which should be fine for our
#### purposes.  We cap memory at 64 MB because that's all we prepare
#### page tables for, below.

	movb $0x88, %ah
	int $0x15
	addl $1024, %eax	# Total kB memory
	cmp $0x10000, %eax	# Cap at 64 MB
	jbe 1f
	mov $0x10000, %eax
1:	shrl $2, %eax		# Total 4 kB pages
	addr32 movl %eax, init_ram_pages - LOADER_PHYS_BASE - 0x20000

#### Enable A20.  Address line 20 is tied low when the machine boots,
#### which prevents addressing memory about 1 MB.  This code fixes it.

# Poll status register while busy.

1:	inb $0x64, %al
	testb $0x2, %al
	jnz 1b

# Send command for writing output port.

	movb $0xd1, %al
	outb %al, $0x64

# Poll status register while busy.

1:	inb $0x64, %al
	testb $0x2, %al
	jnz 1b

# Enable A20 line.

	movb $0xdf, %al
	outb %al, $0x60

# Poll status register while busy.

1:	inb $0x64, %al
	testb $0x2, %al
	jnz 1b

#### Create temporary page directory and page table and set page
#### directory base register.

# Create page directory at 0xf000 (60 kB) and fill with zeroes.
	mov $0xf00, %ax
	mov %ax, %es
	subl %eax, %eax
	subl %edi, %edi
	movl $0x400, %ecx
	rep stosl

# Add PDEs to point to page tables for the first 64 MB of RAM.
# Also add identical PDEs starting at LOADER_PHYS_BASE.
# See [IA32-v3a] section 3.7.6 "Page-Directory and Page-Table Entries"
# for a description of the bits in %eax.

	movl $0x10007, %eax
	movl $0x11, %ecx
	subl %edi, %edi
1:	movl %eax, %es:(%di)
	movl %eax, %es:LOADER_PHYS_BASE >> 20(%di)
	addw $4, %di
	addl $0x1000, %eax
	loop 1b

# Set up page tables for one-to-map linear to physical map for the
# first 64 MB of RAM.
# See [IA32-v3a] section 3.7.6 "Page-Directory and Page-Table Entries"
# for a description of the bits in %eax.

	movw $0x1000, %ax
	movw %ax, %es
	movl $0x7, %eax
	movl $0x4000, %ecx
	subl %edi, %edi
1:	movl %eax, %es:(%di)
	addw $4, %di
	addl $0x1000, %eax
	loop 1b

# Set page directory base register.

	movl $0xf000, %eax
	movl %eax, %cr3

#### Switch to protected mode.

# First, disable interrupts.  We won't set up the IDT until we get
# into C code, so any interrupt would blow us away.

	cli

# Protected mode requires a GDT, so point the GDTR to our GDT.
# We need a data32 prefix to ensure that all 32 bits of the GDT
# descriptor are loaded (default is to load only 24 bits).
# The CPU doesn't need an addr32 prefix but ELF doesn't do 16-bit
# relocations.

	data32 addr32 lgdt gdtdesc - LOADER_PHYS_BASE - 0x20000

# CR0寄存器是x86体系结构中的一个控制寄存器（Control Register），
# 它在保护模式下控制着处理器的操作模式和状态。
# CR0寄存器中包含多个标志位，这些标志位可以开启或关闭处理器的一些重要特性。
# 一些关键的CR0标志位包括：
# PE（Protection Enable）位：当设置为1时，处理器进入保护模式；设置为0时，处理器处于实模式。
# MP（Monitor co-Processor）位：控制位于WAIT指令的处理方式。
# EM（Emulation）位：用于禁用x87浮点单元，当设置为1时，所有x87浮点指令会触发一个异常，允许系统模拟FP运算。
# TS（Task Switched）位：用于决定是否需要保存浮点状态，每次任务切换时可能会被设置。
# ET（Extension Type）位：指示浮点处理器的类型，目前通常设置为1。
# NE（Numeric Error）位：用于启用x87浮点处理器的内部异常处理。
# WP（Write Protect）位：当设置为1时，会阻止超级用户级别的代码向只读页面写入。
# AM（Alignment Mask）位：用于自动检查用户程序的内存对齐。

# Then we turn on the following bits in CR0:
#    PE (Protect Enable): this turns on protected mode.
#    PG (Paging): turns on paging.
#    WP (Write Protect): if unset, ring 0 code ignores
#       write-protect bits in page tables (!).
#    EM (Emulation): forces floating-point instructions to trap.
#       We don't support floating point.

	movl %cr0, %eax
	orl $CR0_PE | CR0_PG | CR0_WP | CR0_EM, %eax
	movl %eax, %cr0

# We're now in protected mode in a 16-bit segment.  The CPU still has
# the real-mode code segment cached in %cs's segment descriptor.  We
# need to reload %cs, and the easiest way is to use a far jump.
# Because we're not running in a 32-bit segment the data32 prefix is
# needed to jump to a 32-bit offset in the target segment.

	data32 ljmp $SEL_KCSEG, $1f

# We're now in protected mode in a 32-bit segment.
# Let the assembler know.

	.code32

# Reload all the other segment registers and the stack pointer to
# point into our new GDT.

1:	mov $SEL_KDSEG, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss
	addl $LOADER_PHYS_BASE, %esp
	movl $0, %ebp			# Null-terminate main()'s backtrace

#### Call pintos_init().

	call pintos_init

# pintos_init() shouldn't ever return.  If it does, spin.

1:	jmp 1b
.endfunc

#### GDT

	.align 8
gdt:
	.quad 0x0000000000000000	# Null segment.  Not used by CPU.
	.quad 0x00cf9a000000ffff	# System code, base 0, limit 4 GB.
	.quad 0x00cf92000000ffff        # System data, base 0, limit 4 GB.

gdtdesc:
	.word	gdtdesc - gdt - 1	# Size of the GDT, minus 1 byte.
	.long	gdt			# Address of the GDT.

#### Physical memory size in 4 kB pages.  This is exported to the rest
#### of the kernel.
.globl init_ram_pages
init_ram_pages:
	.long 0


#ifndef THREADS_LOADER_H
#define THREADS_LOADER_H

/** 16位real mode */
/** Constants fixed by the PC BIOS. */
#define LOADER_BASE 0x7c00      /**< Physical address of loader's base. */
#define LOADER_END  0x7e00      /**< Physical address of end of loader. */

/** Physical address of kernel base. */
#define LOADER_KERN_BASE 0x20000       /**< 128 kB. */

/** Next, the loader creates a basic page table.
This page table maps the 64 MB at the base of virtual memory
 (starting at virtual address 0) directly to the identical physical addresses.
It also maps the same physical memory starting at virtual address LOADER_PHYS_BASE,
 which defaults to 0xc0000000 (3 GB).*/
/** Kernel virtual address at which all physical memory is mapped.
   Must be aligned on a 4 MB boundary. */
#define LOADER_PHYS_BASE 0xc0000000     /**< 3 GB. */

/** Important loader physical addresses. */
#define LOADER_SIG (LOADER_END - LOADER_SIG_LEN)   /**< 0xaa55 BIOS signature. */
#define LOADER_PARTS (LOADER_SIG - LOADER_PARTS_LEN)     /**< Partition table. */
#define LOADER_ARGS (LOADER_PARTS - LOADER_ARGS_LEN)   /**< Command-line args. */
#define LOADER_ARG_CNT (LOADER_ARGS - LOADER_ARG_CNT_LEN) /**< Number of args. */

/** Sizes of loader data structures. */
#define LOADER_SIG_LEN 2
#define LOADER_PARTS_LEN 64
#define LOADER_ARGS_LEN 128
#define LOADER_ARG_CNT_LEN 4

/** GDT selectors defined by loader.
   More selectors are defined by userprog/gdt.h. */
#define SEL_NULL        0x00    /**< Null selector. */
#define SEL_KCSEG       0x08    /**< Kernel code selector. */
#define SEL_KDSEG       0x10    /**< Kernel data selector. */

#ifndef __ASSEMBLER__
#include <stdint.h>

/**< The startup code's first task is actually to obtain the machine's memory size,
by asking the BIOS for the PC's memory size.
The simplest BIOS function to do this can only detect up to 64 MB of RAM,
so that's the practical limit that Pintos can support.
The function stores the memory size, in pages, in global variable init_ram_pages
 */
/** Amount of physical memory, in 4 kB pages. */
extern uint32_t init_ram_pages;
#endif

#endif /**< threads/loader.h */

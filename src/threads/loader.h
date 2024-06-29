#ifndef THREADS_LOADER_H
#define THREADS_LOADER_H

// 16位real mode
/** Constants fixed by the PC BIOS. */
#define LOADER_BASE 0x7c00      /**< Physical address of loader's base. */
#define LOADER_END  0x7e00      /**< Physical address of end of loader. */

/** Physical address of kernel base. */
#define LOADER_KERN_BASE 0x20000       /**< 128 kB. */

// 创建基本页表:
// 加载程序(loader)创建一个基本的页表,用于内存管理.

// 映射虚拟内存的前64 MB:
// 页表将从虚拟地址0开始的64 MB内存直接映射到相同的物理地址. 这意味着虚拟地址0到64 MB之间的内存直接对应于物理地址0到64 MB之间的内存.

// 映射从虚拟地址 LOADER_PHYS_BASE 开始的内存:
// 页表还将从虚拟地址 LOADER_PHYS_BASE(默认为0xc0000000,即3 GB)开始的虚拟内存映射到相同的物理地址. 这意味着从虚拟地址 LOADER_PHYS_BASE 开始的内存映射到物理内存的起始位置.

// Next, the loader creates a basic page table.
// This page table maps the 64 MB at the base of virtual memory
// (starting at virtual address 0) directly to the identical physical addresses.
// It also maps the same physical memory starting at virtual address LOADER_PHYS_BASE,
// which defaults to 0xc0000000 (3 GB).
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

// 获取内存大小:
// 启动代码的第一任务是获取机器的内存大小.
// 这是通过向 BIOS 请求 PC 的内存大小来实现的.

// BIOS 功能限制:
// 最简单的 BIOS 功能只能检测到最多 64 MB 的 RAM.
// 因此,这也是 Pintos 操作系统实际能支持的最大内存大小.

// 内存大小存储:
// 该功能将内存大小以页(pages)为单位存储在全局变量 init_ram_pages 中

// TODO 在哪设置的?
//  The startup code's first task is actually to obtain the machine's memory size,
//  by asking the BIOS for the PC's memory size.
//  The simplest BIOS function to do this can only detect up to 64 MB of RAM,
//  so that's the practical limit that Pintos can support.
//  The function stores the memory size, in pages, in global variable init_ram_pages
/** Amount of physical memory, in 4 kB pages. */
extern uint32_t init_ram_pages;
#endif

#endif /**< threads/loader.h */

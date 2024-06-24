#ifndef USERPROG_PAGEDIR_H
#define USERPROG_PAGEDIR_H

// 32位虚拟地址保存20位page number和12位页偏移
//                31               12 11        0
//               +-------------------+-----------+
//               |    Page Number    |   Offset  |
//               +-------------------+-----------+
//                        Virtual Address


// 页表接口使用 uint32_t* 表示页表,因为这样可以方便访问其内部结构


// Address Translation
// 实际上,80x86 架构上的虚拟到物理转换是通过中间"linear address"进行的,
// 但 Pintos(以及大多数现代 80x86 操作系统)将 CPU 设置为线性地址和虚拟地址是相同的

// 1.虚拟地址(31-22)表示page directory的物理地址
// 2.虚拟地址(21-12)表示page table的物理地址
// 3.虚拟地址(11-0)表示偏移

// 先通过 Page Directory Index 找到 Page Table 的地址,
// 再通过 Page Table Index 找到 Data Page 的地址,
// 再加上 Page Offset 得到实际物理地址.

/*
  31                  22 21                  12 11                   0
+----------------------+----------------------+----------------------+
| Page Directory Index |   Page Table Index   |    Page Offset       |
+----------------------+----------------------+----------------------+
             |                    |                     |
     _______/             _______/                _____/
    /                    /                       /
   /    Page Directory  /      Page Table       /    Data Page
  /     .____________. /     .____________.    /   .____________.
  |1,023|____________| |1,023|____________|    |   |____________|
  |1,022|____________| |1,022|____________|    |   |____________|
  |1,021|____________| |1,021|____________|    \__\|____________|
  |1,020|____________| |1,020|____________|       /|____________|
  |     |            | |     |            |        |            |
  |     |            | \____\|            |_       |            |
  |     |      .     |      /|      .     | \      |      .     |
  \____\|      .     |_      |      .     |  |     |      .     |
       /|      .     | \     |      .     |  |     |      .     |
        |      .     |  |    |      .     |  |     |      .     |
        |            |  |    |            |  |     |            |
        |____________|  |    |____________|  |     |____________|
       4|____________|  |   4|____________|  |     |____________|
       3|____________|  |   3|____________|  |     |____________|
       2|____________|  |   2|____________|  |     |____________|
       1|____________|  |   1|____________|  |     |____________|
       0|____________|  \__\0|____________|  \____\|____________|
                           /                      /
 */

// Page Directory Entry和Page Table Entry结构一样, 只是Physical Address含义不同
// Page Table Entry Format
/*
  31                                   12 11 9      6 5     2 1 0
+---------------------------------------+----+----+-+-+---+-+-+-+
|           Physical Address            | AVL|    |D|A|   |U|W|P|
+---------------------------------------+----+----+-+-+---+-+-+-+
 */

#include <stdbool.h>
#include <stdint.h>

uint32_t *pagedir_create (void);
void pagedir_destroy (uint32_t *pd);
bool pagedir_set_page (uint32_t *pd, void *upage, void *kpage, bool rw);
void *pagedir_get_page (uint32_t *pd, const void *upage);
void pagedir_clear_page (uint32_t *pd, void *upage);
bool pagedir_is_dirty (uint32_t *pd, const void *upage);
void pagedir_set_dirty (uint32_t *pd, const void *upage, bool dirty);
bool pagedir_is_accessed (uint32_t *pd, const void *upage);
void pagedir_set_accessed (uint32_t *pd, const void *upage, bool accessed);
void pagedir_activate (uint32_t *pd);

#endif /**< userprog/pagedir.h */

#ifndef THREADS_PALLOC_H
#define THREADS_PALLOC_H

#include <stddef.h>

// page分配器容易产生碎片,即使n个或更多页是空闲的,也可能无法分配n个连续的页,因为空闲页被已用页分隔开
// 事实上,在不正常的情况下,即使池中一半的page是空闲的,也可能无法分配 2 个连续的page
// 单页请求不会因为碎片而失败,因此应尽可能限制对多个连续page的请求

// page可能无法从中断上下文中分配,但可以释放

// 当一个page被释放时，它的所有字节都被清除 0xcc

/** How to allocate pages. */
enum palloc_flags
  {
    // 这仅适用于内核初始化期间,决不应该允许用户进程引起内核panic
    PAL_ASSERT = 001,           /**< Panic on failure. */
    PAL_ZERO = 002,             /**< Zero page contents. */
    // 从用户池中获取page,如果未设置,则从内核池中分配page
    PAL_USER = 004              /**< User page. */
  };

void palloc_init (size_t user_page_limit);
void *palloc_get_page (enum palloc_flags);
void *palloc_get_multiple (enum palloc_flags, size_t page_cnt);
void palloc_free_page (void *);
void palloc_free_multiple (void *, size_t page_cnt);

#endif /**< threads/palloc.h */

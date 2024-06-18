#ifndef THREADS_MALLOC_H
#define THREADS_MALLOC_H

// 它位于上一节中描述的页面分配器之上
// 块分配器返回的块是从[[[内核池]]]中获取的

// 块分配器使用两种不同的策略来分配内存
// 第一种策略适用于 1 kB 或更小的块(页面大小的四分之一)
// 这些分配将向上舍入到最接近的 2 次方或 16 字节(以较大者为准), 然后它们被分组到一个页面中,仅用于该大小的分配
// 第二种策略适用于大于 1 kB 的块, 这些分配(加上少量的开销)将向上舍入到大小最接近的页面,
// 然后块分配器从页面分配器请求该数量的[[[连续页面]]]

// Notes:
// 只要可以从页面分配器获取页面,小型分配总是会成功
// 大多数小型分配根本不需要页面分配器提供新页面,因为它们满足使用已分配页面的一部分
// 但是,大型分配始终需要调用页面分配器,并且任何需要多个连续页面的分配都可能由于碎片而失败
// 因此,您应该最大限度地减少代码中大型分配的数量,尤其是每个超过大约 4 kB 的分配
// [[[块分配器不能从中断上下文中调用]]]

#include <debug.h>
#include <stddef.h>

void malloc_init (void);
void *malloc (size_t) __attribute__ ((malloc));
void *calloc (size_t, size_t) __attribute__ ((malloc));
void *realloc (void *, size_t);
void free (void *);

#endif /**< threads/malloc.h */

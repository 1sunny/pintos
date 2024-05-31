#ifndef __LIB_STDDEF_H
#define __LIB_STDDEF_H

#define NULL ((void *) 0)

// 为什么不会实际访问地址 0 ?
// 编译器在处理 offsetof 宏时，会在编译时计算表达式 &((TYPE *) 0)->MEMBER 的值，而不是在运行时，是纯粹的编译时操作。
// 编译器知道这个表达式的目的是计算偏移量而不是实际访问内存，所以它不会产生任何真正的内存访问代码。
// offsetof 宏用于获取一个结构体成员相对于结构体起始地址的偏移量。
// 1. (TYPE *) 0: 将地址 0 强制转换为 TYPE 类型的指针。这只是一个临时指针，并不会实际访问地址 0。
// 2. &((TYPE *) 0)->MEMBER: 通过这个指针访问 MEMBER 成员，并取该成员的地址。
//    这将得到 MEMBER 成员相对于结构体起始地址的偏移量。
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *) 0)->MEMBER)

/** GCC predefines the types we need for ptrdiff_t and size_t,
   so that we don't have to guess. */
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __SIZE_TYPE__ size_t;

#endif /**< lib/stddef.h */

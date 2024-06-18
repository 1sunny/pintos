#ifndef USERPROG_GDT_H
#define USERPROG_GDT_H

#include "threads/loader.h"

// x86段寄存器和分段机制 - 小乐叔叔的文章 - 知乎
// https://zhuanlan.zhihu.com/p/324210723
// 在x86保护模式下,段的信息(段基线性地址,长度,权限等)即段描述符占8个字节,
// 段信息无法直接存放在段寄存器中(段寄存器只有2字节).
// Intel的设计是段描述符集中存放在GDT(Global Descriptor Table)或LDT(Local Descriptor Table)中,
// 分别保存在新增的寄存器GDTR和LDTR中. 而段寄存器CS DS SS ES存放的是段描述符在GDT或LDT内的索引值(index).

// 在x86架构中,段选择器(Segment Selectors)是用于选择段描述符的寄存器值.
// 段描述符定义了内存段的属性,比如基地址,段限长和访问权限.
// 段选择器本质上是一个索引,用于从全局描述符表(GDT)或局部描述符表(LDT)中选择一个特定的段描述符
// 段选择器在段寄存器(如 CS, DS, SS, ES, FS, GS)中使用,以便处理器知道应该使用哪个段描述符来进行内存访问

// 段选择器的结构
// 段选择器是一个16位的值,它包含三个主要部分:
// 索引(Index): 指定段描述符在GDT或LDT中的位置
// TI位(Table Indicator): 指示该索引是在GDT中(TI=0)还是在LDT中(TI=1)
// RPL(Requested Privilege Level): 请求的特权级别,用于权限检查

/** Segment selectors.
   More selectors are defined by the loader in loader.h. */
#define SEL_UCSEG       0x1B    /**< User code selector. */
#define SEL_UDSEG       0x23    /**< User data selector. */
#define SEL_TSS         0x28    /**< Task-state segment. */
#define SEL_CNT         6       /**< Number of segments. */

void gdt_init (void);

#endif /**< userprog/gdt.h */

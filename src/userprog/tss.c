#include "userprog/tss.h"
#include <debug.h>
#include <stddef.h>
#include "userprog/gdt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"

/** The Task-State Segment (TSS).

   Instances of the TSS, an x86-specific structure, are used to
   define "tasks", a form of support for multitasking built right
   into the processor.  However, for various reasons including
   portability, speed, and flexibility, most x86 OSes almost
   completely ignore the TSS.  We are no exception.
   TSS是x86特有的结构,用于定义"任务",任务是一种硬件支持的多任务处理方式.
   虽然TSS在多任务处理方面提供了一些支持,但由于多种原因(包括可移植性,速度和灵活性),
   大多数x86操作系统几乎完全忽略了TSS. Pintos操作系统也不例外.

   Unfortunately, there is one thing that can only be done using
   a TSS: stack switching for interrupts that occur in user mode.
   When an interrupt occurs in user mode (ring 3), the processor
   consults the ss0 and esp0 members of the current TSS to
   determine the stack to use for handling the interrupt.  Thus,
   we must create a TSS and initialize at least these fields, and
   this is precisely what this file does.
   尽管大多数情况下忽略了TSS,但有一种情况必须使用TSS:当用户模式(ring 3)下发生中断时.
   此时,处理器会查阅当前TSS中的ss0和esp0成员,确定处理中断时要使用的堆栈. (也就是内核栈?)
   因此,我们必须创建一个TSS并初始化至少这两个字段,这正是这段代码所做的工作.

   When an interrupt is handled by an interrupt or trap gate
   (which applies to all interrupts we handle), an x86 processor
   works like this:

     - If the code interrupted by the interrupt is in the same
       ring as the interrupt handler, then no stack switch takes
       place.  This is the case for interrupts that happen when
       we're running in the kernel.  The contents of the TSS are
       irrelevant for this case.
       如果中断发生时被中断的代码与中断处理程序在同一个特权级(ring)中,则不会发生堆栈切换.
       这种情况通常发生在内核态(ring 0)下. 对于这种情况，TSS的内容无关紧要.

     - If the interrupted code is in a different ring from the
       handler, then the processor switches to the stack
       specified in the TSS for the new ring.  This is the case
       for interrupts that happen when we're in user space.  It's
       important that we switch to a stack that's not already in
       use, to avoid corruption.  Because we're running in user
       space, we know that the current process's kernel stack is
       not in use, so we can always use that.  Thus, when the
       scheduler switches threads, it also changes the TSS's
       stack pointer to point to the new thread's kernel stack.
       (The call is in thread_schedule_tail() in thread.c.)
       如果被中断的代码与中断处理程序在不同的特权级,则处理器会切换到TSS中为新特权级别指定的堆栈.
       这种情况发生在用户空间(ring 3)发生中断时.
       此时,重要的是切换到一个未被使用的堆栈,以避免数据损坏.
       因为我们运行在用户空间,当前进程的[[[内核堆栈是空闲的]]],所以我们可以使用它.

   See [IA32-v3a] 6.2.1 "Task-State Segment (TSS)" for a
   description of the TSS.  See [IA32-v3a] 5.12.1 "Exception- or
   Interrupt-Handler Procedures" for a description of when and
   how stack switching occurs during an interrupt. */
struct tss
  {
    uint16_t back_link, :16;
    void *esp0;                         /**< Ring 0 stack virtual address. */
    uint16_t ss0, :16;                  /**< Ring 0 stack segment selector. */
    void *esp1;
    uint16_t ss1, :16;
    void *esp2;
    uint16_t ss2, :16;
    uint32_t cr3;
    void (*eip) (void);
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint16_t es, :16;
    uint16_t cs, :16;
    uint16_t ss, :16;
    uint16_t ds, :16;
    uint16_t fs, :16;
    uint16_t gs, :16;
    uint16_t ldt, :16;
    uint16_t trace, bitmap;
  };

/** Kernel TSS. */
static struct tss *tss;

/** Initializes the kernel TSS. */
void
tss_init (void) 
{
  /* Our TSS is never used in a call gate or task gate, so only a
     few fields of it are ever referenced, and those are the only
     ones we initialize. */
  tss = palloc_get_page (PAL_ASSERT | PAL_ZERO);
  tss->ss0 = SEL_KDSEG;
  // TODO bitmap是干啥的
  tss->bitmap = 0xdfff;
  tss_update ();
}

/** Returns the kernel TSS. */
struct tss *
tss_get (void) 
{
  ASSERT (tss != NULL);
  return tss;
}

/** Sets the [[[ ring 0 stack pointer ]]] in the TSS to point to the end
   of the thread stack. */
void
tss_update (void) 
{
  ASSERT (tss != NULL);
  // 将esp0指向当前线程的地址末尾,一个线程只有一个Page大小
  // TODO 为什么指向末尾,内核栈从末尾开始吗?
  tss->esp0 = (uint8_t *) thread_current () + PGSIZE;
}

#ifndef THREADS_INTERRUPT_H
#define THREADS_INTERRUPT_H

#include <stdbool.h>
#include <stdint.h>

// =======================================================================================================
// 中断分类:
// Internal interrupts, that is, interrupts [[[ caused directly by CPU instructions. ]]]
//   1. System calls, attempts at invalid memory access (page faults),
//      and attempts to divide by zero are some activities that cause internal interrupts.
//   2. Because they are caused by CPU instructions,
//      internal interrupts are synchronous or synchronized with CPU instructions.
//      [[[[[ intr_disable() does not disable internal interrupts. ]]]]]
//
// External interrupts, that is, interrupts originating outside the CPU.
//   1. These interrupts come from hardware devices such as the system timer,
//      keyboard, serial ports, and disks.
//   2. External interrupts are asynchronous,
//      meaning that their delivery is not synchronized with instruction execution.
//      Handling of external interrupts can be postponed with intr_disable()
//      and related functions (see section Disabling Interrupts).
// =======================================================================================================
// The CPU treats both classes of interrupts largely the same way,
// so Pintos has common infrastructure to handle both classes.
// =======================================================================================================
// Interrupt Infrastructure
// When an interrupt occurs, the CPU saves its most essential state on a stack
// and jumps to an interrupt handler routine.
//   1. The 80x86 architecture supports 256 interrupts, numbered 0 through 255,
//      each with an independent handler defined in an array called the interrupt descriptor table or IDT.
// =======================================================================================================

/** Interrupts on or off? */
enum intr_level 
  {
    INTR_OFF,             /**< Interrupts disabled. */
    INTR_ON               /**< Interrupts enabled. */
  };

// 很少直接设置中断state, 大多数时候应该使用其它synchronization primitives
//   1. 关中断的主要目的是同步kernel threads和外中断handler, 外中断handler处理时不能sleep
//   2. 即使是关中断, 一些外中断也不能被推迟, called non-maskable interrupts.
//      用于处理紧急情况, 比如电脑着火
// 返回当前中断state
enum intr_level intr_get_level (void);
// 返回之前的中断state
enum intr_level intr_set_level (enum intr_level);
enum intr_level intr_enable (void);

// [[[[[ intr_disable() does not disable internal interrupts. ]]]]]
// Turns interrupts off.
// Returns the previous interrupt state.
enum intr_level intr_disable (void);


// [[[ The stack frame of an interrupt handler, ]]] 人为构建的栈帧
// as saved by the CPU, the interrupt stubs, and intr_entry()
/** Interrupt stack frame. */
struct intr_frame
  {
    /* Pushed by intr_entry in intr-stubs.S.
       [[[[[ These are the interrupted task's saved registers. ]]]]] */
    uint32_t edi;               /**< Saved EDI. */
    uint32_t esi;               /**< Saved ESI. */
    uint32_t ebp;               /**< Saved EBP. */
    uint32_t esp_dummy;         /**< Not used. */
    uint32_t ebx;               /**< Saved EBX. */
    uint32_t edx;               /**< Saved EDX. */
    uint32_t ecx;               /**< Saved ECX. */
    uint32_t eax;               /**< Saved EAX. */
    uint16_t gs, :16;           /**< Saved GS segment register. */
    uint16_t fs, :16;           /**< Saved FS segment register. */
    uint16_t es, :16;           /**< Saved ES segment register. */
    uint16_t ds, :16;           /**< Saved DS segment register. */

    /* Pushed by intrNN_stub in intr-stubs.S. */
    uint32_t vec_no;            /**< Interrupt vector number. */

    /* Sometimes pushed by the CPU,
       otherwise for consistency pushed as 0 by intrNN_stub.
       The CPU puts it just under `eip', but we move it here. */
    uint32_t error_code;        /**< Error code. */

    // 为什么要frame_pointer???
    /* Pushed by intrNN_stub in intr-stubs.S.
       This frame pointer eases(缓解) interpretation of backtraces. */
    void *frame_pointer;        /**< Saved EBP (frame pointer). */

    /* [[[ Pushed by the CPU. 就是中断产生的时候CPU自动push的,然后才会进入xxx ]]]
       These are the interrupted task's saved registers. */
    // 被打断线程下一条应该执行的指令
    void (*eip) (void);         /**< Next instruction to execute. */
    uint16_t cs, :16;           /**< Code segment for eip. */
    uint32_t eflags;            /**< Saved CPU flags. */
    void *esp;                  /**< Saved stack pointer. */
    uint16_t ss, :16;           /**< Data segment for esp. */
  };

typedef void intr_handler_func (struct intr_frame *);

void intr_init (void);
void intr_register_ext (uint8_t vec, intr_handler_func *, const char *name);
void intr_register_int (uint8_t vec, int dpl, enum intr_level,
                        intr_handler_func *, const char *name);
// 返回当前是否在处理外中断
bool intr_context (void);
void intr_yield_on_return (void);

void intr_dump_frame (const struct intr_frame *);
const char *intr_name (uint8_t vec);

#endif /**< threads/interrupt.h */

#include "threads/switch.h"

#### [[[[[[[[[[[[[[[[ !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
#### struct thread *switch_threads (struct thread *cur, struct thread *next);
####
#### Switches from CUR, which must be the running thread, to NEXT,
#### [[[ which must also be running switch_threads(), returning CUR in
#### NEXT's context. ]]]
#### 这个函数works假设我们要切换到的那个thread也在running switch_threads,只是运行到switch_threads被切换了
#### 所以只需要保存少部分寄存器到栈上(TODO 为什么只需要保存少部分?)
#### This function works by assuming that the thread we're switching
#### into is also running switch_threads().  Thus, all it has to do is
#### preserve a few registers on the stack, [[[[[[[[[[ then switch stacks ]]]]]]]]]] and
#### restore the registers.  [[[[ As part of switching stacks we record the
#### current stack pointer in CUR's thread structure. ]]]]
#### [[[[[[[[[[[[[[[[ !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

.globl switch_threads
.func switch_threads
switch_threads:
	# Save caller's register state.
	# TODO 为什么可以 destroy %eax, %ecx, %edx
	# Note that the SVR4 ABI allows us to destroy %eax, %ecx, %edx,
	# but requires us to preserve %ebx, %ebp, %esi, %edi.  See
	# [SysV-ABI-386] pages 3-11 and 3-12 for details.
	#
	# This stack frame must match the one set up by thread_create()
	# in size.
	pushl %ebx
	pushl %ebp
	pushl %esi
	pushl %edi

	# Get offsetof (struct thread, stack).
.globl thread_stack_ofs
	mov thread_stack_ofs, %edx

// struct switch_threads_frame
//  {
//    uint32_t edi;               /**<  0: Saved %edi. */
//    uint32_t esi;               /**<  4: Saved %esi. */
//    uint32_t ebp;               /**<  8: Saved %ebp. */
//    uint32_t ebx;               /**< 12: Saved %ebx. */
//    void (*eip) (void);         /**< 16: Return address. */
//    struct thread *cur;         /**< 20: switch_threads()'s CUR argument. */
//    struct thread *next;        /**< 24: switch_threads()'s NEXT argument. */
//  };
// struct thread *switch_threads (struct thread *cur, struct thread *next);
// SWITCH_CUR = edi, esi, ebp, ebx, return address, cur, next
//             ^
//            esp
// 可以看 struct switch_threads_frame
# /** Offsets used by switch.S. */
#  #define SWITCH_CUR      20
#  #define SWITCH_NEXT     24
	# Save current stack pointer to old thread's stack, if any.
	movl SWITCH_CUR(%esp), %eax
# 基址变址寻址的格式: Effective Address = base + (index * scale) + displacement
# (base, index, scale)
# base: 基址寄存器（这里是 %eax），包含了基址地址。
# index: 变址寄存器（这里是 %edx），提供了偏移量。
# scale: 比例因子，表示变址寄存器的值要乘以的系数。合法的比例因子是 1、2、4、8。
# 将当前的栈指针 %esp 保存到当前线程的 stack 成员中
	movl %esp, (%eax,%edx,1)

	# Restore stack pointer from new thread's stack.
	movl SWITCH_NEXT(%esp), %ecx
	movl (%ecx,%edx,1), %esp

// 这里因为esp已经换成了之前线程的了,所以pop出来的就是之前线程保存的寄存器
	# Restore caller's register state.
	popl %edi
	popl %esi
	popl %ebp
	popl %ebx
        ret
.endfunc

##################################### [[[[[ 这是内核栈 ]]]]] ####################
##                   4 kB +---------------------------------+
##                        |               aux               |
##                        |            function             |
##                        |            eip (NULL)           | kernel_thread_frame
##                        +---------------------------------+
##                        |      eip (to kernel_thread)     | switch_entry_frame
##                        +---------------------------------+
##                        |               next              |
##                        |               cur               |
##                        |      eip (to switch_entry)      |
##                        |               ebx               |
##                        |               ebp               |
##                        |               esi               |
##                        |               edi               | switch_threads_frame
##                        +---------------------------------+
##                        |          kernel stack           |
##                        |                |                |
##                        |                |                |
##                        |                V                |
##                        |         grows downward          |
## sizeof (struct thread) +---------------------------------+
##                        |              magic              |
##                        |                :                |
##                        |                :                |
##                        |               name              |
##                        |              status             |
##                   0 kB +---------------------------------+
# 所以这个是在模拟schedule的行为,只是没有真正调用switch_threads而已,
# 但线程切换的时候会从switch_threads,switch_threads_frame返回,所以需要Discard switch_threads() arguments(cur,next).
.globl switch_entry
.func switch_entry
switch_entry:
	# Discard switch_threads() arguments.
	addl $8, %esp

	# Call thread_schedule_tail(prev).
	# prev = eax, eax是switch_threads中的cur(被切换的线程)
	pushl %eax
.globl thread_schedule_tail
	call thread_schedule_tail
	# 退回 pushl %eax
	addl $4, %esp

	# Start thread proper.
	ret
.endfunc

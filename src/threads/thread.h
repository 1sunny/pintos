#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include <vm/vm.h>
#include "synch.h"
#include "fixed_point.h"

extern const char* thread_status_names[];

/** States in a thread's life cycle. */
enum thread_status
  {
    // thread_current() returns the running thread
    THREAD_RUNNING,     /**< Running thread. */
    // could be selected to run the next time the scheduler is invoked.
    // Ready threads are kept in a doubly linked list called ready_list.
    THREAD_READY,       /**< Not running but ready to run. */
    // waiting for something,
    // e.g. a lock to become available, an interrupt to be invoked. */
    // won't be scheduled again until it transitions to the THREAD_READY
    // state with a call to thread_unblock()
    THREAD_BLOCKED,     /**< Waiting for an event to trigger. */
    // will be destroyed by the scheduler after switching to the next thread
    THREAD_DYING        /**< About to be destroyed. */
  };

// each new thread receives the numerically next higher tid,
// starting from 1 for the initial process.
/** Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /**< Error value for tid_t. */

// --- Lab2: Task 4 ---
struct child_info {
    tid_t child_tid;
    struct thread *child_thread;
    int child_exit_code;
    bool waited; // 该child是否已经等待过
    struct list_elem elem;
};
// --- Lab2: Task 4 ---

struct open_file {
    struct file *file;
    int fd;
    struct dir *dir;
    struct list_elem elem;
};

/** Thread priorities. */
#define PRI_MIN 0                       /**< Lowest priority. */
#define PRI_DEFAULT 31                  /**< Default priority. */
#define PRI_MAX 63                      /**< Highest priority. */

/* tom: after you read what's below, you might wonder why Pintos
 * allocates the thread
 * control block on the thread kernel stack.  One reason is to save
 * work -- but our goal isn't to build the fastest possible operating
 * system, so that's pretty lame.  Another reason is that "malloc"
 * uses locks and therefore the current thread, and so that means
 * you can't use malloc until the thread system is initalized, but if
 * the thread system needs to call malloc, ...
 *
 * This way, we can find the current thread from
 * the current stack pointer.  (On a uniprocessor, you
 * can stick the current thread in a static variable, but on a multiprocessor,
 * each processor has its own current thread -- but each processor
 * has its own stack pointer, so that works out!)
 *
 * OK, then, why does Pintos put the thread control block at the end
 * of the stack where it might be clobbered by a procedure call, rather
 * than underneath the stack where it will be safe?  I would have, but they
 * chose to do it this way in part because they want to reuse the stack
 * used in "main()" to become a thread stack, and the loader didn't
 * reserve enough room under the stack pointer for the thread control block.
 *
 * Similarly, rather than having a separate list of threads, the
 * list element data structure is embedded inside the thread control
 * block.  This is a bit of a pain for bookkeeping, but it has the upside
 * that you only need to allocate one block of data per thread.
 * This reduces error handling code, in case you might run out of memory --
 * you'll only get that error when you init the thread, not when it
 * tries to wait.
 */
/** A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/** The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /**< Thread identifier. */
    enum thread_status status;          /**< Thread state. */
    char name[16];                      /**< Name (for debugging purposes). */
// Every thread has its own stack to keep track of its state.
// When the thread is running, the CPU's stack pointer register tracks the top of the stack
// and this member is unused.
// TODO 哪行代码保存的? switch.S: movl %esp, (%eax,%edx,1)
// But when the CPU switches to another thread, this member saves the thread's stack pointer.
// No other members are needed to save the thread's registers,
// because the other registers that must be saved are saved on the stack(TODO 什么stack: 内核栈吧).
// TODO 为什么sp不能保存在switch_threads_frame(内核栈)上?
// When an interrupt occurs, whether in the kernel or a user program,
// an struct intr_frame is pushed onto the stack.
// When the interrupt occurs in a user program,
// the struct intr_frame is always at the very top of the page.
// See section Interrupt Handling, for more information.
// TODO
    uint8_t *stack;                     /**< Saved stack pointer. */
    int priority;                       /**< Priority. */
    struct list_elem allelem;           /**< List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /**< List element. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    // uint32_t *pagedir;                  /**< Page directory. */
    struct process* pcb; /* Process control block if this thread is a userprog */
#endif

    // Stack overflow tends to change this value, triggering the assertion.
    /* Owned by thread.c. */
    unsigned magic;                     /**< Detects stack overflow. */
    // --- Lab1: Task 1 ---
    int64_t sleep_until_ticks;
    struct semaphore *sema;
    // --- Lab1: Task 1 ---
    // --- Lab1: Task 2 ---
    int priority_before_donate;
    struct list locks;
    struct lock *waiting_lock;
    int nice;
    fixed_t recent_cpu;
    // --- Lab1: Task 2 ---
    // --- Lab2: Task 4 ---
    // int exit_code;
    // struct thread *parent;
    // struct list child_list;
    // struct child_info *self_in_parent_child_list; // parent中保存自己的信息,需要自己维护
    // tid_t waiting_tid; //正在等待的tid
    // struct semaphore wait_sema; // 执行wait时,child exit时通过这个唤醒自己
    // struct semaphore exec_sema; // 执行exec时,child通过这个唤醒自己
    // int exec_result;
    // --- Lab2: Task 4 ---
    // int next_fd;
    // struct list open_file_list;
    // struct file *executing_file;
#ifdef VM
    /* Table for whole virtual memory owned by thread. */
    struct supplemental_page_table spt;
    void *esp;
    uint32_t stack_bottom;
    mapid_t next_mapid;
#endif
    // struct dir *pwd;
};

/** If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

bool is_thread (struct thread *) UNUSED;
// void thread_print (struct thread *t);
void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/** Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);
void thread_mlfqs_update_priority (struct thread *t);
void thread_mlfqs_increase_recent_cpu_by_one (void);
void thread_mlfqs_update_load_avg_and_recent_cpu (void);

bool thread_less (const struct list_elem *, const struct list_elem *, void *);
#endif /**< threads/thread.h */

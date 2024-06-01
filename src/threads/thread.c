#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/** Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/** List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/** List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/** Idle thread. */
static struct thread *idle_thread;

/** Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/** Lock used by allocate_tid(). */
static struct lock tid_lock;

/** Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /**< Return address. */
    thread_func *function;      /**< Function to call. */
    void *aux;                  /**< Auxiliary data for function. */
  };

/** Statistics. */
static long long idle_ticks;    /**< # of timer ticks spent idle. */
static long long kernel_ticks;  /**< # of timer ticks in kernel threads. */
static long long user_ticks;    /**< # of timer ticks in user programs. */

/** Scheduling. */
#define TIME_SLICE 4            /**< # of timer ticks to give each thread. */
static unsigned thread_ticks;   /**< # of timer ticks since last yield. */

/** If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

// Called by pintos_init() to initialize the thread system.
// Its main purpose is to [[[ create a struct thread for Pintos's initial thread ]]].
// This is possible because the [[[ Pintos loader puts the initial thread's stack at the top of a page ]]],
// in the [[ same position as any other Pintos thread ]].

// Before thread_init() runs, thread_current() will fail
// because the running thread's magic value is incorrect.
// Lots of functions call thread_current() directly or indirectly,
// including lock_acquire() for locking a lock,
// so thread_init() is called early in Pintos initialization.
/** Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  // 保证中断是关闭的
  ASSERT (intr_get_level () == INTR_OFF);

  // tid_lock: Lock used by allocate_tid().
  lock_init (&tid_lock);
  list_init (&ready_list);
  // List of all processes.  Processes are added to this list
  // when they are first scheduled and removed when they exit.
  list_init (&all_list);

  // Returns the running thread pointer: struct thread *
  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

// Called by pintos_init() to start the scheduler.
// Creates the idle thread(空闲线程), that is, the thread that is scheduled when no other thread is ready.
/** Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  // Then enables interrupts, which as a side effect enables the scheduler
  // because the [[[ scheduler runs on return from the timer interrupt ]]],
  // 调度程序会在定时器中断返回时运行
  // using intr_yield_on_return().
  /* Start preemptive thread scheduling(打开中断, 启动抢占式线程调度). */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

// keeps track of thread statistics and triggers the scheduler when a time slice expires.
/** Called by the timer interrupt handler at each timer tick.
   Thus, [[[ this function runs in an external interrupt context. ??? ]]] */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  // 执行抢占
  // TIME_SLICE: # of timer ticks to give each thread
  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

// Called during Pintos shutdown to print thread statistics.
/** Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/** Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and [[[ adds it to the ready queue. ]]]  [[[ Returns the thread identifier ]]]
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then [[[ the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns. ]]]  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) // auxiliary: 表示辅助的或额外的参数
{
// 这里还没关中断, init_thread中关的
  struct thread *t;
  struct kernel_thread_frame *kf;
  // Stack frame for switch_entry()
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO); // PAL_ZERO : Zero page contents
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

// When thread_create() creates a new thread,
// it goes through a fair amount of trouble to get it started properly.
// In particular, the new thread hasn't started running yet,
// so there's no way for it to be running inside switch_threads()
// [[[ as the scheduler expects. ???
// 为什么scheduler expects, 就是说线程切换必须是线程自己运行到schedule函数??? ]]]
// To solve the problem, thread_create() creates some _fake stack frames_
// in the new thread's stack:

  // struct kernel_thread_frame *kf;
  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;          // Return address, 没有返回地址，如果这个函数结束了, 会调用thread_exit
  kf->function = function; // Function to call, 相当于是上一个栈帧里传递的第一个参数
  kf->aux = aux;           // Function arg    , 相当于是上一个栈帧里传递的第二个参数

  // struct switch_entry_frame *ef;
  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  // struct switch_threads_frame *sf;
  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  // point eip to switch_entry(), indicating it to be the function that called switch_entry().
  // 把eip设置为switch_entry, 表示是由switch_entry调用的switch_threads
  sf->eip = switch_entry;  // Return address
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

  return tid;
}

/** Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  // 主动调用thread_block
  schedule ();
}

/** Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  // 在关中断的情况下进行, 防止当前函数被打断?
  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
// --- Lab1: Task 2 ---
  // TODO 这个函数可能在中断上下文中吗?
  // 应该是在intr_disable中调用吧?
  if (t->priority > thread_current()->priority) {
    if (intr_context()) {
      intr_yield_on_return();
    } else {
      thread_yield();
    }
  }
// --- Lab1: Task 2 ---
  intr_set_level (old_level);
}

/** Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/** Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     [[[[[ If either of these assertions fire, then your thread may
     have overflowed its stack. ]]]]]  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/** Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/** Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/** Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());
  // 需要关中断
  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/** Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/** Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  thread_current ()->priority = new_priority;
}

/** Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/** Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  /* Not yet implemented. */
}

/** Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  /* Not yet implemented. */
  return 0;
}

/** Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* Not yet implemented. */
  return 0;
}

/** Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  /* Not yet implemented. */
  return 0;
}

/** Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

// 开中断, 运行thread_create传递的函数, 函数返回后调用thread_exit
/** Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /**< The scheduler runs with interrupts off. */
  function (aux);       /**< Execute the thread function. */
  thread_exit ();       /**< If function() returns, kill the thread. */
}

// 为什么可以直接通过返回page开始的地址就当作struct thread *
// This is possible because the Pintos loader puts the initial thread's stack at the top of a page,
// in the same position as any other Pintos thread.
/** Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/** Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/** Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  // 把sp设置为页的末尾
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;
// --- Lab1: Task 1 ---
  t->sleep_until_ticks = -1;
  t->sema = NULL;
// --- Lab1: Task 1 ---

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/** Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/** Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  ([[[ If the running thread can continue running, then it
   will be in the run queue. ]]])  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
// --- Lab1: Task 2 ---
  // 这个函数应该是在关中断时运行, 所以不存在同步问题
  ASSERT (intr_get_level () == INTR_OFF);
  // TODO idle_thread在哪初始化的? thread_start -> thread_create
  // TODO idle_thread的priority是多少? PRI_MIN(0)
  if (list_empty (&ready_list)) {
    return idle_thread;
  }
  else {
    int priority = -1;
    struct thread *highest_priority_thread = NULL;
    struct list_elem *e;
    for (e = list_begin (&ready_list); e != list_end (&ready_list);
         e = list_next (e))
    {
      struct thread *curr = list_entry (e, struct thread, elem);
      if (curr->priority > priority) {
        priority = curr->priority;
        highest_priority_thread = curr;
      }
    }
    ASSERT (highest_priority_thread != NULL);
    return highest_priority_thread;
  }
// --- Lab1: Task 2 ---

  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/** Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  // thread_exit中设置THREAD_DYING, 然后主动调用schedule
  // initial_thread: Initial thread, the thread running init.c:main()
  // [[[[[ These couldn't be freed prior to the thread switch
  // because the switch needed to use it. ]]]]]
  // 就是说thread_exit时不能palloc_free_page, 因为switch needed to use it(pku文档)...
  /* If the thread we switched from is dying, destroy its struct
     thread. [[[ This must happen late so that thread_exit() doesn't
     pull out the rug under itself. ??? ]]]  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

// called only by the three public thread functions that need to switch threads:
//   thread_block()
//   thread_exit()
//   thread_yield()

// [[[[[ Before any of these functions call schedule(),
// they disable interrupts (or ensure that they are already disabled)
// and then change the running thread's state to something other than running. ]]]]]
/** Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  // 需要关中断才能进行线程调度,TODO 为什么调度的时候不能处理中断?
  // 在调度的时候timer到了,又应该进行调度...
  ASSERT (intr_get_level () == INTR_OFF);
  // status: Thread state
  ASSERT (cur->status != THREAD_RUNNING);
  // Returns true if T appears to point to a valid thread
  ASSERT (is_thread (next));

  // [[[[[ The thread we switched to [[[[[ was also running ]]]]] inside switch_threads(),
  // as are all the threads not currently running, ]]]]]
  // so the new thread now returns out of switch_threads(),
  // returning the previously running thread.
  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/** Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/** Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

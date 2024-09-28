#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include <filesys/file.h>
#include <filesys/filesys.h>
#include <filesys/directory.h>
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
#include "malloc.h"

const char* thread_status_names[] = {"RU", "RE", "B", "D"};

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
  // 保证中断是关闭的, 谁关的? 初始化好就还没开过, pintos_init后续调用thread_start开启
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

// pintos_init中唯一调用
// 用于开起抢占式调度,开启之前只有main一个线程,Called by pintos_init()
// 创建idle thread(min priority), 这个线程在其它现在阻塞时被scheduled
// TODO main线程会被阻塞吗?
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

  dprintf("[%s:%s:%d] call sema_down for idle in thread_start\n",
         thread_current()->name, thread_status_names[thread_current()->status], thread_current()->priority);
  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
  // 中断依然开启的, sema_down关闭后又恢复
}

// [[[ 这个函数不能调用printf ]]]
// keeps track of thread statistics and triggers the scheduler when a time slice expires.
/** Called by the timer interrupt handler at each timer tick.
   Thus, [[[ this function runs in an external interrupt context. ??? ]]] */
void
thread_tick (void) 
{
  // 为什么不加 Assert in an external interrupt context
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pcb != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  // 执行抢占
  // TIME_SLICE: # of timer ticks to give each thread
  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE) {
    // 难道是因为这里, 在外中断中睡眠了
    // dprintf("[%s:%s:%d] call intr_yield_on_return in thread_tick\n",
    //        thread_current ()->name, thread_status_names[thread_current ()->status], thread_current ()->priority);
    intr_yield_on_return ();
  }
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

  t->self_in_parent_child_list = malloc(sizeof(struct child_info));
  if (t->self_in_parent_child_list == NULL) {
    PANIC("out of memory");
  }
  t->self_in_parent_child_list->child_tid = tid;
  t->self_in_parent_child_list->child_thread = t;
  t->self_in_parent_child_list->child_exit_code = 0;
  t->self_in_parent_child_list->waited = false;

  t->parent = thread_current();
  // 把自己的信息添加到parent的child_list中
  list_push_back(&t->parent->child_list, &t->self_in_parent_child_list->elem);

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

  dprintf("[%s:%s:%d] call thread_unblock for [%s:%s:%d] in thread_create\n",
         thread_current()->name, thread_status_names[thread_current()->status], thread_current()->priority,
         t->name, thread_status_names[t->status], t->priority);
  /* Add to run queue. */
  thread_unblock (t);
  // --- Lab1: Task 2 ---
  if (priority > thread_current ()->priority) {
    thread_yield ();
  }
  // --- Lab1: Task 2 ---
  return tid;
}

// [[[ 这个函数不能调用printf ]]]
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
  // 主动调用schedule
  schedule ();
  // schedule后谁来开中断? 好像笔记里记录了
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
  // ASSERT (t->status == THREAD_RUNNING);

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
// TODO 感觉应该把最后的关中断移动到前面来
// #ifdef USERPROG
//   process_exit ();
// #endif

  struct thread *curr = thread_current();
  struct list_elem *e;
  // 在sema_up parent前就要释放打开的文件
  for (e = list_begin (&curr->open_file_list); e != list_end (&curr->open_file_list); ) {
    struct open_file *entry = list_entry(e, struct open_file, elem);
    e = list_next (e);
    lock_acquire(&filesys_lock);
    file_close(entry->file);
    lock_release(&filesys_lock);
    free(entry);
  }

  lock_acquire(&filesys_lock);
  if (curr->executing_file) {
    // file_allow_write();
    file_close(curr->executing_file);
  }
  lock_release(&filesys_lock);

  for (e = list_begin (&curr->child_list); e != list_end (&curr->child_list); ) {
    struct child_info *child = list_entry(e, struct child_info, elem);
    // 先移动迭代器
    e = list_next (e);
    if (child->child_thread) {
      // 如果child还没die, 需要告诉child自己die了
      child->child_thread->parent = NULL;
    } else {
      // 如果child die了, 释放其内存
      free(child);
    }
  }
  // TODO 这里直接访问parent的数据可以吗? 这里应该要同步一下,可能要关个中断,因为下面访问allelem就关了的
  if (curr->parent == NULL) {
    // parent die了, 自己释放动态分配的内存
    free(curr->self_in_parent_child_list);
  } else {
    curr->self_in_parent_child_list->child_exit_code = curr->exit_code;
    if (curr->parent->waiting_tid == curr->tid) {
      // 如果parent在等待自己, 唤醒parent
      curr->self_in_parent_child_list->child_thread = NULL;
      sema_up(&curr->parent->wait_sema);
    } else {
      // 置空parent中保存的信息, 表示已die
      curr->self_in_parent_child_list->child_thread = NULL;
    }
  }

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  dprintf("[%s:%s:%d] exit and call schedule\n", thread_current()->name, thread_status_names[thread_current()->status], thread_current()->priority);
  intr_disable (); // 在哪开的? 线程结束时自己调用的thread_exit, 中断肯定是开的
  // schedule后谁来开启中断啊
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  // schedule后谁来开中断?
  NOT_REACHED ();
}

// 1. kernel_thread调用intr_enable, 然后执行 function
// 2. function被timer打断进入intr_handler, intr_handler中中断是关的(CPU自己关)
// 3. intr_handler处理了timer中断调用thread_yield, 后续通过schedule重新恢复执行
//    但是中断还是关的啊, 什么时候恢复呢: intr-stubs.S中恢复eflags时恢复的
// 只有intr_handler中如果yield_on_return, 就会调用
// yields the CPU但是当前线程不sleep,可能会被立即调度
/** Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());
  // 这里处于关中断(如果只是在intr_handler被调用), 如果打印太多在bochs里会卡死
  // dprintf("[%s:%s:%d] yield and call schedule\n",
  //         cur->name, thread_status_names[cur->status], cur->priority);
  // 最好还是不打印,在关中断里面打印会扰乱输出
  // dprintf("[%s:%s:%d] yd\n", cur->name, thread_status_names[cur->status], cur->priority);
  // 需要关中断
  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  // 新线程, 返回intr_handler, 那中断里面记录的eip还有啥用?
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
  // thread_current ()->priority = new_priority;
  if (thread_mlfqs)
    return;

  // 也是不需要关中断的
  enum intr_level old_level = intr_disable ();

  struct thread *curr_thread = thread_current ();
  curr_thread->priority_before_donate = new_priority;

  // 如果不持有锁(不会被donate)或者new_priority大于现在的priority
  if (list_empty (&curr_thread->locks) || new_priority > curr_thread->priority) {
    curr_thread->priority = new_priority;
    thread_yield ();
  }

  intr_set_level (old_level);
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


// idle thread最初是被thread_start放在ready list中,
// 然后会被scheduled一次,at which point it initializes idle_thread(全局静态变量)
// 之后,idle thread就不会出现在ready list中
// 在next_thread_to_run()中没有ready thread的时候会返回idle thread
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
  dprintf("[%s:%s:%d] in idle\n", idle_thread->name, thread_status_names[idle_thread->status], idle_thread->priority);
  sema_up (idle_started);

  for (;;) 
    {
      dprintf("idle running and call thread_block\n");
      /* Let someone else run. */
      intr_disable (); // 在哪开的, ↓
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
  struct thread *curr = thread_current();
  dprintf("[%s:%s:%d] RU\n",
          curr->name, thread_status_names[curr->status], curr->priority);
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
bool
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
// --- Lab1: Task 2 ---
  t->priority_before_donate = priority;
  list_init (&t->locks);
  t->waiting_lock = NULL;
// --- Lab1: Task 2 ---
// --- Lab2: Task 4 ---
  list_init(&t->child_list);
  sema_init(&t->exec_sema, 0);
  t->exec_result = -1;
  t->waiting_tid = TID_ERROR;
  sema_init(&t->wait_sema, 0);
// --- Lab2: Task 4 ---
  t->next_fd = 2;
  list_init(&t->open_file_list);

#ifdef VM
  t->next_mapid = 0;
#endif
  t->pcb = NULL;

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

bool
thread_less (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  return list_entry(a, struct thread, elem)->priority < list_entry(b, struct thread, elem)->priority;
}

// [[[ 这个函数不能调用printf ]]]
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
    struct list_elem *max = list_max(&ready_list, thread_less, NULL);
    struct thread *max_priority_thread = list_entry (max, struct thread, elem);
    list_remove(max);
    ASSERT (max_priority_thread != NULL);
    return max_priority_thread;
  }
// --- Lab1: Task 2 ---
}

/** Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, [[[[[ and interrupts are
   still disabled. 现在是切换后的线程了, 但中断依然是关的, 多久开呢??? ]]]]]
   This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).
   一个线程第一次被schedule是switch_entry模拟schedule行为调用switch_threads

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.
   printf只能在函数的最后或者结束才能添加, 否则如果锁不在自己手上会一直循环去要求schedule,
   等拿锁的那个thread放锁, 但就陷入了循环, 因为在要求schedule的路上又会调用printf
   所以需要等切换完成后再调用printf,如果切换到了拿到printf锁的就不需要再拿锁了

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
      // 释放这个内核线程对应的页(包括了thread结构体)
      palloc_free_page (prev);
    }
}

// called only by the three public thread functions that need to switch threads:
//   thread_block()
//   thread_exit()
//   thread_yield(): 中断时CPU把eflags保存到了intr_frame,然后屏蔽中断?中断结束时恢复(进而允许外中断)

// 调用schedule需要保证关中断 (但是schedule后谁来开呢后续???)
// 并且running process's state已经被修改为合适的状态
// [[[ 这个函数不能调用printf ]]]
// [[[[[ Before any of these functions call schedule(),
// they disable interrupts (or ensure that they are already disabled)
// and then change the running thread's state to something other than running. ]]]]]
/** Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

 我去这里居然提到了......
   [[[[[ It's not safe to call printf() until thread_schedule_tail()
   has completed. ]]]]] */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  // 需要关中断才能进行线程调度,
  // TODO 为什么调度的时候不能处理中断?
  //   外中断 handler和 kernel thread的共享数据只能通过关中断来保护, 因为外中断 handler不能 sleep
  // TODO 为什么外中断 handler不能 sleep?
  //   sleep的话会导致当前被打断的 thread sleep(中断处理是在当前 thread下 ?), 直到 handler被再次调用,
  //   但可能这个 handler在等 sleeping thread release a lock
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
  // 切换到之前运行到这的 next thread, 为prev
  thread_schedule_tail (prev);
  // 中断还未打开
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

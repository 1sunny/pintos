#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
  
/** See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/** Number of timer ticks since OS booted. */
static int64_t ticks;

/** Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/** Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  // 注册时钟中断
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/** Calibrates(校准) loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  // 以最大的 2 次幂来近似loops_per_tick, 仍小于一个定时器刻度
  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  // 然后再尝试加上后面的二进制位
  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/** Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  // 需要关中断才能保证不会在获取ticks的时候timer tick
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/** Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

// until time has advanced by at least ticks timer ticks.
/** Sleeps for [[[ approximately ]]] TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t sleep_ticks)
{
  int64_t start = timer_ticks ();

  ASSERT (intr_get_level () == INTR_ON);
  // A tight loop that calls thread_yield() is one form of busy waiting.

  // Although a working implementation is provided, it "busy waits,"
  // it spins in a loop checking the current time and calling thread_yield()
  // until enough time has gone by.

  // Unless the system is otherwise idle,
  // the thread need not wake up after exactly ticks.
  // Just put it on the ready queue after they have waited
  // for the right amount of time.

// 原始代码:
//  while (timer_elapsed (start) < ticks)
//     thread_yield ();
// --- Lab1: Task 1 ---
  if (sleep_ticks <= 0) {
    return;
  }
  thread_current ()->sleep_until_ticks = start + sleep_ticks;
  struct semaphore sema;
  thread_current ()->sema = &sema;
  sema_init (&sema, 0);
  // 在睡眠之前想打印,但锁可能在其它线程,进而错过了最准确的睡眠的时机
  // dprintf("[%s:%s:%d] timer_sleep from %lld to %lld\n",
  //        thread_current()->name, thread_status_names[thread_current()->status], thread_current()->priority,
  //        start, thread_current ()->sleep_until_ticks);
  sema_down(&sema);
  thread_current ()->sleep_until_ticks = -1;
  thread_current ()->sema = NULL;
// --- Lab1: Task 1 ---
}

/** Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/** Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/** Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/** Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/** Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/** Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/** Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}
// --- Lab1: Task 1 ---
void thread_sleep_check (struct thread *t, void *aux UNUSED) {
  if (t->sema != NULL && t->sleep_until_ticks <= ticks) {
    // Unlike most synchronization primitives,
    // sema_up() may be called inside an external interrupt handler
    // 因为sema_up不会sleep
    sema_up(t->sema);
    t->sleep_until_ticks = -1;
    t->sema = NULL;
  }
}
// --- Lab1: Task 1 ---
/** Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  // 这里中断应该由CPU关了的吧?
  ticks++;
  if (thread_mlfqs) {
    thread_mlfqs_increase_recent_cpu_by_one ();
    if (ticks % TIMER_FREQ == 0)
      thread_mlfqs_update_load_avg_and_recent_cpu ();
    else if (ticks % 4 == 0)
      thread_mlfqs_update_priority (thread_current ());
  }
// --- Lab1: Task 1 ---
// 这里放在中间感觉比较好,这样如果sleeping thread满足条件,thread_tick时就有可能被唤醒
// In addition, when modifying some global variable,
// e.g., a global list, you will need to use some synchronization primitive
// as well to ensure it is not modified or read concurrently
// (e.g., a timer interrupt occurs during the modification and
// we switch to run another thread).
// 但是interrupt handler不能sleep啊, 怎么同步???
// 关中断的时候不需要同步吧,自然就是同步的
  thread_foreach(thread_sleep_check, NULL);
// --- Lab1: Task 1 ---
  thread_tick ();
  // Hint: You need to decide where to check whether
  // the elapsed time exceeded the sleep time.
  // 遍历一下线程列表,进行唤醒?
  // 应该是要给struct thread加属性, 这会影响某些偏移吗, 加在最后应该不会有影响吧
}

/** Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
// 等待一个新的tick间隔开始
  while (ticks == start)
    barrier ();
// Without an optimization barrier in the loop,
// the compiler could conclude that the loop would never terminate,
// because start and ticks start out equal and the loop itself
// never changes them.
// It could then "optimize" the function into an infinite loop,
// which would definitely be undesirable.

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  // start != ticks表示loops次循环超过了一个tick间隔
  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/** Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
// The goal of this loop is to busy-wait by counting loops down
// from its original value to 0.
// Without the barrier, the compiler could delete the loop entirely,
// because it produces no useful output and has no side effects.
// The barrier forces the compiler to pretend that the loop body has an important effect.
}

/** Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/** Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}

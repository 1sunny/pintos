/** This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/** Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

// Semaphores are internally built out of disabling interrupt
// (see section Disabling Interrupts) and
// thread blocking and unblocking (thread_block() and thread_unblock()).

/** Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

static bool
thread_greater_priority_first (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  return list_entry(a, struct thread, elem)->priority > list_entry(b, struct thread, elem)->priority;
}

// [[[ 这个函数不能调用printf ]]]
// TODO 注释里的interrupt handler指外中断handler还是所有handler???
// 这个函数可能会sleep, 所以不能在interrupt handler中被调用(原因是死锁那个原因吗??intr_register_ext上的注释)

// 调用该函数前可能会禁用中断, 但如果休眠, 下一个被调度的thread可能会重新开启中断.
/** Down or "P" operation on a semaphore. [[[ Waits for SEMA's value
   to become positive and then atomically decrements it. ]]]

 什么情况下会在中断关闭时被调用? timer_sleep中
   This function [[[ may sleep ]]], so it must not be called within an
   interrupt handler.  This function [[[ may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. ]]] */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  // 没在处理外中断
  ASSERT (!intr_context ());

  // TODO 为什么要关中断
  old_level = intr_disable ();
  // 相当于这个线程不会被时间片打断, 这个函数也不会被interrupt handler调用
  // TODO 但如果这个线程触发内中断, 并且内中断如果也需要访问这个sema,那不是会...
  // 除非内中断不会访问这个sema
  // [[[[[[[[[[[[[[[[[[[[ !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  // Pintos中通过关闭中断来解决的问题: 在 kernel thread 和 interrupt handler_ 中共享的数据
  //   因为 interrupt handlers 不能 sleep, 也就不能获取 locks
  //   意味着共享数据在 kernel thread 处理时只能关中断来保护
  // [[[[[[[[[[[[[[[[[[[[ !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  while (sema->value == 0)
    {
      // TODO 对sema->waiters访问为什么不用加锁
      list_push_back (&sema->waiters, &thread_current ()->elem);
      thread_block (); // thread_current ()->status = THREAD_BLOCKED; schedule ();
    }
  sema->value--;
  intr_set_level (old_level);
  // old_level什么情况下是OFF ?
}

/** Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

// [[[ 这个函数不能调用printf ]]]
// sema_up可以被外中断handler调用, 因为不会sleep吗
// 会通知所有waiters
/** Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema)
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  bool should_yield = false;
  old_level = intr_disable ();
  if (!list_empty (&sema->waiters)) {
    struct thread *t = list_entry (list_pop_front(&sema->waiters),
                                   struct thread, elem);
    if (t->priority >= thread_current()->priority) {
      should_yield = true;
    }
    thread_unblock (t);
    // 原始代码:
    // thread_unblock (list_entry (list_pop_front (&sema->waiters),
    //                             struct thread, elem));
  }
  sema->value++;

  if (should_yield) {
    if (intr_context()) {
      intr_yield_on_return();
    } else {
      thread_yield ();
    }
  }
  intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/** Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/** Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/** Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

static bool
lock_greater_priority_first (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  return list_entry (a, struct lock, elem)->max_thread_priority > list_entry (b, struct lock, elem)->max_thread_priority;
}

static void
thread_update_lock_based_priority (struct thread *t) {
  ASSERT(intr_get_level() == INTR_OFF);

  int max_priority = t->priority_before_donate;
  if (!list_empty (&t->locks)) {
    list_sort(&t->locks, lock_greater_priority_first, NULL);
    int lock_priority = list_entry (list_front (&t->locks), struct lock, elem)->max_thread_priority;
    if (lock_priority > max_priority)
      max_priority = lock_priority;
   }
   t->priority = max_priority;
}

/** Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep.
   函数可能会sleep, 不能在interrupt handler(应该是外中断handler吧?)中调用
   可能调用的时候是关中断的(intq_getc, intq_putc...), 但如果sleep的话后续中断会被打开
*/
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  struct thread *curr_thread = thread_current ();
  struct lock *l;
  enum intr_level old_level = intr_disable ();
  // TODO 这里不需要关中断吗, 锁应该是线程间共享数据, 线程间共享数据按理来说应该用锁来同步, 但现在处理的本身就是锁
  if (lock->holder != NULL && !thread_mlfqs) {
    curr_thread->waiting_lock = lock;
    l = lock;
    // 如果while更新到一半,另一个线程也acquire相同的锁, [[[[[ 当然因为这个线程优先级高所以不会出现这样的情况 ]]]]]
    // 所以感觉整个函数里不需要额外的关中断(不关依然可以通过测试),但还是关一下吧
    while (l && curr_thread->priority > l->max_thread_priority) {
      l->max_thread_priority = curr_thread->priority;
      thread_update_lock_based_priority(l->holder);
      l = l->holder->waiting_lock;
    }
  }
  // 为什么在这里打印要panic, 因为线程还没初始化好? 是因为会循环调用printf导致栈溢出, magic对不上
  // printf("call sema_down in lock_acquire\n");
  // become positive and then atomically decrements it
  sema_down (&lock->semaphore);

  if (!thread_mlfqs) {
    curr_thread->waiting_lock = NULL;
    lock->max_thread_priority = curr_thread->priority;
    list_insert_ordered(&thread_current()->locks, &lock->elem, lock_greater_priority_first, NULL);
  }
  // 这里也会panic: // ASSERT (t->status == THREAD_RUNNING);
  lock->holder = thread_current ();
  // 这里不会,因为 schedule返回了, 看schedule中的注释
  // printf("[%s:%s:%d] called sema_down in lock_acquire\n",
  //        thread_current()->name, thread_status_names[thread_current()->status], thread_current()->priority);
  intr_set_level (old_level);
}

/** Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/** Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  if (!thread_mlfqs) {
    enum intr_level old_level = intr_disable ();
    // 从当前线程中移除这个锁
    list_remove (&lock->elem);
    thread_update_lock_based_priority(thread_current());
    intr_set_level (old_level);
  }

  lock->holder = NULL;
  // 这里面应该进行一次yield感觉
  sema_up (&lock->semaphore);
}

// 无法检测任意线程是否持有某个锁, 因为answer could change before the caller could act on it.
/** Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/** One semaphore in a list. */
struct semaphore_elem 
  {
    struct list_elem elem;              /**< List element. */
    struct semaphore semaphore;         /**< This semaphore. */
  };

/** Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

// 调用函数前需要持有对应的锁
// 被唤醒后可能条件依然不满足, 需要继续wait
// 一个条件变量只能对应一个锁, 一个锁可以对应多个条件变量
// 这个函数可能会sleep, 所以不能在interrupt handler中被调用(原因是死锁那个原因吗??intr_register_ext上的注释)
// 调用该函数前可能会禁用中断, 但如果休眠, 下一个被调度的thread可能会重新开启中断.
/** Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning. [[[ LOCK must be held before calling
   this function. ]]]

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
  list_push_back (&cond->waiters, &waiter.elem);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/** If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) 
    sema_up (&list_entry (list_pop_front (&cond->waiters),
                          struct semaphore_elem, elem)->semaphore);
}

/** Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}

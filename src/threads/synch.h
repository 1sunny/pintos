#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/** A counting semaphore. */
struct semaphore 
  {
    unsigned value;             /**< Current value. */
    struct list waiters;        /**< List of waiting threads. */
  };

void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);

/** Lock. */
struct lock 
  {
    struct thread *holder;      /**< Thread holding lock (for debugging). */
    struct semaphore semaphore; /**< Binary semaphore controlling access. */
    struct list_elem elem;
    int max_thread_priority;
  };

void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);
bool lock_less (const struct list_elem *, const struct list_elem *, void *);

/** Condition variable. */
struct condition 
  {
    struct list waiters;        /**< List of waiting threads. */
  };

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

/** Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile ("" : : : "memory")

#endif /**< threads/synch.h */

// volatile int a = 0;
// volatile int b = 0;
//
// void update_variables() {
//     a = 1;
//     barrier();
//     b = 2;
// }
// In this code, the barrier() ensures that the write to a happens
// strictly before the write to b.
// Without the barrier, the compiler might reorder these writes
// if it thinks it can improve performance by doing so.
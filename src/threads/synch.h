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

/* Readers-writers lock. */
#define RW_READER 1
#define RW_WRITER 0

struct rw_lock {
    struct lock lock;
    struct condition read, write;
    int AR, WR, AW, WW;
};

void rw_lock_init(struct rw_lock*);
void rw_lock_acquire(struct rw_lock*, bool reader);
void rw_lock_release(struct rw_lock*, bool reader);

/* tom: DO NOT USE THIS ROUTINE UNDER ANY CIRCUMSTANCES!!!!!
 * It is not an "optimization" nor is it a "barrier" -- it is dangerous code.
 * It is only included here because its needed for some configuration
 * code in one of the device drivers, and even there it could have
 * been engineered around.  This should be a private routine inside
 * a class file, but we're not in C++ so we can't do that.
 *
 * OK, now that I've piqued your interest, what this does is to prevent
 * the compiler from reordering instructions across the call to this
 * routine.  But if your code depends on that, it means you've badly
 * designed it. Use structured synchronization code -- locks and
 * condition variables instead.  Ignore the reference guide.
 */
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
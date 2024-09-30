/* Main spawns thread A that joins on main
   and thread B that joins on A, then exits.
   After thread A joins, it exits the process,
   which should wake thread B and thread B will
   immediately be killed.
   */

#include "tests/lib.h"
#include "tests/main.h"
#include <syscall.h>
#include <pthread.h>

void thread_function(void* arg_);
void other_function(void* arg_);

/* Thread function that tests exit conditions by exiting */
void thread_function(void* arg_) {
  int* main_tid = (int*)arg_;
  msg("Thread starting");
  pthread_check_join(*main_tid);
  exit(162);
  fail("Should not be here");
}

void other_function(void* arg_) {
  int* last_tid = (int*)arg_;
  pthread_check_join(*last_tid);
  fail("Should not be here");
  exit(9583);
  fail("Should not be here");
}

void test_main(void) {
  msg("Main starting");
  tid_t main_tid = get_tid();
  tid_t last_tid = pthread_check_create(thread_function, &main_tid);
  pthread_check_create(other_function, &last_tid);
  pthread_exit();
  fail("Should not be here");
}

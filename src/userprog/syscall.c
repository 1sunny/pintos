#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "process.h"

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static int
get_arg_int(struct intr_frame *f, int num) {
  return *(int *)(f->esp + num * 4);
}

static char*
get_arg_str(struct intr_frame *f, int num) {
  return *(char **)(f->esp + num * 4);
}

static void
syscall_exit(struct intr_frame *f) {
  int exit_code = get_arg_int(f, 1);
  thread_current()->exit_code = exit_code;
  thread_exit();
}

static void
syscall_write(struct intr_frame *f) {
  int fd = get_arg_int(f, 1);
  char *buf = get_arg_str(f, 2);
  int n = get_arg_int(f, 3);

  if (fd == 1) {
    putbuf(buf, n);
    f->eax = n;
  }
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int syscall_num = *(int *)f->esp;
  switch (syscall_num) {
    case SYS_EXIT:
      syscall_exit(f);
      break;
    case SYS_WRITE:
      syscall_write(f);
      break;
    default:
      printf ("unimplemented system call!\n");
      thread_exit ();
  }
}

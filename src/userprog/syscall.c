#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <threads/vaddr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "process.h"

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
kill_process() {
  thread_current()->exit_code = -1;
  thread_exit ();
}

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
          : "=&a" (result) : "m" (*uaddr));
  return result;
}

/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
          : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

void read_user_addr(void *dst, void *src, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (src + i >= PHYS_BASE) {
      kill_process();
    }
    int value = get_user(src + i);
    if (value == -1) {
      kill_process();
    } else {
      ((uint8_t*)dst)[i] = value;
    }
  }
}

static int
get_arg_int(struct intr_frame *f, int num) {
  void* buf[4];
  read_user_addr(buf, f->esp + num * 4, 4);
  return *(int *)buf;
}

static char*
get_arg_str(struct intr_frame *f, int num) {
  void* buf[4];
  read_user_addr(buf, f->esp + num * 4, 4);
  char *str = *(char **)buf;
  int i = 0;
  for (;;) {
    if (str + i >= PHYS_BASE) {
      kill_process();
    }
    int value = get_user(str + i);
    if (value == -1) {
      kill_process();
    } else if (value == '\0') {
      break;
    }
    i++;
  }
  return str;
}

static void
syscall_exit(struct intr_frame *f) {
  int exit_code = get_arg_int(f, 1);
  thread_current()->exit_code = exit_code;
  thread_exit();
}

static void
syscall_exec(struct intr_frame *f) {
  char *exec_args = get_arg_str(f, 1);
  // printf("exec_args: %s\n", exec_args);
  f->eax = process_execute(exec_args);
}

static void
syscall_wait(struct intr_frame *f) {
  int pid = get_arg_int(f, 1);
  f->eax = process_wait(pid);
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
    case SYS_EXEC:
      syscall_exec(f);
      break;
    case SYS_WAIT:
      syscall_wait(f);
      break;
    case SYS_WRITE:
      syscall_write(f);
      break;
    default:
      printf ("unimplemented system call!\n");
      thread_exit ();
  }
}

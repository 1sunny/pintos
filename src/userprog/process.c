#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads/malloc.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

struct start_process_args {
    char* args;                /* Executable's file name */
    struct process* parent_process; /* PCB of the parent process */
    struct semaphore exec_wait;     /* Down'd by process_execute, up'd by start_process */
    bool success;                   /* Set by start_process, returned to process_execute */
};

struct pthread_args {
    stub_fun sf;
    pthread_fun tf;
    void* arg;
    struct semaphore create_wait;
    struct process* pcb;
    bool success;
};

static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
bool setup_thread(struct pthread_args*, void (**eip)(void), void** esp);

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;

  /* Kill the kernel if we did not succeed */
  ASSERT(success);

  /* Initialize PCB */
  list_init(&t->pcb->child_exit_statuses);
}

static void
get_file_name(char *file_name, const char *str) {
  int u = 0;
  while (str[u] != ' ' && str[u] != '\0') {
    file_name[u] = str[u];
    u++;
  }
  file_name[u] = '\0';
}

// run_task会调用process_execute
// new thread可能在process_execute返回前被调度运行
/** Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
pid_t
process_execute (const char *args)
{
  char *args_copy;
  tid_t tid;
// if (args[13]=='1'&&args[14]==' '){
//   int a;
//   a++;
// }
  // TODO 为什么有race啊?
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  // 从内核池获取页面
  args_copy = palloc_get_page (0);
  if (args_copy == NULL)
    return TID_ERROR;
  strlcpy (args_copy, args, PGSIZE);

  char file_name[30];
  get_file_name(file_name, args_copy);
  // printf("[process_execute] file_name: %s\n", file_name);

  /* Create arguments to start_process */
  struct start_process_args* process_args = malloc(sizeof(struct start_process_args));
  if (process_args == NULL) {
    palloc_free_page(args_copy);
    return TID_ERROR;
  }
  process_args->args = args_copy;
  process_args->parent_process = thread_current()->pcb;
  sema_init(&process_args->exec_wait, 0);
  process_args->success = false;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (file_name, PRI_DEFAULT, start_process, process_args);
  if (tid == TID_ERROR) {
    // start_process中会释放
    // palloc_free_page (args_copy);
    return -1;
  }
// TODO 如果新创建的线程在此之前运行完,sema_down就不会被唤醒了
//   struct thread *curr = thread_current();
//   sema_down(&curr->exec_sema);

  sema_down(&process_args->exec_wait);

  /* If start_process failed, should return TID_ERROR */
  if (!process_args->success) {
    tid = TID_ERROR;
  }
  free(process_args);
  // return curr->exec_result;
  return tid;
}

static void push_uint32(void **esp, uint32_t v) {
  *esp -= 4;
  *((uint32_t *)(*esp)) = v;
}

static void
close_open_files (struct list *open_file_list) {
  struct list_elem *e;
  // 在sema_up parent前就要释放打开的文件
  for (e = list_begin (open_file_list); e != list_end (open_file_list); ) {
    struct open_file *entry = list_entry(e, struct open_file, elem);
    e = list_next (e);
    lock_acquire(&filesys_lock);
    file_close(entry->file);
    lock_release(&filesys_lock);
    free(entry);
  }
}

// TODO 这个函数运行时算内核线程吧?
/** A thread function that loads a user process and starts it
   running. */
static void
start_process (void *args_)
{
  struct start_process_args* process_args = (struct start_process_args*)args_;
  char* args = process_args->args;

// TODO 什么时候释放?
#ifdef VM
  supplemental_page_table_init (&thread_current ()->spt);
#endif
  char file_name[30];
  get_file_name(file_name, args);

  struct thread *curr = thread_current();

  struct intr_frame if_;
  bool success, pcb_success;
  bool es_success;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  struct exit_status* new_es = malloc(sizeof(struct exit_status));
  success = pcb_success = new_pcb != NULL;
  es_success = new_es != NULL;
  success = pcb_success && es_success;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    new_pcb->next_fd = 2;
    list_init(&(new_pcb->open_file_list));
    curr->pcb = new_pcb;

    // Continue initializing the PCB as normal
    curr->pcb->main_thread = curr;
    strlcpy(curr->pcb->process_name, curr->name, sizeof curr->name);
    curr->pcb->exit_status = new_es;
    list_init(&curr->pcb->child_exit_statuses);

    // Initialize exit status
    curr->pcb->exit_status->pid = curr->tid;
    curr->pcb->exit_status->status = -1;
    curr->pcb->exit_status->waited = false;
    curr->pcb->exit_status->exited = false;
    curr->pcb->exit_status->ref_cnt = 2;
    lock_init(&curr->pcb->exit_status->ref_cnt_lock);
    sema_init(&curr->pcb->exit_status->exit_wait, 0);

    // Add exit status to parent
    list_push_back(&process_args->parent_process->child_exit_statuses, &curr->pcb->exit_status->elem);

    // Initialize user_thread related fields in the new PCB
    list_init(&(curr->pcb->user_threads));
    lock_init(&(curr->pcb->pthread_lock));

    // Add current main thread to list
    struct user_thread* ut_main = (struct user_thread*)malloc(sizeof(struct user_thread));
    success = success && ut_main != NULL;
    if (success) {
      ut_main->tid = curr->tid;
      ut_main->exited = false;
      ut_main->waited = false;
      sema_init(&ut_main->join_wait, 0);
      ut_main->stack = NULL;
      list_push_back(&curr->pcb->user_threads, &ut_main->elem);
    } else {
      free(ut_main);
    }

    // Initialize user-level synchronization fields in the new PCB
    list_init(&(curr->pcb->all_locks));
    list_init(&(curr->pcb->all_semaphores));
    lock_init(&(curr->pcb->sync_locks));
    lock_init(&(curr->pcb->sync_semaphores));
    curr->pcb->num_locks = 0;
    curr->pcb->num_semaphores = 0;
  }

  if (success) {
    // main thread没有pwd, 因为main thread初始化时filesys还没初始化, 不能打开dir
    //TODO process_args->parent_process == NULL
    if (process_args->parent_process != NULL && process_args->parent_process->pwd != NULL) {
      // TODO 这里是不是要加锁
      curr->pcb->pwd = dir_reopen(process_args->parent_process->pwd);
    }
    else {
      curr->pcb->pwd = dir_open_root();
    }
  }

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    // 模拟intr_entry中保存到intr_frame的寄存器
    // TODO 但为什么只设置段寄存器 ?
    // load 中设置了 eip 和 esp
    // TODO 为什么可以都用 SEL_UDSEG ?
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    // 中断开启
    if_.eflags = FLAG_IF | FLAG_MBS;


    lock_acquire(&filesys_lock);
    // load中会active pagedir
    success = load(file_name, &if_.eip, &if_.esp);
    lock_release(&filesys_lock);
  }

  /* Handle failure with successful exit status and PCB malloc.
   Must remove exit status from parent. */
  if (!success && es_success && pcb_success) {
    struct list_elem* removed = list_pop_back(&process_args->parent_process->child_exit_statuses);
    ASSERT(removed == &curr->pcb->exit_status->elem);
  }

  /* Handle failure with successful exit status malloc.
   Must free exit status. */
  if (!success && es_success) {
    free(curr->pcb->exit_status);
  }

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = curr->pcb;
    curr->pcb = NULL;

    // Destroy the user file list and close all associated files
    close_open_files(&pcb_to_free->open_file_list);
    free(pcb_to_free);
  }

  /* Set success for parent, and wake parent. */
  process_args->success = success;
  sema_up(&process_args->exec_wait);

  /* If load failed, quit. */
  if (!success) {
    palloc_free_page (args);
    thread_exit ();
  }

  // load中调用setup_stack设置好了esp指向PHY_BASE
  ASSERT(if_.esp == PHYS_BASE);

  int argc = 0;
  char *argv[65]; // 可以传递给内核的命令行参数不超过 128 字节: 128/2+1=65
  // TODO 页表切换了 为什么还能直接访问args地址,初始的init_page_dir包含哪些
  // args: 0xc10900c这个是包含在init_page_dir里的
  char *save_ptr = args;
  char *arg;
  while ((arg = strtok_r (NULL, " ", &save_ptr)) != NULL) {
    size_t len = strlen(arg) + 1;
    if_.esp -= len;
    memcpy(if_.esp, arg, len);
    if (argc < 65) {
      argv[argc++] = if_.esp;
    }
    // printf("argv[%d]: %s\n", argc-1, argv[argc-1]);
  }
// TODO 虽然现在页表切换了, 但file_name=0xc109000是内核中的地址, 可以不同的内核线程来释放?
  palloc_free_page (args);
  if (argc > 1){
    curr->pcb->tmp = atoi(argv[1]);
    if (curr->pcb->tmp == 1) {
      int a;
      a++;
    }
    // printf("tmp: %d\n", curr->pcb->tmp);
  }

  if_.esp = (void *)ROUND_DOWN((uint32_t)if_.esp, 4);
  // 0
  push_uint32(&if_.esp, 0);
  // argv[argc-1] ... argv[0]
  for (int i = argc-1; i >= 0; i--) {
    push_uint32(&if_.esp, (uint32_t)argv[i]);
  }
  // argv
  push_uint32(&if_.esp, (uint32_t)if_.esp);
  // argc
  push_uint32(&if_.esp, argc);
  // return address 0
  push_uint32(&if_.esp, 0);

  // intr_frame 是在内核栈上还是用户栈?
  // 模拟从 interrupt 返回来启动用户线程(TODO 但是为什么要这样呢?)
  // 因为 intr_exit(intr-stubs.S) 按 struct intr_frame 结构处理栈上的数据
  // 我们把 esp 指向我们的 stack frame 并跳转到 intr_exit
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

// 等待TID die然后返回exit status
// -1: 该TID被kernel终止
// -1: TID invalid或者不是当前进程的子进程, 或者已经为这个TID调用过process_wait了
/** Waits for process with PID child_pid to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (pid_t child_pid UNUSED)
{
  struct list* child_exit_statuses = &thread_current()->pcb->child_exit_statuses;
  struct exit_status* child_exit_status = NULL;
  for (struct list_elem* e = list_begin(child_exit_statuses); e != list_end(child_exit_statuses);
       e = list_next(e)) {
    struct exit_status* ec = list_entry(e, struct exit_status, elem);
    if (ec->pid == child_pid) {
      child_exit_status = ec;
      break;
    }
  }

  if (child_exit_status == NULL || child_exit_status->waited) {
    return -1;
  }

  child_exit_status->waited = true;

  if (child_exit_status->exited) {
    return child_exit_status->status;
  }

  sema_down(&child_exit_status->exit_wait);
  return child_exit_status->status;
}

/** Free the current process's resources. */
void
process_exit (int status)
{
  struct thread *cur = thread_current ();

  // TODO 什么时候会出现这种情况,其它线程释放了pcb吗? 应该不会吧,不应该是最后退出的释放吗
  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    ASSERT(false);
    thread_exit();
    NOT_REACHED();
  }

  // cur->exit_code = status;

  lock_acquire(&cur->pcb->pthread_lock);

  // TODO 下面这些都应该在pcb中,那child_list呢?
  // main没有pwd
  if(cur->pcb->pwd) {
    dir_close (cur->pcb->pwd);
    cur->pcb->pwd = NULL;
  }
  // TODO 这里这样做不太好
  if (strcmp(cur->name, "main") != 0) {
#ifdef VM
    supplemental_page_table_kill (&cur->spt);
#endif
  }

  // printf("[%d] exiting\n", cur->pcb->tmp);
  /* Set and print exit status, if not yet set. */
  if (!cur->pcb->exit_status->exited) {
    cur->pcb->exit_status->exited = true;
    cur->pcb->exit_status->status = status;
    printf("%s: exit(%d)\n", cur->pcb->process_name, status);
  }

  lock_release(&cur->pcb->pthread_lock);

  if (is_main_thread(cur, cur->pcb)) {
    pthread_exit_main();
  } else {
    pthread_exit();
  }
}

/* Free the current process's resources and signal waiting process. */
static void
destroy_process(void) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  // TODO 到这里了应该没有其它thread会访问pcb了吧
  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
    // printf("pagedir_destroy\n");
  }

  while (!list_empty(&cur->pcb->child_exit_statuses)) {
    struct list_elem* e = list_pop_front(&cur->pcb->child_exit_statuses);
    struct exit_status* exit_status = list_entry(e, struct exit_status, elem);
    lock_acquire(&exit_status->ref_cnt_lock);
    exit_status->ref_cnt -= 1;
    int ref_cnt = exit_status->ref_cnt;
    lock_release(&exit_status->ref_cnt_lock);
    if (ref_cnt == 0) {
      free(exit_status);
    }
  }

  lock_acquire(&filesys_lock);
  // printf("%s lock_acquire\n", cur->name);
  if (cur->pcb->executing_file) {
    file_close(cur->pcb->executing_file);
  }
  // printf("%s lock release\n", cur->name);
  lock_release(&filesys_lock);

  lock_acquire(&cur->pcb->exit_status->ref_cnt_lock);
  int ref_cnt = cur->pcb->exit_status->ref_cnt -= 1;
  lock_release(&cur->pcb->exit_status->ref_cnt_lock);
  if (ref_cnt == 0) {
    free(cur->pcb->exit_status);
  } else {
    sema_up(&cur->pcb->exit_status->exit_wait);
  }

  while (!list_empty(&cur->pcb->user_threads)) {
    struct list_elem* e = list_pop_front(&cur->pcb->user_threads);
    struct user_thread* ut = list_entry(e, struct user_thread, elem);
    free(ut);
  }

  while (!list_empty(&cur->pcb->all_locks)) {
    struct list_elem* e = list_pop_front(&cur->pcb->all_locks);
    struct user_lock* u_lock = list_entry(e, struct user_lock, elem);
    free(u_lock);
  }
  while (!list_empty(&cur->pcb->all_semaphores)) {
    struct list_elem* e = list_pop_front(&cur->pcb->all_semaphores);
    struct user_lock* u_sem = list_entry(e, struct user_semaphore, elem);
    free(u_sem);
  }

  close_open_files(&cur->pcb->open_file_list);

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;

  free(pcb_to_free);
}

/** Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();
  // TODO 什么时候 t->pcb=NULL ?
  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update ();
}

/** We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/** ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/** For use with ELF types in printf(). */
#define PE32Wx PRIx32   /**< Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /**< Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /**< Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /**< Print Elf32_Half in hexadecimal. */

/** Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

// https://en.wikipedia.org/wiki/Executable_and_Linkable_Format

// 用途:
// 程序头表(Program Header Table)描述的是进程执行时所需的内存布局,它告诉系统如何创建进程的地址空间,
// 哪些部分应该加载到内存,如何设置内存保护等.
// 它主要用于程序的加载阶段,即在操作系统加载可执行文件到内存并准备运行时使用.

// 组成部分:
// 每个程序头条目(Program Header Entry)描述一个段(Segment),
// 包含段的类型,虚拟地址,物理地址,文件中的偏移,段的大小,内存中的大小,权限等信息.
// 典型的段类型包括可执行段(LOAD),动态链接信息段(DYNAMIC),解释器段(INTERP)等.

// 位置:
// 程序头表通常位于文件的开头,它的位置和大小由ELF文件头中的字段指示.
/** Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    // 段类型,表示该段的类型或属性,例如可加载段(PT_LOAD),动态链接信息(PT_DYNAMIC)等.
    Elf32_Word p_type;
    // 文件偏移,表示该段在文件中的起始位置(以字节为单位).
    Elf32_Off  p_offset;
    // 虚拟地址,表示该段在进程虚拟地址空间中的起始地址.
    Elf32_Addr p_vaddr;
    // 物理地址,表示该段在物理内存中的起始地址.在某些系统中可能忽略这个字段.
    Elf32_Addr p_paddr;
    // 文件大小,表示该段在文件中的大小(以字节为单位).
    Elf32_Word p_filesz;
    // 内存大小,表示该段在内存中的大小(以字节为单位).可能大于p_filesz,用于包含未初始化的数据.
    Elf32_Word p_memsz;
    // 段标志,表示该段的权限和属性,例如可执行(PF_X),可写(PF_W),可读(PF_R)等.
    Elf32_Word p_flags;
    // 对齐要求,表示该段在内存中的对齐方式,通常是2的幂值.
    Elf32_Word p_align;
  };

/** Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /**< Ignore. */
#define PT_LOAD    1            /**< Loadable segment. */
#define PT_DYNAMIC 2            /**< Dynamic linking info. */
#define PT_INTERP  3            /**< Name of dynamic loader. */
#define PT_NOTE    4            /**< Auxiliary info. */
#define PT_SHLIB   5            /**< Reserved. */
#define PT_PHDR    6            /**< Program header table. */
#define PT_STACK   0x6474e551   /**< Stack segment. */

/** Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /**< Executable. */
#define PF_W 2          /**< Writable. */
#define PF_R 4          /**< Readable. */

// Section Header
// 用途:
// 节头表(Section Header Table)描述的是文件中的各个节(Section),每个节包含特定类型的数据,如代码,数据,符号表,重定位信息等.
// 它主要用于链接阶段,即在编译器和链接器处理目标文件时使用,帮助它们组织和管理文件中的数据.

// 组成部分:
// 每个节头条目(Section Header Entry)描述一个节,包含节的名称,类型,文件中的偏移,大小,地址对齐要求,链接和重定位信息等.
// 常见的节类型包括代码段(.text),数据段(.data),只读数据段(.rodata),符号表(.symtab),字符串表(.strtab)等.

// 位置:
// 节头表通常位于文件的末尾,它的位置和大小同样由ELF文件头中的字段指示.

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

// 将一个ELF可执行文件加载到当前线程的地址空间中,并设置该可执行文件的入口点和初始栈指针
/** Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create ();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate ();

  // TODO 这里是不是没获取锁啊
  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  file_deny_write(file);
  t->pcb->executing_file = file;

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              // TODO 同一个段应该不会在同一个页吧? 这也是为什么需要4KB对齐(p_align字段指定)
              // p_offset:表示该段在文件中的起始位置
              // 用于获取phdr.p_offset所在页的起始地址(页对齐地址).
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              // p_vaddr:表示该段在进程虚拟地址空间中的起始地址
              // 获取phdr.p_vaddr所在页的起始地址(页对齐地址).TODO 是指应该加载到虚拟地址为mem_page的地方吗?
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              // 获取phdr.p_vaddr在页内的偏移量
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  // 不同的段可能共享同一个页面的一部分.例如:
                  // 代码段和数据段:代码段(.text)和数据段(.data)可能在同一个页面中连续存放.
                  // 这样做可以减少页面数量,节省内存.

                  // phdr.p_filesz 是段在文件中的大小,但不一定是段在内存中的实际大小.
                  // 要加page_offset是因为:
                  // 假设p_filesz=2KB,phdr.p_vaddr=1KB,应该读取[1KB,3KB],但是1KB之前的内容也需要读取.
                  // 1KB之前可能是其它段的数据(虽然不同段不是应该页面对齐吗),1KB之前的可以重复覆盖
                  read_bytes = page_offset + phdr.p_filesz;
                  // p_memsz表示该段在内存中的大小(以字节为单位).可能大于p_filesz,用于包含未初始化的数据.
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              // 第一个phdr.p_offset等于0,也就是说ELF header也会被加载?
              // 是的,如果 p_offset 值为 0,那么在这个段被加载到内存中的时候,ELF header 也会被加载.
              // 这意味着段的实际数据从文件的开头开始,包括 ELF header.
              // 这种情况虽然不常见,但在某些特殊情况下可能会发生,例如在一些简化的 ELF 文件中,
              // 数据段和 ELF header 紧密排列在一起,
              // 导致加载器将整个文件内容(包括 ELF header)加载到内存中.
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  // file_close (file); thread_exit或process_exit时关闭
  return success;
}

/** load() helpers. */


/** Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/** Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  if (pagedir_get_page (t->pcb->pagedir, upage) == NULL) {
    if (pagedir_set_page (t->pcb->pagedir, upage, kpage, writable)) {
      // printf("install page: %p -> %p (%d)\n", upage, kpage, writable);
      return true;
    }
    printf("install_page: already exits !!!");
  }
  return false;
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(struct pthread_args* args, void (**eip)(void) UNUSED, void** esp UNUSED) {
  process_activate();
  if (!setup_stack(esp)) {
    return false;
  }
  // Push args->arg and args->tf onto stack with null return address
  *((char**)esp) -= 4;
  **((void***)esp) = args->arg;

  *((char**)esp) -= 4;
  **((pthread_fun***)esp) = args->tf;

  *((char**)esp) -= 4;
  **((void***)esp) = NULL;

  *eip = args->sf;

  return true;
}

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, void* arg UNUSED) {
  lock_acquire(&thread_current()->pcb->pthread_lock);
  struct pthread_args* arguments = (struct pthread_args*)malloc(sizeof(struct pthread_args));
  tid_t tid;
  arguments->sf = sf;
  arguments->tf = tf;
  arguments->arg = arg;
  arguments->pcb = thread_current()->pcb;
  sema_init(&(arguments->create_wait), 0);
  tid = thread_create("stub", PRI_DEFAULT, start_pthread, arguments);
  sema_down(&(arguments->create_wait));
  if (!arguments->success) {
    tid = TID_ERROR;
  }
  free(arguments);
  lock_release(&thread_current()->pcb->pthread_lock);
  return tid;
}

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_ UNUSED) {
  struct intr_frame if_;
  struct pthread_args* args = (struct pthread_args*)exec_;
  struct thread* t = thread_current();
  t->pcb = args->pcb;

  struct user_thread* u_thread = (struct user_thread*)malloc(sizeof(struct user_thread));
  if (u_thread == NULL) {
    args->success = false;
    return;
  }
  u_thread->tid = t->tid;
  u_thread->exited = false;
  u_thread->waited = false;
  sema_init(&(u_thread->join_wait), 0);
  // TODO 这个不需要加锁吗? pthread_execute中加了锁
  list_push_back(&t->pcb->user_threads, &u_thread->elem);

  /* Set interrupt frame flags, copied from start_process() */
  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  bool success = setup_thread(args, &if_.eip, &if_.esp);

  if (!success) {
    // Free u_thread and remove from list
    struct list_elem* removed = list_pop_back(&t->pcb->user_threads);
    ASSERT(removed == &u_thread->elem);
    free(u_thread);
  } else {
    u_thread->stack = pg_round_down(if_.esp);
  }
  args->success = success;
  sema_up(&args->create_wait);

  if (!success) {
    pthread_exit();
  }

  /* Simulate a return from an interrupt to start user_thread running */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid UNUSED) {
  struct thread *t = thread_current();
  lock_acquire(&t->pcb->pthread_lock);
  // Check for self-join
  if (tid == t->tid) {
    lock_release(&t->pcb->pthread_lock);
    return TID_ERROR;
  }

  struct list* user_threads = &t->pcb->user_threads;
  struct user_thread* ut;
  for (struct list_elem* e = list_begin(user_threads); e != list_end(user_threads);
       e = list_next(e)) {
    ut = list_entry(e, struct user_thread, elem);
    if (ut->tid == tid) {
      if (ut->exited && !ut->waited) {
        lock_release(&t->pcb->pthread_lock);
        return ut->tid;
      } else if (!ut->waited) {
        ut->waited = true;
        lock_release(&t->pcb->pthread_lock);
        // printf("[%d:%s] join wait %d\n", t->tid, t->name, tid);
        sema_down(&ut->join_wait);
        // printf("[%d:%s] wake\n", t->tid, t->name);
        lock_acquire(&t->pcb->pthread_lock);
        // TODO 这里为什么要remove?
        // 到这里证明 ut一定调用了pthread_exit,pthread_exit中sema_up(&ut->join_wait);
        // 应该删除不删除都可以
        // printf("[%d:%s] remove\n", t->tid, t->name);
        list_remove(&ut->elem);
        free(ut);
        lock_release(&t->pcb->pthread_lock);
        // printf("[%d:%s] release\n", t->tid, t->name);
        return tid;
      }
      break;
    }
  }

  lock_release(&t->pcb->pthread_lock);
  return TID_ERROR;
}

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {
  struct thread* t = thread_current();
  lock_acquire(&t->pcb->pthread_lock);

  struct list* user_threads = &t->pcb->user_threads;
  for (struct list_elem* e = list_begin(user_threads); e != list_end(user_threads);
       e = list_next(e)) {
    struct user_thread* ut = list_entry(e, struct user_thread, elem);
    if (ut->tid == t->tid) {
      ut->exited = true;
      // Free user thread stack and free struct user_thread if waited if true?
      uint8_t* kpage = pagedir_get_page(t->pcb->pagedir, ut->stack);
      if (kpage != NULL) {
        palloc_free_page(kpage);
      }
      pagedir_clear_page(t->pcb->pagedir, ut->stack);

      // printf("[%d:%s] pthread_exit\n", t->tid, t->name);
      sema_up(&ut->join_wait);
      lock_release(&t->pcb->pthread_lock);
      thread_exit();
    }
  }
}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {
  struct thread* t = thread_current();
  lock_acquire(&t->pcb->pthread_lock);

  struct list* user_threads = &t->pcb->user_threads;
  // Main thread should be at front of list
  struct list_elem* e = list_front(user_threads);
  struct user_thread* ut_main = list_entry(e, struct user_thread, elem);
  ut_main->exited = true;
  // printf("main sema_up\n");
  sema_up(&ut_main->join_wait);
  // 只需要唤醒了一次等待main的线程,因为只能等待一次

  for (struct list_elem* e = list_begin(user_threads); e != list_end(user_threads);
       e = list_next(e)) {
    struct user_thread* ut = list_entry(e, struct user_thread, elem);
    // TODO 为什么要!ut->waited, 因为有人可能在wait它, 不要重复wait
    if (!ut->exited && !ut->waited) {
      lock_release(&t->pcb->pthread_lock);
      // printf("[%d:%s] join wait in exit_main: %d\n", t->tid, t->name, ut->tid);
      sema_down(&ut->join_wait);
      // printf("[%d:%s] wake\n", t->tid, t->name);
      lock_acquire(&t->pcb->pthread_lock);
    }
  }

  /* Set and print exit status to 0, if not yet set. */
  if (!t->pcb->exit_status->exited) {
    t->pcb->exit_status->exited = true;
    t->pcb->exit_status->status = 0;
    printf("%s: exit(%d)\n", t->pcb->process_name, 0);
  }

  lock_release(&t->pcb->pthread_lock);

  destroy_process();
  thread_exit();
}

#ifndef VM
/** Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
    // 从file中读PAGE_READ_BYTES字节并清零最后PAGE_ZERO_BYTES字节
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      // printf("got %p\n", kpage);
      if (kpage == NULL) {
        // printf("nooooooooooooo\n");
        return false;
      }

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      // printf("%s read %d to %p\n", thread_current()->name, ofs, upage);
      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      ofs += page_read_bytes;
    }
  return true;
}

/** Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  struct thread* t = thread_current();
  uint8_t* kpage;
  bool success = false;

  // // TODO 为啥叫 kpage ? 不是从 user pool 获取的吗
  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    // Map to first available virtual user page
    uint8_t* page_boundary = (uint8_t*)PHYS_BASE - PGSIZE;
    while (page_boundary >= 0) {
      if (pagedir_get_page(t->pcb->pagedir, page_boundary) == NULL) {
        success = install_page(page_boundary, kpage, true);
        break;
      }
      page_boundary -= PGSIZE;
    }
    if (success)
      *esp = page_boundary + PGSIZE; // TODO 这里为什么要-20
      // TODO 为啥不设置 ebp 呢 ?
    else
      palloc_free_page(kpage);
  }
  return success;
}


#else

struct load_segment_info {
    size_t page_read_bytes;
    off_t ofs;
};

bool
lazy_load_segment (struct page *page, void *aux) {
  /* TODO: Load the segment from the file */
  struct load_segment_info *info = aux;
  size_t page_read_bytes = info->page_read_bytes;
  size_t page_zero_bytes = PGSIZE - page_read_bytes;
  // TODO 需不需要重新打开文件啥的
  struct thread *curr = thread_current();
  // TODO 这里好像应该加锁啊
  file_seek(curr->executing_file, info->ofs);
  // TODO 这里page->frame->kva和page->va应该都是可以的
  int32_t bytes_read = file_read(curr->executing_file, page->frame->kva, page_read_bytes);
  if (bytes_read != (int) page_read_bytes) {
    PANIC("lazy_load_segment");
    return false;
  }
  // printf("%s read %d\n", curr->name, info->ofs);
  if (page_zero_bytes > 0) {
    memset (page->frame->kva + page_read_bytes, 0, page_zero_bytes);
  }
  // TODO 什么时候install_page吗? vm_do_claim_page中install过了
  return true;
  /* TODO: This called when the first page fault occurs on address VA. */
  /* TODO: VA is available when calling this function. */
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
  // read_bytes是加载这个段需要读取的字节数
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  while (read_bytes > 0 || zero_bytes > 0) {
    /* Do calculate how to fill this page.
     * We will read PAGE_READ_BYTES bytes from FILE
     * and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    // [[[ 和正常的load_segment区别在于正常的会读取文件,这里只是一个uninit页并设置页表,
    // page fault时再根据根据设置的init函数进行初始化吧 ]]]
    /* TODO: Set up aux to pass information to the lazy_load_segment. */
    // TODO 1.这里应该pass什么信息? 看一下加载segment需要什么信息!
    // TODO 1.记得释放
    struct load_segment_info *aux = malloc(sizeof (struct load_segment_info));
    *aux = (struct load_segment_info) {
            .page_read_bytes      = page_read_bytes,
            .ofs                  = ofs,
    };
    // TODO 不加载文件是肯定的,但要设置页表吗?
    // lazy_load_segment: 可执行文件页面的初始化器,在出现页面错误时被调用
    // enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux
    if (!vm_alloc_page_with_initializer (VM_ANON, upage,
                                         writable, lazy_load_segment, aux))
      return false;
    struct thread *curr = thread_current();
    // printf("%s need to read (%d,%d) to %p\n", curr->name, ofs - ofs % PGSIZE, ofs % PGSIZE, upage);
    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
    ofs += page_read_bytes;
  }
  return true;
}

/* Create a PAGE of stack at the USER_STACK(PHYS_BASE). Return true on success. */
static bool
setup_stack (void **esp) {
  void *stack_bottom = (void *) (((uint8_t *) PHYS_BASE) - PGSIZE);

  /* TODO: Map the stack on stack_bottom and claim the page immediately.
   * TODO: If success, set the rsp accordingly.
   * TODO: You should mark the page is stack. */
  /* TODO: Your code goes here */
  if (!vm_alloc_page_with_initializer (VM_ANON | VM_MARKER_STACK, stack_bottom,
                                       true, NULL, NULL)) { // init为NULL,到时候不会执行init
    PANIC("setup_stack");
    return false;
  }
  if (vm_claim_page(stack_bottom)) {
    *esp = PHYS_BASE;
    thread_current()->stack_bottom = (uint32_t) stack_bottom;
    return true;
  }
  PANIC("setup_stack");
  return false;
}
#endif /* VM */

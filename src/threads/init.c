#include "threads/init.h"
#include <console.h>
#include <debug.h>
#include <inttypes.h>
#include <limits.h>
#include <random.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devices/kbd.h"
#include "devices/input.h"
#include "devices/serial.h"
#include "devices/shutdown.h"
#include "devices/timer.h"
#include "devices/vga.h"
#include "devices/rtc.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include "threads/thread.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "userprog/exception.h"
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#else
#include "tests/threads/tests.h"
#endif
#ifdef FILESYS
#include "devices/block.h"
#include "devices/ide.h"
#include "devices/pci.h"
#include "filesys/filesys.h"
#include "filesys/fsutil.h"
#endif

/** Page directory with kernel mappings only. */
uint32_t *init_page_dir;

#ifdef FILESYS
/** -f: Format the file system? */
static bool format_filesys;

/** -filesys, -scratch, -swap: Names of block devices to use,
   overriding the defaults. */
static const char *filesys_bdev_name;
static const char *scratch_bdev_name;
#ifdef VM
static const char *swap_bdev_name;
#endif
#endif /**< FILESYS */

/** -ul: Maximum number of pages to put into palloc's user pool. */
static size_t user_page_limit = SIZE_MAX;

static void bss_init (void);
static void paging_init (void);

static char **read_command_line (void);
static char **parse_options (char **argv);
static void run_actions (char **argv);
static void usage (void);

#ifdef FILESYS
static void locate_block_devices (void);
static void locate_block_device (enum block_type, const char *name);
#endif

int pintos_init (void) NO_RETURN;

// When pintos_init() starts, the system is in a pretty raw state.
// We're in 32-bit protected mode with paging enabled,
// but hardly anything else is ready.
/** Pintos main entry point. */
int
pintos_init (void)
{
  char **argv;

  // clears out the kernel's "BSS", which is the traditional name for a segment that should be initialized to all zeros. In most C implementations, whenever you declare a variable outside a function without providing an initializer, that variable goes into the BSS. Because it's all zeros, the BSS isn't stored in the image that the loader brought into memory. We just use memset() to zero it out.
  /* Clear BSS. */
  /* tom: the loader does not zero uninitialized data, so we need to;
   * otherwise, your code might do unexpected things.
   * It is not that the loader is lazy -- it is constrained to fit
   * into a small space.
   */
  bss_init ();

  /* Break command line into arguments and parse options. */
  /* tom: The loader passes us arguments in a pre-defined location
   * in memory.  These arguments parameterize the OS -- for example,
   * how big we should assume memory to be, so we need to parse the
   * arguments early.  But they also specify what tests to run, which we
   * don't use until later -- after the OS has been initialized. */
  argv = read_command_line ();
  argv = parse_options (argv);

  // initializes the thread system
  // It is called so early in initialization because a valid thread structure
  // is a prerequisite for acquiring a lock,
  // and lock acquisition in turn is important to other Pintos subsystems.
  /* Initialize ourselves as a thread so we can use locks,
     then enable console locking. */
  /* tom: the order of initalization code matters.  malloc
   * uses a lock to protect its shared data structures.  Although we're
   * running single-threaded for now, so locks will always be free,
   * the lock code checks the current thread (to verify the lock is
   * being released by the lock holder).  So we need to have set up the
   * current thread before we can allow calls to the lock system (in
   * thread_init).  We need to set up malloc before we can allow calls
   * that allocate memory.  We need to set up the console (which uses
   * locks) before we can do printf.  And so forth. */
  thread_init ();
  // initialize the console and print a startup message to the console
  console_init ();  

  /* Greet user. */
  printf ("Pintos booting with %'"PRIu32" kB RAM...\n",
          init_ram_pages * PGSIZE / 1024);

  /* Initialize memory system. */
  // sets up the kernel page allocator, which doles out memory one or more pages at a time (see section Page Allocator)
  /* tom: two memory allocators -- one for pages (palloc) and one for
  * variable size things (malloc).  malloc uses palloc, so we need
  * to init palloc first */
  palloc_init (user_page_limit);
  // sets up the allocator that handles allocations of arbitrary-size blocks of memory (see section Block Allocator)
  malloc_init ();
  // sets up a page table for the kernel (see section Page Table)
  paging_init ();

  /* Segmentation. */
#ifdef USERPROG
  tss_init ();
  gdt_init ();
#endif

  /* Initialize interrupt handlers. */
  // sets up the CPU's interrupt descriptor table (IDT) to ready it for interrupt handling (see section Interrupt Handling)
  intr_init (); // 初始化了中断但还没开启, thread_start中才开启的(开始抢占式调度)
  // prepare for handling timer interrupts and keyboard interrupts, respectively
  timer_init ();
  kbd_init ();
  // sets up to merge serial and keyboard input into one stream
  input_init ();
#ifdef USERPROG
  // prepare to handle interrupts caused by user programs using exception_init()
  // and syscall_init()
  exception_init ();
  syscall_init ();
#endif

  // Now that interrupts are set up, we can start the scheduler with
  // thread_start(), which creates the idle thread and enables interrupts.
  /* Start thread scheduler and enable interrupts. */
  thread_start ();
  // With interrupts enabled, interrupt-driven serial port I/O becomes possible,
  // so we use serial_init_queue() to switch to that mode
  serial_init_queue ();
  /* tom: calibrating the timer involves counting the number
  * of loop iterations between timer interrupts, to measure
  * the relative speed of the processor.  So interrupts need to be on */
  // calibrates(校准) the timer for accurate short delays
  timer_calibrate ();

#ifdef USERPROG
  /* Give main thread a minimal PCB so it can launch the first process */
  userprog_init();
#endif

#ifdef FILESYS
  /* Initialize network. */
  pci_init();

  /* Initialize file system. */
  // initialize the IDE disks with ide_init(),
  // then the file system with filesys_init()
  ide_init ();
  locate_block_devices ();
  filesys_init (format_filesys);
#endif

#ifdef VM
  vm_init ();
#endif

  printf ("Boot complete.\n");
  
  if (*argv != NULL) {
    /* Run actions specified on kernel command line. */
    // parses and executes actions specified on the kernel command line,
    // such as run to run a test (in project 1) or a user program
    // (in later projects)
    run_actions (argv);
  } else {
    // TODO: no command line passed to kernel. Run interactively
    const int cmd_maxlen = 100;
    char cmd_buf[cmd_maxlen];
    while (true) {
      printf("PKUOS>");
      memset(cmd_buf, 0, cmd_maxlen);
      size_t cmd_index = 0;
      while (true) {
        char ch = input_getc();
        // printf("char int: %d\n", ch);
        if (ch == 13) {
          printf("\n");
          break;
        }
        // TODO backspace
        if (cmd_index >= cmd_maxlen) {
          continue;
        }
        // As the user types in a printable character, display the character.
        if (ch > 31) {
          printf("%c", ch);
        }
        cmd_buf[cmd_index++] = ch;
      }
      printf("cmd_buf: %s, len: %d\n", cmd_buf, strnlen(cmd_buf, cmd_maxlen));
      if (strcmp(cmd_buf, "whoami") == 0) {
        printf("1sunny\n");
      } else if (strcmp(cmd_buf, "exit") == 0) {
        // If the user input is exit, the monitor will quit to allow the kernel to finish.
        break;
      } else {
        printf("invalid command\n");
      }
    }
  }

  /* Finish up. */
  shutdown ();
  thread_exit ();
}

// 将ELF加载到内存运行时可以知道.bss的起始地址和结束地址,直接将这段地址初始化为0即可
/** Clear the "BSS", a segment that should be initialized to
   zeros.  It isn't actually stored on disk or zeroed by the
   kernel loader, so we have to zero it ourselves.

   The start and end of the BSS segment is recorded by the
   linker as _start_bss and _end_bss.  See kernel.lds. */
static void
bss_init (void) 
{
  // 为什么是char
  extern char _start_bss, _end_bss;
  memset (&_start_bss, 0, &_end_bss - &_start_bss);
}

// 把所有可用的内存页都映射到了init_page_dir(里面只有内核的页表映射)
// 只是往init_page_dir中添加了映射
/** Populates the base page directory and page table with the
   kernel virtual mapping, and then sets up the CPU to use the
   new page directory.  Points init_page_dir to the page
   directory it creates. */
static void
paging_init (void)
{
  uint32_t *pd, *pt;
  size_t page;
  extern char _start, _end_kernel_text;

  pd = init_page_dir = palloc_get_page (PAL_ASSERT | PAL_ZERO);
  pt = NULL;
  // 相当于只是
  // init_ram_pages=99, page_used_for_init_page_dir=1
  int page_used_for_init_page_dir = 0;
  for (page = 0; page < init_ram_pages; page++)
    {
      uintptr_t paddr = page * PGSIZE;
      char *vaddr = ptov (paddr);
      size_t pde_idx = pd_no (vaddr);
      size_t pte_idx = pt_no (vaddr);
      bool in_kernel_text = &_start <= vaddr && vaddr < &_end_kernel_text;

      if (pd[pde_idx] == 0)
        {
          page_used_for_init_page_dir++;
          pt = palloc_get_page (PAL_ASSERT | PAL_ZERO);
          pd[pde_idx] = pde_create (pt);
        }

      // 添加映射 vaddr -> 它的物理地址(vtop(vaddr))
      pt[pte_idx] = pte_create_kernel (vaddr, !in_kernel_text);
    }
  printf("init_ram_pages: %d, page_used_for_init_page_dir: %d\n", init_ram_pages, page_used_for_init_page_dir);
  // TODO 相当于之前都是在物理内存上? 之前也是虚拟内存,是loader创建的临时页表(把物理内存映射到了3G开始的地方)
  /* Store the physical address of the page directory into CR3
     aka PDBR (page directory base register).  This activates our
     new page tables immediately.  See [IA32-v2a] "MOV--Move
     to/from Control Registers" and [IA32-v3a] 3.7.5 "Base Address
     of the Page Directory". */
  asm volatile ("movl %0, %%cr3" : : "r" (vtop (init_page_dir)));
  // TODO 这一行后会page fault吗? 应该不会吧,之前映射到LOADER_PHYS_BASE开始的,现在也在
}

/** Breaks the kernel command line into words and returns them as
   an argv-like array. */
static char **
read_command_line (void) 
{
  // 每个参数至少需要1个字符和一个空格，所以/2
  static char *argv[LOADER_ARGS_LEN / 2 + 1];
  char *p, *end;
  int argc;
  int i;

  // 通过虚拟地址读取 argc
  argc = *(uint32_t *) ptov (LOADER_ARG_CNT);
  p = ptov (LOADER_ARGS);
  end = p + LOADER_ARGS_LEN;
  for (i = 0; i < argc; i++) 
    {
      if (p >= end)
        PANIC ("command line arguments overflow");

      // \0在内存中
      argv[i] = p;
      p += strnlen (p, end - p) + 1;
    }
  argv[argc] = NULL;

  /* Print kernel command line. */
  printf ("Kernel command line:");
  for (i = 0; i < argc; i++)
    if (strchr (argv[i], ' ') == NULL)
      printf (" %s", argv[i]);
    else
      printf (" '%s'", argv[i]);
  printf ("\n");

  return argv;
}

/** Parses options in ARGV[]
   and returns the first non-option argument. */
static char **
parse_options (char **argv) 
{
  for (; *argv != NULL && **argv == '-'; argv++)
    {
      char *save_ptr;
      char *name = strtok_r (*argv, "=", &save_ptr);
      char *value = strtok_r (NULL, "", &save_ptr);
      
      if (!strcmp (name, "-h"))
        usage ();
      else if (!strcmp (name, "-q"))
        shutdown_configure (SHUTDOWN_POWER_OFF);
      else if (!strcmp (name, "-r"))
        shutdown_configure (SHUTDOWN_REBOOT);
#ifdef FILESYS
      else if (!strcmp (name, "-f"))
        format_filesys = true;
      else if (!strcmp (name, "-filesys"))
        filesys_bdev_name = value;
      else if (!strcmp (name, "-scratch"))
        scratch_bdev_name = value;
#ifdef VM
      else if (!strcmp (name, "-swap"))
        swap_bdev_name = value;
#endif
#endif
      else if (!strcmp (name, "-rs"))
        random_init (atoi (value));
      else if (!strcmp (name, "-mlfqs"))
        thread_mlfqs = true;
#ifdef USERPROG
      else if (!strcmp (name, "-ul"))
        user_page_limit = atoi (value);
#endif
      else
        PANIC ("unknown option `%s' (use -h for help)", name);
    }

  /* Initialize the random number generator based on the system
     time.  This has no effect if an "-rs" option was specified.

     When running under Bochs, this is not enough by itself to
     get a good seed value, because the pintos script sets the
     initial time to a predictable value, not to the local time,
     for reproducibility.  To fix this, give the "-r" option to
     the pintos script to request real-time execution. */
  random_init (rtc_get_time ());
  
  return argv;
}

// run_actions中actions: {"run", 2, run_task},
/** Runs the task specified in ARGV[1]. */
static void
run_task (char **argv)
{
  const char *task = argv[1];
  
  printf ("Executing '%s':\n", task);
#ifdef USERPROG
  process_wait (process_execute (task));
#else
  run_test (task);
#endif
  printf ("Execution of '%s' complete.\n", task);
}

/** Executes all of the actions specified in ARGV[]
   up to the null pointer sentinel. */
/*
 * tom: this is a complex bit of code, but you can ignore it.
 * in practice, it just calls "void run_task()"; the other part here
 * is for testing the file system.  And for assignment 1, run_task just
 * calls "void run_test()" -- the specific test routine.
 * */
static void
run_actions (char **argv) 
{
  /* An action. */
  struct action 
    {
      char *name;                       /**< Action name. */
      int argc;                         /**< # of args, including action name. */
      void (*function) (char **argv);   /**< Function to execute action. */
    };

  /* Table of supported actions. */
  static const struct action actions[] = 
    {
      {"run", 2, run_task},
#ifdef FILESYS
      {"ls", 1, fsutil_ls},
      {"cat", 2, fsutil_cat},
      {"rm", 2, fsutil_rm},
      {"extract", 1, fsutil_extract},
      {"append", 2, fsutil_append},
#endif
      {NULL, 0, NULL},
    };

  while (*argv != NULL)
    {
      const struct action *a;
      int i;

      /* Find action name. */
      for (a = actions; ; a++)
        if (a->name == NULL)
          PANIC ("unknown action `%s' (use -h for help)", *argv);
        else if (!strcmp (*argv, a->name))
          break;

      /* Check for required arguments. */
      for (i = 1; i < a->argc; i++)
        if (argv[i] == NULL)
          PANIC ("action `%s' requires %d argument(s)", *argv, a->argc - 1);

      /* Invoke action and advance. */
      a->function (argv);
      argv += a->argc;
    }
  
}

/** Prints a kernel command line help message and powers off the
   machine. */
static void
usage (void)
{
  printf ("\nCommand line syntax: [OPTION...] [ACTION...]\n"
          "Options must precede actions.\n"
          "Actions are executed in the order specified.\n"
          "\nAvailable actions:\n"
#ifdef USERPROG
          "  run 'PROG [ARG...]' Run PROG and wait for it to complete.\n"
#else
          "  run TEST           Run TEST.\n"
#endif
#ifdef FILESYS
          "  ls                 List files in the root directory.\n"
          "  cat FILE           Print FILE to the console.\n"
          "  rm FILE            Delete FILE.\n"
          "Use these actions indirectly via `pintos' -g and -p options:\n"
          "  extract            Untar from scratch device into file system.\n"
          "  append FILE        Append FILE to tar file on scratch device.\n"
#endif
          "\nOptions:\n"
          "  -h                 Print this help message and power off.\n"
          "  -q                 Power off VM after actions or on panic.\n"
          "  -r                 Reboot after actions.\n"
#ifdef FILESYS
          "  -f                 Format file system device during startup.\n"
          "  -filesys=BDEV      Use BDEV for file system instead of default.\n"
          "  -scratch=BDEV      Use BDEV for scratch instead of default.\n"
#ifdef VM
          "  -swap=BDEV         Use BDEV for swap instead of default.\n"
#endif
#endif
          "  -rs=SEED           Set random number seed to SEED.\n"
          "  -mlfqs             Use multi-level feedback queue scheduler.\n"
#ifdef USERPROG
          "  -ul=COUNT          Limit user memory to COUNT pages.\n"
#endif
          );
  shutdown_power_off ();
}

#ifdef FILESYS
/** Figure out what block devices to cast in the various Pintos roles. */
static void
locate_block_devices (void)
{
  locate_block_device (BLOCK_FILESYS, filesys_bdev_name);
  locate_block_device (BLOCK_SCRATCH, scratch_bdev_name);
#ifdef VM
  locate_block_device (BLOCK_SWAP, swap_bdev_name);
#endif
}

/** Figures out what block device to use for the given ROLE: the
   block device with the given NAME, if NAME is non-null,
   otherwise the first block device in probe order of type
   ROLE. */
static void
locate_block_device (enum block_type role, const char *name)
{
  struct block *block = NULL;

  if (name != NULL)
    {
      block = block_get_by_name (name);
      if (block == NULL)
        PANIC ("No such block device \"%s\"", name);
    }
  else
    {
      for (block = block_first (); block != NULL; block = block_next (block))
        if (block_type (block) == role)
          break;
    }

  if (block != NULL)
    {
      printf ("%s: using %s\n", block_type_name (role), block_name (block));
      block_set_role (role, block);
    }
}
#endif

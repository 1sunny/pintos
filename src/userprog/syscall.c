#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <threads/vaddr.h>
#include <filesys/filesys.h>
#include <filesys/file.h>
#include <devices/input.h>
#include <filesys/directory.h>
#include <filesys/inode.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "process.h"
#include "string.h"
#include "threads/malloc.h"

static void syscall_handler (struct intr_frame *);

const char *syscall_names[] = {
        "HALT",
        "EXIT",
        "EXEC",
        "WAIT",
        "CREATE",
        "REMOVE",
        "OPEN",
        "FILESIZE",
        "READ",
        "WRITE",
        "SEEK",
        "TELL",
        "CLOSE",
        "MMAP",
        "MUNMAP",
        "CHDIR",
        "MKDIR",
        "READDIR",
        "ISDIR",
        "INUMBER"
};

static const char*
get_syscall_name(int enum_val) {
  if (enum_val < 0 || enum_val >= sizeof(syscall_names) / sizeof(syscall_names[0])) {
    return "UNKNOWN_ENUM";
  }
  return syscall_names[enum_val];
}

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

static void
read_user_addr(uint8_t *dst, uint8_t *src, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (src + i >= PHYS_BASE) {
      kill_process();
    }
    int value = get_user(src + i);
    if (value == -1) {
      kill_process();
    } else {
      if (dst != NULL) {
        dst[i] = value;
      }
    }
  }
}

static void
check_write_user_addr(uint8_t *dst, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (dst + i >= PHYS_BASE) {
      kill_process();
    }
    if (!put_user(dst + i, 0)) {
      kill_process();
    }
  }
}

static int
get_arg_int(struct intr_frame *f, int num) {
  uint8_t buf[4];
  read_user_addr(buf, f->esp + num * 4, 4);
  return *(int *)buf;
}

static char*
get_arg_str(struct intr_frame *f, int num) {
  uint8_t buf[4];
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

static void*
get_buf(struct intr_frame *f, int num, size_t n, bool write) {
  uint8_t buf[4];
  read_user_addr(buf, f->esp + num * 4, 4);
  uint8_t *res = *(uint8_t **)buf;
  if (try_pin_pages(res, n, false) == false) {
    kill_process();
  }
  if (write) {
    check_write_user_addr(res, n);
  } else {
    read_user_addr(NULL, res, n);
  }
  return (void *)res;
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
syscall_create(struct intr_frame *f) {
  char *file_name = get_arg_str(f, 1);
  int initial_size = get_arg_int(f, 2);
  lock_acquire(&filesys_lock);
  f->eax = filesys_create(file_name, initial_size, false);
  lock_release(&filesys_lock);
}

static void
syscall_remove(struct intr_frame *f) {
  char *file = get_arg_str(f, 1);
  lock_acquire(&filesys_lock);
  f->eax = filesys_remove(file);
  lock_release(&filesys_lock);
}

static void
syscall_open(struct intr_frame *f) {
  char *file_name = get_arg_str(f, 1);

  struct thread *curr = thread_current();
  int fd = curr->next_fd;
  curr->next_fd++;
  struct open_file *of = malloc(sizeof(struct open_file));
  if (of == NULL) {
    PANIC("out of memory");
  }
  of->fd = fd;

  lock_acquire(&filesys_lock);
  struct file *file = filesys_open(file_name);
  lock_release(&filesys_lock);

  if (file) {
    of->file = file;
    list_push_back(&curr->open_file_list, &of->elem);
    f->eax = fd;
  } else {
    free(of);
    f->eax = -1;
  }
}

static struct open_file*
find_open_file(int fd) {
  struct thread *curr = thread_current();
  struct list_elem *e;
  for (e = list_begin (&curr->open_file_list); e != list_end (&curr->open_file_list); e = list_next (e)) {
    struct open_file *entry = list_entry(e, struct open_file, elem);
    if (entry->fd == fd) {
      return entry;
    }
  }
  return NULL;
}

static void
syscall_filesize(struct intr_frame *f) {
  int fd = get_arg_int(f, 1);

  int size = -1;

  struct open_file *of = find_open_file(fd);
  if (of) {
    lock_acquire(&filesys_lock);
    size = file_length(of->file);
    lock_release(&filesys_lock);
  }
  f->eax = size;
}

static void
syscall_read(struct intr_frame *f) {
  int fd = get_arg_int(f, 1);
  // char *buf = get_arg_str(f, 2);
  size_t size = get_arg_int(f, 3);
  uint8_t *buf = get_buf(f, 2, size, true);

  if (fd == 0) {
    int read = 0;
    while (size--) {
      buf[read] = input_getc();
      read++;
    }
    f->eax = read;
  } else if (fd == 1) {
    f->eax = -1;
  } else {
    struct open_file *of = find_open_file(fd);
    if (of) {
      lock_acquire(&filesys_lock);
      f->eax = file_read(of->file, buf, size);
      lock_release(&filesys_lock);
    } else {
      f->eax = -1;
    }
  }
  unpin_pages(buf, size);
}

static void
syscall_write(struct intr_frame *f) {
  int fd = get_arg_int(f, 1);
  // char *buf = get_arg_str(f, 2);
  size_t size = get_arg_int(f, 3);
  char *buf = get_buf(f, 2, size, false);
  if (fd == 0) {
    f->eax = -1;
  } else if (fd == 1) {
    putbuf(buf, size);
    f->eax = size;
  } else {
    struct open_file *of = find_open_file(fd);
    if (of) {
      lock_acquire(&filesys_lock);
      // dir不能写
      if (inode_get_file_type(file_get_inode(of->file)) != REGULAR) {
        f->eax = -1;
      } else {
        f->eax = file_write(of->file, buf, size);
      }
      lock_release(&filesys_lock);
    } else {
      f->eax = -1;
    }
  }
  unpin_pages(buf, size);
}

static void
syscall_seek(struct intr_frame *f) {
  int fd = get_arg_int(f, 1);
  int position = get_arg_int(f, 2);
  if (fd > 1) {
    struct open_file *of = find_open_file(fd);
    if (of){
      lock_acquire(&filesys_lock);
      file_seek(of->file, position);
      lock_release(&filesys_lock);
    }
  }
}

static void
syscall_tell(struct intr_frame *f) {
  int fd = get_arg_int(f, 1);
  if (fd > 1) {
    struct open_file *of = find_open_file(fd);
    if (of){
      lock_acquire(&filesys_lock);
      f->eax = file_tell(of->file);
      lock_release(&filesys_lock);
    } else {
      f->eax = -1;
    }
  } else {
    PANIC("syscall_tell");
  }
}

static void
syscall_close(struct intr_frame *f) {
  int fd = get_arg_int(f, 1);
  if (fd > 1) {
    struct open_file *of = find_open_file(fd);
    if (of) {
      list_remove(&of->elem);
      lock_acquire(&filesys_lock);
      file_close(of->file);
      free(of);
      lock_release(&filesys_lock);
    }
  }
}

static bool
lazy_load_file (struct page *page, void *aux) {
  struct load_file_info *info = aux;

  file_seek(info->file, info->ofs);
  size_t bytes_read = file_read(info->file, page->frame->kva, info->read_bytes);
  if (bytes_read != info->read_bytes) {
    PANIC("lazy_load_file");
    return false;
  }
  if (info->read_bytes != PGSIZE) {
    memset (page->frame->kva + info->read_bytes, 0, PGSIZE - info->read_bytes);
  }
  return true;
}

static void
syscall_mmap(struct intr_frame *f) {
  int fd = get_arg_int(f, 1);
  uint8_t *addr = (uint8_t *)get_arg_int(f, 2);
  if (fd <= 1 || addr == NULL || (uint32_t)addr % PGSIZE != 0) {
    goto fail;
  }
  struct open_file *open_file = find_open_file(fd);
  if (open_file == NULL) {
    goto fail;
  }
  size_t file_len = file_length(open_file->file);
  if (file_len == 0) {
    goto fail;
  }
  struct thread *curr = thread_current();
  // check addr over lap
  // TODO check when stack growth
  struct supplemental_page_table *spt = &curr->spt;
  for (uint8_t *i = addr; i < addr + file_len; i += PGSIZE) {
    if (spt_find_page (spt, i) != NULL) {
      goto fail;
    }
  }

  struct file *mmap_file = file_reopen(open_file->file);
  // TODO 之前page里的aux好像没释放
  for (uint8_t *pg_base = addr; pg_base < addr + file_len; pg_base += PGSIZE) {
    struct load_file_info *aux = malloc(sizeof (struct load_file_info));
    *aux = (struct load_file_info) {
            .file       = mmap_file,
            .ofs        = pg_base - addr,
            .read_bytes = (pg_base + PGSIZE < addr + file_len) ? PGSIZE : addr + file_len - pg_base,
            .map_id     = curr->next_mapid,
    };
    if (vm_alloc_page_with_initializer(VM_FILE, pg_base, true, lazy_load_file, aux) == false) {
      PANIC("syscall_mmap");
    }
  }

  f->eax = curr->next_mapid;
  curr->next_mapid++;
  return;

fail:
  f->eax = -1;
}

static void
syscall_munmap(struct intr_frame *f) {
  mapid_t id = get_arg_int(f, 1);
  struct thread *curr = thread_current();

  struct list_elem *e;
  for (e = list_begin (&curr->spt.page_list); e != list_end (&curr->spt.page_list); ) {
    struct page *entry = list_entry(e, struct page, spt_elem);
    struct list_elem *save_e = e;
    e = list_next (e);
    if (page_get_type(entry) == VM_FILE && entry->map_id == id) {
      list_remove(save_e);
      vm_dealloc_page(entry);
      return;
    }
  }
}

// Changes the current working directory of the process to dir, which may be relative or absolute.
static void
syscall_chdir (struct intr_frame *f) {
  char *dir_path = get_arg_str(f, 1);
  struct dir *dir = dir_open_path(dir_path);
  if (dir == NULL) {
    f->eax = 0;
    return;
  }
  const char *file_name = strrchr(dir_path, '/');
  if (file_name != NULL) {
    file_name++;
  } else {
    file_name = dir_path;
  }
  // printf("file_name: %s, dir->inode->sector: %d\n", file_name, inode_get_inumber(dir_get_inode(dir)));
  struct inode *inode = NULL;
  dir_lookup(dir, file_name, &inode, true);
  if (inode == NULL) {
    f->eax = 0;
    return;
  }
  thread_current()->current_dir_sector = inode_get_inumber(inode);
  // printf("current_dir_sector: %d\n", inode_get_inumber(inode));
  f->eax = 1;
}

static void
syscall_mkdir (struct intr_frame *f) {
  char *dir_path = get_arg_str(f, 1);
  bool success = filesys_create(dir_path, 0, true);
  f->eax = success;
}

static void
syscall_readdir (struct intr_frame *f) {
  PANIC("syscall_readdir");
}

static void
syscall_isdir (struct intr_frame *f) {
  PANIC("syscall_isdir");
}

static void
syscall_inumber (struct intr_frame *f) {
  PANIC("syscall_inumber");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  thread_current()->esp = f->esp;
  int syscall_num = get_arg_int(f, 0);
  // printf("syscall: %s\n", get_syscall_name(syscall_num));
  switch (syscall_num) {
    case SYS_EXIT:
      syscall_exit(f);
      break;
    case SYS_EXEC:
      syscall_exec(f);
      break;
    case SYS_WAIT:
      syscall_wait(f);
      break;
    case SYS_CREATE:
      syscall_create(f);
      break;
    case SYS_REMOVE:
      syscall_remove(f);
      break;
    case SYS_OPEN:
      syscall_open(f);
      break;
    case SYS_FILESIZE:
      syscall_filesize(f);
      break;
    case SYS_READ:
      syscall_read(f);
      break;
    case SYS_WRITE:
      syscall_write(f);
      break;
    case SYS_SEEK:
      syscall_seek(f);
      break;
    case SYS_TELL:
      syscall_tell(f);
      break;
    case SYS_CLOSE:
      syscall_close(f);
      break;
    case SYS_MMAP:
      syscall_mmap(f);
      break;
    case SYS_MUNMAP:
      syscall_munmap(f);
      break;
    case SYS_CHDIR:
      syscall_chdir(f);
      break;
    case SYS_MKDIR:
      syscall_mkdir(f);
      break;
    case SYS_READDIR:
      syscall_readdir(f);
      break;
    case SYS_ISDIR:
      syscall_isdir(f);
      break;
    case SYS_INUMBER:
      syscall_inumber(f);
      break;
    default:
      printf ("unimplemented system call!\n");
      thread_exit ();
  }
}

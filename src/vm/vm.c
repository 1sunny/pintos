/* vm.c: Generic interface for virtual memory objects. */

#include <stdio.h>
#include <userprog/process.h>
#include <userprog/exception.h>
#include <userprog/pagedir.h>
#include "threads/malloc.h"
#include "threads/pte.h"
#include "vm/vm.h"
#include "vm/inspect.h"

static struct frame_table frame_table;

static void
frame_table_init(void) {
  list_init(&frame_table.frame_list);
  struct frame *frame = NULL;
  while (true) {
    frame = malloc(sizeof (struct frame));
    if (frame == NULL) {
      break;
    }
    frame->kva = palloc_get_page(PAL_USER | PAL_ZERO);
    if (frame->kva == NULL) {
      free(frame);
      break;
    }
    frame->page = NULL;
    frame->occupied_thread = NULL;
    frame->pined = false;
    list_push_back(&frame_table.frame_list, &frame->frame_table_elem);
  }
}

// TODO 在pintos_init里调用, ok
/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
  vm_anon_init ();
  vm_file_init ();
#ifdef FILESYS  /* For project 4 */
  // TODO
#endif
  // register_inspect_intr ();
  /* DO NOT MODIFY UPPER LINES. */
  /* TODO: Your code goes here. */
  frame_table_init();
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
  int ty = VM_TYPE (page->operations->type);
  switch (ty) {
    case VM_UNINIT:
      return VM_TYPE (page->uninit.type);
    default:
      return ty;
  }
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

typedef bool (*page_initializer_t)(struct page *, enum vm_type, void *kva);

static page_initializer_t
get_page_initializer(enum vm_type type) {
  enum vm_type page_type = VM_TYPE(type);
  if (page_type == VM_ANON) {
    return anon_initializer;
  } else if (page_type == VM_FILE) {
    return file_backed_initializer;
  } else if (page_type == VM_PAGE_CACHE) {
    PANIC("vm_alloc_page_with_initializer");
  } else {
    PANIC("vm_alloc_page_with_initializer");
  }
}

// 创建一个page,只是将其加入supplemental_page_table,并不初始化和设置页表
// 如果想创建一个页,不要直接创建,通过这个函数或者vm_alloc_page
/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
                                vm_initializer *init, void *aux) {

  // 这里不直接比较type和VM_UNINIT是因为type可能还包含额外的信息
  ASSERT (VM_TYPE(type) != VM_UNINIT)

  struct supplemental_page_table *spt = &thread_current ()->spt;

  /* Check wheter the upage is already occupied or not. */
  if (spt_find_page (spt, upage) == NULL) {
    /* TODO: Create the page, fetch the initialier according to the VM type,
     * TODO: and then create "uninit" page struct by calling uninit_new. You
     * TODO: should modify the field after calling the uninit_new. */
    // TODO 1.直接malloc page?
    struct page *page = malloc(sizeof (struct page));
    page_initializer_t initializer = get_page_initializer(type);
    // 创建一个未初始化的页面,设置好页面初始化函数initializer和init
    uninit_new(page, upage, init, type, aux, initializer);
    page->writable = writable;
    page->map_id = -1;
    if (VM_TYPE(type) == VM_FILE) {
      ASSERT(aux);
      page->map_id = ((struct load_file_info *)aux)->map_id;
    }

    /* TODO: Insert the page into the spt. */
    return spt_insert_page(spt, page);
  }
  err:
  return false;
}

bool unpin_pages(uint8_t *addr_start, size_t n) {
  struct supplemental_page_table *spt = &thread_current()->spt;
  uint8_t *addr_end = addr_start + n;
  for (uint8_t *pg = pg_round_down(addr_start); pg <= addr_end; pg += PGSIZE) {
    struct page *page = spt_find_page(spt, pg);
    ASSERT(page != NULL);
    ASSERT(page->frame != NULL);
    ASSERT(page->frame->pined == true);
    page->frame->pined = false;
  }
  return true;
}

bool
try_pin_pages(uint8_t *addr_start, size_t n, bool write) {
  struct supplemental_page_table *spt = &thread_current()->spt;
  uint8_t *addr_end = addr_start + n;
  for (uint8_t *pg = pg_round_down(addr_start); pg <= addr_end; pg += PGSIZE) {
    struct page *page = spt_find_page(spt, pg);
    // TODO 应该可以去掉Lab 2的错误处理了
    if (page == false || (write && page->writable == false)) {
      // 对于恶意指针
      return false;
    }
    if (page->frame == NULL) {
      vm_do_claim_page(page);
    }
    ASSERT(page->frame != NULL);
    page->frame->pined = true;
  }
  return true;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
  /* TODO: Fill this function. */
  struct list_elem *e;
  for (e = list_begin (&spt->page_list); e != list_end (&spt->page_list); e = list_next (e)) {
    struct page *entry = list_entry(e, struct page, spt_elem);
    if (entry->va == va) {
      return entry;
    }
  }
  return NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
                 struct page *page UNUSED) {
  /* TODO: Fill this function. */
  struct list_elem *e;
  for (e = list_begin (&spt->page_list); e != list_end (&spt->page_list); e = list_next (e)) {
    struct page *entry = list_entry(e, struct page, spt_elem);
    if (entry->va == page->va) {
      return false;
    }
  }
  list_push_back(&spt->page_list, &page->spt_elem);
  return true;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
  ASSERT(spt != NULL && page != NULL);
  list_remove(&page->spt_elem);
  vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
  struct frame *victim = NULL;
  /* TODO: The policy for eviction is up to you. */
  struct list_elem *e;
  for (e = list_begin (&frame_table.frame_list); e != list_end (&frame_table.frame_list); e = list_next (e)) {
    struct frame *entry = list_entry(e, struct frame, frame_table_elem);
    if (entry->pined == false) {
      victim = list_entry(e, struct frame, frame_table_elem);
      list_remove(e);
      list_push_back(&frame_table.frame_list, e);
      return victim;
    }
  }
  PANIC("vm_get_victim");
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
  struct frame *victim UNUSED = vm_get_victim ();
  ASSERT(victim->pined == false);
  /* TODO: swap out the victim and return the evicted frame. */
  if (swap_out(victim->page) == false) {
    PANIC("vm_get_victim");
  }
  victim->page->frame = NULL; // 这里考虑加锁的时候肯定不好加
  victim->page = NULL;
  victim->occupied_thread = NULL;
  return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
  struct frame *frame = NULL;
  /* TODO: Fill this function. */
  // TODO 1.frame本身应该malloc吧? 需要一个数据结构来维护frames
  // frame要释放吗,可以只是把page置空表示没被占用
  // TODO 这些需要同步吧

  struct thread *curr = thread_current();
  struct list_elem *e;
  for (e = list_begin (&frame_table.frame_list); e != list_end (&frame_table.frame_list); e = list_next (e)) {
    struct frame *entry = list_entry(e, struct frame, frame_table_elem);
    if (entry->occupied_thread == NULL) {
      entry->occupied_thread = curr;
      return entry;
    }
  }
// TODO 加上pined,frame应该提前全部分配好?
  if (frame == NULL) {
    frame = vm_evict_frame();
    frame->occupied_thread = curr;
    return frame;
  }
  NOT_REACHED();
  ASSERT (frame->page == NULL);
  return frame;
}

static bool
is_valid_esp(uint32_t esp) {
  return (uint32_t)PHYS_BASE - esp <= 1024 * 1024;
}

/* Growing the stack. */
static bool
vm_stack_growth (void *addr UNUSED) {
  uint32_t upage = (uint32_t) pg_round_down(addr);
  if (!is_valid_esp(upage)) {
    // 栈最大1MB
    printf("stack limit\n");
    return false;
  }
  uint32_t stack_bottom = thread_current()->stack_bottom;
  ASSERT((stack_bottom - upage) % PGSIZE == 0);
  for (uint32_t i = upage; i < stack_bottom; i += PGSIZE) {
    if (!vm_alloc_page_with_initializer (VM_ANON | VM_MARKER_STACK, (void *) i,
                                         true, NULL, NULL)) { // init为NULL,到时候不会执行init
      PANIC("vm_stack_growth");
    }
    bool status = vm_claim_page((void *) i);
    ASSERT(status);
  }
  thread_current()->stack_bottom = upage;
  return true;
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
  PANIC("vm_handle_wp");
}

// -> vm_try_handle_fault -> vm_do_claim_page -> swap_in
// ->

// user和kernel都可能
// addr: the virtual address that was accessed to cause the fault
/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
                     bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
  struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
  struct page *page = NULL;
  /* TODO: Validate the fault */
  /* TODO: Your code goes here */
  // if (!user) {
  //   return false;
  // }
  void *old_addr = addr;
  addr = pg_round_down(addr);
  page = spt_find_page(spt, addr);
  // printf("page: %p, fault_addr: %p, eip: %p\n", page, old_addr, f->eip);
  if (page == NULL) {
    // check stack growth
    uint32_t esp = (uint32_t) (user ? f->esp : thread_current()->esp);
    // printf("user esp: %p, fault addr: %p\n", (void*)esp, addr);
    // For Project 3: The bad address lies approximately 64MB below the code segment,
    // so there is no ambiguity that this attempt must be rejected
    // even after stack growth is implemented.
    // Moreover, a good stack growth heuristics should probably
    // not grow the stack for the purpose of reading the system call number and arguments.
    if (write && is_valid_esp(esp) && (uint32_t) addr < esp
        && esp - (uint32_t)addr <= 16 * PGSIZE) {
      return vm_stack_growth(addr);
    }
    return false;
  }
  // 内核代码中出现fault,不能用kill函数,只能把线程exit,
  // 那么vm_try_handle_fault中就只管页不在和stack grow,除此之外的内核fault就只能是syscall的检测引起
  // TODO 应该使用下面的代码,只不过把page_fault_kill改成return false,
  // 不用也可以是因为vm_do_claim_page中判断了install_page的情况,
  // 对于write错误,install_page是失败
  // if (page->writable == false && write) {
  //   page_fault_kill(f, addr, user, write, not_present);
  // }
  // TODO not_present怎么用?
  return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
  struct thread *curr = thread_current();

  destroy (page);

  if (page->frame) {
    ASSERT(page->frame->occupied_thread == curr);
    page->frame->occupied_thread = NULL;
    ASSERT(curr->pcb != NULL);
    pagedir_clear_page(curr->pcb->pagedir, page->va);
  }
  free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
  struct page *page = NULL;
  /* TODO: Fill this function */
  // TODO 1.直接malloc分配page? 还是根据va获取? 应该是根据va去spt里面找,
  //  因为vm_alloc_page_with_initializer中分配过了
  struct supplemental_page_table *spt = &thread_current ()->spt;
  page = spt_find_page(spt, va);

  ASSERT(page != NULL);
  return vm_do_claim_page (page);
}

// 把之前创建的但没(加载,设置页表)的page加载并设置页表
/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
  ASSERT(page != NULL);

  struct frame *frame = vm_get_frame ();

  /* Set links */
  frame->page = page;
  page->frame = frame;

  /* TODO: Insert page table entry to map page's VA to frame's PA. */
  // 在页表中添加从虚拟地址到物理地址的映射
  if (!install_page(page->va, frame->kva, page->writable)) {
    // TODO 什么情况会失败? 对于只写的进行write就会吧,因为
    // TODO 应该把这个frame释放吧
    ASSERT("vm_do_claim_page");
    return false;
  }
  return swap_in (page, frame->kva);
}

// TODO thread_init中应该要调用这个吧
/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
  list_init(&spt->page_list);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
                              struct supplemental_page_table *src UNUSED) {
  PANIC("supplemental_page_table_copy");
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
  /* TODO: Destroy all the supplemental_page_table hold by thread and
   * TODO: writeback all the modified contents to the storage. */
  ASSERT(spt != NULL);
  struct list_elem *e;
  for (e = list_begin (&spt->page_list); e != list_end (&spt->page_list); ) {
    struct page *entry = list_entry(e, struct page, spt_elem);
    ASSERT(entry);
    e = list_next (e);
    vm_dealloc_page(entry);
  }
  // TODO: writeback all the modified contents to the storage.
}
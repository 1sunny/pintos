#ifndef VM_ANON_H
#define VM_ANON_H
#include <devices/block.h>
#include <threads/vaddr.h>
#include "list.h"
// #include "vm/vm.h"
struct page;
enum vm_type;

struct swap_slot {
  block_sector_t start_sector;
  bool occupied;
  struct list_elem swap_table_elem;
};

struct swap_table {
  struct list swap_slot_list;
};

struct anon_page {
  bool is_stack_page;
  struct swap_slot *swap_slot;
  bool dirty;
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);
void swap_table_init(void);
#endif

/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include <threads/malloc.h>
#include <userprog/pagedir.h>
#include "anon.h"
#include "vm/vm.h"
#include "stdio.h"
#include "threads/thread.h"

struct block *swap_disk;
static struct swap_table swap_table;

static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

void swap_table_init(void) {
	swap_disk = block_get_role(BLOCK_SWAP);

	list_init(&swap_table.swap_slot_list);
	block_sector_t swap_size = block_size(swap_disk);
	for (block_sector_t i = 0; i < swap_size; i += PGSIZE / BLOCK_SECTOR_SIZE) {
		struct swap_slot *slot = malloc(sizeof(struct swap_slot));
		slot->start_sector = i;
		slot->occupied = false;
		list_push_back(&swap_table.swap_slot_list, &slot->swap_table_elem);
	}
}

static struct swap_slot*
swap_get_slot(void) {
	struct list_elem *e;
	for (e = list_begin (&swap_table.swap_slot_list); e != list_end (&swap_table.swap_slot_list); e = list_next (e)) {
		struct swap_slot *entry = list_entry(e, struct swap_slot, swap_table_elem);
		if (entry->occupied == false) {
			entry->occupied = true;
			return entry;
		}
	}
	PANIC("swap_get_slot");
}

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_table_init();
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	// 因为每个页面最初都是uninit page,page->operations是uninit_ops,
	// uninit_ops.swap_in.uninit_initialize中会调用这个函数
	/* Set up the handler */
	page->operations = &anon_ops;
	// TODO 可能需要更新 anon_page 中的某些信息,目前该结构为空.
	struct anon_page *anon_page = &page->anon;
	anon_page->is_stack_page = type & VM_MARKER_STACK;
	anon_page->swap_slot = NULL;
	anon_page->dirty = false;
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	struct swap_slot *slot = anon_page->swap_slot;
	ASSERT(slot != NULL);
	if (true) { // anon_page->dirty || anon_page->is_stack_page
		// read from swap
		for (block_sector_t i = 0; i < PGSIZE / BLOCK_SECTOR_SIZE; ++i) {
      // 这里buffer不能用page->va,应该page->va被映射为不可写,在读的时候会出现page fault,
      // 而在处理page fault的时候又发送page fault,
      // 会导致synch.c中ASSERT (!lock_held_by_current_thread (lock)); Fail
			block_read(swap_disk, slot->start_sector + i, page->frame->kva + i * BLOCK_SECTOR_SIZE);
		}
		slot->occupied = false;
		anon_page->swap_slot = NULL;
	} else {
		// read from executable file
	}
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	struct thread *curr = page->frame->occupied_thread;
	ASSERT(is_thread(curr));
	bool dirty = pagedir_is_dirty(curr->pagedir, page->va);
	if (true) { // dirty || anon_page->is_stack_page
		struct swap_slot *slot = swap_get_slot();
		ASSERT(slot);
		// write to swap
		for (block_sector_t i = 0; i < PGSIZE / BLOCK_SECTOR_SIZE; ++i) {
			block_write(swap_disk, slot->start_sector + i, page->frame->kva + i * BLOCK_SECTOR_SIZE);
		}

		anon_page->swap_slot = slot;
	}
	anon_page->dirty |= dirty;
	// remove pte !
	pagedir_clear_page(curr->pagedir, page->va);
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	if (anon_page->swap_slot) {
		anon_page->swap_slot->occupied = false;
		anon_page->swap_slot = NULL;
	}
}

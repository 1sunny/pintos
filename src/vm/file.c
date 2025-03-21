/* file.c: Implementation of memory backed file object (mmaped object). */

#include <userprog/pagedir.h>
#include <userprog/process.h>
#include "vm/vm.h"
#include "threads/malloc.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
	// PANIC("vm_file_init");
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
	file_page->info = page->uninit.aux;
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
	PANIC("file_backed_swap_in");
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	PANIC("file_backed_swap_out");
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	struct load_file_info *info = file_page->info;
	// TODO 应该是线程自己调用吧?
	struct thread *curr = thread_current();
	ASSERT(page->frame->occupied_thread == curr);
	ASSERT(is_thread(curr));
	bool dirty = pagedir_is_dirty(curr->pcb->pagedir, page->va);
	if (dirty) {
		// TODO acquire lock
		// TODO file_page->file什么时候关闭
		file_write_at(info->file, page->va, info->read_bytes, info->ofs);
	}
	free(info);
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	PANIC("do_mmap");
}

/* Do the munmap */
void
do_munmap (void *addr) {
	PANIC("do_munmap");
}

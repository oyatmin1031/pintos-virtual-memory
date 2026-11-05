/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "lib/kernel/list.h"
#include "string.h"

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
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;

	return true;
}

void
read_mmap_file (struct file_page *file_page, void *kva) {
	file_read_at(file_page->file, kva, file_page->read_bytes_plan, file_page->ofs);
	void *page_zero_area = kva + file_page->read_bytes_plan;
	memset(page_zero_area, 0, file_page->zero_bytes_plan);
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	page->frame->kva = kva;

	read_mmap_file (file_page, kva);

	file_page->is_swapped = false;

	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	uint64_t *page_using_pml4 = page->remember_pml4;

	bool is_dirty = pml4_is_dirty(page_using_pml4, page->va);

	if (is_dirty) {
		file_write_at(file_page->file, page->frame->kva, file_page->read_bytes_plan, file_page->ofs);

		is_dirty = false;

		pml4_set_dirty(page_using_pml4, page->va, is_dirty);
	}
	file_page->is_swapped = true;

	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;

	if (pml4_is_dirty(thread_current()->pml4, page->va)) {
		file_write_at(file_page->file, page->frame->kva, file_page->read_bytes_plan, file_page->ofs);
	}

	pml4_clear_page(thread_current()->pml4, page->va);
	if (!file_page->is_swapped) {
		list_remove(&page->frame->using_frame_elem);
		page->frame->page = NULL;
	}
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {

	struct file *open_file = file_reopen(file);
	if (open_file == NULL) {
		return NULL;
	}
	
	int page_count = (length % PGSIZE == 0) ? length / PGSIZE : length / PGSIZE + 1;
	
	for (int i = 0; i < page_count; i++) {
		if (spt_find_page(&thread_current()->spt, addr + PGSIZE * i) != NULL) {
			file_close(open_file);
			return NULL;
		}
	}

	uint32_t read_bytes_plan = (length <= file_length(open_file) - offset) ? length : file_length(open_file) - offset;
	size_t zero_bytes = PGSIZE * page_count - read_bytes_plan;

	if (!split_and_register_pages(open_file, offset, addr, read_bytes_plan, zero_bytes, writable, VM_FILE, lazy_load_segment)) {
		return NULL;
	}

	struct page *mmaping_page = spt_find_page(&thread_current()->spt, addr);
	
	if (mmaping_page != NULL) {
		struct mmaping_info *curr_mmap_info = malloc(sizeof(struct mmaping_info));

		curr_mmap_info->file = open_file;
		curr_mmap_info->page_count = page_count;
		curr_mmap_info->start_addr = addr;

		list_push_back(&thread_current()->mmaping_list, &curr_mmap_info->mmaping_elem);

		return mmaping_page->va;
	}
	else {
		return NULL;
	}
}

static struct list_elem *
find_mmaping_info (void *addr) {
	struct list *mmaping_list = &thread_current()->mmaping_list;
	struct list_elem *cur_mmaping_elem = list_begin(mmaping_list);

	while (cur_mmaping_elem != list_end(mmaping_list)) {
		struct mmaping_info *mmap_info = list_entry(cur_mmaping_elem, struct mmaping_info, mmaping_elem);
		if (mmap_info->start_addr == addr) {
			return cur_mmaping_elem;
		}
		
		cur_mmaping_elem = list_next(cur_mmaping_elem);
	}
	return NULL;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct list_elem *found_mmaping_elem = find_mmaping_info (addr);
	if (found_mmaping_elem == NULL) {
		return;
	}

	struct mmaping_info *mmap_info = list_entry(found_mmaping_elem, struct mmaping_info, mmaping_elem);
	struct supplemental_page_table *curr_spt = &thread_current()->spt;

	for (int i = 0; i < mmap_info->page_count; i++) {
		struct page *find_page = spt_find_page(curr_spt, pg_round_down(addr) + PGSIZE * i);
		
		if (find_page != NULL) {
			if (!spt_remove_page(curr_spt, find_page)) {
				return;
			}
		}
	}

	list_remove(found_mmaping_elem);
	file_close(mmap_info->file);
	free(mmap_info);
}

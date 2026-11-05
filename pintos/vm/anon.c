/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "lib/kernel/list.h"
#include "threads/vaddr.h"

/* DO NOT MODIFY BELOW LINE */
struct disk *swap_disk;
static struct bitmap *swap_table;
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

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);
	size_t swap_slot_count = disk_size(swap_disk) / 8;

	swap_table = bitmap_create(swap_slot_count);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	anon_page->swap_table_num = -1;

	return true;
}

void
read_swap_disk (struct anon_page *anon_page, void *kva) {
	size_t empty_slot_num = anon_page->swap_table_num;	

	int total_slot_count = PGSIZE / DISK_SECTOR_SIZE;

	for (int i = 0; i < total_slot_count; i++) {
		disk_sector_t disk_sector_num = empty_slot_num * 8 + i;
		void *write_pos = (DISK_SECTOR_SIZE * i) + kva;
		disk_read(swap_disk, disk_sector_num, write_pos);
	}
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	page->frame->kva = kva;

	read_swap_disk (anon_page, page->frame->kva);

	bitmap_flip(swap_table, anon_page->swap_table_num);
	anon_page->swap_table_num = -1;

	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	size_t empty_slot_num = bitmap_scan_and_flip(swap_table, 0, 1, false);
	if (empty_slot_num == BITMAP_ERROR) {
		PANIC("빈 슬롯 없음");
	}

	int total_slot_count = PGSIZE / DISK_SECTOR_SIZE;

	for (int i = 0; i < total_slot_count; i++) {
		disk_sector_t disk_sector_num = empty_slot_num * 8 + i;
		void *write_pos = (DISK_SECTOR_SIZE * i) + page->frame->kva;
		disk_write(swap_disk, disk_sector_num, write_pos);
	}

	anon_page->swap_table_num = empty_slot_num;

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	size_t st_num = anon_page->swap_table_num;

	if (st_num != -1) {
		bitmap_flip(swap_table, st_num);
		return;
	}
	page->frame->page = NULL;
	list_remove(&page->frame->using_frame_elem);
}
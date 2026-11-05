/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"
#include "lib/kernel/hash.h"
#include "devices/disk.h"
#include "vm/anon.h"
#include "string.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
* intialize codes. */
struct list frame_table;

void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
	list_init(&frame_table);
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
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


/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
		struct page *new_page = malloc (sizeof(struct page));

		if (new_page == NULL) {
			goto err;
		}

		void *va = pg_round_down(upage);

		if (VM_TYPE(type) == VM_ANON) {
			uninit_new (new_page, va, init, type, aux, anon_initializer);
		}
		else if (VM_TYPE(type) == VM_FILE) {
			uninit_new (new_page, va, init, type, aux, file_backed_initializer);
		}
		else {
			free(new_page);
			goto err;
		}

		new_page->can_write = writable;

		if (spt_insert_page (spt, new_page)) {
			return true;
		}
		else {
			free(new_page);
			goto err;
		}
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page *page = NULL;
	/* TODO: Fill this function. */

	struct page tmp;
	tmp.va = pg_round_down(va);

	struct hash_elem *find_elem = hash_find(&spt->spt_hash, &tmp.hash_elem);

	if (find_elem == NULL) {
		return NULL;
	}

	page = hash_entry(find_elem, struct page, hash_elem);

	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
		struct page *page) {
	int succ = false;
	/* TODO: Fill this function. */

	if (hash_find(&spt->spt_hash, &page->hash_elem) != NULL) {
		return succ;
	}

	succ = (!hash_insert(&spt->spt_hash, &page->hash_elem)) ? true : false;

	return succ;
}

bool
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	
	if (hash_delete(&spt->spt_hash, &page->hash_elem) == NULL) {
		return false;
	}

	vm_dealloc_page (page);

	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct list_elem *oldest_frame = list_pop_front(&frame_table);
	struct frame *victim = list_entry(oldest_frame, struct frame, using_frame_elem);
	 /* TODO: The policy for eviction is up to you. */

	return (victim != NULL) ? victim : NULL;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	struct page *victim_connected_page = victim->page;

	swap_out(victim_connected_page);
	pml4_clear_page(victim_connected_page->remember_pml4, victim_connected_page->va);

	victim_connected_page->frame = NULL;

	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame;
	/* TODO: Fill this function. */
	
	frame = malloc(sizeof(struct frame));
	if (frame == NULL) {
		// PANIC("말록 실패");
		PANIC("프레임 확보 실패");
	}
	
	frame->kva = palloc_get_page(PAL_USER);
	if (frame->kva == NULL) {
		// 스왑 아웃
		// PANIC("todo");
		free(frame);
		frame = vm_evict_frame();
	}
	frame->page = NULL;
	list_push_back(&frame_table, &frame->using_frame_elem);

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
	if (vm_alloc_page(VM_ANON, addr, true)) {
		if (vm_claim_page(addr)) {
			thread_current()->stack_bottom = addr;
		}
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

bool
can_stack_grow (uint64_t rsp, uint64_t page_fault_point) {
	if (page_fault_point < rsp - 8) {
		return false;
	}
	if (USER_STACK <= page_fault_point) {
		return false;
	}
	if (USER_STACK - page_fault_point > (1 << 20)) {
		return false;
	}

	return true;
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
		bool user, bool write, bool not_present) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	if (addr == NULL || !is_user_vaddr(addr)) {
		return false;
	}

	struct page *page = spt_find_page(spt, addr);

	if (page != NULL) {
		if ((write && !page->can_write) || !not_present) {
			return false;
		}

		return vm_do_claim_page (page);
	}

	uint64_t rsp = (user) ? f->rsp : thread_current()->fault_rsp;
	uint64_t page_fault_point = (uint64_t)addr;
	
	if (can_stack_grow(rsp, page_fault_point)) {
		void *page_bottom = pg_round_down(addr);
		uintptr_t old_stack_bottom = thread_current()->stack_bottom;

		vm_stack_growth(page_bottom);
		
		return (old_stack_bottom != thread_current()->stack_bottom); 
	}
	
	return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	/* TODO: Fill this function */
	struct page *page = spt_find_page(&thread_current()->spt, va);

	if (page == NULL) {
		return false;
	}
	
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* TODO: 페이지의 가상 주소(VA)를 프레임의 물리 주소(PA)에 매핑하도록, 페이지 테이블 엔트리를 추가하세요. */
	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->can_write)) {
		return false;
	}

	page->remember_pml4 = thread_current()->pml4;

	return swap_in (page, frame->kva);
}

static uint64_t
page_hash (const struct hash_elem *e, void *aux UNUSED) {
	struct page *p = hash_entry(e, struct page, hash_elem);

	return hash_bytes (&p->va, sizeof(p->va));
}

static bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
	// uint64_t a_va = (uint64_t)hash_entry(a, struct page, hash_elem)->va;
	// uint64_t b_va = (uint64_t)hash_entry(b, struct page, hash_elem)->va;

	// return a_va < b_va;
	const struct page *a = hash_entry (a_, struct page, hash_elem);
	const struct page *b = hash_entry (b_, struct page, hash_elem);

	return a->va < b->va;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {	
	hash_init (&spt->spt_hash, page_hash, page_less, NULL);
}

struct page_plan *
setup_aux (struct file *file, off_t ofs, size_t page_read_bytes, size_t page_zero_bytes) {
	struct page_plan *aux = malloc(sizeof(struct page_plan));
	
	if (aux == NULL) {
		return NULL;
	}

	aux->file = file;
	aux->ofs = ofs;
	aux->read_bytes_plan = page_read_bytes;
	aux->zero_bytes_plan = page_zero_bytes;

	return aux;
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	// src에서 dst로 spt를 복사해야 함.
	struct hash_iterator hash_now;
	hash_first(&hash_now, &src->spt_hash);
	
	while (hash_next(&hash_now)) {
		struct page *src_page = hash_entry(hash_cur(&hash_now), struct page, hash_elem);
	
		if (src_page->operations->type == VM_UNINIT) {
			struct page_plan *src_aux = src_page->uninit.aux;
			struct page_plan *aux = setup_aux (src_aux->file, src_aux->ofs, src_aux->read_bytes_plan, src_aux->zero_bytes_plan);
	
			if (!vm_alloc_page_with_initializer(page_get_type(src_page), src_page->va, src_page->can_write, src_page->uninit.init, aux)) {
				free(aux);
				return false;
			}
		}
		else {
			if (!vm_alloc_page(page_get_type(src_page), src_page->va, src_page->can_write)) {
				return false;
			}
		}
		
		struct page *dst_page = spt_find_page (dst, src_page->va);	
		if (dst_page == NULL || !vm_claim_page(src_page->va)) {
			return false;
		}

		switch (src_page->operations->type)
		{
		case VM_ANON:
			struct anon_page *anon_page = &src_page->anon;
			if (anon_page->swap_table_num == -1) {
				memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
				break;
			}

			read_swap_disk(anon_page, dst_page->frame->kva);
			break;
		
		case VM_FILE:
			struct file_page *file_page = &src_page->file;
			if (!file_page->is_swapped) {
				memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
				break;
			}

			read_mmap_file(file_page, dst_page->frame->kva);
			break;
		
		default:
			break;
		}
	}

	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	while (!hash_empty(&spt->spt_hash)) {
		struct hash_iterator tmp;
		hash_first(&tmp, &spt->spt_hash);

		struct hash_elem *now_hash_elem = hash_next(&tmp);

		struct page *now_page = hash_entry(hash_delete(&spt->spt_hash, now_hash_elem), struct page, hash_elem);
		vm_dealloc_page(now_page);
	}
}
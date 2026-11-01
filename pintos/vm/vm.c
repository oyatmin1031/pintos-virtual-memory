/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "vm/uninit.h"
#include "vm/file.h"
#include "threads/mmu.h"
#include "userprog/process.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
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

/* Returns a hash value for page p. */
uint64_t 
page_hash (const struct hash_elem *e, void *aux UNUSED) {
  const struct page *p = hash_entry (e, struct page, hash_elem);
  return hash_bytes (&p->va, sizeof p->va);
}

/* Returns true if page a precedes page b. */
bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);

  return a->va < b->va;
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

	ASSERT (VM_TYPE(type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		struct page *page = malloc(sizeof(struct page));
		if (page == NULL) {
			free(page);
			return false;
		}

		switch (VM_TYPE(type)) {
		case VM_ANON:
			uninit_new(page, upage, init, type, aux, anon_initializer);
			break;
		case VM_FILE:
			uninit_new(page, upage, init, type, aux, file_backed_initializer);
			break;
		default:
			goto err;
		}
		page->writable = writable;

		if (!spt_insert_page(spt, page)) {
			goto err;
		}
		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	
	struct page p;
	struct hash_elem *e;

	p.va = pg_round_down(va);
	e = hash_find(&spt->hash, &p.hash_elem);

	page = (e != NULL) ? hash_entry(e, struct page, hash_elem) : NULL;

	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	/* 주어진 보조 페이지 테이블에 struct page를 삽입. */
	int succ = hash_insert(&spt->hash, &page->hash_elem) == NULL ? true : false;

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;

	/* TODO: 페이지 할당 실패 시 스왑 아웃 처리 */
	frame = malloc(sizeof(struct frame));
	if (frame == NULL) {
		return NULL;
	}
	ASSERT (frame != NULL);

	if ((frame->kva = palloc_get_page(PAL_USER)) == NULL) {
		PANIC("todo");
	}
	frame->page = NULL;
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	
	/* 사용자 메모리 주소 여부 */
	if (addr == NULL || is_kernel_vaddr(addr)) {
		return false;
	}

	/* 페이지 유무 */
	if (!not_present) {
		return false;
	}

	/* 유효한 페이지 폴트인지 검사 */
	page = spt_find_page(spt, addr);
	if (page == NULL) {
		return false;
	}

	/* 접근한 쓰기 권한과 페이지의 쓰기 권한 검사 */
	if (write && !page->writable) {
		return false;
	}

	return vm_do_claim_page (page);
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
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	if((page = spt_find_page(&thread_current()->spt, va)) == NULL) {
		return false;
	}

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	if (frame == NULL) {
		return false;
	}

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* 현재 스레드의 페이지 테이블을 물리 프레임으로 연결  */
	struct thread *curr = thread_current();
	if (!pml4_set_page(curr->pml4, page->va, frame->kva, page->writable)) {
		palloc_free_page(frame->kva);
		free(frame);
		page->frame = NULL;

		return false;
	}

	/* TODO: swap_in 실패시 처리? */
	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	/* TODO: hash_init 예외처리 */
	if (!hash_init(&spt->hash, page_hash, page_less, NULL)) {
		PANIC("hash init failed");
	}
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	
	/* src의 각 페이지를 순회하면 dst에 엔트리를 정확하게 복제 */
	struct hash_iterator iter;
	hash_first(&iter, &src->hash);
	while(hash_next(&iter)) {
		struct page *page = hash_entry(hash_cur(&iter), struct page, hash_elem);

		/* 페이지가 아직 로딩되지 않은 경우 보관된 정보를 복사 */
		if (page->operations->type == VM_UNINIT) {
			struct uninit_page *uninit_p = &page->uninit;
			struct page_args *child_args = NULL;

			/* 로딩 정보 복제, 실행 파일 참조 */
			if (uninit_p->aux != NULL) {
				child_args = malloc(sizeof(*child_args));
				if (child_args == NULL) {
					return false;
				}

				*child_args = *(struct page_args *)uninit_p->aux;
				child_args->file = thread_current()->running_file;
			}

			if (!vm_alloc_page_with_initializer(uninit_p->type, page->va, page->writable, uninit_p->init, child_args)) {
				free(child_args);
				return false;
			}

		} else {
			/* spt에 가상주소 등록 */
			if (!vm_alloc_page(VM_ANON, page->va, page->writable)) {
				return false;
			}

			/* 프레임 확보, 가상주소 매핑 */
			if (!vm_claim_page(page->va)) {
				return false;
			}
			/* 데이터 복사 */
			memcpy(spt_find_page(dst, page->va)->frame->kva, page->frame->kva, PGSIZE);
		}
	}
	
	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	while(!hash_empty(&spt->hash)) {
		struct hash_iterator iter;
		hash_first(&iter, &spt->hash);
		hash_next(&iter);

		struct hash_elem *elem = hash_cur(&iter);
		struct page *page = hash_entry(elem, struct page, hash_elem);

		hash_delete(&spt->hash, elem);
		destroy(page);
		free(page);
	}
}

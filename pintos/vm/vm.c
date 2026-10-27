/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"

/* Hash and compare SPT entries by virtual page address. */
//- page_hash(): 가상 주소를 이용해 해시값을 계산하는 함수
//page_less(): 두 페이지의 가상 주소를 비교하는 함수
/* Returns a hash value for page p. */
uint64_t
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
  const struct page *p = hash_entry (p_, struct page, hash_elem);
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
	//가상 페이지 중복 검사
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		//페이지 할당 해주기
		struct page *page = malloc(sizeof(struct page));
		//예외 검사
		if(page == NULL){
			goto err;
		}
		//
		bool (*initializer) (struct page *, enum vm_type, void *) = NULL;

		switch (VM_TYPE(type)) {
    	case VM_ANON:
        	initializer = anon_initializer;
        	break;

    	case VM_FILE:
        	initializer = file_backed_initializer;
        	break;

    	default:
        free(page);
        goto err;
}
	uninit_new(page, upage, init, type, aux, initializer);
	page->writable = writable;
	


		/* TODO: Insert the page into the spt. */
	if(spt_insert_page(spt,page) == false){
		free(page);
		goto err;
		}
		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page(struct supplemental_page_table *spt, void *va) {

    /* 1. 검색할 페이지 시작 주소 구하기 */
    struct page key = {
        .va = pg_round_down(va),
    };

    /* 2. SPT의 Hash Table에서 검색 */
    struct hash_elem *found = hash_find(&spt->pages, &key.hash_elem);

    /* 3. 검색 실패 */
    if (found == NULL)
        return NULL;

    /* 4. 찾은 hash_elem에서 struct page 주소 구하기 */
    return hash_entry(found, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	/* TODO: Fill this function. */


	 return hash_insert (&spt->pages, &page->hash_elem) == NULL;
	//NULL: 새 페이지가 삽입됨 -> true
	//기존 요소의 주소: 같은 va가 이미 있어서 삽입되지 않음 -> false
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
	/* TODO: Fill this function. */
	//프레임 관리할 구조체 할당 해주기
	frame = malloc(sizeof(struct frame));
	if(frame == NULL){
		PANIC("frame allocation fail");
	}
	//진짜 프레임 할당 RAM에서 비어 있는 물리 페이지를 찾아서 할당
	frame->kva = palloc_get_page(PAL_USER);
	if(frame->kva == NULL){
		free(frame);
		PANIC("todo:EVICTION");
	}

    frame->page = NULL;


	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
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
vm_claim_page(void *va) {
    /* 1. 현재 프로세스의 SPT에서 페이지 검색 */
    struct page *page = spt_find_page(&thread_current()->spt, va);

    /* 2. 해당 페이지가 없으면 실패 */
    if (page == NULL)
        return false;

    /* 3. 찾은 페이지를 실제 프레임에 올리기 */
    return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	bool success = pml4_set_page (thread_current ()->pml4, page->va,frame->kva, page->writable);
	//false가 나오면 기존에 선언했던 것들 할당 했던것들 다 free나 null -> 메모리 누수 떄문에.
	if (!success) {
    page->frame = NULL;
    frame->page = NULL;
    palloc_free_page (frame->kva);
    free (frame);
    return false;
}
	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void 
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init (&spt->pages, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}

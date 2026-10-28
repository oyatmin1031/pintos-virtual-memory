/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"

/* 문서의 page_hash 예제: 가상 페이지 주소를 해시값으로 바꾼다.
 * 해시 테이블은 이 값으로 버킷을 선택한다. 해시값을 page에 저장하지는 않는다. */
uint64_t
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
  /* 테이블이 받은 hash_elem에서 그것을 포함하는 원래 page의 주소를 구한다. */
  const struct page *p = hash_entry (p_, struct page, hash_elem);
  /* &p->va가 가리키는 주소값의 바이트들을 읽는다. 사용자 페이지 내용은 읽지 않는다. */
  return hash_bytes (&p->va, sizeof p->va);
}
/* 문서의 page_less 예제: 두 페이지의 가상 주소 대소 관계를 반환한다. */
bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
  /* 두 연결 요소를 각각의 page로 되돌린다. */
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);

  /* 해시 테이블은 A < B도 B < A도 거짓이면 같은 키로 판단한다.
   * 이 비교 함수 때문에 버킷 리스트가 주소순으로 정렬되는 것은 아니다. */
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

	/* VM_TYPE은 marker 비트를 제외한 기본 타입을 구한다.
	 * 호출자는 최종 타입 ANON/FILE을 지정하고, 생성되는 현재 상태는 UNINIT이다. */
	ASSERT (VM_TYPE(type) != VM_UNINIT);

	/* 새 페이지를 등록할 현재 프로세스의 SPT를 가져온다. */
	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* 같은 가상 페이지가 이미 있으면 새로 등록하지 않고 실패한다.
	 * upage는 페이지 시작 주소라는 전제로 사용한다. */
	if (spt_find_page (spt, upage) == NULL) {
		/* 커널 메모리에 관리용 page 구조체를 할당한다. 사용자용 4KB frame은 아니다. */
		struct page *page = malloc(sizeof(struct page));
		/* 관리 구조체조차 확보하지 못하면 공통 실패 경로로 이동한다. */
		if(page == NULL){
			goto err;
		}
		/* page, type, kva를 받아 bool을 반환하는 함수의 주소를 담을 변수.
		 * init은 내용 로딩용이고, initializer는 페이지 타입 초기화용이다. */
		bool (*initializer) (struct page *, enum vm_type, void *) = NULL;

		/* 최초 claim 때 어떤 타입 초기화 함수를 호출할지 선택만 한다. */
		switch (VM_TYPE(type)) {
    	case VM_ANON:
        	/* 함수 호출이 아니라 anon_initializer의 주소를 저장한다. */
        	initializer = anon_initializer;
        	break;

    	case VM_FILE:
        	/* mmap 등의 파일 기반 페이지에 사용할 타입 초기화 함수. */
        	initializer = file_backed_initializer;
        	break;

    	default:
        /* 지원하지 않는 타입: 방금 만든 관리 구조체를 반환한다. */
        free(page);
        goto err;
}
	/* UNINIT 상태, va, 최종 type, 두 초기화 함수와 aux를 기록한다.
	 * 이 호출은 frame을 확보하거나 init을 실행하지 않는다. */
	uninit_new(page, upage, init, type, aux, initializer);
	/* uninit_new가 page 전체를 초기화하므로 추가 필드인 쓰기 권한은 그 뒤에 저장한다. */
	page->writable = writable;
	


	/* 등록한 주소로 이후 최초 접근에서 page를 찾을 수 있도록 SPT에 넣는다. */
	if(spt_insert_page(spt,page) == false){
		/* 삽입 실패 시 page만 해제한다. aux의 실패 시 정리는 호출자가 맡는다. */
		free(page);
		goto err;
		}
		/* SPT 등록 성공. 아직 실제 사용자 메모리 내용이 로딩된 상태는 아니다. */
		return true;
	}
err:
	/* 주소 중복, 할당 실패, 지원하지 않는 타입, 삽입 실패의 공통 반환값. */
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page(struct supplemental_page_table *spt, void *va) {

    /* 문서의 page_lookup 예제와 같은 검색용 임시 page.
     * 0x401123 같은 페이지 중간 주소도 시작 주소 0x401000으로 바꿔 찾는다. */
    struct page key = {
        .va = pg_round_down(va),
    };

    /* 임시 page를 삽입하지 않고 키만 전달한다.
     * hash_find가 page_hash로 버킷을 찾고 page_less로 일치하는 주소를 찾는다. */
    struct hash_elem *found = hash_find(&spt->pages, &key.hash_elem);

    /* 3. 검색 실패 */
    if (found == NULL)
        return NULL;

    /* 검색용 key의 주소가 아니라, SPT에 이미 등록된 실제 page를 반환한다. */
    return hash_entry(found, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	/* hash_insert는 중복 키가 없으면 연결 요소를 버킷 리스트에 넣고 NULL을 반환한다.
	 * 같은 va가 이미 있으면 기존 요소를 반환하므로 false가 된다. */
	 return hash_insert (&spt->pages, &page->hash_elem) == NULL;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	/* 해제된 page의 연결 요소가 SPT에 남지 않도록 먼저 분리한다. */
	hash_delete (&spt->pages, &page->hash_elem);
	vm_dealloc_page (page);
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
	/* 반환할 관리 구조체의 포인터. malloc 전에는 아직 구조체가 없다. */
	struct frame *frame = NULL;
	/* 어떤 물리 페이지를 관리하는지 기록할 frame 구조체를 커널에 할당한다. */
	frame = malloc(sizeof(struct frame));
	/* 관리 구조체를 확보하지 못하면 커널을 중단한다. */
	if(frame == NULL){
		PANIC("frame allocation fail");
	}
	/* user pool의 비어 있는 물리 페이지 하나를 확보한다.
	 * 반환값은 그 프레임에 커널이 접근할 수 있는 가상 주소(kva)이다. */
	frame->kva = palloc_get_page(PAL_USER);
	/* 현재 단계에는 eviction이 없으므로 물리 페이지가 부족하면 중단한다. */
	if(frame->kva == NULL){
		/* 먼저 확보했던 frame 관리 구조체는 반환한다. */
		free(frame);
		PANIC("todo:EVICTION");
	}

    /* malloc은 0으로 초기화하지 않는다. 아직 연결된 가상 page가 없음을 직접 기록한다. */
    frame->page = NULL;


	/* ASSERT는 값을 초기화하지 않고, 위 작업의 결과가 조건을 만족하는지 확인한다. */
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	/* 물리 페이지 주소 자체가 아니라 그 주소를 담은 frame 구조체를 반환한다. */
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
vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr,
                    bool user UNUSED, bool write, bool not_present) {

    struct supplemental_page_table *spt = &thread_current()->spt;
    struct page *page = NULL;

    // 1. 유효한 사용자 가상 주소인지 확인
    if (addr == NULL || !is_user_vaddr(addr))
        return false;

    // 2. 매핑되지 않은 페이지 때문에 발생한 Fault인지 확인
	//not_present = 페이지 부재로 발생한 fault이면 true
    if (!not_present)
        return false;

    // 3. SPT에서 페이지 검색
    page = spt_find_page(spt, addr);

    if (page == NULL)
        return false;

    // 4. 쓰기 권한 확인
    if (write && !page->writable)
        return false;

    // 5. 실제 RAM 프레임 할당 및 페이지 로딩
    return vm_do_claim_page(page);
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
    /* va에 대응하는 관리 정보가 현재 프로세스에 등록돼 있는지 검색한다. */
    struct page *page = spt_find_page(&thread_current()->spt, va);

    /* 등록되지 않은 페이지는 여기서 새로 만들지 않고 실패한다. */
    if (page == NULL)
        return false;

    /* 찾은 page에 프레임을 연결하고 매핑 및 내용 로딩을 수행하는 함수로 넘긴다. */
    return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	/* 사용자용 물리 페이지와 그것을 관리할 frame 구조체를 함께 확보한다. */
	struct frame *frame = vm_get_frame ();

	/* frame에서 자신을 사용하는 page를 찾을 수 있게 연결한다. */
	frame->page = page;
	/* page에서도 자신이 올라간 frame을 찾을 수 있게 역방향으로 연결한다. */
	page->frame = frame;

	/* 현재 프로세스의 PML4에 사용자 va -> frame 매핑과 쓰기 권한을 등록한다.
	 * frame->kva의 물리 주소 변환은 pml4_set_page 내부에서 처리한다. */
	bool success = pml4_set_page (thread_current ()->pml4, page->va,frame->kva, page->writable);
	/* 현재 operations의 swap_in을 호출한다. 최초 UNINIT이면 uninit_initialize가
	 * 타입 초기화 후 내용 로딩 콜백을 실행한다. 매핑 실패 시에는 호출하지 않는다. */
	if (success && swap_in (page, frame->kva))
		return true;

	/* 로딩 실패로 매핑된 빈 프레임이 남지 않도록 물리 메모리를 반환하기 전에 지운다. */
	if (success)
		pml4_clear_page (thread_current ()->pml4, page->va);

	/* 매핑 또는 로딩 실패: 양방향 연결과 두 종류의 할당을 모두 되돌린다. */
	page->frame = NULL;
	frame->page = NULL;
	palloc_free_page (frame->kva);
	free (frame);
	return false;
}

/* Initialize new supplemental page table */
void 
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	/* 문서의 hash_init 예제: 빈 버킷들을 만들고 주소 기준 두 함수를 등록한다.
	 * 지금 page_hash/page_less를 실행하는 것이 아니라 이후 검색/삽입 때 사용할
	 * 함수 포인터를 저장한다. 이 호출만으로 page나 사용자 frame이 생성되지는 않는다. */
	if (!hash_init (&spt->pages, page_hash, page_less, NULL))
		PANIC ("SPT initialization failed");
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

/* vm.c: Generic interface for virtual memory objects. */

#include <string.h>
#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

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

/* 허용된 스택 주소에 페이지를 생성하고 실제 메모리를 준비한다. */
static void
vm_stack_growth (void *addr) {
	struct supplemental_page_table *spt = &thread_current ()->spt;

	addr = pg_round_down (addr);

	struct page *page = spt_find_page (spt, addr);
	if (page != NULL)
		return;

	/* 쓰기 가능한 anonymous 스택 페이지를 등록한다. */
	if (!vm_alloc_page (VM_ANON | VM_MARKER_0, addr, true))
		return;

	/* 등록된 페이지의 주소를 page 변수에 새로 저장한다. */
	page = spt_find_page (spt, addr);

	/* frame 확보·매핑에 실패하면 등록한 페이지도 정리한다. */
	if (!vm_claim_page (addr)) {
		spt_remove_page (spt, page);
		return;
	}

	/* 새 스택 페이지를 0으로 초기화한다. */
	memset (page->frame->kva, 0, PGSIZE);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* 잘못된 접근은 거부하고, 스택 성장 또는 기존 페이지 로딩으로 fault를 복구한다.
 * 복구에 성공하면 true를 반환한다. */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
		bool user, bool write, bool not_present) {
	struct thread *curr = thread_current ();
	struct supplemental_page_table *spt = &curr->spt;

	/* NULL이나 커널 주소에 대한 접근은 허용하지 않는다. */
	if (addr == NULL || !is_user_vaddr (addr))
		return false;

	/* 권한 위반은 페이지를 새로 할당해서 해결하지 않는다. */
	if (!not_present)
		return false;

	struct page *page = spt_find_page (spt, addr);

	if (page == NULL) {
		/* 커널 fault에서는 저장해 둔 사용자 rsp를 사용한다. */
		uintptr_t user_rsp = user ? (uintptr_t) f->rsp : curr->user_rsp;

		uintptr_t fault_addr = (uintptr_t) addr;

		/* 스택은 USER_STACK 아래 최대 1MB까지 허용한다. */
		if (fault_addr < (uintptr_t) USER_STACK - (1UL << 20)
			|| fault_addr >= (uintptr_t) USER_STACK)
			return false;

		/* PUSH가 rsp를 갱신하기 전에 접근하는 rsp - 8도 허용한다.
		 * user_rsp < 8 검사는 뺄셈에서 정수가 넘치는 것을 막는다. */
		if (user_rsp < 8 || fault_addr < user_rsp - 8)
			return false;

		/* 정상적인 스택 접근이면 새 페이지를 생성하고 claim한다. */
		vm_stack_growth (addr);

		/* growth는 void 함수이므로 등록·frame·매핑 결과를 차례로 확인한다.
		 * 이미 claim했으므로 여기서 다시 claim하지 않는다. */
		page = spt_find_page (spt, addr);
		if (page == NULL)
			return false;

		/* 실제 물리 프레임이 연결됐는지 확인한다. */
		if (page->frame == NULL)
			return false;

		/* 사용자 주소에 매핑됐는지 확인한다. */
		if (pml4_get_page (curr->pml4, addr) == NULL)
			return false;

		return true;
	}

	/* 기존 페이지에 쓰려면 쓰기 권한이 있어야 한다. */
	if (write && !page->writable)
		return false;

	/* 이미 frame이 있으면 중복 할당하지 않고 매핑을 확인한다. */
	if (page->frame != NULL) {
		if (pml4_get_page (curr->pml4, addr) == NULL)
			return false;

		return true;
	}

	/* SPT에 등록되어 있지만 아직 RAM에 없는 페이지를 로딩한다. */
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
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED, struct supplemental_page_table *src UNUSED){
	/* 페이지 생성 함수는 현재 스레드의 SPT에 등록하므로 자식 문맥에서 호출한다. */
	ASSERT (dst == &thread_current ()->spt);

	struct hash_iterator i;	
	struct hash_elem *e;
	hash_first (&i, &src->pages);

	while ((e = hash_next (&i)) != NULL) {
    	struct page *page = hash_entry (e, struct page, hash_elem);
		switch(page->operations->type){
			case VM_UNINIT: {
				/* 현재는 실행 파일용 anonymous 페이지의 로딩 정보만 복사한다. */
				if (VM_TYPE (page->uninit.type) != VM_ANON)
					return false;
				/* 부모에게 로딩 정보가 없는 경우 자식도 NULL을 전달한다. */
				struct lazy_load_info *child_aux = NULL;
				if (page->uninit.aux != NULL) {
					/* 현재 loader의 aux는 lazy_load_info이다.
					 * 구조체를 따로 만들어 부모가 aux를 해제해도 자식은 유지한다. */
					const struct lazy_load_info *parent_aux = page->uninit.aux;
					child_aux = malloc (sizeof *child_aux);
					if (child_aux == NULL)
						return false;
					*child_aux = *parent_aux;

					/* file 포인터는 값 복사만 하면 부모의 파일을 가리킨다.
					 * fork가 미리 복제한 자식 실행 파일을 빌려 사용하도록 바꾼다. */
					child_aux->file = thread_current ()->running_file;
					if (child_aux->file == NULL) {
						free (child_aux);
						return false;
					}
				}

				/* aux의 유무와 관계없이 자식 page는 등록해야 한다. */
				if (!vm_alloc_page_with_initializer (
					page->uninit.type,  /* 부모 페이지가 초기화될 최종 타입 */
					page->va,          /* 같은 사용자 가상 주소 */
					page->writable,    /* 같은 쓰기 권한 */
					page->uninit.init,  /* 나중에 내용을 로딩할 함수 */
					child_aux          /* 별도로 복제한 자식용 로딩 정보 */
				)) {
					/* 등록 성공 전까지 aux는 이 복사 코드가 소유한다. */
					free (child_aux);
					return false;
				}
				break;
			}
			case VM_ANON:
			if (!vm_alloc_page (VM_ANON, page->va, page->writable))
    			return false;
			if (!vm_claim_page (page->va))
    			return false;
			struct page *child_page = spt_find_page (dst, page->va);
			memcpy(child_page->frame->kva,page->frame->kva,PGSIZE);
			break;

			case VM_FILE:
				return false;

			default:
				/* 다음 구현 대상. 아직 복사하지 않은 타입을 성공으로 처리하지 않는다. */
				return false;
		}
}
	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* 전체 SPT를 순회할 상태와 현재 가져온 해시 요소의 주소를 준비한다. */
	struct hash_iterator i;
	struct hash_elem *e;

	/* 이 SPT의 pages 해시 테이블을 순회하도록 준비한다. 아직 요소를 가져오지는 않는다. */
	hash_first (&i, &spt->pages);

	/* 다음 요소가 있으면 e에 저장한다. 모든 요소가 없어지면 NULL이 되어 끝난다. */
	while ((e = hash_next (&i)) != NULL) {
		/* 해시 연결 요소를 포함하고 있는 원래 page 구조체의 주소를 구한다. */
		struct page *page = hash_entry (e, struct page, hash_elem);
		/* page 메모리를 해제하기 전에 버킷 리스트에서 연결을 먼저 제거한다.
		 * hash_delete 자체는 page를 free하지 않는다. */
		hash_delete (&spt->pages, e);
		/* operations의 destroy로 aux/frame 관리 정보를 정리하고 page도 free한다. */
		vm_dealloc_page(page);
		/* hash_delete가 iterator를 무효로 만들므로 남은 테이블의 처음부터 다시 준비한다. */
		hash_first(&i,&spt->pages);
	}
	/* 모든 page는 위에서 해제했다. NULL 콜백으로 버킷 배열만 반환한다.
	 * thread 안에 들어 있는 spt 구조체 자체를 free하는 것은 아니다. */
	hash_destroy (&spt->pages, NULL);
	/* 현재 소유권 설계에서 PML4와 매핑된 물리 페이지는 호출자가 이후 정리한다.
	 * exec이 이 SPT를 다시 쓰려면 새 프로그램 로딩 전에 재초기화해야 한다. */

}

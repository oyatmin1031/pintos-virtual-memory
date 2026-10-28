/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/malloc.h"


/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
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

	swap_disk = NULL;
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* 최초 UNINIT 페이지의 uninit_initialize가 호출하는 타입 초기화 함수.
	 * anon_ops에는 VM_ANON 타입과 anonymous용 swap_in/out/destroy가 들어 있다.
	 * 포인터를 교체하면 이 page는 이후 anonymous 방식으로 관리된다. */
	page->operations = &anon_ops;
	/* 현재 anon_page는 비어 있으므로 추가 상태 초기화가 없다.
	 * 성공을 반환하면 uninit_initialize가 다음으로 lazy_load_segment 같은
	 * 내용 로딩 콜백을 실행한다. 이 함수 자체에서 파일 내용을 읽지는 않는다. */
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	/* anonymous 전용 관리 정보의 주소. 현재 구조체는 비어 있어 아직 사용하지 않는다. */
	struct anon_page *anon_page = &page->anon;
	/* page->frame에 이미 저장된 frame 구조체의 주소를 가져온다.
	 * &page->frame은 포인터 멤버의 주소이므로 여기서 사용할 값이 아니다. */
	struct frame *frame = page->frame;
	/* 연결된 frame 관리 구조체가 있을 때만 정리한다. */
	if(page->frame != NULL){
		/* frame에서 page로 향하는 역방향 연결을 끊는다. */
		frame->page = NULL;
		/* malloc으로 만든 관리 구조체만 반환한다.
		 * 현재 종료 흐름에서 실제 물리 페이지는 이후 pml4_destroy가 반환한다. */
		free(frame);
		/* page가 해제된 frame 구조체를 계속 가리키지 않도록 한다. */
		page->frame = NULL;

	}
	/* page 자체는 vm_dealloc_page가 해제하므로 여기서 다시 free하지 않는다. */
}

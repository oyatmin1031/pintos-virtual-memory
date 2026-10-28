/* uninit.c: Implementation of uninitialized page.
 *
 * All of the pages are born as uninit page. When the first page fault occurs,
 * the handler chain calls uninit_initialize (page->operations.swap_in).
 * The uninit_initialize function transmutes the page into the specific page
 * object (anon, file, page_cache), by initializing the page object,and calls
 * initialization callback that passed from vm_alloc_page_with_initializer
 * function.
 * */

#include "vm/vm.h"
#include "vm/uninit.h"
#include "threads/malloc.h"

static bool uninit_initialize (struct page *page, void *kva);
static void uninit_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations uninit_ops = {
	.swap_in = uninit_initialize,
	.swap_out = NULL,
	.destroy = uninit_destroy,
	.type = VM_UNINIT,
};

/* DO NOT MODIFY this function */
void
uninit_new (struct page *page, void *va, vm_initializer *init,
		enum vm_type type, void *aux,
		bool (*initializer)(struct page *, enum vm_type, void *)) {
	ASSERT (page != NULL);

	*page = (struct page) {
		.operations = &uninit_ops,
		.va = va,
		.frame = NULL, /* no frame for now */
		.uninit = (struct uninit_page) {
			.init = init,
			.type = type,
			.aux = aux,
			.page_initializer = initializer,
		}
	};
}

/* Initalize the page on first fault */
static bool
uninit_initialize (struct page *page, void *kva) {
	struct uninit_page *uninit = &page->uninit;

	/* Fetch first, page_initialize may overwrite the values */
	vm_initializer *init = uninit->init;
	void *aux = uninit->aux;

	/* TODO: You may need to fix this function. */
	return uninit->page_initializer (page, uninit->type, kva) &&
		(init ? init (page, aux) : true);
}

/* Free the resources hold by uninit_page. Although most of pages are transmuted
 * to other page objects, it is possible to have uninit pages when the process
 * exit, which are never referenced during the execution.
 * PAGE will be freed by the caller. */
static void
uninit_destroy (struct page *page) {
	/* 새로 할당하는 것이 아니라, page 내부의 미초기화 관리 정보를 가리킨다. */
	struct uninit_page *uninit UNUSED = &page->uninit;
	/* 스택처럼 aux가 없는 페이지라면 해제할 로딩 정보가 없다. */
	if(uninit ->aux == NULL){
		return;
	}
	/* load_segment에서 동적 할당한 로딩 정보 전체를 반환한다.
	 * 그 안의 file은 프로세스에서 빌려 쓴 참조이므로 여기서 닫지 않는다. */
	free(uninit->aux);
	/* free는 포인터 값을 바꾸지 않으므로 해제된 주소가 남지 않도록 정리한다. */
	uninit->aux = NULL;
	/* uninit은 page 내부 공간이므로 따로 free하지 않는다.
	 * page 구조체 자체의 free는 호출자인 vm_dealloc_page가 수행한다. */

}

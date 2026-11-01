# VM 진입 이후 변경 기록: 의도, 문제, 선택 이유, 실제 코드

작성일: 2026-10-30 (KST)

## 1. 문서의 기준과 읽는 방법

이 문서는 Project 2 이후 VM 작업이 시작되기 직전인 `b9defdb`를 기준으로 작성했다. 현재 체크아웃한 브랜치는 `minho`, HEAD는 `9a409c1`이다. **현재 작업 트리에 반영된 변경**과 **다른 브랜치에서만 진행된 변경**을 분리한다. 다른 브랜치의 코드를 현재 구현 완료 상태로 해석하면 안 된다.

- 현재 브랜치의 VM 커밋: `d5d497e`(SPT), `9a409c1`(프레임·claim).
- 현재 미커밋 변경: `.vscode/settings.json`, `exception.c`, `process.c`, `syscall.c`, `anon.c`, `vm.c`의 6개 파일.
- 비교한 다른 브랜치: 로컬에 저장된 원격 추적 참조 `origin/sunghyun/SPT`, `origin/feat/Anonymous-Page/Ryuuuuu5618`, `origin/master`. 네트워크 fetch는 하지 않았다.
- 설명의 근거: Git 커밋 메시지·diff, 현재 소스, 남아 있는 테스트 산출물이다. 당시 대화나 디버깅 일지는 없으므로 작성자의 실제 생각을 단정하지 않는다. ‘이유’는 코드 동작으로 설명할 수 있는 기술적 이유이고, 로그로 입증하지 못한 장애 원인은 ‘추론’으로 표시한다.
- 변경 코드는 뒤쪽 부록에 **원본 diff 전체**를 넣었다. `-`는 변경 전, `+`는 변경 후, 공백으로 시작하는 줄은 주변 문맥이다. 주석·TODO·미완성 코드도 그대로 보존했다. 각 커밋 diff는 해당 커밋의 부모와 비교한 것이며, 서로 다른 브랜치의 diff를 차례로 적용하라는 뜻이 아니다.

## 2. 전체적으로 무엇을 바꾸려 했나

기존 Project 2의 즉시 적재 방식에서, **가상 페이지 정보 등록과 실제 물리 메모리 할당을 분리하는 방식**으로 넘어가는 작업이다.

| 단계 | 맡는 일 | 이 단계에서 하지 않는 일 |
| --- | --- | --- |
| SPT 등록 | 가상 주소, 쓰기 권한, 최종 페이지 타입, 나중에 읽을 파일 정보를 기록 | 사용자 프레임 확보·파일 내용 읽기 |
| page claim | 프레임 확보, page와 frame 연결, PML4 매핑, 내용 초기화 | 등록되지 않은 주소를 무조건 유효하게 만들기 |
| page fault 복구 | 유효한 지연 적재 페이지인지 판별한 뒤 claim | 읽기 전용 페이지에 대한 쓰기를 허용하기 |
| fork·exec·종료 | 페이지와 로딩 정보의 복사·소유권·정리 관리 | 부모·자식이 해제 가능한 aux를 공유하기 |
| 스택 확장 | 조건에 맞는 스택 접근에 한해 새 페이지 등록·claim | 모든 미등록 주소를 스택으로 취급하기 |

현재 작업 트리에 추가된 흐름은 다음과 같다.

```text
load_segment()
  → 페이지별 page_args 준비
  → vm_alloc_page_with_initializer(VM_ANON, ..., lazy_load_segment, aux)
  → uninit_new() → SPT 등록 (아직 프레임 없음)

첫 메모리 접근
  → page_fault() → vm_try_handle_fault()
  → SPT 조회·접근 권한 확인 → vm_do_claim_page()
  → vm_get_frame() → page ↔ frame 연결 → pml4_set_page()
  → swap_in() → uninit_initialize()
  → anon_initializer() → lazy_load_segment()
  → 파일 읽기·나머지 0 초기화 → 실행 재개
```

여기서 최초 `swap_in()` 호출은 디스크 스왑 구현이 완료됐다는 뜻이 아니다. UNINIT 상태의 함수 포인터를 통해 초기화 함수를 호출하는 것이며, 현재 anonymous의 실제 swap-in/out은 미구현이다.

## 3. 현재 브랜치: 커밋된 변경

### 3.1 SPT를 해시 테이블로 구현 — `d5d497e`

**바꾸려던 것:** 빈 `supplemental_page_table`과 조회·삽입 함수 골격을 실제 페이지 관리 자료구조로 바꾸었다.

**기존 문제:** PML4만으로는 ‘합법적인 가상 페이지지만 아직 RAM에 없는 상태’의 메타데이터를 관리할 수 없다. 기존 `spt_find_page()`는 실제 조회를 하지 않았고 삽입 함수도 항상 실패했다.

**변경 내용:** `vm.h`에 해시 헤더, `page.hash_elem`, `spt.hash`를 추가했다. `page_hash()`는 가상 주소 값의 바이트로 해시를 계산하고, `page_less()`는 주소를 비교한다. 조회에서는 `pg_round_down(va)`로 페이지 시작 주소를 구해 검색한다. 삽입은 `hash_insert()`의 반환값이 NULL일 때 성공으로 처리한다. 초기화 실패는 PANIC으로 처리했다.

**그렇게 한 이유:** 페이지 중간 주소로 fault가 나도 같은 4KB 페이지를 찾을 수 있어야 하고, 같은 가상 페이지의 중복 등록도 막아야 한다. 저장소에 이미 있는 해시 구현을 사용하므로 별도 자료구조가 필요 없다. 해시 키는 페이지 내용이 아니라 `page->va`이다.

**연결되는 곳:** 프로세스 초기화의 `supplemental_page_table_init()`, 로더의 페이지 등록, fault/시스템콜의 조회, fork의 복사, 종료의 정리다. 다만 이 커밋만으로 복사·정리가 구현되지는 않았다. 원본 코드는 부록 A-1에 있다.

### 3.2 프레임 확보와 페이지 매핑 구현 — `9a409c1`

**바꾸려던 것:** SPT에 등록한 페이지를 실제 사용자 메모리로 사용할 수 있도록 claim 경로를 연결했다.

**기존 문제:** `vm_get_frame()`이 프레임을 확보하지 않았고, `vm_claim_page()`도 대상 페이지를 찾지 않았다. page와 frame의 포인터만 연결해도 CPU가 사용자 주소를 해석할 수 있는 것은 아니므로 PML4 등록이 필요했다.

**변경 내용:** `page.writable`을 추가하고, `malloc()`으로 관리용 frame을 만든 뒤 `palloc_get_page(PAL_USER)`로 사용자 물리 페이지를 확보한다. `vm_claim_page()`는 SPT에서 페이지를 찾고, `vm_do_claim_page()`는 양방향 연결 후 `pml4_set_page()`로 매핑한다. 매핑 실패 시 물리 페이지와 frame 구조체를 반환하고 `page->frame`을 NULL로 되돌린다.

**그렇게 한 이유:** page는 가상 페이지의 관리 정보이고 frame은 RAM의 실제 저장 공간이므로 둘을 구분한다. `frame->kva`는 물리 주소 자체가 아니라 커널이 해당 프레임에 접근하는 가상 주소이며, 물리 주소 변환은 PML4 함수 내부에서 처리한다. 쓰기 권한도 매핑에 전달해야 코드 페이지 보호를 유지할 수 있다.

**남은 부분:** user pool이 부족하면 `PANIC("todo")`이며 eviction은 없다. `swap_in()` 실패 후 복구는 TODO 상태이다. 원본 코드는 부록 A-2에 있다.

## 4. 현재 작업 트리: 아직 커밋하지 않은 변경

### 4.1 VM 프로젝트를 기본 실행 대상으로 선택

`.vscode/settings.json`의 `pintos.project`를 1에서 2로 바꾸고 IntelliSense defines에 `VM`을 추가했다. 프로젝트 배열이 `["threads", "userprog", "vm", "ALL"]`이므로 인덱스 2는 VM이다.

**문제와 이유:** 실행·빌드 대상으로 VM을 선택하고 에디터가 `#ifdef VM` 내부 코드도 해석하도록 맞추려는 변경이다. 실제 VM 빌드의 `-DVM`은 `pintos/vm/Make.vars`에 이미 있다. 에디터의 defines 추가와 실제 컴파일 옵션은 역할이 다르다.

### 4.2 UNINIT 페이지를 SPT에 등록하는 생성 함수 구현

`vm_alloc_page_with_initializer()`에 page 할당, 타입별 initializer 선택, `uninit_new()`, 권한 저장, SPT 삽입을 추가했다. 빠져 있던 `ASSERT` 뒤 세미콜론과 관련 헤더도 보완했다.

**문제와 이유:** 로더가 ‘나중에 적재할 페이지’를 등록할 진입점이 비어 있었다. 최종 타입이 `VM_ANON`이어도 처음에는 UNINIT으로 만들어야 첫 claim 시 타입 초기화와 내용 적재를 순서대로 실행할 수 있다. `uninit_new()`가 page 전체를 초기화하므로 `writable`은 그 뒤에 기록한다. `VM_TYPE(type)`으로 marker 비트를 제외하므로 `VM_ANON | VM_MARKER_0`도 anonymous initializer를 선택한다.

**현재 한계:** 지원하지 않는 타입이나 삽입 실패로 `err`에 가면 이미 할당한 page를 해제하지 않는다. `free(page)`가 있는 곳은 `malloc()`이 NULL을 반환한 분기로, 정상 할당된 page의 실패 경로 정리를 대신하지 못한다.

### 4.3 실행 파일을 첫 접근 시 읽도록 지연 적재 연결

`process.c`에 `struct page_args`를 추가했다. 파일 포인터·offset·읽을 크기·0으로 채울 크기를 페이지마다 저장한다. `load_segment()`는 aux를 동적 할당해 SPT 등록 함수에 넘기고, 등록 실패 시 해제한다. 다음 페이지로 갈 때 `ofs += page_read_bytes`도 수행한다.

`lazy_load_segment()`는 `page->frame->kva`에 `file_read_at()`으로 파일을 읽고, 남은 공간을 `memset()`으로 0으로 채운다. 성공·읽기 실패 시 aux를 해제한다.

**문제와 이유:** 나중에 실행될 콜백에 지역 변수 주소를 넘기면 수명이 끝난 메모리를 읽게 된다. 페이지별 정보를 힙에 보관해야 최초 접근까지 유지된다. `file_read_at()`은 페이지마다 저장된 offset을 사용하므로 공유 파일의 현재 위치에 의존하지 않는다. offset을 증가시키지 않으면 여러 페이지가 같은 파일 구간을 읽게 된다. 실제로 그 문제가 발생했다는 로그는 없으며, 해당 변경이 예방하는 문제를 설명한 것이다.

**현재 한계:** 이 콜백에는 파일 락 처리가 없고, 한 번도 접근하지 않은 페이지의 aux를 해제할 `uninit_destroy()`는 아직 비어 있다. 실행 파일 포인터는 복제하거나 소유하지 않고 빌려 쓰므로 프로세스 수명·fork 복사와 함께 관리해야 한다.

### 4.4 최초 스택 페이지는 즉시 claim

`setup_stack()`에서 `USER_STACK - PGSIZE`에 `VM_ANON | VM_MARKER_0` 페이지를 등록하고 즉시 claim한 뒤 0으로 초기화한다. `if_->rsp`는 `USER_STACK`으로 설정한다.

**문제와 이유:** 첫 사용자 명령 실행 전 커널이 인자를 스택에 써야 하므로 초기 스택은 실행 파일 세그먼트와 달리 바로 사용할 수 있어야 한다. marker는 스택 용도 표시에 쓰지만 현재 `vm_stack_growth()`가 비어 있어 자동 확장까지 되는 것은 아니다.

**현재 한계:** claim 실패 시 등록된 SPT 항목을 제거하지 않는다. 0 초기화는 매핑된 사용자 주소 `stack_bottom`을 통해 수행한다.

### 4.5 page fault를 종료보다 먼저 복구 시도

기존 `page_fault()`에는 `not_present || write || user`이면 즉시 `thread_exit()` 하는 코드가 있었다. 이를 주석 처리하고 먼저 `vm_try_handle_fault()`를 호출하도록 했다. 복구할 수 없는 사용자 fault는 `exit_status = -1`로 종료한다.

`vm_try_handle_fault()`는 NULL·커널 주소, present 페이지의 권한 위반, SPT 미등록 주소, 읽기 전용 페이지에 대한 쓰기를 거부한다. 통과한 페이지는 claim한다.

**문제와 이유:** VM에서 not-present fault는 정상적인 첫 접근에도 발생한다. 이를 무조건 종료하면 lazy loading 콜백이 실행될 기회가 없다. 반면 보호 위반을 새 페이지 할당으로 처리하면 읽기 전용 페이지의 보호가 깨지므로 구분해야 한다.

**현재 한계:** SPT에 없는 주소는 항상 거부한다. 스택 확장은 없으며 `f`·`user`도 아직 사용하지 않는다. 사용자 fault 종료가 카운터 증가보다 앞에 있어 출력의 `0 page faults`만으로 fault가 없었다고 판단할 수도 없다.

### 4.6 시스템콜의 사용자 버퍼와 문자열 검사를 VM에 맞게 변경

`is_valid_buffer()`의 인자를 `(void *buffer, size_t size, bool is_need_write)`로 바꾸었다. 버퍼가 걸치는 각 페이지를 SPT에서 찾고, 필요하면 쓰기 권한을 확인하며, PML4에 아직 없으면 `vm_claim_page()`로 적재한다.

- `sys_read()`는 사용자 버퍼에 데이터를 쓰므로 `true`를 전달한다.
- `sys_write()`는 사용자 버퍼에서 데이터를 읽으므로 `false`를 전달한다.
- `is_valid_string()`은 한 바이트를 검증한 뒤 읽어 NUL까지 진행한다.
- 문자열 검사는 fork·exec·create·remove·open에 연결했다. open에서는 검증 전에 `strlen()`을 하던 경로를 제거했다.

**문제와 이유:** PML4에 매핑되지 않았다는 이유만으로 주소를 거부하면 정상적인 lazy 페이지까지 잘못된 포인터가 된다. 첫 페이지만 검사하면 다음 페이지가 잘못된 버퍼를 놓치며, 검증 전에 `strlen()`을 하면 커널이 잘못된 주소를 먼저 읽을 수 있다. 읽기·쓰기 방향도 구분해야 read가 읽기 전용 메모리에 쓰는 상황을 막을 수 있다.

**현재 한계:** `size == 0`일 때 `buffer + size - 1` 계산을 별도로 처리하지 않는다. 끝 주소 정수 넘침 검사도 없다. 끝의 다음 주소인 `buffer + size`로 커널 경계를 검사하므로 정확히 경계 직전에서 끝나는 범위에 대한 검토가 필요하다. SPT 조회가 `#ifdef VM`으로 분리되어 있지 않아 Project 2 빌드 호환성도 확인이 필요하다. 문자열은 NUL까지 한 바이트씩 검사하므로 반복 조회가 많다.

### 4.7 anonymous 타입 초기화의 성공 반환 추가

`anon_initializer()`에서 `page->operations = &anon_ops` 이후 `return true`를 추가했다.

**문제와 이유:** bool 함수의 반환값이 없으면 `uninit_initialize()`가 뒤의 내용 적재 콜백을 호출할지 판단할 수 없다. 타입 초기화 성공을 명시해야 `initializer(...) && init(...)` 흐름이 의도대로 동작한다. 실제 anonymous 스왑 구현을 추가한 것은 아니다.

현재 미커밋 변경의 원문은 부록 A-3에 있다.

## 5. 다른 브랜치의 진행 기록 — 현재 minho에는 미반영

### 5.1 별도로 구현한 SPT·프레임 버전

`3f08918`은 `origin/sunghyun/SPT` 계열의 최초 SPT 구현이며 멤버 이름이 `spt.pages`이다. `b743b34`는 `origin/feat/Anonymous-Page/Ryuuuuu5618`의 SPT·프레임·claim 구현으로 `spt.spt_hash`, `page.can_write`를 사용한다. 현재 minho의 이름은 `spt.hash`, `page.writable`이다.

해결하려는 문제는 3절과 같지만 별도 브랜치의 대안 구현이다. 단순히 코드를 복사하면 필드 이름과 실패 처리의 차이로 충돌할 수 있다. `origin/master`의 `01225d7`은 `b9defdb`와 `d5d497e`를 합친 merge이며, 현재 minho의 frame 커밋까지 포함한 merge는 아니다. 원문은 부록 B-1, B-2에 있다.

### 5.2 anonymous 페이지 생성 — `182f23b`

SPT 버전 위에 `writable`, UNINIT 생성, initializer 선택, anonymous 성공 반환, 프레임 확보와 claim 매핑을 추가했다. 지원하지 않는 타입·SPT 삽입 실패 시 page를 해제한다. `lazy_load_info` 구조체도 준비했다.

**이유:** SPT에 메타데이터를 저장하는 단계에서 실제 지연 초기화 가능한 페이지를 등록하는 단계로 진행하기 위해서다. 이 커밋만으로 파일 적재 콜백까지 완성된 것은 아니다. 원문은 부록 B-3에 있다.

### 5.3 lazy loading과 미초기화 페이지 정리 — `8c6b72f`

파일 적재 콜백, 페이지별 aux, offset 증가, 최초 스택 claim, fault 검사를 추가했다. 현재 미커밋 변경과 목적은 비슷하지만 다음 보완이 들어 있다.

- `lazy_load_segment()`는 현재 스레드가 이미 `file_lock`을 가지고 있는지 확인하고 필요할 때만 획득한다. 자신이 획득했을 때만 해제한다. 같은 락을 다시 잡는 경로를 피하려는 처리다.
- `spt_remove_page()`는 `hash_delete()` 후 page를 해제한다. 해제된 연결 요소가 해시에 남는 문제를 막는다.
- `vm_do_claim_page()`는 매핑뿐 아니라 `swap_in()` 실패도 처리한다. 매핑을 지운 뒤 page/frame 연결과 할당을 되돌린다.
- 초기 스택 claim 실패 시 SPT 등록을 취소한다. 초기화는 `page->frame->kva`로 수행한다.
- `uninit_destroy()`에서 접근되지 않은 페이지의 aux를 반환한다.

**이유:** 정상 적재 경로만 구현하면 실패·종료 시 메모리가 남는다. 이미 적재된 페이지는 loader가 aux를 반환하고, 끝내 접근하지 않은 페이지는 UNINIT destroy가 반환하도록 수명을 나누었다. 다만 전체 SPT 정리 루틴은 다음 커밋에서 연결된다. 원문은 부록 B-4에 있다.

### 5.4 fork 복사와 종료·exec의 수명 관리 — `ff2768f`

`supplemental_page_table_copy()`와 `supplemental_page_table_kill()`을 구현하고 `lazy_load_info`를 `process.h`로 옮겨 loader와 복사 함수가 공유하도록 했다.

**문제와 선택 이유:**

- UNINIT 페이지: aux 구조체를 따로 복사하고 자식의 실행 파일 참조로 바꾼다. 부모의 aux나 파일 참조를 그대로 공유하면 부모의 적재·종료가 자식의 나중 적재를 깨뜨릴 수 있다.
- 이미 초기화된 ANON 페이지: 자식 프레임을 claim하고 `PGSIZE`만큼 복사한다. 현재 구현은 부모 페이지가 RAM에 있다는 전제다. swap-out된 부모까지 지원하는 복사는 아니다.
- FILE 및 미지원 타입: 성공으로 가장하지 않고 실패를 반환한다. mmap 복사까지 완료됐다는 뜻이 아니다.
- fork: 실패 정리도 유효한 SPT를 보도록 초기화를 앞당기고, 자식 aux가 사용할 실행 파일을 SPT 복사 전에 복제한다. error에서 파일을 직접 닫는 코드를 제거해 process cleanup과의 이중 close를 피한다.
- 종료: SPT에서 항목을 삭제한 다음 타입별 destroy와 page free를 수행하고 해시 버킷도 반환한다. iterator는 삭제 후 다시 초기화한다.
- ANON destroy: frame 관리 구조체를 해제한다. 매핑된 실제 물리 페이지는 뒤의 `pml4_destroy()`가 반환하는 기존 종료 순서에 맡긴다. 이 소유권 전제는 일반적인 개별 페이지 제거까지 자동으로 해결해 주지는 않는다.
- exec: cleanup으로 없앤 SPT를 새 프로그램 적재 전에 다시 초기화한다.
- 버퍼 검사: 기존 PML4 기반 판별을 SPT 판별로 변경했다. 전체 범위·권한 검사는 다음 커밋에서 보완한다.

원문은 부록 B-5에 있다.

### 5.5 Project 2 동작·주소 검사 보완 — `9686fa4`

`user_page_ready()`로 페이지 준비를 모으고, VM 빌드에서는 SPT와 claim을 사용하며 비VM 빌드에서는 PML4로 검사한다. `check_user_string()`은 검사한 페이지 안에서만 제한된 길이의 문자열을 읽는다. read/write의 권한 방향을 구분하고, 버퍼 크기 0·끝 주소 넘침·전체 범위를 처리한다. 복구 불가능한 사용자 fault는 -1 종료를 추가했다.

**이유:** VM 전환 후에도 기존 시스템콜이 잘못된 포인터를 안전하게 거부해야 하고, 정상 lazy 주소는 받아들여야 한다. 파일 락을 잡기 전에 페이지를 준비하며, exec은 `strlcpy()` 전, 파일 이름은 `strnlen()` 전에 검사한다. 커밋 제목은 ‘project2 에러 고침’이지만 이 제목만으로 모든 테스트 통과를 단정하지 않는다. 원문은 부록 B-6에 있다.

### 5.6 스택 자동 확장 — `f33ce08`

`thread.user_rsp`를 추가하고 시스템콜 진입 시 `f->rsp`를 저장한다. fault 처리에서 SPT에 없는 주소라도 아래 조건에 맞으면 스택 페이지를 생성·claim·0 초기화한다.

- 주소는 `USER_STACK` 미만이고 그 아래 최대 1MiB 범위여야 한다.
- `user_rsp < 8`을 거부하고, fault 주소가 `user_rsp - 8` 이상이어야 한다.
- 사용자 fault는 `f->rsp`, 커널에서 사용자 버퍼를 만지는 경우는 저장한 `user_rsp`를 기준으로 한다.
- 등록 후 claim에 실패하면 SPT 항목을 제거한다. growth 이후 등록·frame·PML4 매핑을 확인하고 중복 claim을 피한다.
- 시스템콜의 `user_page_ready()`도 같은 fault 처리 함수를 호출해 아직 등록되지 않은 정상 스택 버퍼를 준비한다.

**문제와 이유:** 스택은 실행 중 아래쪽으로 자라므로 처음부터 SPT에 모든 페이지가 있는 것은 아니다. `PUSH`의 rsp 갱신 전 접근도 허용해야 하며, 커널의 현재 스택 포인터를 사용자 스택으로 오해해서는 안 된다. 원문은 부록 B-7에 있다.

## 6. 실제로 남아 있는 테스트 기록과 해석

`pintos/vm/build/results`와 개별 `.result`·`.output` 파일이 남아 있다. `results`의 파일 수정 시각은 2026-10-30 23:32:35 KST이다. **이 문서를 작성하면서 테스트를 새로 실행하지 않았다.** 산출물에는 소스 커밋·미커밋 diff의 지문이 없으므로 현재 코드로 생성됐다고 확정하지 않는다. 아래는 보존된 실행 기록의 관찰이다.

| 테스트 | 저장된 결과·증상 | 현재 코드와의 관련성 |
| --- | --- | --- |
| `fork-once` | 자식이 `exit(81)` 대신 `exit(-1)`, 부모가 -1을 받음 | 현재 SPT copy가 비어 있는 것은 fork 주소 공간 복사에 필요한 미구현 부분이다. 이 로그의 단일 원인으로 확정한 것은 아니다. |
| `exec-once` | 자식 실행 출력 없이 `exec-once: exit(-1)` | 현재 SPT kill이 비어 있어 기존 항목이 남을 수 있다. 새 프로그램 등록 실패 등과의 관계는 추론이며 당시 fault 지점은 로그로 특정하지 못했다. |
| `pt-grow-stack` | begin 후 `exit(-1)`, checksum·end 없음 | 현재 growth가 비어 있고 SPT 미등록 주소를 거부하는 구조와 부합한다. |
| `lazy-anon` | PASS | 일부 지연 적재 경로의 성공 기록이며 swap·fork·전체 VM 완성의 증거는 아니다. |
| `read-boundary`, `write-boundary`, `read-zero`, `write-zero` | pass | 저장된 케이스의 성공이며 현재 범위 계산의 모든 경계값이 안전하다는 뜻은 아니다. |
| mmap·swap·cow 계열 | results에 다수 FAIL | 현재 미구현 범위와 겹친다. 각 실패의 구체 원인은 별도 분석하지 않았다. |

총계와 전체 결과 원문은 부록 C에 수록했다. 기록된 실패와 코드에서 추론한 원인을 구분해 읽어야 한다.

## 7. 현재까지 구현한 것과 남은 것

| 항목 | 현재 minho 작업 트리 | 다른 브랜치의 추가 진행 |
| --- | --- | --- |
| SPT 초기화·조회·삽입 | 구현 | 필드 이름이 다른 구현도 존재 |
| frame 확보·PML4 매핑 | 구현, 메모리 부족 시 PANIC | 여전히 eviction 미구현 |
| UNINIT 등록·anonymous 타입 전환 | 미커밋으로 구현 | 커밋으로 존재 |
| 실행 파일 지연 적재·초기 스택 | 미커밋으로 구현 | 락·실패 정리 보완 |
| fault 검사·복구 우선 처리 | 미커밋으로 구현 | 스택 확장까지 진행 |
| 버퍼 전체 페이지·쓰기 권한 검사 | 미커밋으로 구현, 경계값·비VM 호환 검토 필요 | 크기 0·넘침·비VM 분기 보완 |
| SPT copy·kill | 함수 본문 미구현 | UNINIT/ANON 복사·종료 정리 구현 |
| UNINIT/ANON destroy | 실질 정리 미구현 | aux·frame 관리 정보 반환 |
| 스택 자동 확장 | 미구현 | 1MiB 제한·사용자 rsp 기준 구현 |
| swap·eviction·mmap·COW | 미구현 | 여기서 확인한 후속 커밋들로 완성되지 않음 |

현재 코드에 남은 주요 문제는 다음과 같다. 이번 요청은 기록 작성이므로 소스 수정은 하지 않았다.

1. `spt_remove_page()`가 해시에서 항목을 삭제하지 않고 page만 해제하며 void 함수에서 `return true`를 한다.
2. SPT copy·kill과 타입별 destroy가 비어 있어 fork·exec·종료의 수명 관리가 완성되지 않았다.
3. 페이지 생성 중 실패 시 page 누수, claim의 `swap_in()` 실패 시 매핑·프레임 정리 누락이 있다.
4. 프레임 확보 실패는 PANIC이며 실제 스왑 함수도 반환 없는 골격이다.
5. 버퍼의 크기 0·주소 넘침·경계값과 비VM 빌드를 검증해야 한다.
6. 현재 코드의 일반적인 새 anonymous 페이지는 initializer만으로 0을 채우지 않는다. 초기 스택은 setup에서, 실행 파일은 lazy loader에서 초기화하므로 다른 anonymous 생성 경로가 생기면 초기화 책임도 정해야 한다.

## 8. 변경 원문 부록 안내

부록 A는 현재 브랜치에 실제 있는 두 커밋과 미커밋 변경이다. 부록 B는 다른 브랜치의 별도 진행 기록이다. 부록 C는 기존 테스트 산출물이다. 모든 diff는 문서 작성 시 저장소에서 직접 추출했으며 설명을 위해 코드를 고쳐 적지 않았다.

## 부록 A-1. d5d497e — SPT 관리 함수 구현

```diff
commit d5d497eff154ec9fa6834568accf6414d3ecb7d2
Author: Minho Lee <oyatmin1031@gmail.com>
Date:   Mon Oct 26 17:26:34 2026 +0900

    feat: spt 관리 함수 구현

diff --git a/pintos/include/vm/vm.h b/pintos/include/vm/vm.h
index e061b73..c61db1b 100644
--- a/pintos/include/vm/vm.h
+++ b/pintos/include/vm/vm.h
@@ -2,6 +2,7 @@
 #define VM_VM_H
 #include <stdbool.h>
 #include "threads/palloc.h"
+#include "lib/kernel/hash.h"
 
 enum vm_type {
 	/* page not initialized */
@@ -46,6 +47,7 @@ struct page {
 	struct frame *frame;   /* Back reference for frame */
 
 	/* Your implementation */
+	struct hash_elem hash_elem;
 
 	/* Per-type data are binded into the union.
 	 * Each function automatically detects the current union */
@@ -85,6 +87,7 @@ struct page_operations {
  * We don't want to force you to obey any specific design for this struct.
  * All designs up to you for this. */
 struct supplemental_page_table {
+	struct hash hash;
 };
 
 #include "threads/thread.h"
diff --git a/pintos/vm/vm.c b/pintos/vm/vm.c
index 94d58f9..18fa80f 100644
--- a/pintos/vm/vm.c
+++ b/pintos/vm/vm.c
@@ -3,6 +3,7 @@
 #include "threads/malloc.h"
 #include "vm/vm.h"
 #include "vm/inspect.h"
+#include "threads/mmu.h"
 
 /* Initializes the virtual memory subsystem by invoking each subsystem's
  * intialize codes. */
@@ -32,6 +33,23 @@ page_get_type (struct page *page) {
 	}
 }
 
+/* Returns a hash value for page p. */
+uint64_t 
+page_hash (const struct hash_elem *e, void *aux UNUSED) {
+  const struct page *p = hash_entry (e, struct page, hash_elem);
+  return hash_bytes (&p->va, sizeof p->va);
+}
+
+/* Returns true if page a precedes page b. */
+bool
+page_less (const struct hash_elem *a_,
+           const struct hash_elem *b_, void *aux UNUSED) {
+  const struct page *a = hash_entry (a_, struct page, hash_elem);
+  const struct page *b = hash_entry (b_, struct page, hash_elem);
+
+  return a->va < b->va;
+}
+
 /* Helpers */
 static struct frame *vm_get_victim (void);
 static bool vm_do_claim_page (struct page *page);
@@ -64,7 +82,14 @@ err:
 struct page *
 spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
 	struct page *page = NULL;
-	/* TODO: Fill this function. */
+	
+	struct page p;
+	struct hash_elem *e;
+
+	p.va = pg_round_down(va);
+	e = hash_find(&spt->hash, &p.hash_elem);
+
+	page = (e != NULL) ? hash_entry(e, struct page, hash_elem) : NULL;
 
 	return page;
 }
@@ -73,8 +98,8 @@ spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
 bool
 spt_insert_page (struct supplemental_page_table *spt UNUSED,
 		struct page *page UNUSED) {
-	int succ = false;
-	/* TODO: Fill this function. */
+	/* 주어진 보조 페이지 테이블에 struct page를 삽입. */
+	int succ = hash_insert(&spt->hash, &page->hash_elem) == NULL ? true : false;
 
 	return succ;
 }
@@ -174,6 +199,10 @@ vm_do_claim_page (struct page *page) {
 /* Initialize new supplemental page table */
 void
 supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
+	/* TODO: hash_init 예외처리 */
+	if (!hash_init(&spt->hash, page_hash, page_less, NULL)) {
+		PANIC("hash init failed");
+	}
 }
 
 /* Copy supplemental page table from src to dst */

```

## 부록 A-2. 9a409c1 — frame 관리 함수 구현

```diff
commit 9a409c13d4dc7a27ab0ef03afced8f973b7cb2d8
Author: Minho Lee <oyatmin1031@gmail.com>
Date:   Mon Oct 26 17:28:24 2026 +0900

    feat: frame 관리 함수 구현

diff --git a/pintos/include/vm/vm.h b/pintos/include/vm/vm.h
index c61db1b..61cb20c 100644
--- a/pintos/include/vm/vm.h
+++ b/pintos/include/vm/vm.h
@@ -48,6 +48,7 @@ struct page {
 
 	/* Your implementation */
 	struct hash_elem hash_elem;
+	bool writable;
 
 	/* Per-type data are binded into the union.
 	 * Each function automatically detects the current union */
diff --git a/pintos/vm/vm.c b/pintos/vm/vm.c
index 18fa80f..e38097e 100644
--- a/pintos/vm/vm.c
+++ b/pintos/vm/vm.c
@@ -136,10 +136,18 @@ vm_evict_frame (void) {
 static struct frame *
 vm_get_frame (void) {
 	struct frame *frame = NULL;
-	/* TODO: Fill this function. */
 
+	/* TODO: 페이지 할당 실패 시 스왑 아웃 처리 */
+	frame = malloc(sizeof(struct frame));
+	if (frame == NULL) {
+		return NULL;
+	}
 	ASSERT (frame != NULL);
-	ASSERT (frame->page == NULL);
+
+	if ((frame->kva = palloc_get_page(PAL_USER)) == NULL) {
+		PANIC("todo");
+	}
+	frame->page = NULL;
 	return frame;
 }
 
@@ -178,6 +186,9 @@ bool
 vm_claim_page (void *va UNUSED) {
 	struct page *page = NULL;
 	/* TODO: Fill this function */
+	if((page = spt_find_page(&thread_current()->spt, va)) == NULL) {
+		return false;
+	}
 
 	return vm_do_claim_page (page);
 }
@@ -186,13 +197,26 @@ vm_claim_page (void *va UNUSED) {
 static bool
 vm_do_claim_page (struct page *page) {
 	struct frame *frame = vm_get_frame ();
+	if (frame == NULL) {
+		return false;
+	}
 
 	/* Set links */
 	frame->page = page;
 	page->frame = frame;
 
 	/* TODO: Insert page table entry to map page's VA to frame's PA. */
+	/* 현재 스레드의 페이지 테이블을 물리 프레임으로 연결  */
+	struct thread *curr = thread_current();
+	if (!pml4_set_page(curr->pml4, page->va, frame->kva, page->writable)) {
+		palloc_free_page(frame->kva);
+		free(frame);
+		page->frame = NULL;
+
+		return false;
+	}
 
+	/* TODO: swap_in 실패시 처리? */
 	return swap_in (page, frame->kva);
 }
 

```

## 부록 A-3. 현재 미커밋 변경 전체 — HEAD 대비

```diff
diff --git a/.vscode/settings.json b/.vscode/settings.json
index f6fdb82..be7e4a0 100644
--- a/.vscode/settings.json
+++ b/.vscode/settings.json
@@ -1,10 +1,11 @@
 {
     "pintos.projects": ["threads", "userprog", "vm", "ALL"],
-    "pintos.project": 1,
+    "pintos.project": 2,
     "pintos.testShortcutEnabled": true,
     "C_Cpp.default.defines": [
         "USERPROG",
-        "FILESYS"
+        "FILESYS",
+        "VM"
     ],
     "C_Cpp.default.includePath": [
         "${workspaceFolder}/pintos/include"
diff --git a/pintos/userprog/exception.c b/pintos/userprog/exception.c
index 24a5207..8b03079 100644
--- a/pintos/userprog/exception.c
+++ b/pintos/userprog/exception.c
@@ -140,9 +140,9 @@ page_fault (struct intr_frame *f) {
 	write = (f->error_code & PF_W) != 0;
 	user = (f->error_code & PF_U) != 0;
 
-	if( not_present || write || user ) {
-		thread_exit();
-	}
+	// if( not_present || write || user ) {
+	// 	thread_exit();
+	// }
 
 #ifdef VM
 	/* For project 3 and later. */
@@ -150,6 +150,11 @@ page_fault (struct intr_frame *f) {
 		return;
 #endif
 
+	if (user) {
+		thread_current()->exit_status = -1;
+		thread_exit();
+	}
+
 	/* Count page faults. */
 	page_fault_cnt++;
 
diff --git a/pintos/userprog/process.c b/pintos/userprog/process.c
index 58c531b..51979e9 100644
--- a/pintos/userprog/process.c
+++ b/pintos/userprog/process.c
@@ -893,11 +893,36 @@ install_page (void *upage, void *kpage, bool writable) {
  * If you want to implement the function for only project 2, implement it on the
  * upper block. */
 
+ /* 적재할 파일의 정보를 담은 구조체 */
+struct page_args {
+	struct file *file;		/* 파일 */
+	off_t offset;			/* 오프셋 */
+	size_t read_bytes;		/* 파일에서 읽을 크기 */
+	size_t zero_bytes;		/* 파일을 읽지 않고 0으로 채울 크기 */
+};
+
 static bool
 lazy_load_segment (struct page *page, void *aux) {
+	struct page_args *args = (struct page_args *)aux;
+	
+	/* Get a page of memory. */
+	uint8_t *kpage = page->frame->kva;
+
+	/* 파일에서 kpage 위치부터 read_bytes 만큼 읽기*/
+	off_t read_size = file_read_at(args->file, kpage, args->read_bytes, args->offset);
+	if (read_size != args->read_bytes) {
+		free(args);
+		return false;
+	}
+	memset (kpage + args->read_bytes, 0, args->zero_bytes);
+	free(args);
+	return true;
+
 	/* TODO: Load the segment from the file */
+	
 	/* TODO: This called when the first page fault occurs on address VA. */
 	/* TODO: VA is available when calling this function. */
+
 }
 
 /* Loads a segment starting at offset OFS in FILE at address
@@ -929,15 +954,26 @@ load_segment (struct file *file, off_t ofs, uint8_t *upage,
 		size_t page_zero_bytes = PGSIZE - page_read_bytes;
 
 		/* TODO: Set up aux to pass information to the lazy_load_segment. */
-		void *aux = NULL;
+		struct page_args *args = malloc(sizeof(struct page_args));
+		if (args == NULL) {
+			return false;
+		}
+		args->file = file;
+		args->offset = ofs;
+		args->read_bytes = page_read_bytes;
+		args->zero_bytes = page_zero_bytes;
+		void *aux = (void *)args;
 		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
-					writable, lazy_load_segment, aux))
+					writable, lazy_load_segment, aux)) {
+			free(args);
 			return false;
+		}
 
 		/* Advance. */
 		read_bytes -= page_read_bytes;
 		zero_bytes -= page_zero_bytes;
 		upage += PGSIZE;
+		ofs += page_read_bytes;
 	}
 	return true;
 }
@@ -945,15 +981,22 @@ load_segment (struct file *file, off_t ofs, uint8_t *upage,
 /* Create a PAGE of stack at the USER_STACK. Return true on success. */
 static bool
 setup_stack (struct intr_frame *if_) {
-	bool success = false;
 	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);
 
-	/* TODO: Map the stack on stack_bottom and claim the page immediately.
-	 * TODO: If success, set the rsp accordingly.
-	 * TODO: You should mark the page is stack. */
-	/* TODO: Your code goes here */
+	/* 첫 번째 스택 페이지 즉시 할당 */
+	if (!vm_alloc_page(VM_ANON|VM_MARKER_0, stack_bottom, true)) {
+		return false;
+	}
 
-	return success;
+	if (!vm_claim_page(stack_bottom)) {
+		return false;
+	}
+
+	/* 페이지 0으로 초기화 */
+	memset(stack_bottom, 0, PGSIZE);
+	if_->rsp = USER_STACK;
+
+	return true;
 }
 #endif /* VM */
 
diff --git a/pintos/userprog/syscall.c b/pintos/userprog/syscall.c
index 2f77d35..9396694 100644
--- a/pintos/userprog/syscall.c
+++ b/pintos/userprog/syscall.c
@@ -39,7 +39,8 @@ void syscall_handler (struct intr_frame *);
 static bool is_invaild_filename(char * filename);
 static struct fd_info * seek_fd_info (struct thread *curr, int fd);
 static struct fd_info * dup_fd_info(struct fd_info *oldfd_info, int newfd);
-static bool is_valid_buffer(char *buffer, int size, void* arg);
+static bool is_valid_buffer(void *buffer, size_t size, bool is_need_write);
+static bool is_valid_string(char *fname);
 
 static void sys_halt (struct intr_frame *f);
 static void sys_exit (struct intr_frame *f);
@@ -143,7 +144,12 @@ sys_exit (struct intr_frame *f) {
 static void 
 sys_fork (struct intr_frame *f) {
 	char *fname = (void *)f->R.rdi;	
-	
+
+	if (!is_valid_string(fname)) {
+		thread_current()->exit_status = -1;
+		thread_exit();
+	}
+
 	if(fname == NULL || is_kernel_vaddr(fname)) {
 		thread_current()->exit_status = -1;
 		thread_exit();
@@ -166,7 +172,12 @@ sys_exec (struct intr_frame *f) {
 	if (buffer == NULL){
 		f->R.rax = -1;
 		return;
-	}		
+	}
+	
+	if (!is_valid_string(fname)) {
+		thread_current()->exit_status = -1;
+		thread_exit();
+	}
 
 	/* 널문자까지 포함해서 page size여야함 */
 	if ((strlcpy(buffer, (const char *)fname, PAGE_SIZE)) >= PAGE_SIZE){
@@ -195,6 +206,11 @@ static void
 sys_create (struct intr_frame *f) {
 	char *fname = (char *)f->R.rdi;
 	unsigned int initsize = f->R.rsi;
+
+	if (!is_valid_string(fname)) {
+		thread_current()->exit_status = -1;
+		thread_exit();
+	}
 	
 	if (fname == '\0'  || is_kernel_vaddr(fname)) {
 		thread_current()->exit_status = -1;
@@ -215,6 +231,11 @@ static void
 sys_remove (struct intr_frame *f) {
 	char *fname = (char *)f->R.rdi;
 
+	if (!is_valid_string(fname)) {
+		thread_current()->exit_status = -1;
+		thread_exit();
+	}
+
 	if(fname == NULL || is_kernel_vaddr(fname)) {
 		thread_current()->exit_status = -1;
 		thread_exit();
@@ -236,12 +257,12 @@ sys_open (struct intr_frame *f) {
 
 	struct file *file;
 
-	if(fname == NULL || is_kernel_vaddr(fname)) {
+	if(fname == NULL || is_kernel_vaddr(fname) || !is_valid_string(fname)) {
 		thread_current()->exit_status = -1;
 		thread_exit();
 	}
 	
-	if (!is_valid_buffer(fname, strlen(fname), '\0') || is_invaild_filename(fname)) {
+	if (is_invaild_filename(fname)) {
 		f->R.rax = -1;       
 		return;
 	}
@@ -298,7 +319,7 @@ sys_read (struct intr_frame *f) {
 	char *buffer = (char *)f->R.rsi;
 	unsigned size = f->R.rdx;
 
-	if(!is_valid_buffer(buffer, size, '\0')){
+	if(!is_valid_buffer(buffer, size, true)){
 		thread_current()->exit_status = -1;
 		thread_exit();
 	}
@@ -382,7 +403,7 @@ sys_write (struct intr_frame *f) {
 	int fd = f->R.rdi;
 	uint64_t size = f->R.rdx;
 
-	if (!is_valid_buffer(buffer, size, NULL)) {
+	if (!is_valid_buffer(buffer, size, false)) {
 		thread_current()->exit_status = -1;
 		thread_exit(); // 접근하면 안 되는 포인터를 주면 프로세스 종료 
 		
@@ -533,14 +554,46 @@ static bool is_invaild_filename(char * filename){
 	return !((size > 0) && (size <= (NAME_MAX)));
 }
 
-static bool is_valid_buffer(char *buffer, int size, void *arg) {
+static bool is_valid_buffer(void *buffer, size_t size, bool is_need_write) {
 	struct thread *curr = thread_current();
-	if (buffer == arg || is_kernel_vaddr(buffer + size) || pml4_get_page(curr->pml4, buffer) == NULL) {
+	if (buffer == NULL || is_kernel_vaddr(buffer + size)) {
 		return false;
 	}
+
+	int64_t start = pg_round_down(buffer);
+	int64_t end = pg_round_down(buffer + size - 1);
+
+	for (int64_t addr = start; addr <= end; addr += PGSIZE) {
+		struct page *page = spt_find_page(&curr->spt, addr);
+
+		if (page == NULL) {
+			return false;
+		}
+		if (is_need_write && !page->writable) {
+			return false;
+		}
+		/* 유효하지만 아직 적재되지 않은 경우 */
+		if (pml4_get_page(curr->pml4, addr) == NULL && !vm_claim_page(addr)) {
+			return false;
+		}
+	}
+
 	return true;
 }
 
+static bool
+is_valid_string(char *fname) {
+	while (true) {
+		if (!is_valid_buffer(fname, 1, false))
+			return false;
+
+		if (*fname == '\0')
+			return true;
+
+		fname++;
+	}
+}
+
 static struct fd_info * seek_fd_info (struct thread *curr, int fd) {
 	struct list_elem *ptr;
 	struct list *list = &curr->file_list;
diff --git a/pintos/vm/anon.c b/pintos/vm/anon.c
index 5c93e0c..415d43e 100644
--- a/pintos/vm/anon.c
+++ b/pintos/vm/anon.c
@@ -31,6 +31,7 @@ anon_initializer (struct page *page, enum vm_type type, void *kva) {
 	page->operations = &anon_ops;
 
 	struct anon_page *anon_page = &page->anon;
+	return true;
 }
 
 /* Swap in the page by read contents from the swap disk. */
diff --git a/pintos/vm/vm.c b/pintos/vm/vm.c
index e38097e..476cc0f 100644
--- a/pintos/vm/vm.c
+++ b/pintos/vm/vm.c
@@ -3,6 +3,8 @@
 #include "threads/malloc.h"
 #include "vm/vm.h"
 #include "vm/inspect.h"
+#include "vm/uninit.h"
+#include "vm/file.h"
 #include "threads/mmu.h"
 
 /* Initializes the virtual memory subsystem by invoking each subsystem's
@@ -62,17 +64,34 @@ bool
 vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
 		vm_initializer *init, void *aux) {
 
-	ASSERT (VM_TYPE(type) != VM_UNINIT)
+	ASSERT (VM_TYPE(type) != VM_UNINIT);
 
 	struct supplemental_page_table *spt = &thread_current ()->spt;
 
 	/* Check wheter the upage is already occupied or not. */
 	if (spt_find_page (spt, upage) == NULL) {
-		/* TODO: Create the page, fetch the initialier according to the VM type,
-		 * TODO: and then create "uninit" page struct by calling uninit_new. You
-		 * TODO: should modify the field after calling the uninit_new. */
-
-		/* TODO: Insert the page into the spt. */
+		struct page *page = malloc(sizeof(struct page));
+		if (page == NULL) {
+			free(page);
+			return false;
+		}
+
+		switch (VM_TYPE(type)) {
+		case VM_ANON:
+			uninit_new(page, upage, init, type, aux, anon_initializer);
+			break;
+		case VM_FILE:
+			uninit_new(page, upage, init, type, aux, file_backed_initializer);
+			break;
+		default:
+			goto err;
+		}
+		page->writable = writable;
+
+		if (!spt_insert_page(spt, page)) {
+			goto err;
+		}
+		return true;
 	}
 err:
 	return false;
@@ -169,6 +188,27 @@ vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
 	struct page *page = NULL;
 	/* TODO: Validate the fault */
 	/* TODO: Your code goes here */
+	
+	/* 사용자 메모리 주소 여부 */
+	if (addr == NULL || is_kernel_vaddr(addr)) {
+		return false;
+	}
+
+	/* 페이지 유무 */
+	if (!not_present) {
+		return false;
+	}
+
+	/* 유효한 페이지 폴트인지 검사 */
+	page = spt_find_page(spt, addr);
+	if (page == NULL) {
+		return false;
+	}
+
+	/* 접근한 쓰기 권한과 페이지의 쓰기 권한 검사 */
+	if (write && !page->writable) {
+		return false;
+	}
 
 	return vm_do_claim_page (page);
 }

```

## 부록 B-1. 3f08918 — 별도 SPT 구현 (현재 minho 미반영)

```diff
commit 3f089182b561781a4fecacbcd4ad15361fa73ea6
Author: sungtaeng488 <sungtaeng4884835@gmail.com>
Date:   Mon Oct 26 19:17:27 2026 +0900

    Implement supplemental page table initialization, lookup, and insertion

diff --git a/pintos/include/vm/vm.h b/pintos/include/vm/vm.h
index e061b73..c8fc296 100644
--- a/pintos/include/vm/vm.h
+++ b/pintos/include/vm/vm.h
@@ -2,6 +2,7 @@
 #define VM_VM_H
 #include <stdbool.h>
 #include "threads/palloc.h"
+#include "lib/kernel/hash.h"
 
 enum vm_type {
 	/* page not initialized */
@@ -45,6 +46,8 @@ struct page {
 	void *va;              /* Address in terms of user space */
 	struct frame *frame;   /* Back reference for frame */
 
+	struct hash_elem hash_elem;  /* SPT 해시 테이블에 연결할 요소 */
+
 	/* Your implementation */
 
 	/* Per-type data are binded into the union.
@@ -85,6 +88,7 @@ struct page_operations {
  * We don't want to force you to obey any specific design for this struct.
  * All designs up to you for this. */
 struct supplemental_page_table {
+	struct hash pages;
 };
 
 #include "threads/thread.h"
diff --git a/pintos/vm/vm.c b/pintos/vm/vm.c
index 94d58f9..9fd8b24 100644
--- a/pintos/vm/vm.c
+++ b/pintos/vm/vm.c
@@ -3,6 +3,26 @@
 #include "threads/malloc.h"
 #include "vm/vm.h"
 #include "vm/inspect.h"
+#include "threads/vaddr.h"
+
+/* Hash and compare SPT entries by virtual page address. */
+//- page_hash(): 가상 주소를 이용해 해시값을 계산하는 함수
+//page_less(): 두 페이지의 가상 주소를 비교하는 함수
+/* Returns a hash value for page p. */
+uint64_t
+page_hash (const struct hash_elem *p_, void *aux UNUSED) {
+  const struct page *p = hash_entry (p_, struct page, hash_elem);
+  return hash_bytes (&p->va, sizeof p->va);
+}
+/* Returns true if page a precedes page b. */
+bool
+page_less (const struct hash_elem *a_,
+           const struct hash_elem *b_, void *aux UNUSED) {
+  const struct page *a = hash_entry (a_, struct page, hash_elem);
+  const struct page *b = hash_entry (b_, struct page, hash_elem);
+
+  return a->va < b->va;
+}
 
 /* Initializes the virtual memory subsystem by invoking each subsystem's
  * intialize codes. */
@@ -62,21 +82,34 @@ err:
 
 /* Find VA from spt and return page. On error, return NULL. */
 struct page *
-spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
-	struct page *page = NULL;
-	/* TODO: Fill this function. */
+spt_find_page(struct supplemental_page_table *spt, void *va) {
 
-	return page;
+    /* 1. 검색할 페이지 시작 주소 구하기 */
+    struct page key = {
+        .va = pg_round_down(va),
+    };
+
+    /* 2. SPT의 Hash Table에서 검색 */
+    struct hash_elem *found = hash_find(&spt->pages, &key.hash_elem);
+
+    /* 3. 검색 실패 */
+    if (found == NULL)
+        return NULL;
+
+    /* 4. 찾은 hash_elem에서 struct page 주소 구하기 */
+    return hash_entry(found, struct page, hash_elem);
 }
 
 /* Insert PAGE into spt with validation. */
 bool
 spt_insert_page (struct supplemental_page_table *spt UNUSED,
 		struct page *page UNUSED) {
-	int succ = false;
 	/* TODO: Fill this function. */
 
-	return succ;
+
+	 return hash_insert (&spt->pages, &page->hash_elem) == NULL;
+	//NULL: 새 페이지가 삽입됨 -> true
+	//기존 요소의 주소: 같은 va가 이미 있어서 삽입되지 않음 -> false
 }
 
 void
@@ -174,6 +207,7 @@ vm_do_claim_page (struct page *page) {
 /* Initialize new supplemental page table */
 void
 supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
+	hash_init (&spt->pages, page_hash, page_less, NULL);
 }
 
 /* Copy supplemental page table from src to dst */

```

## 부록 B-2. b743b34 — 별도 SPT·frame·claim 구현 (현재 minho 미반영)

```diff
commit b743b348ca697e0a92c36ee28eff9ca8045bf244
Author: 류선우 <johnwoo2648@gmail.com>
Date:   Mon Oct 26 19:21:45 2026 +0900

    feat(vm): SPT 해시 테이블, frame 할당, page claim 구현

diff --git a/pintos/include/vm/vm.h b/pintos/include/vm/vm.h
index e061b73..64de1b9 100644
--- a/pintos/include/vm/vm.h
+++ b/pintos/include/vm/vm.h
@@ -46,6 +46,9 @@ struct page {
 	struct frame *frame;   /* Back reference for frame */
 
 	/* Your implementation */
+	struct hash_elem hash_elem;
+
+	bool can_write;
 
 	/* Per-type data are binded into the union.
 	 * Each function automatically detects the current union */
@@ -85,6 +88,8 @@ struct page_operations {
  * We don't want to force you to obey any specific design for this struct.
  * All designs up to you for this. */
 struct supplemental_page_table {
+	struct hash spt_hash;
+	// 페이지 정보
 };
 
 #include "threads/thread.h"
diff --git a/pintos/vm/vm.c b/pintos/vm/vm.c
index 94d58f9..3b7d6be 100644
--- a/pintos/vm/vm.c
+++ b/pintos/vm/vm.c
@@ -3,6 +3,9 @@
 #include "threads/malloc.h"
 #include "vm/vm.h"
 #include "vm/inspect.h"
+#include "threads/mmu.h"
+
+#include "lib/kernel/hash.h"
 
 /* Initializes the virtual memory subsystem by invoking each subsystem's
  * intialize codes. */
@@ -62,20 +65,37 @@ err:
 
 /* Find VA from spt and return page. On error, return NULL. */
 struct page *
-spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
+spt_find_page (struct supplemental_page_table *spt, void *va) {
 	struct page *page = NULL;
 	/* TODO: Fill this function. */
 
+	struct page tmp;
+	tmp.va = pg_round_down(va);
+
+	struct hash_elem *find_elem = hash_find(&spt->spt_hash, &tmp.hash_elem);
+
+	if (find_elem == NULL) {
+		return NULL;
+	}
+
+	page = hash_entry(find_elem, struct page, hash_elem);
+
 	return page;
 }
 
 /* Insert PAGE into spt with validation. */
 bool
-spt_insert_page (struct supplemental_page_table *spt UNUSED,
-		struct page *page UNUSED) {
+spt_insert_page (struct supplemental_page_table *spt,
+		struct page *page) {
 	int succ = false;
 	/* TODO: Fill this function. */
 
+	if (hash_find(&spt->spt_hash, &page->hash_elem) != NULL) {
+		return succ;
+	}
+
+	succ = (!hash_insert(&spt->spt_hash, &page->hash_elem)) ? true : false;
+
 	return succ;
 }
 
@@ -110,9 +130,22 @@ vm_evict_frame (void) {
  * space.*/
 static struct frame *
 vm_get_frame (void) {
-	struct frame *frame = NULL;
+	struct frame *frame;
 	/* TODO: Fill this function. */
+	
+	frame = malloc(sizeof(struct frame));
 
+	if (frame == NULL) {
+		PANIC("말록 실패");
+	}
+	
+	frame->kva = palloc_get_page(PAL_USER);
+	if (frame->kva == NULL) {
+		// 스왑 아웃
+		PANIC("todo");
+	}
+	frame->page = NULL;
+	
 	ASSERT (frame != NULL);
 	ASSERT (frame->page == NULL);
 	return frame;
@@ -150,10 +183,14 @@ vm_dealloc_page (struct page *page) {
 
 /* Claim the page that allocate on VA. */
 bool
-vm_claim_page (void *va UNUSED) {
-	struct page *page = NULL;
+vm_claim_page (void *va) {
 	/* TODO: Fill this function */
+	struct page *page = spt_find_page(&thread_current()->spt, va);
 
+	if (page == NULL) {
+		return false;
+	}
+	
 	return vm_do_claim_page (page);
 }
 
@@ -167,13 +204,37 @@ vm_do_claim_page (struct page *page) {
 	page->frame = frame;
 
 	/* TODO: Insert page table entry to map page's VA to frame's PA. */
+	/* TODO: 페이지의 가상 주소(VA)를 프레임의 물리 주소(PA)에 매핑하도록, 페이지 테이블 엔트리를 추가하세요. */
+	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->can_write)) {
+		return false;
+	}
 
 	return swap_in (page, frame->kva);
 }
 
+static uint64_t
+page_hash (const struct hash_elem *e, void *aux UNUSED) {
+	struct page *p = hash_entry(e, struct page, hash_elem);
+
+	return hash_bytes (&p->va, sizeof(p->va));
+}
+
+static bool
+page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
+	// uint64_t a_va = (uint64_t)hash_entry(a, struct page, hash_elem)->va;
+	// uint64_t b_va = (uint64_t)hash_entry(b, struct page, hash_elem)->va;
+
+	// return a_va < b_va;
+	const struct page *a = hash_entry (a_, struct page, hash_elem);
+	const struct page *b = hash_entry (b_, struct page, hash_elem);
+
+	return a->va < b->va;
+}
+
 /* Initialize new supplemental page table */
 void
-supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
+supplemental_page_table_init (struct supplemental_page_table *spt) {	
+	hash_init (&spt->spt_hash, page_hash, page_less, NULL);
 }
 
 /* Copy supplemental page table from src to dst */
@@ -187,4 +248,4 @@ void
 supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
 	/* TODO: Destroy all the supplemental_page_table hold by thread and
 	 * TODO: writeback all the modified contents to the storage. */
-}
+}
\ No newline at end of file

```

## 부록 B-3. 182f23b — anonymous 페이지 생성 (현재 minho 미반영)

```diff
commit 182f23bb16a44de8021381b28c34ea803ef8ace9
Author: sungtaeng488 <sungtaeng4884835@gmail.com>
Date:   Tue Oct 27 19:03:54 2026 +0900

    add:익명 페이지

diff --git a/pintos/include/vm/vm.h b/pintos/include/vm/vm.h
index c8fc296..5a6d87a 100644
--- a/pintos/include/vm/vm.h
+++ b/pintos/include/vm/vm.h
@@ -3,6 +3,7 @@
 #include <stdbool.h>
 #include "threads/palloc.h"
 #include "lib/kernel/hash.h"
+#include "threads/mmu.h"
 
 enum vm_type {
 	/* page not initialized */
@@ -49,6 +50,7 @@ struct page {
 	struct hash_elem hash_elem;  /* SPT 해시 테이블에 연결할 요소 */
 
 	/* Your implementation */
+	bool writable; /* 읽고 쓰기한거?*/
 
 	/* Per-type data are binded into the union.
 	 * Each function automatically detects the current union */
@@ -76,7 +78,7 @@ struct page_operations {
 	bool (*swap_in) (struct page *, void *);
 	bool (*swap_out) (struct page *);
 	void (*destroy) (struct page *);
-	enum vm_type type;
+	enum vm_type type; 
 };
 
 #define swap_in(page, v) (page)->operations->swap_in ((page), v)
diff --git a/pintos/userprog/process.c b/pintos/userprog/process.c
index 58c531b..7e6a1a1 100644
--- a/pintos/userprog/process.c
+++ b/pintos/userprog/process.c
@@ -43,6 +43,13 @@ struct fork_args {
 	bool success;
 };
 
+struct lazy_load_info {
+    struct file *file;
+    off_t offset;
+    size_t read_bytes;
+    size_t zero_bytes;
+};
+
 
 /* General process initializer for initd and other process. */
 static void
@@ -845,6 +852,7 @@ load_segment (struct file *file, off_t ofs, uint8_t *upage,
 			return false;
 		}
 
+
 		/* Advance. */
 		read_bytes -= page_read_bytes;
 		zero_bytes -= page_zero_bytes;
diff --git a/pintos/vm/anon.c b/pintos/vm/anon.c
index 5c93e0c..d130b37 100644
--- a/pintos/vm/anon.c
+++ b/pintos/vm/anon.c
@@ -21,6 +21,7 @@ static const struct page_operations anon_ops = {
 void
 vm_anon_init (void) {
 	/* TODO: Set up the swap_disk. */
+
 	swap_disk = NULL;
 }
 
@@ -28,9 +29,9 @@ vm_anon_init (void) {
 bool
 anon_initializer (struct page *page, enum vm_type type, void *kva) {
 	/* Set up the handler */
+	//이 페이지를 VM_ANON으로 관리하도록 전환
 	page->operations = &anon_ops;
-
-	struct anon_page *anon_page = &page->anon;
+	return true;
 }
 
 /* Swap in the page by read contents from the swap disk. */
diff --git a/pintos/vm/vm.c b/pintos/vm/vm.c
index 9fd8b24..c2c0e0d 100644
--- a/pintos/vm/vm.c
+++ b/pintos/vm/vm.c
@@ -64,17 +64,49 @@ bool
 vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
 		vm_initializer *init, void *aux) {
 
-	ASSERT (VM_TYPE(type) != VM_UNINIT)
+	ASSERT (VM_TYPE(type) != VM_UNINIT);
 
 	struct supplemental_page_table *spt = &thread_current ()->spt;
 
 	/* Check wheter the upage is already occupied or not. */
+	//가상 페이지 중복 검사
 	if (spt_find_page (spt, upage) == NULL) {
 		/* TODO: Create the page, fetch the initialier according to the VM type,
 		 * TODO: and then create "uninit" page struct by calling uninit_new. You
 		 * TODO: should modify the field after calling the uninit_new. */
+		//페이지 할당 해주기
+		struct page *page = malloc(sizeof(struct page));
+		//예외 검사
+		if(page == NULL){
+			goto err;
+		}
+		//
+		bool (*initializer) (struct page *, enum vm_type, void *) = NULL;
+
+		switch (VM_TYPE(type)) {
+    	case VM_ANON:
+        	initializer = anon_initializer;
+        	break;
+
+    	case VM_FILE:
+        	initializer = file_backed_initializer;
+        	break;
+
+    	default:
+        free(page);
+        goto err;
+}
+	uninit_new(page, upage, init, type, aux, initializer);
+	page->writable = writable;
+	
+
 
 		/* TODO: Insert the page into the spt. */
+	if(spt_insert_page(spt,page) == false){
+		free(page);
+		goto err;
+		}
+		return true;
 	}
 err:
 	return false;
@@ -145,6 +177,20 @@ static struct frame *
 vm_get_frame (void) {
 	struct frame *frame = NULL;
 	/* TODO: Fill this function. */
+	//프레임 관리할 구조체 할당 해주기
+	frame = malloc(sizeof(struct frame));
+	if(frame == NULL){
+		PANIC("frame allocation fail");
+	}
+	//진짜 프레임 할당 RAM에서 비어 있는 물리 페이지를 찾아서 할당
+	frame->kva = palloc_get_page(PAL_USER);
+	if(frame->kva == NULL){
+		free(frame);
+		PANIC("todo:EVICTION");
+	}
+
+    frame->page = NULL;
+
 
 	ASSERT (frame != NULL);
 	ASSERT (frame->page == NULL);
@@ -183,11 +229,16 @@ vm_dealloc_page (struct page *page) {
 
 /* Claim the page that allocate on VA. */
 bool
-vm_claim_page (void *va UNUSED) {
-	struct page *page = NULL;
-	/* TODO: Fill this function */
+vm_claim_page(void *va) {
+    /* 1. 현재 프로세스의 SPT에서 페이지 검색 */
+    struct page *page = spt_find_page(&thread_current()->spt, va);
 
-	return vm_do_claim_page (page);
+    /* 2. 해당 페이지가 없으면 실패 */
+    if (page == NULL)
+        return false;
+
+    /* 3. 찾은 페이지를 실제 프레임에 올리기 */
+    return vm_do_claim_page(page);
 }
 
 /* Claim the PAGE and set up the mmu. */
@@ -200,12 +251,20 @@ vm_do_claim_page (struct page *page) {
 	page->frame = frame;
 
 	/* TODO: Insert page table entry to map page's VA to frame's PA. */
-
+	bool success = pml4_set_page (thread_current ()->pml4, page->va,frame->kva, page->writable);
+	//false가 나오면 기존에 선언했던 것들 할당 했던것들 다 free나 null -> 메모리 누수 떄문에.
+	if (!success) {
+    page->frame = NULL;
+    frame->page = NULL;
+    palloc_free_page (frame->kva);
+    free (frame);
+    return false;
+}
 	return swap_in (page, frame->kva);
 }
 
 /* Initialize new supplemental page table */
-void
+void 
 supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
 	hash_init (&spt->pages, page_hash, page_less, NULL);
 }

```

## 부록 B-4. 8c6b72f — Lazy loading·uninit_destroy (현재 minho 미반영)

```diff
commit 8c6b72f141614468ecf51263d0145155b1252330
Author: sungtaeng488 <sungtaeng4884835@gmail.com>
Date:   Wed Oct 28 11:29:57 2026 +0900

    add:Lazy loading + uninit_destroy

diff --git a/pintos/include/vm/vm.h b/pintos/include/vm/vm.h
index 5a6d87a..abdcb48 100644
--- a/pintos/include/vm/vm.h
+++ b/pintos/include/vm/vm.h
@@ -47,10 +47,11 @@ struct page {
 	void *va;              /* Address in terms of user space */
 	struct frame *frame;   /* Back reference for frame */
 
-	struct hash_elem hash_elem;  /* SPT 해시 테이블에 연결할 요소 */
+	/* 내부 list_elem을 통해 이 page를 SPT의 버킷 리스트에 연결한다. */
+	struct hash_elem hash_elem;
 
 	/* Your implementation */
-	bool writable; /* 읽고 쓰기한거?*/
+	bool writable; /* 사용자 쓰기 허용 여부. PML4 매핑의 rw 인자로 사용한다. */
 
 	/* Per-type data are binded into the union.
 	 * Each function automatically detects the current union */
@@ -90,6 +91,7 @@ struct page_operations {
  * We don't want to force you to obey any specific design for this struct.
  * All designs up to you for this. */
 struct supplemental_page_table {
+	/* 프로세스별 해시 테이블. 키는 page->va이고 연결 요소는 page->hash_elem이다. */
 	struct hash pages;
 };
 
diff --git a/pintos/userprog/exception.c b/pintos/userprog/exception.c
index 24a5207..378e1e0 100644
--- a/pintos/userprog/exception.c
+++ b/pintos/userprog/exception.c
@@ -140,14 +140,12 @@ page_fault (struct intr_frame *f) {
 	write = (f->error_code & PF_W) != 0;
 	user = (f->error_code & PF_U) != 0;
 
-	if( not_present || write || user ) {
-		thread_exit();
-	}
+	
 
 #ifdef VM
-	/* For project 3 and later. */
-	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
-		return;
+    /* 처리 가능한 page fault면 로딩 후 실행을 재개한다. */
+    if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
+        return;
 #endif
 
 	/* Count page faults. */
diff --git a/pintos/userprog/process.c b/pintos/userprog/process.c
index 7e6a1a1..28ef1d0 100644
--- a/pintos/userprog/process.c
+++ b/pintos/userprog/process.c
@@ -43,11 +43,13 @@ struct fork_args {
 	bool success;
 };
 
+/* 문서의 aux: 최초 접근 때 실행 파일을 읽기 위해 페이지마다 보관하는 정보.
+ * load_segment()는 이 정보를 준비하고, lazy_load_segment()가 나중에 사용한다. */
 struct lazy_load_info {
-    struct file *file;
-    off_t offset;
-    size_t read_bytes;
-    size_t zero_bytes;
+    struct file *file;    /* 읽을 실행 파일의 참조. 파일 내용 자체를 복사하는 것은 아니다. */
+    off_t offset;        /* 이 페이지의 데이터를 읽기 시작할 파일 내 바이트 위치. */
+    size_t read_bytes;   /* 파일에서 읽어 프레임에 채울 바이트 수. */
+    size_t zero_bytes;   /* 프레임의 나머지 부분을 0으로 채울 바이트 수. */
 };
 
 
@@ -903,10 +905,40 @@ install_page (void *upage, void *kpage, bool writable) {
 
 static bool
 lazy_load_segment (struct page *page, void *aux) {
-	/* TODO: Load the segment from the file */
-	/* TODO: This called when the first page fault occurs on address VA. */
-	/* TODO: VA is available when calling this function. */
+	/* 최초 claim 때 aux에 보관한 로딩 정보를 사용해 프레임의 내용을 채운다. */
+	struct lazy_load_info *info = (struct lazy_load_info *)aux;
+
+    // 1. 실제 데이터를 저장할 프레임 주소
+    uint8_t *kva = (uint8_t *)page->frame->kva;
+
+    /* 파일 연산을 보호한다. 이미 호출자가 file_lock을 잡고 있으면 재획득하지 않는다.
+     * 이 함수에서 직접 획득한 경우에만 아래에서 해제한다. */
+    bool need_lock = !lock_held_by_current_thread (&file_lock);
+    if (need_lock)
+        lock_acquire (&file_lock);
+
+    // 2. 실행 파일에서 데이터 읽기
+    off_t bytes_read = file_read_at(info->file,kva,info->read_bytes,info->offset);
+
+    if (need_lock)
+        lock_release (&file_lock);
+
+    // 3. 읽기에 실패했다면
+    if (bytes_read != (off_t)info->read_bytes) {
+        free(info);
+        return false;
+    }
+
+    // 4. 나머지 메모리를 0으로 초기화
+    memset(kva + info->read_bytes, 0, info->zero_bytes);
+
+    // 5. 로딩 정보 해제
+    free(info);
+
+    return true;
 }
+	
+
 
 /* Loads a segment starting at offset OFS in FILE at address
  * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
@@ -925,43 +957,91 @@ lazy_load_segment (struct page *page, void *aux) {
 static bool
 load_segment (struct file *file, off_t ofs, uint8_t *upage,
 		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
+	/* segment 전체 크기는 4KB 페이지 단위여야 한다. ASSERT는 조건만 검사한다. */
 	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
+	/* 등록할 사용자 가상 주소는 페이지 시작 주소여야 한다. */
 	ASSERT (pg_ofs (upage) == 0);
+	/* segment를 처음 읽기 시작할 파일 위치도 페이지 단위로 정렬돼 있어야 한다. */
 	ASSERT (ofs % PGSIZE == 0);
 
+	/* 파일 데이터 또는 0 채움 영역이 남아 있는 동안 한 번에 한 페이지씩 등록한다. */
 	while (read_bytes > 0 || zero_bytes > 0) {
 		/* Do calculate how to fill this page.
 		 * We will read PAGE_READ_BYTES bytes from FILE
 		 * and zero the final PAGE_ZERO_BYTES bytes. */
+		/* 남은 파일 데이터와 한 페이지 크기 중 작은 값을 선택한다. */
 		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
+		/* 파일 데이터를 제외한 나머지 공간은 나중에 0으로 채운다. */
 		size_t page_zero_bytes = PGSIZE - page_read_bytes;
 
 		/* TODO: Set up aux to pass information to the lazy_load_segment. */
-		void *aux = NULL;
-		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
-					writable, lazy_load_segment, aux))
+		/* 지역 변수의 주소를 넘기면 함수 종료 후 사용할 수 없으므로,
+		 * 나중의 최초 접근까지 유지될 페이지별 로딩 정보를 동적 할당한다. */
+		struct lazy_load_info *aux = malloc (sizeof *aux);
+		/* 로딩 정보를 할당하지 못했으면 이 segment의 등록을 중단한다. */
+		if (aux == NULL)
+			return false;
+
+		/* 파일은 나중에도 열려 있어야 한다. 여기서는 포인터만 보관한다. */
+		aux->file = file;
+		/* 다음 반복에서 ofs가 바뀌기 전에 현재 페이지의 읽기 위치를 저장한다. */
+		aux->offset = ofs;
+		/* 이번 4KB 페이지 중 파일 데이터로 채울 크기를 저장한다. */
+		aux->read_bytes = page_read_bytes;
+		/* 나머지 공간의 크기. read_bytes와 합하면 PGSIZE가 된다. */
+		aux->zero_bytes = page_zero_bytes;
+
+		/* 최종 타입 VM_ANON, 사용자 주소 upage, 쓰기 권한 writable을 전달한다.
+		 * lazy_load_segment는 지금 실행하지 않고 최초 claim 때 호출하도록 등록한다.
+		 * 이 단계에서는 page 정보만 SPT에 넣고 물리 프레임은 할당하지 않는다. */
+		if (!vm_alloc_page_with_initializer (VM_ANON, upage, writable, lazy_load_segment, aux)) {
+			/* 등록 실패: aux를 맡길 page가 없으므로 호출한 쪽에서 해제한다. */
+			free (aux);
 			return false;
+		}
+		/* 등록 성공: page가 aux를 보관하므로 여기서는 해제하지 않는다.
+		 * 추후 lazy loader 또는 미초기화 페이지의 종료 정리에서 해제해야 한다. */
 
 		/* Advance. */
+		/* segment 전체에서 아직 등록하지 않은 파일 데이터 크기를 줄인다. */
 		read_bytes -= page_read_bytes;
+		/* 아직 등록하지 않은 0 채움 영역의 크기도 줄인다. */
 		zero_bytes -= page_zero_bytes;
+		/* 다음 사용자 가상 페이지의 시작 주소로 이동한다. */
 		upage += PGSIZE;
+		/* 다음 페이지에 사용할 파일 위치를 실제 읽을 크기만큼 이동한다. */
+		ofs += page_read_bytes;
 	}
+	
+	/* 모든 페이지의 로딩 정보를 등록했다는 뜻이며, 실제 로딩 완료는 아니다. */
 	return true;
 }
 
 /* Create a PAGE of stack at the USER_STACK. Return true on success. */
 static bool
 setup_stack (struct intr_frame *if_) {
-	bool success = false;
-	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);
+	void *stack_bottom = (uint8_t *) USER_STACK - PGSIZE;
+	struct supplemental_page_table *spt = &thread_current ()->spt;
 
-	/* TODO: Map the stack on stack_bottom and claim the page immediately.
-	 * TODO: If success, set the rsp accordingly.
-	 * TODO: You should mark the page is stack. */
-	/* TODO: Your code goes here */
+	/* 첫 스택 페이지를 SPT에 등록한다. */
+	if (!vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true))
+		return false;
 
-	return success;
+	/* 즉시 프레임을 확보하고 매핑한다. */
+	if (!vm_claim_page (stack_bottom)) {
+		struct page *page = spt_find_page (spt, stack_bottom);
+		spt_remove_page (spt, page);
+		return false;
+	}
+
+	/* 확보한 스택 페이지를 0으로 채운다. */
+	struct page *page = spt_find_page (spt, stack_bottom);
+	memset (page->frame->kva, 0, PGSIZE);
+
+	/* 인자를 넣기 전의 초기 스택 위치를 설정한다. */
+	if_->rsp = USER_STACK;
+
+	return true;
 }
 #endif /* VM */
 
@@ -993,4 +1073,4 @@ bool isnt_stdin_or_stdout(struct fd_info *fd_info) {
 		return false;
 	}
 	return true;
-}
\ No newline at end of file
+}
diff --git a/pintos/vm/anon.c b/pintos/vm/anon.c
index d130b37..9c2f1b3 100644
--- a/pintos/vm/anon.c
+++ b/pintos/vm/anon.c
@@ -28,9 +28,13 @@ vm_anon_init (void) {
 /* Initialize the file mapping */
 bool
 anon_initializer (struct page *page, enum vm_type type, void *kva) {
-	/* Set up the handler */
-	//이 페이지를 VM_ANON으로 관리하도록 전환
+	/* 최초 UNINIT 페이지의 uninit_initialize가 호출하는 타입 초기화 함수.
+	 * anon_ops에는 VM_ANON 타입과 anonymous용 swap_in/out/destroy가 들어 있다.
+	 * 포인터를 교체하면 이 page는 이후 anonymous 방식으로 관리된다. */
 	page->operations = &anon_ops;
+	/* 현재 anon_page는 비어 있으므로 추가 상태 초기화가 없다.
+	 * 성공을 반환하면 uninit_initialize가 다음으로 lazy_load_segment 같은
+	 * 내용 로딩 콜백을 실행한다. 이 함수 자체에서 파일 내용을 읽지는 않는다. */
 	return true;
 }
 
@@ -50,4 +54,5 @@ anon_swap_out (struct page *page) {
 static void
 anon_destroy (struct page *page) {
 	struct anon_page *anon_page = &page->anon;
+	
 }
diff --git a/pintos/vm/uninit.c b/pintos/vm/uninit.c
index 32bbee8..1e2858e 100644
--- a/pintos/vm/uninit.c
+++ b/pintos/vm/uninit.c
@@ -65,4 +65,11 @@ uninit_destroy (struct page *page) {
 	struct uninit_page *uninit UNUSED = &page->uninit;
 	/* TODO: Fill this function.
 	 * TODO: If you don't have anything to do, just return. */
+	//정리할 로딩 정보가 없으면 여기서 끝낸다
+	if(uninit ->aux == NULL){
+		return;
+	}
+	free(uninit->aux);
+	uninit->aux = NULL;
+
 }
diff --git a/pintos/vm/vm.c b/pintos/vm/vm.c
index c2c0e0d..10a937f 100644
--- a/pintos/vm/vm.c
+++ b/pintos/vm/vm.c
@@ -5,22 +5,25 @@
 #include "vm/inspect.h"
 #include "threads/vaddr.h"
 
-/* Hash and compare SPT entries by virtual page address. */
-//- page_hash(): 가상 주소를 이용해 해시값을 계산하는 함수
-//page_less(): 두 페이지의 가상 주소를 비교하는 함수
-/* Returns a hash value for page p. */
+/* 문서의 page_hash 예제: 가상 페이지 주소를 해시값으로 바꾼다.
+ * 해시 테이블은 이 값으로 버킷을 선택한다. 해시값을 page에 저장하지는 않는다. */
 uint64_t
 page_hash (const struct hash_elem *p_, void *aux UNUSED) {
+  /* 테이블이 받은 hash_elem에서 그것을 포함하는 원래 page의 주소를 구한다. */
   const struct page *p = hash_entry (p_, struct page, hash_elem);
+  /* &p->va가 가리키는 주소값의 바이트들을 읽는다. 사용자 페이지 내용은 읽지 않는다. */
   return hash_bytes (&p->va, sizeof p->va);
 }
-/* Returns true if page a precedes page b. */
+/* 문서의 page_less 예제: 두 페이지의 가상 주소 대소 관계를 반환한다. */
 bool
 page_less (const struct hash_elem *a_,
            const struct hash_elem *b_, void *aux UNUSED) {
+  /* 두 연결 요소를 각각의 page로 되돌린다. */
   const struct page *a = hash_entry (a_, struct page, hash_elem);
   const struct page *b = hash_entry (b_, struct page, hash_elem);
 
+  /* 해시 테이블은 A < B도 B < A도 거짓이면 같은 키로 판단한다.
+   * 이 비교 함수 때문에 버킷 리스트가 주소순으로 정렬되는 것은 아니다. */
   return a->va < b->va;
 }
 
@@ -64,51 +67,62 @@ bool
 vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
 		vm_initializer *init, void *aux) {
 
+	/* VM_TYPE은 marker 비트를 제외한 기본 타입을 구한다.
+	 * 호출자는 최종 타입 ANON/FILE을 지정하고, 생성되는 현재 상태는 UNINIT이다. */
 	ASSERT (VM_TYPE(type) != VM_UNINIT);
 
+	/* 새 페이지를 등록할 현재 프로세스의 SPT를 가져온다. */
 	struct supplemental_page_table *spt = &thread_current ()->spt;
 
-	/* Check wheter the upage is already occupied or not. */
-	//가상 페이지 중복 검사
+	/* 같은 가상 페이지가 이미 있으면 새로 등록하지 않고 실패한다.
+	 * upage는 페이지 시작 주소라는 전제로 사용한다. */
 	if (spt_find_page (spt, upage) == NULL) {
-		/* TODO: Create the page, fetch the initialier according to the VM type,
-		 * TODO: and then create "uninit" page struct by calling uninit_new. You
-		 * TODO: should modify the field after calling the uninit_new. */
-		//페이지 할당 해주기
+		/* 커널 메모리에 관리용 page 구조체를 할당한다. 사용자용 4KB frame은 아니다. */
 		struct page *page = malloc(sizeof(struct page));
-		//예외 검사
+		/* 관리 구조체조차 확보하지 못하면 공통 실패 경로로 이동한다. */
 		if(page == NULL){
 			goto err;
 		}
-		//
+		/* page, type, kva를 받아 bool을 반환하는 함수의 주소를 담을 변수.
+		 * init은 내용 로딩용이고, initializer는 페이지 타입 초기화용이다. */
 		bool (*initializer) (struct page *, enum vm_type, void *) = NULL;
 
+		/* 최초 claim 때 어떤 타입 초기화 함수를 호출할지 선택만 한다. */
 		switch (VM_TYPE(type)) {
     	case VM_ANON:
+        	/* 함수 호출이 아니라 anon_initializer의 주소를 저장한다. */
         	initializer = anon_initializer;
         	break;
 
     	case VM_FILE:
+        	/* mmap 등의 파일 기반 페이지에 사용할 타입 초기화 함수. */
         	initializer = file_backed_initializer;
         	break;
 
     	default:
+        /* 지원하지 않는 타입: 방금 만든 관리 구조체를 반환한다. */
         free(page);
         goto err;
 }
+	/* UNINIT 상태, va, 최종 type, 두 초기화 함수와 aux를 기록한다.
+	 * 이 호출은 frame을 확보하거나 init을 실행하지 않는다. */
 	uninit_new(page, upage, init, type, aux, initializer);
+	/* uninit_new가 page 전체를 초기화하므로 추가 필드인 쓰기 권한은 그 뒤에 저장한다. */
 	page->writable = writable;
 	
 
 
-		/* TODO: Insert the page into the spt. */
+	/* 등록한 주소로 이후 최초 접근에서 page를 찾을 수 있도록 SPT에 넣는다. */
 	if(spt_insert_page(spt,page) == false){
+		/* 삽입 실패 시 page만 해제한다. aux의 실패 시 정리는 호출자가 맡는다. */
 		free(page);
 		goto err;
 		}
+		/* SPT 등록 성공. 아직 실제 사용자 메모리 내용이 로딩된 상태는 아니다. */
 		return true;
 	}
 err:
+	/* 주소 중복, 할당 실패, 지원하지 않는 타입, 삽입 실패의 공통 반환값. */
 	return false;
 }
 
@@ -116,19 +130,21 @@ err:
 struct page *
 spt_find_page(struct supplemental_page_table *spt, void *va) {
 
-    /* 1. 검색할 페이지 시작 주소 구하기 */
+    /* 문서의 page_lookup 예제와 같은 검색용 임시 page.
+     * 0x401123 같은 페이지 중간 주소도 시작 주소 0x401000으로 바꿔 찾는다. */
     struct page key = {
         .va = pg_round_down(va),
     };
 
-    /* 2. SPT의 Hash Table에서 검색 */
+    /* 임시 page를 삽입하지 않고 키만 전달한다.
+     * hash_find가 page_hash로 버킷을 찾고 page_less로 일치하는 주소를 찾는다. */
     struct hash_elem *found = hash_find(&spt->pages, &key.hash_elem);
 
     /* 3. 검색 실패 */
     if (found == NULL)
         return NULL;
 
-    /* 4. 찾은 hash_elem에서 struct page 주소 구하기 */
+    /* 검색용 key의 주소가 아니라, SPT에 이미 등록된 실제 page를 반환한다. */
     return hash_entry(found, struct page, hash_elem);
 }
 
@@ -136,18 +152,16 @@ spt_find_page(struct supplemental_page_table *spt, void *va) {
 bool
 spt_insert_page (struct supplemental_page_table *spt UNUSED,
 		struct page *page UNUSED) {
-	/* TODO: Fill this function. */
-
-
+	/* hash_insert는 중복 키가 없으면 연결 요소를 버킷 리스트에 넣고 NULL을 반환한다.
+	 * 같은 va가 이미 있으면 기존 요소를 반환하므로 false가 된다. */
 	 return hash_insert (&spt->pages, &page->hash_elem) == NULL;
-	//NULL: 새 페이지가 삽입됨 -> true
-	//기존 요소의 주소: 같은 va가 이미 있어서 삽입되지 않음 -> false
 }
 
 void
 spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
+	/* 해제된 page의 연결 요소가 SPT에 남지 않도록 먼저 분리한다. */
+	hash_delete (&spt->pages, &page->hash_elem);
 	vm_dealloc_page (page);
-	return true;
 }
 
 /* Get the struct frame, that will be evicted. */
@@ -175,25 +189,32 @@ vm_evict_frame (void) {
  * space.*/
 static struct frame *
 vm_get_frame (void) {
+	/* 반환할 관리 구조체의 포인터. malloc 전에는 아직 구조체가 없다. */
 	struct frame *frame = NULL;
-	/* TODO: Fill this function. */
-	//프레임 관리할 구조체 할당 해주기
+	/* 어떤 물리 페이지를 관리하는지 기록할 frame 구조체를 커널에 할당한다. */
 	frame = malloc(sizeof(struct frame));
+	/* 관리 구조체를 확보하지 못하면 커널을 중단한다. */
 	if(frame == NULL){
 		PANIC("frame allocation fail");
 	}
-	//진짜 프레임 할당 RAM에서 비어 있는 물리 페이지를 찾아서 할당
+	/* user pool의 비어 있는 물리 페이지 하나를 확보한다.
+	 * 반환값은 그 프레임에 커널이 접근할 수 있는 가상 주소(kva)이다. */
 	frame->kva = palloc_get_page(PAL_USER);
+	/* 현재 단계에는 eviction이 없으므로 물리 페이지가 부족하면 중단한다. */
 	if(frame->kva == NULL){
+		/* 먼저 확보했던 frame 관리 구조체는 반환한다. */
 		free(frame);
 		PANIC("todo:EVICTION");
 	}
 
+    /* malloc은 0으로 초기화하지 않는다. 아직 연결된 가상 page가 없음을 직접 기록한다. */
     frame->page = NULL;
 
 
+	/* ASSERT는 값을 초기화하지 않고, 위 작업의 결과가 조건을 만족하는지 확인한다. */
 	ASSERT (frame != NULL);
 	ASSERT (frame->page == NULL);
+	/* 물리 페이지 주소 자체가 아니라 그 주소를 담은 frame 구조체를 반환한다. */
 	return frame;
 }
 
@@ -209,15 +230,36 @@ vm_handle_wp (struct page *page UNUSED) {
 
 /* Return true on success */
 bool
-vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
-		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
-	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
-	struct page *page = NULL;
-	/* TODO: Validate the fault */
-	/* TODO: Your code goes here */
-
-	return vm_do_claim_page (page);
+vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr,
+                    bool user UNUSED, bool write, bool not_present) {
+
+    struct supplemental_page_table *spt = &thread_current()->spt;
+    struct page *page = NULL;
+
+    // 1. 유효한 사용자 가상 주소인지 확인
+    if (addr == NULL || !is_user_vaddr(addr))
+        return false;
+
+    // 2. 매핑되지 않은 페이지 때문에 발생한 Fault인지 확인
+	//not_present = 페이지 부재로 발생한 fault이면 true
+    if (!not_present)
+        return false;
+
+    // 3. SPT에서 페이지 검색
+    page = spt_find_page(spt, addr);
+
+    if (page == NULL)
+        return false;
+
+    // 4. 쓰기 권한 확인
+    if (write && !page->writable)
+        return false;
+
+    // 5. 실제 RAM 프레임 할당 및 페이지 로딩
+    return vm_do_claim_page(page);
 }
+	
+	
 
 /* Free the page.
  * DO NOT MODIFY THIS FUNCTION. */
@@ -230,49 +272,67 @@ vm_dealloc_page (struct page *page) {
 /* Claim the page that allocate on VA. */
 bool
 vm_claim_page(void *va) {
-    /* 1. 현재 프로세스의 SPT에서 페이지 검색 */
+    /* va에 대응하는 관리 정보가 현재 프로세스에 등록돼 있는지 검색한다. */
     struct page *page = spt_find_page(&thread_current()->spt, va);
 
-    /* 2. 해당 페이지가 없으면 실패 */
+    /* 등록되지 않은 페이지는 여기서 새로 만들지 않고 실패한다. */
     if (page == NULL)
         return false;
 
-    /* 3. 찾은 페이지를 실제 프레임에 올리기 */
+    /* 찾은 page에 프레임을 연결하고 매핑 및 내용 로딩을 수행하는 함수로 넘긴다. */
     return vm_do_claim_page(page);
 }
 
 /* Claim the PAGE and set up the mmu. */
 static bool
 vm_do_claim_page (struct page *page) {
+	/* 사용자용 물리 페이지와 그것을 관리할 frame 구조체를 함께 확보한다. */
 	struct frame *frame = vm_get_frame ();
 
-	/* Set links */
+	/* frame에서 자신을 사용하는 page를 찾을 수 있게 연결한다. */
 	frame->page = page;
+	/* page에서도 자신이 올라간 frame을 찾을 수 있게 역방향으로 연결한다. */
 	page->frame = frame;
 
-	/* TODO: Insert page table entry to map page's VA to frame's PA. */
+	/* 현재 프로세스의 PML4에 사용자 va -> frame 매핑과 쓰기 권한을 등록한다.
+	 * frame->kva의 물리 주소 변환은 pml4_set_page 내부에서 처리한다. */
 	bool success = pml4_set_page (thread_current ()->pml4, page->va,frame->kva, page->writable);
-	//false가 나오면 기존에 선언했던 것들 할당 했던것들 다 free나 null -> 메모리 누수 떄문에.
-	if (!success) {
-    page->frame = NULL;
-    frame->page = NULL;
-    palloc_free_page (frame->kva);
-    free (frame);
-    return false;
-}
-	return swap_in (page, frame->kva);
+	/* 현재 operations의 swap_in을 호출한다. 최초 UNINIT이면 uninit_initialize가
+	 * 타입 초기화 후 내용 로딩 콜백을 실행한다. 매핑 실패 시에는 호출하지 않는다. */
+	if (success && swap_in (page, frame->kva))
+		return true;
+
+	/* 로딩 실패로 매핑된 빈 프레임이 남지 않도록 물리 메모리를 반환하기 전에 지운다. */
+	if (success)
+		pml4_clear_page (thread_current ()->pml4, page->va);
+
+	/* 매핑 또는 로딩 실패: 양방향 연결과 두 종류의 할당을 모두 되돌린다. */
+	page->frame = NULL;
+	frame->page = NULL;
+	palloc_free_page (frame->kva);
+	free (frame);
+	return false;
 }
 
 /* Initialize new supplemental page table */
 void 
 supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
-	hash_init (&spt->pages, page_hash, page_less, NULL);
+	/* 문서의 hash_init 예제: 빈 버킷들을 만들고 주소 기준 두 함수를 등록한다.
+	 * 지금 page_hash/page_less를 실행하는 것이 아니라 이후 검색/삽입 때 사용할
+	 * 함수 포인터를 저장한다. 이 호출만으로 page나 사용자 frame이 생성되지는 않는다. */
+	if (!hash_init (&spt->pages, page_hash, page_less, NULL))
+		PANIC ("SPT initialization failed");
 }
 
 /* Copy supplemental page table from src to dst */
 bool
 supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
 		struct supplemental_page_table *src UNUSED) {
+			
+
+
+
+
 }
 
 /* Free the resource hold by the supplemental page table */

```

## 부록 B-5. ff2768f — SPT 복사·종료 정리 (현재 minho 미반영)

```diff
commit ff2768f50c64791c46753da1b282e2493028d9e8
Author: sungtaeng488 <sungtaeng4884835@gmail.com>
Date:   Wed Oct 28 17:31:07 2026 +0900

    add:is_valid_buffet(수정)+spt_copy

diff --git a/pintos/include/userprog/process.h b/pintos/include/userprog/process.h
index a858b80..4d72f10 100644
--- a/pintos/include/userprog/process.h
+++ b/pintos/include/userprog/process.h
@@ -1,10 +1,23 @@
 #ifndef USERPROG_PROCESS_H
 #define USERPROG_PROCESS_H
 
+#include <stddef.h>
+#include "filesys/off_t.h"
 #include "threads/thread.h"
 
 struct file;
 
+#ifdef VM
+/* 문서의 aux: 최초 접근 때 실행 파일을 읽기 위해 페이지마다 보관하는 정보.
+ * process.c의 loader와 vm.c의 SPT 복사 코드가 같은 구조체 정의를 사용한다. */
+struct lazy_load_info {
+    struct file *file;    /* 읽을 실행 파일의 참조. 파일 내용 자체를 복사하는 것은 아니다. */
+    off_t offset;        /* 이 페이지의 데이터를 읽기 시작할 파일 내 바이트 위치. */
+    size_t read_bytes;   /* 파일에서 읽어 프레임에 채울 바이트 수. */
+    size_t zero_bytes;   /* 프레임의 나머지 부분을 0으로 채울 바이트 수. */
+};
+#endif
+
 tid_t process_create_initd (const char *file_name);
 tid_t process_fork (const char *name, struct intr_frame *if_);
 int process_exec (void *f_name);
diff --git a/pintos/userprog/process.c b/pintos/userprog/process.c
index 28ef1d0..fc881e0 100644
--- a/pintos/userprog/process.c
+++ b/pintos/userprog/process.c
@@ -43,16 +43,6 @@ struct fork_args {
 	bool success;
 };
 
-/* 문서의 aux: 최초 접근 때 실행 파일을 읽기 위해 페이지마다 보관하는 정보.
- * load_segment()는 이 정보를 준비하고, lazy_load_segment()가 나중에 사용한다. */
-struct lazy_load_info {
-    struct file *file;    /* 읽을 실행 파일의 참조. 파일 내용 자체를 복사하는 것은 아니다. */
-    off_t offset;        /* 이 페이지의 데이터를 읽기 시작할 파일 내 바이트 위치. */
-    size_t read_bytes;   /* 파일에서 읽어 프레임에 채울 바이트 수. */
-    size_t zero_bytes;   /* 프레임의 나머지 부분을 0으로 채울 바이트 수. */
-};
-
-
 /* General process initializer for initd and other process. */
 static void
 process_init (void) {
@@ -209,6 +199,11 @@ __do_fork (void *aux) {
 	memcpy (&if_, parent_if, sizeof (struct intr_frame));
 	if_.R.rax = 0;
 
+#ifdef VM
+	/* 이후 어떤 단계에서 실패해도 process_cleanup이 유효한 SPT를 정리하도록 한다. */
+	supplemental_page_table_init (&current->spt);
+#endif
+
 	/* 2. Duplicate PT */
 	current->pml4 = pml4_create();
 	if (current->pml4 == NULL)
@@ -216,7 +211,14 @@ __do_fork (void *aux) {
 
 	process_activate (current);
 #ifdef VM
-	supplemental_page_table_init (&current->spt);
+	/* 자식 aux가 빌려 쓸 실행 파일은 SPT 복사 전에 확보해야 한다. */
+	if (parent->running_file != NULL) {
+		lock_acquire (&file_lock);
+		current->running_file = file_duplicate (parent->running_file);
+		lock_release (&file_lock);
+		if (current->running_file == NULL)
+			goto error;
+	}
 	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
 		goto error;
 #else
@@ -233,18 +235,21 @@ __do_fork (void *aux) {
 
 	/* Finally, switch to the newly created process. */
 	if (succ) {
+#ifndef VM
 		if (parent->running_file != NULL) {
 			/* running file 추가 */
 			lock_acquire(&file_lock);
 			current->running_file = file_duplicate(parent->running_file);
 			lock_release(&file_lock);
 		}
+#endif
 		sema_up(&((struct fork_args *)aux)->sema);
 		do_iret (&if_);
 	}
 error:
 	((struct fork_args *)aux)->success = false;
-	file_close(current->running_file);
+	/* thread_exit -> process_exit -> process_cleanup에서 SPT와 실행 파일을 정리한다.
+	 * 여기서 먼저 파일을 닫으면 cleanup에서 같은 참조를 다시 닫게 된다. */
 	sema_up(&((struct fork_args *)aux)->sema);
 	thread_exit ();
 }
@@ -347,6 +352,13 @@ process_exec (void *f_name) {
 	/* We first kill the current context */
 	process_cleanup ();
 
+#ifdef VM
+	/* cleanup에서 기존 SPT의 page들과 해시 버킷을 해제했다.
+	 * 새 프로그램의 page를 등록할 빈 SPT를 다시 준비한다.
+	 * exit은 재사용하지 않으므로 이 초기화는 exec 경로에서만 수행한다. */
+	supplemental_page_table_init (&thread_current ()->spt);
+#endif
+
 	/* And then load the binary */
 	lock_acquire(&file_lock);
 	success = load (file_name, &_if);
diff --git a/pintos/userprog/syscall.c b/pintos/userprog/syscall.c
index 2f77d35..8102d69 100644
--- a/pintos/userprog/syscall.c
+++ b/pintos/userprog/syscall.c
@@ -535,7 +535,7 @@ static bool is_invaild_filename(char * filename){
 
 static bool is_valid_buffer(char *buffer, int size, void *arg) {
 	struct thread *curr = thread_current();
-	if (buffer == arg || is_kernel_vaddr(buffer + size) || pml4_get_page(curr->pml4, buffer) == NULL) {
+	if (buffer == arg || is_kernel_vaddr(buffer + size) || spt_find_page(&curr->spt, buffer) == NULL) {
 		return false;
 	}
 	return true;
diff --git a/pintos/vm/anon.c b/pintos/vm/anon.c
index 9c2f1b3..fd1c813 100644
--- a/pintos/vm/anon.c
+++ b/pintos/vm/anon.c
@@ -2,6 +2,8 @@
 
 #include "vm/vm.h"
 #include "devices/disk.h"
+#include "threads/malloc.h"
+
 
 /* DO NOT MODIFY BELOW LINE */
 static struct disk *swap_disk;
@@ -53,6 +55,21 @@ anon_swap_out (struct page *page) {
 /* Destroy the anonymous page. PAGE will be freed by the caller. */
 static void
 anon_destroy (struct page *page) {
+	/* anonymous 전용 관리 정보의 주소. 현재 구조체는 비어 있어 아직 사용하지 않는다. */
 	struct anon_page *anon_page = &page->anon;
-	
+	/* page->frame에 이미 저장된 frame 구조체의 주소를 가져온다.
+	 * &page->frame은 포인터 멤버의 주소이므로 여기서 사용할 값이 아니다. */
+	struct frame *frame = page->frame;
+	/* 연결된 frame 관리 구조체가 있을 때만 정리한다. */
+	if(page->frame != NULL){
+		/* frame에서 page로 향하는 역방향 연결을 끊는다. */
+		frame->page = NULL;
+		/* malloc으로 만든 관리 구조체만 반환한다.
+		 * 현재 종료 흐름에서 실제 물리 페이지는 이후 pml4_destroy가 반환한다. */
+		free(frame);
+		/* page가 해제된 frame 구조체를 계속 가리키지 않도록 한다. */
+		page->frame = NULL;
+
+	}
+	/* page 자체는 vm_dealloc_page가 해제하므로 여기서 다시 free하지 않는다. */
 }
diff --git a/pintos/vm/uninit.c b/pintos/vm/uninit.c
index 1e2858e..3bea837 100644
--- a/pintos/vm/uninit.c
+++ b/pintos/vm/uninit.c
@@ -10,6 +10,7 @@
 
 #include "vm/vm.h"
 #include "vm/uninit.h"
+#include "threads/malloc.h"
 
 static bool uninit_initialize (struct page *page, void *kva);
 static void uninit_destroy (struct page *page);
@@ -62,14 +63,18 @@ uninit_initialize (struct page *page, void *kva) {
  * PAGE will be freed by the caller. */
 static void
 uninit_destroy (struct page *page) {
+	/* 새로 할당하는 것이 아니라, page 내부의 미초기화 관리 정보를 가리킨다. */
 	struct uninit_page *uninit UNUSED = &page->uninit;
-	/* TODO: Fill this function.
-	 * TODO: If you don't have anything to do, just return. */
-	//정리할 로딩 정보가 없으면 여기서 끝낸다
+	/* 스택처럼 aux가 없는 페이지라면 해제할 로딩 정보가 없다. */
 	if(uninit ->aux == NULL){
 		return;
 	}
+	/* load_segment에서 동적 할당한 로딩 정보 전체를 반환한다.
+	 * 그 안의 file은 프로세스에서 빌려 쓴 참조이므로 여기서 닫지 않는다. */
 	free(uninit->aux);
+	/* free는 포인터 값을 바꾸지 않으므로 해제된 주소가 남지 않도록 정리한다. */
 	uninit->aux = NULL;
+	/* uninit은 page 내부 공간이므로 따로 free하지 않는다.
+	 * page 구조체 자체의 free는 호출자인 vm_dealloc_page가 수행한다. */
 
 }
diff --git a/pintos/vm/vm.c b/pintos/vm/vm.c
index 10a937f..e0104e7 100644
--- a/pintos/vm/vm.c
+++ b/pintos/vm/vm.c
@@ -1,9 +1,11 @@
 /* vm.c: Generic interface for virtual memory objects. */
 
+#include <string.h>
 #include "threads/malloc.h"
 #include "vm/vm.h"
 #include "vm/inspect.h"
 #include "threads/vaddr.h"
+#include "userprog/process.h"
 
 /* 문서의 page_hash 예제: 가상 페이지 주소를 해시값으로 바꾼다.
  * 해시 테이블은 이 값으로 버킷을 선택한다. 해시값을 page에 저장하지는 않는다. */
@@ -111,7 +113,6 @@ vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
 	page->writable = writable;
 	
 
-
 	/* 등록한 주소로 이후 최초 접근에서 page를 찾을 수 있도록 SPT에 넣는다. */
 	if(spt_insert_page(spt,page) == false){
 		/* 삽입 실패 시 page만 해제한다. aux의 실패 시 정리는 호출자가 맡는다. */
@@ -326,18 +327,101 @@ supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
 
 /* Copy supplemental page table from src to dst */
 bool
-supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
-		struct supplemental_page_table *src UNUSED) {
-			
-
-
-
-
+supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED, struct supplemental_page_table *src UNUSED){
+	/* 페이지 생성 함수는 현재 스레드의 SPT에 등록하므로 자식 문맥에서 호출한다. */
+	ASSERT (dst == &thread_current ()->spt);
+
+	struct hash_iterator i;	
+	struct hash_elem *e;
+	hash_first (&i, &src->pages);
+
+	while ((e = hash_next (&i)) != NULL) {
+    	struct page *page = hash_entry (e, struct page, hash_elem);
+		switch(page->operations->type){
+			case VM_UNINIT: {
+				/* 현재는 실행 파일용 anonymous 페이지의 로딩 정보만 복사한다. */
+				if (VM_TYPE (page->uninit.type) != VM_ANON)
+					return false;
+				/* 부모에게 로딩 정보가 없는 경우 자식도 NULL을 전달한다. */
+				struct lazy_load_info *child_aux = NULL;
+				if (page->uninit.aux != NULL) {
+					/* 현재 loader의 aux는 lazy_load_info이다.
+					 * 구조체를 따로 만들어 부모가 aux를 해제해도 자식은 유지한다. */
+					const struct lazy_load_info *parent_aux = page->uninit.aux;
+					child_aux = malloc (sizeof *child_aux);
+					if (child_aux == NULL)
+						return false;
+					*child_aux = *parent_aux;
+
+					/* file 포인터는 값 복사만 하면 부모의 파일을 가리킨다.
+					 * fork가 미리 복제한 자식 실행 파일을 빌려 사용하도록 바꾼다. */
+					child_aux->file = thread_current ()->running_file;
+					if (child_aux->file == NULL) {
+						free (child_aux);
+						return false;
+					}
+				}
+
+				/* aux의 유무와 관계없이 자식 page는 등록해야 한다. */
+				if (!vm_alloc_page_with_initializer (
+					page->uninit.type,  /* 부모 페이지가 초기화될 최종 타입 */
+					page->va,          /* 같은 사용자 가상 주소 */
+					page->writable,    /* 같은 쓰기 권한 */
+					page->uninit.init,  /* 나중에 내용을 로딩할 함수 */
+					child_aux          /* 별도로 복제한 자식용 로딩 정보 */
+				)) {
+					/* 등록 성공 전까지 aux는 이 복사 코드가 소유한다. */
+					free (child_aux);
+					return false;
+				}
+				break;
+			}
+			case VM_ANON:
+			if (!vm_alloc_page (VM_ANON, page->va, page->writable))
+    			return false;
+			if (!vm_claim_page (page->va))
+    			return false;
+			struct page *child_page = spt_find_page (dst, page->va);
+			memcpy(child_page->frame->kva,page->frame->kva,PGSIZE);
+			break;
+
+			case VM_FILE:
+				return false;
+
+			default:
+				/* 다음 구현 대상. 아직 복사하지 않은 타입을 성공으로 처리하지 않는다. */
+				return false;
+		}
+}
+	return true;
 }
 
 /* Free the resource hold by the supplemental page table */
 void
 supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
-	/* TODO: Destroy all the supplemental_page_table hold by thread and
-	 * TODO: writeback all the modified contents to the storage. */
+	/* 전체 SPT를 순회할 상태와 현재 가져온 해시 요소의 주소를 준비한다. */
+	struct hash_iterator i;
+	struct hash_elem *e;
+
+	/* 이 SPT의 pages 해시 테이블을 순회하도록 준비한다. 아직 요소를 가져오지는 않는다. */
+	hash_first (&i, &spt->pages);
+
+	/* 다음 요소가 있으면 e에 저장한다. 모든 요소가 없어지면 NULL이 되어 끝난다. */
+	while ((e = hash_next (&i)) != NULL) {
+		/* 해시 연결 요소를 포함하고 있는 원래 page 구조체의 주소를 구한다. */
+		struct page *page = hash_entry (e, struct page, hash_elem);
+		/* page 메모리를 해제하기 전에 버킷 리스트에서 연결을 먼저 제거한다.
+		 * hash_delete 자체는 page를 free하지 않는다. */
+		hash_delete (&spt->pages, e);
+		/* operations의 destroy로 aux/frame 관리 정보를 정리하고 page도 free한다. */
+		vm_dealloc_page(page);
+		/* hash_delete가 iterator를 무효로 만들므로 남은 테이블의 처음부터 다시 준비한다. */
+		hash_first(&i,&spt->pages);
+	}
+	/* 모든 page는 위에서 해제했다. NULL 콜백으로 버킷 배열만 반환한다.
+	 * thread 안에 들어 있는 spt 구조체 자체를 free하는 것은 아니다. */
+	hash_destroy (&spt->pages, NULL);
+	/* 현재 소유권 설계에서 PML4와 매핑된 물리 페이지는 호출자가 이후 정리한다.
+	 * exec이 이 SPT를 다시 쓰려면 새 프로그램 로딩 전에 재초기화해야 한다. */
+
 }

```

## 부록 B-6. 9686fa4 — Project 2 주소 검사 보완 (현재 minho 미반영)

```diff
commit 9686fa4bde0069e01cd26ba83bb5ae69b0809097
Author: sungtaeng488 <sungtaeng4884835@gmail.com>
Date:   Thu Oct 29 10:37:29 2026 +0900

    add:project2 에러 고침

diff --git a/pintos/userprog/exception.c b/pintos/userprog/exception.c
index 378e1e0..9f8f009 100644
--- a/pintos/userprog/exception.c
+++ b/pintos/userprog/exception.c
@@ -151,6 +151,14 @@ page_fault (struct intr_frame *f) {
 	/* Count page faults. */
 	page_fault_cnt++;
 
+	/* 정상적인 lazy loading은 위에서 먼저 복구한다.
+	 * 복구할 수 없는 사용자 접근은 해당 프로세스만 -1로 종료한다.
+	 * 커널 fault는 아래의 기존 진단 경로에 남겨 실제 커널 버그를 숨기지 않는다. */
+	if (user) {
+		thread_current ()->exit_status = -1;
+		thread_exit ();
+	}
+
 	/* If the fault is true fault, show info and exit. */
 	printf ("Page fault at %p: %s error %s page in %s context.\n",
 			fault_addr,
diff --git a/pintos/userprog/syscall.c b/pintos/userprog/syscall.c
index 8102d69..016ea46 100644
--- a/pintos/userprog/syscall.c
+++ b/pintos/userprog/syscall.c
@@ -17,6 +17,7 @@
 #include "filesys/filesys.h"
 #include "devices/input.h"
 #include "threads/palloc.h"
+#include "threads/mmu.h"
 
 void syscall_entry (void);
 void syscall_handler (struct intr_frame *);
@@ -39,7 +40,9 @@ void syscall_handler (struct intr_frame *);
 static bool is_invaild_filename(char * filename);
 static struct fd_info * seek_fd_info (struct thread *curr, int fd);
 static struct fd_info * dup_fd_info(struct fd_info *oldfd_info, int newfd);
-static bool is_valid_buffer(char *buffer, int size, void* arg);
+static bool user_page_ready (const void *addr, bool writable);
+static void check_user_string (const char *str, size_t limit);
+static bool is_valid_buffer (const void *buffer, size_t size, bool writable);
 
 static void sys_halt (struct intr_frame *f);
 static void sys_exit (struct intr_frame *f);
@@ -161,6 +164,8 @@ sys_fork (struct intr_frame *f) {
 static void 
 sys_exec (struct intr_frame *f) {
 	char *fname = (void *)f->R.rdi;
+	/* strlcpy가 읽을 범위를 먼저 확인한다. 잘못된 주소를 커널에서 읽으면 안 된다. */
+	check_user_string (fname, PAGE_SIZE);
 	char *buffer = palloc_get_page(0);
 
 	if (buffer == NULL){
@@ -241,7 +246,8 @@ sys_open (struct intr_frame *f) {
 		thread_exit();
 	}
 	
-	if (!is_valid_buffer(fname, strlen(fname), '\0') || is_invaild_filename(fname)) {
+	/* 길이를 재려고 strlen부터 호출하면 주소 검사 전에 fault가 발생한다. */
+	if (is_invaild_filename(fname)) {
 		f->R.rax = -1;       
 		return;
 	}
@@ -298,7 +304,8 @@ sys_read (struct intr_frame *f) {
 	char *buffer = (char *)f->R.rsi;
 	unsigned size = f->R.rdx;
 
-	if(!is_valid_buffer(buffer, size, '\0')){
+	/* read는 사용자 버퍼에 쓰므로 모든 페이지에 쓰기 권한도 필요하다. */
+	if(!is_valid_buffer(buffer, size, true)){
 		thread_current()->exit_status = -1;
 		thread_exit();
 	}
@@ -382,7 +389,8 @@ sys_write (struct intr_frame *f) {
 	int fd = f->R.rdi;
 	uint64_t size = f->R.rdx;
 
-	if (!is_valid_buffer(buffer, size, NULL)) {
+	/* write는 사용자 버퍼를 읽기만 하므로 읽기 전용 페이지도 허용한다. */
+	if (!is_valid_buffer(buffer, size, false)) {
 		thread_current()->exit_status = -1;
 		thread_exit(); // 접근하면 안 되는 포인터를 주면 프로세스 종료 
 		
@@ -529,16 +537,73 @@ static struct fd_info * dup_fd_info(struct fd_info *oldfd_info, int newfd) {
 
 static bool is_invaild_filename(char * filename){
 	size_t size;
+	/* create/open/remove/fork가 공유하는 문자열 검사 지점이다.
+	 * strnlen이 실제로 읽기 전에 페이지가 유효하고 RAM에 있는지 확인한다. */
+	check_user_string (filename, NAME_MAX + 1);
 	size = strnlen(filename, NAME_MAX+1);
 	return !((size > 0) && (size <= (NAME_MAX)));
 }
 
-static bool is_valid_buffer(char *buffer, int size, void *arg) {
-	struct thread *curr = thread_current();
-	if (buffer == arg || is_kernel_vaddr(buffer + size) || spt_find_page(&curr->spt, buffer) == NULL) {
+/* 커널이 사용자 메모리를 읽거나 쓰기 전에 해당 페이지를 준비한다. */
+static bool
+user_page_ready (const void *addr, bool writable) {
+	if (addr == NULL || !is_user_vaddr (addr))
+		return false;
+	struct thread *curr = thread_current ();
+#ifdef VM
+	struct page *page = spt_find_page (&curr->spt, (void *) addr);
+	if (page == NULL || (writable && !page->writable))
+		return false;
+	/* SPT에만 등록된 lazy 페이지는 정상 주소다. 파일 락을 잡기 전에 claim한다. */
+	if (page->frame == NULL && !vm_claim_page (page->va))
+		return false;
+#else
+	/* Project 2 빌드는 SPT 대신 실제 페이지 테이블에서 확인한다. */
+	uint64_t *pte = pml4e_walk (curr->pml4, (uint64_t) addr, 0);
+	if (pte == NULL || (writable && !is_writable (pte)))
 		return false;
+#endif
+	return pml4_get_page (curr->pml4, addr) != NULL;
+}
+
+/* 문자열은 길이도 아직 모르므로, 검사한 페이지 안에서만 문자를 읽는다.
+ * limit까지 NUL이 없으면 길이 초과 판단은 기존 호출자가 맡는다. */
+static void
+check_user_string (const char *str, size_t limit) {
+	uintptr_t addr = (uintptr_t) str;
+	while (limit > 0) {
+		if (!user_page_ready ((const void *) addr, false)) {
+			thread_current ()->exit_status = -1;
+			thread_exit ();
+		}
+		size_t count = PGSIZE - pg_ofs (addr);
+		if (count > limit)
+			count = limit;
+		for (size_t i = 0; i < count; i++)
+			if (*(const char *) (addr + i) == '\0')
+				return;
+		addr += count;
+		limit -= count;
+	}
+}
+
+static bool
+is_valid_buffer (const void *buffer, size_t size, bool writable) {
+	uintptr_t start = (uintptr_t) buffer;
+	if (buffer == NULL || !is_user_vaddr (buffer))
+		return false;
+	if (size == 0)
+		return true;
+	/* 끝 주소 계산의 정수 넘침과 커널 주소 영역 침범을 먼저 막는다. */
+	if (size - 1 > (uintptr_t) KERN_BASE - 1 - start)
+		return false;
+	uintptr_t last = start + size - 1;
+	for (uintptr_t addr = start; ; addr = (uintptr_t) pg_round_down (addr) + PGSIZE) {
+		if (!user_page_ready ((const void *) addr, writable))
+			return false;
+		if (pg_round_down (addr) == pg_round_down (last))
+			return true;
 	}
-	return true;
 }
 
 static struct fd_info * seek_fd_info (struct thread *curr, int fd) {
@@ -552,4 +617,4 @@ static struct fd_info * seek_fd_info (struct thread *curr, int fd) {
 	}
 
 	return NULL;
-}
\ No newline at end of file
+}

```

## 부록 B-7. f33ce08 — stack growth (현재 minho 미반영)

```diff
commit f33ce08b090aebc96b4621935b4abcb72dd921df
Author: sungtaeng488 <sungtaeng4884835@gmail.com>
Date:   Thu Oct 29 15:38:25 2026 +0900

    add:stack_growth 구현

diff --git a/pintos/include/threads/thread.h b/pintos/include/threads/thread.h
index 90df1bf..73e0f35 100644
--- a/pintos/include/threads/thread.h
+++ b/pintos/include/threads/thread.h
@@ -122,6 +122,8 @@ typedef int tid_t;
 #ifdef VM
 	/* Table for whole virtual memory owned by thread. */
 	struct supplemental_page_table spt;
+	/* 시스템콜 진입 시의 사용자 스택 포인터를 보관한다. */
+	uintptr_t user_rsp;
 #endif
 
 	/* Owned by thread.c. */
diff --git a/pintos/userprog/syscall.c b/pintos/userprog/syscall.c
index 016ea46..0b1ae46 100644
--- a/pintos/userprog/syscall.c
+++ b/pintos/userprog/syscall.c
@@ -76,7 +76,11 @@ syscall_init (void) {
 
 /* The main system call interface */
 void
-syscall_handler (struct intr_frame *f UNUSED) {
+syscall_handler (struct intr_frame *f) {
+#ifdef VM
+	/* 커널에서 참고할 사용자 스택 포인터를 저장한다. */
+	thread_current ()->user_rsp = (uintptr_t) f->rsp;
+#endif
 
 	uint64_t syscall_num = f->R.rax;
 	switch (syscall_num){
@@ -552,6 +556,17 @@ user_page_ready (const void *addr, bool writable) {
 	struct thread *curr = thread_current ();
 #ifdef VM
 	struct page *page = spt_find_page (&curr->spt, (void *) addr);
+	if (page == NULL) {
+		/* 아직 SPT에 없더라도 정상적인 스택 접근일 수 있다.
+		 * fault 처리와 같은 성장 조건을 사용해 페이지를 준비한다.
+		 * user=false이므로 f 대신 시스템콜 진입 때 저장한 user_rsp를 사용한다.
+		 * not_present=true는 현재 페이지가 없다는 뜻이다. */
+		if (!vm_try_handle_fault (NULL, (void *) addr, false, writable, true))
+			return false;
+
+		/* 성장 과정에서 등록한 페이지를 가져와 아래 권한 검사도 수행한다. */
+		page = spt_find_page (&curr->spt, (void *) addr);
+	}
 	if (page == NULL || (writable && !page->writable))
 		return false;
 	/* SPT에만 등록된 lazy 페이지는 정상 주소다. 파일 락을 잡기 전에 claim한다. */
diff --git a/pintos/vm/vm.c b/pintos/vm/vm.c
index e0104e7..d6df065 100644
--- a/pintos/vm/vm.c
+++ b/pintos/vm/vm.c
@@ -219,9 +219,32 @@ vm_get_frame (void) {
 	return frame;
 }
 
-/* Growing the stack. */
+/* 허용된 스택 주소에 페이지를 생성하고 실제 메모리를 준비한다. */
 static void
-vm_stack_growth (void *addr UNUSED) {
+vm_stack_growth (void *addr) {
+	struct supplemental_page_table *spt = &thread_current ()->spt;
+
+	addr = pg_round_down (addr);
+
+	struct page *page = spt_find_page (spt, addr);
+	if (page != NULL)
+		return;
+
+	/* 쓰기 가능한 anonymous 스택 페이지를 등록한다. */
+	if (!vm_alloc_page (VM_ANON | VM_MARKER_0, addr, true))
+		return;
+
+	/* 등록된 페이지의 주소를 page 변수에 새로 저장한다. */
+	page = spt_find_page (spt, addr);
+
+	/* frame 확보·매핑에 실패하면 등록한 페이지도 정리한다. */
+	if (!vm_claim_page (addr)) {
+		spt_remove_page (spt, page);
+		return;
+	}
+
+	/* 새 스택 페이지를 0으로 초기화한다. */
+	memset (page->frame->kva, 0, PGSIZE);
 }
 
 /* Handle the fault on write_protected page */
@@ -229,38 +252,75 @@ static bool
 vm_handle_wp (struct page *page UNUSED) {
 }
 
-/* Return true on success */
+/* 잘못된 접근은 거부하고, 스택 성장 또는 기존 페이지 로딩으로 fault를 복구한다.
+ * 복구에 성공하면 true를 반환한다. */
 bool
-vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr,
-                    bool user UNUSED, bool write, bool not_present) {
+vm_try_handle_fault (struct intr_frame *f, void *addr,
+		bool user, bool write, bool not_present) {
+	struct thread *curr = thread_current ();
+	struct supplemental_page_table *spt = &curr->spt;
 
-    struct supplemental_page_table *spt = &thread_current()->spt;
-    struct page *page = NULL;
+	/* NULL이나 커널 주소에 대한 접근은 허용하지 않는다. */
+	if (addr == NULL || !is_user_vaddr (addr))
+		return false;
 
-    // 1. 유효한 사용자 가상 주소인지 확인
-    if (addr == NULL || !is_user_vaddr(addr))
-        return false;
+	/* 권한 위반은 페이지를 새로 할당해서 해결하지 않는다. */
+	if (!not_present)
+		return false;
 
-    // 2. 매핑되지 않은 페이지 때문에 발생한 Fault인지 확인
-	//not_present = 페이지 부재로 발생한 fault이면 true
-    if (!not_present)
-        return false;
+	struct page *page = spt_find_page (spt, addr);
 
-    // 3. SPT에서 페이지 검색
-    page = spt_find_page(spt, addr);
+	if (page == NULL) {
+		/* 커널 fault에서는 저장해 둔 사용자 rsp를 사용한다. */
+		uintptr_t user_rsp = user ? (uintptr_t) f->rsp : curr->user_rsp;
 
-    if (page == NULL)
-        return false;
+		uintptr_t fault_addr = (uintptr_t) addr;
 
-    // 4. 쓰기 권한 확인
-    if (write && !page->writable)
-        return false;
+		/* 스택은 USER_STACK 아래 최대 1MB까지 허용한다. */
+		if (fault_addr < (uintptr_t) USER_STACK - (1UL << 20)
+			|| fault_addr >= (uintptr_t) USER_STACK)
+			return false;
 
-    // 5. 실제 RAM 프레임 할당 및 페이지 로딩
-    return vm_do_claim_page(page);
+		/* PUSH가 rsp를 갱신하기 전에 접근하는 rsp - 8도 허용한다.
+		 * user_rsp < 8 검사는 뺄셈에서 정수가 넘치는 것을 막는다. */
+		if (user_rsp < 8 || fault_addr < user_rsp - 8)
+			return false;
+
+		/* 정상적인 스택 접근이면 새 페이지를 생성하고 claim한다. */
+		vm_stack_growth (addr);
+
+		/* growth는 void 함수이므로 등록·frame·매핑 결과를 차례로 확인한다.
+		 * 이미 claim했으므로 여기서 다시 claim하지 않는다. */
+		page = spt_find_page (spt, addr);
+		if (page == NULL)
+			return false;
+
+		/* 실제 물리 프레임이 연결됐는지 확인한다. */
+		if (page->frame == NULL)
+			return false;
+
+		/* 사용자 주소에 매핑됐는지 확인한다. */
+		if (pml4_get_page (curr->pml4, addr) == NULL)
+			return false;
+
+		return true;
+	}
+
+	/* 기존 페이지에 쓰려면 쓰기 권한이 있어야 한다. */
+	if (write && !page->writable)
+		return false;
+
+	/* 이미 frame이 있으면 중복 할당하지 않고 매핑을 확인한다. */
+	if (page->frame != NULL) {
+		if (pml4_get_page (curr->pml4, addr) == NULL)
+			return false;
+
+		return true;
+	}
+
+	/* SPT에 등록되어 있지만 아직 RAM에 없는 페이지를 로딩한다. */
+	return vm_do_claim_page (page);
 }
-	
-	
 
 /* Free the page.
  * DO NOT MODIFY THIS FUNCTION. */

```

## 부록 C. 기존 테스트 결과 원문

전체 141개: pass 83개, FAIL 58개. 재실행 결과가 아닌 기존 산출물이다.

출처: `pintos/vm/build/results`

```text
pass tests/userprog/args-none
pass tests/userprog/args-single
pass tests/userprog/args-multiple
pass tests/userprog/args-many
pass tests/userprog/args-dbl-space
pass tests/userprog/halt
pass tests/userprog/exit
pass tests/userprog/create-normal
pass tests/userprog/create-empty
pass tests/userprog/create-null
pass tests/userprog/create-bad-ptr
pass tests/userprog/create-long
pass tests/userprog/create-exists
pass tests/userprog/create-bound
pass tests/userprog/open-normal
pass tests/userprog/open-missing
pass tests/userprog/open-boundary
pass tests/userprog/open-empty
pass tests/userprog/open-null
pass tests/userprog/open-bad-ptr
pass tests/userprog/open-twice
pass tests/userprog/close-normal
pass tests/userprog/close-twice
pass tests/userprog/close-bad-fd
pass tests/userprog/read-normal
pass tests/userprog/read-bad-ptr
pass tests/userprog/read-boundary
pass tests/userprog/read-zero
pass tests/userprog/read-stdout
pass tests/userprog/read-bad-fd
pass tests/userprog/write-normal
pass tests/userprog/write-bad-ptr
pass tests/userprog/write-boundary
pass tests/userprog/write-zero
pass tests/userprog/write-stdin
pass tests/userprog/write-bad-fd
FAIL tests/userprog/fork-once
FAIL tests/userprog/fork-multiple
FAIL tests/userprog/fork-recursive
FAIL tests/userprog/fork-read
FAIL tests/userprog/fork-close
FAIL tests/userprog/fork-boundary
FAIL tests/userprog/exec-once
FAIL tests/userprog/exec-arg
FAIL tests/userprog/exec-boundary
pass tests/userprog/exec-missing
pass tests/userprog/exec-bad-ptr
FAIL tests/userprog/exec-read
FAIL tests/userprog/wait-simple
FAIL tests/userprog/wait-twice
FAIL tests/userprog/wait-killed
pass tests/userprog/wait-bad-pid
FAIL tests/userprog/multi-recurse
FAIL tests/userprog/multi-child-fd
pass tests/userprog/rox-simple
FAIL tests/userprog/rox-child
FAIL tests/userprog/rox-multichild
pass tests/userprog/bad-read
pass tests/userprog/bad-write
pass tests/userprog/bad-read2
pass tests/userprog/bad-write2
pass tests/userprog/bad-jump
pass tests/userprog/bad-jump2
FAIL tests/vm/pt-grow-stack
pass tests/vm/pt-grow-bad
FAIL tests/vm/pt-big-stk-obj
pass tests/vm/pt-bad-addr
pass tests/vm/pt-bad-read
pass tests/vm/pt-write-code
pass tests/vm/pt-write-code2
FAIL tests/vm/pt-grow-stk-sc
pass tests/vm/page-linear
FAIL tests/vm/page-parallel
FAIL tests/vm/page-merge-seq
FAIL tests/vm/page-merge-par
FAIL tests/vm/page-merge-stk
FAIL tests/vm/page-merge-mm
pass tests/vm/page-shuffle
FAIL tests/vm/mmap-read
FAIL tests/vm/mmap-close
FAIL tests/vm/mmap-unmap
FAIL tests/vm/mmap-overlap
FAIL tests/vm/mmap-twice
FAIL tests/vm/mmap-write
FAIL tests/vm/mmap-ro
FAIL tests/vm/mmap-exit
FAIL tests/vm/mmap-shuffle
FAIL tests/vm/mmap-bad-fd
FAIL tests/vm/mmap-clean
FAIL tests/vm/mmap-inherit
FAIL tests/vm/mmap-misalign
FAIL tests/vm/mmap-null
FAIL tests/vm/mmap-over-code
FAIL tests/vm/mmap-over-data
FAIL tests/vm/mmap-over-stk
FAIL tests/vm/mmap-remove
FAIL tests/vm/mmap-zero
FAIL tests/vm/mmap-bad-fd2
FAIL tests/vm/mmap-bad-fd3
FAIL tests/vm/mmap-zero-len
FAIL tests/vm/mmap-off
FAIL tests/vm/mmap-bad-off
FAIL tests/vm/mmap-kernel
FAIL tests/vm/lazy-file
pass tests/vm/lazy-anon
FAIL tests/vm/swap-file
FAIL tests/vm/swap-anon
FAIL tests/vm/swap-iter
FAIL tests/vm/swap-fork
pass tests/filesys/base/lg-create
pass tests/filesys/base/lg-full
pass tests/filesys/base/lg-random
pass tests/filesys/base/lg-seq-block
pass tests/filesys/base/lg-seq-random
pass tests/filesys/base/sm-create
pass tests/filesys/base/sm-full
pass tests/filesys/base/sm-random
pass tests/filesys/base/sm-seq-block
pass tests/filesys/base/sm-seq-random
FAIL tests/filesys/base/syn-read
pass tests/filesys/base/syn-remove
FAIL tests/filesys/base/syn-write
pass tests/threads/alarm-single
pass tests/threads/alarm-multiple
pass tests/threads/alarm-simultaneous
pass tests/threads/alarm-priority
pass tests/threads/alarm-zero
pass tests/threads/alarm-negative
pass tests/threads/priority-change
pass tests/threads/priority-donate-one
pass tests/threads/priority-donate-multiple
pass tests/threads/priority-donate-multiple2
pass tests/threads/priority-donate-nest
pass tests/threads/priority-donate-sema
pass tests/threads/priority-donate-lower
pass tests/threads/priority-fifo
pass tests/threads/priority-preempt
pass tests/threads/priority-sema
pass tests/threads/priority-condvar
pass tests/threads/priority-donate-chain
FAIL tests/vm/cow/cow-simple

```

### userprog/fork-once: 개별 판정 원문

출처: `pintos/vm/build/tests/userprog/fork-once.result`

```text
FAIL
Test output failed to match any acceptable form.

Acceptable output:
  (fork-once) begin
  (fork-once) child run
  child: exit(81)
  (fork-once) Parent: child exit status is 81
  (fork-once) end
  fork-once: exit(0)
Differences in `diff -u' format:
  (fork-once) begin
- (fork-once) child run
- child: exit(81)
- (fork-once) Parent: child exit status is 81
+ child: exit(-1)
+ (fork-once) Parent: child exit status is -1
  (fork-once) end
  fork-once: exit(0)

```

### userprog/exec-once: 개별 판정 원문

출처: `pintos/vm/build/tests/userprog/exec-once.result`

```text
FAIL
Test output failed to match any acceptable form.

Acceptable output:
  (exec-once) begin
  (exec-once) I'm your father
  (child-simple) run
  exec-once: exit(81)
Differences in `diff -u' format:
  (exec-once) begin
  (exec-once) I'm your father
- (child-simple) run
- exec-once: exit(81)
+ exec-once: exit(-1)

```

### vm/pt-grow-stack: 개별 판정 원문

출처: `pintos/vm/build/tests/vm/pt-grow-stack.result`

```text
FAIL
Test output failed to match any acceptable form.

Acceptable output:
  (pt-grow-stack) begin
  (pt-grow-stack) cksum: 3424492700
  (pt-grow-stack) end
Differences in `diff -u' format:
  (pt-grow-stack) begin
- (pt-grow-stack) cksum: 3424492700
- (pt-grow-stack) end

(Process exit codes are excluded for matching purposes.)

```

### vm/lazy-anon: 개별 판정 원문

출처: `pintos/vm/build/tests/vm/lazy-anon.result`

```text
PASS

```


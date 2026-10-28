#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <stddef.h>
#include "filesys/off_t.h"
#include "threads/thread.h"

struct file;

#ifdef VM
/* 문서의 aux: 최초 접근 때 실행 파일을 읽기 위해 페이지마다 보관하는 정보.
 * process.c의 loader와 vm.c의 SPT 복사 코드가 같은 구조체 정의를 사용한다. */
struct lazy_load_info {
    struct file *file;    /* 읽을 실행 파일의 참조. 파일 내용 자체를 복사하는 것은 아니다. */
    off_t offset;        /* 이 페이지의 데이터를 읽기 시작할 파일 내 바이트 위치. */
    size_t read_bytes;   /* 파일에서 읽어 프레임에 채울 바이트 수. */
    size_t zero_bytes;   /* 프레임의 나머지 부분을 0으로 채울 바이트 수. */
};
#endif

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
bool get_first_string_cpy(const char * str, char *buffer, int size);
struct fd_info * make_fd_info (int fd, struct file *file);
bool isnt_stdin_or_stdout(struct fd_info *fd_info);

#endif /* userprog/process.h */

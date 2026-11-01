#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct file;

 /* 적재할 파일의 정보를 담은 구조체 */
struct page_args {
	struct file *file;		/* 파일 */
	off_t offset;			/* 오프셋 */
	size_t read_bytes;		/* 파일에서 읽을 크기 */
	size_t zero_bytes;		/* 파일을 읽지 않고 0으로 채울 크기 */
};

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

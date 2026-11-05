#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct file;

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
bool get_first_string_cpy(const char * str, char *buffer, int size);
struct fd_info * make_fd_info (int fd, struct file *file);
bool isnt_stdin_or_stdout(struct fd_info *fd_info);
bool lazy_load_segment (struct page *page, void *aux);
bool split_and_register_pages (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable,
		enum vm_type type, vm_initializer *init);

#endif /* userprog/process.h */

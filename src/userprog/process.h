#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include <stdint.h>

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

// /* The process control block for a given process. Since
//    there can be multiple threads per process, we need a separate
//    PCB from the TCB. All TCBs in a process will have a pointer
//    to the PCB, and the PCB will have a pointer to the main thread
//    of the process, which is `special`. */
// struct process {
//     /* Owned by process.c. */
//     uint32_t* pagedir;          /* Page directory. */
//     char process_name[16];      /* Name of the main thread */
//     struct thread* main_thread; /* Pointer to main thread */
// };
/* A process's exit status. The PCB for each process contains a
   pointer to its exit status, and a list of its children's exit
   statuses. */
struct exit_status {
    pid_t pid;                  /* Process id */
    int status;                 /* Exit status */
    bool exited;                /* True if exited, false otherwise */
    bool waited;                /* True if waited, false otherwise */
    int ref_cnt;                /* Initialize to 2 */
    struct lock ref_cnt_lock;   /* Lock for ref_cnt */
    struct semaphore exit_wait; /* Down'd by parent's process_wait, up'd by process_exit */
    struct list_elem elem;      /* List element for PCB's child_exit_statuses */
};

struct user_lock {
    struct lock kernel_lock;
    struct list_elem elem;
    char lockID;
} /*user_lock*/;

struct user_semaphore {
    struct semaphore kernel_semaphore;
    struct list_elem elem;
    char semaID;
} /*user_semaphore*/;

struct user_thread {
    tid_t tid;
    uint8_t* stack;
    struct list_elem elem;
    struct semaphore join_wait;
    bool exited;
    bool waited;
} /*user_thread*/;

/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
    /* Owned by process.c. */
    uint32_t* pagedir;               /* Page directory. */
    char process_name[16];           /* Name of the main thread */
    struct thread* main_thread;      /* Pointer to main thread */
    // struct file* exec_file;          /* The file executed by this process */
    struct exit_status* exit_status; /* Pointer to this process's exit status */
    struct list child_exit_statuses; /* List of children's exit statuses */
    // user_file_list user_files;       /* List of open files */
    struct file *executing_file;
    int next_fd;
    struct list open_file_list;
    struct dir *pwd;
    // int num_opened_files;            /* Number of files previously opened */

    /* User Threads fields*/
    struct lock pthread_lock;
    struct list user_threads;

    // User synchronization primitive fields
    struct list all_locks;
    struct list all_semaphores;
    struct lock sync_locks;
    struct lock sync_semaphores;
    unsigned char num_locks;
    unsigned char num_semaphores;
    int tmp;
};

void userprog_init(void);

pid_t process_execute (const char *args);
int process_wait (pid_t);
void process_exit (int status);
void process_activate (void);
bool install_page (void *upage, void *kpage, bool writable);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

#endif /**< userprog/process.h */

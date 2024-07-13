#ifndef USERPROG_EXCEPTION_H
#define USERPROG_EXCEPTION_H

/** Page fault error code bits that describe the cause of the exception.  */
#include <stdbool.h>
#include "threads/interrupt.h"

#define PF_P 0x1    /**< 0: not-present page. 1: access rights violation. */
#define PF_W 0x2    /**< 0: read, 1: write. */
#define PF_U 0x4    /**< 0: kernel, 1: user process. */

void exception_init (void);
void exception_print_stats (void);
void page_fault_kill (struct intr_frame *f, void *fault_addr,
                      bool user, bool write, bool not_present);
#endif /**< userprog/exception.h */

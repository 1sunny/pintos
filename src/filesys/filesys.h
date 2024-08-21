#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include <threads/synch.h>
#include "filesys/off_t.h"

extern struct lock filesys_lock;

/** Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /**< Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /**< Root directory file inode sector. */

/** Block device that contains the file system. */
struct block *fs_device;

void filesys_init (bool format);
void filesys_done (void);
const char* get_path_file_name (const char *path);
 bool filesys_create (const char *name, off_t initial_size, bool is_dir);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);

#endif /**< filesys/filesys.h */

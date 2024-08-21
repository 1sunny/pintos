#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include <threads/synch.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "buffer_cache.h"

/** Partition that contains the file system. */
struct block *fs_device;

struct lock filesys_lock;

static void do_format (void);

/** Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  lock_init(&filesys_lock);

  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  buffer_cache_init();
  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/** Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

/** Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
 filesys_create (const char *name, off_t initial_size, bool is_dir)
{
  block_sector_t inode_sector = 0;

  struct dir *dir = dir_open_path(name);
  if (dir == NULL) {
    return false;
  }
  // printf("name: %s\n", name);
  const char *file_name = strrchr(name, '/');
  if (file_name != NULL) {
    file_name++;
  } else {
    file_name = name;
  }
  // printf("file_name: %s, dir->inode->sector: %d\n", file_name, inode_get_inumber(dir_get_inode(dir)));

  bool success = (dir != NULL
                     // 分配一个sector当作创建的文件的inode_disk存放位置
                  && free_map_allocate (1, &inode_sector)
                     // 在这个sector上创建inode_disk
                  && (is_dir ? dir_create(inode_sector, 0, inode_get_inumber(dir_get_inode(dir))) : inode_create (inode_sector, initial_size, REGULAR))
                  && dir_add (dir, file_name, inode_sector, is_dir));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

// 这个是根据路径和文件名进行查找到inode,然后用inode调用file_open来malloc一个struct file(pos,deny_write)
// TODO 系统调用应该只会使用这个来open吧? 不会使用file_open
/** Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir = dir_open_path(name);
  // printf("name: %s\n", name);
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, name, &inode, false);
  dir_close (dir);

  return file_open (inode);
}

/** Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  // TODO
  struct dir *dir = dir_open_root ();
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir); 

  return success;
}

/** Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
